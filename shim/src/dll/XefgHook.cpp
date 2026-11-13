// XefgHook.cpp - the shim's hooks on the exports of libxess_fg.dll (XeSS frame generation): its log (IGDEXT_XEFG_LOG),
// more than 4x (XESS_FG=5..8), the game's own multi-frame number under XESS_FG, and what the low latency of
// XellHook.cpp needs of XeSS-FG: whether frame generation is on, and the swap chain it hands the game
// (XefgAnswers.cpp).
#include "XellHook.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstdint>

// XeFG (libxess_fg.dll) diagnostics, IGDEXT_XEFG_LOG=1 (XESS_LOG=1 sets it): XeFG's own log through its public
// xefgSwapChainSetLoggingCallback, and what the game asks for and gets: the maximum number of interpolated frames after
// context creation (where an Intel Graphics Software override, tools' ControlLib.dll with XESS_FG, takes effect), the
// value the game passes at initialization, and every change of the number. Written to C:\igdext_xefg.log.
typedef int32_t (*PFN_XefgCreate)(void* dev, void** ctx);
typedef int32_t (*PFN_XefgInit)(void* ctx, void* queue, const void* params);
typedef int32_t (*PFN_XefgSetNum)(void* ctx, uint32_t n);
typedef int32_t (*PFN_XefgSetLog)(void* ctx, int32_t level, void (*cb)(const char*, int32_t, void*), void* user);
typedef int32_t (*PFN_XefgGetProps)(void* ctx, void* props);
typedef int32_t (*PFN_XefgGetInit)(void* ctx, void* params);
#pragma pack(push, 8)
struct XefgProps { uint32_t requiredDescriptorCount; uint64_t tempBufferHeapSize, tempTextureHeapSize, constantBufferSize; uint32_t maxSupportedInterpolations; };
struct XefgInitParams { void* pApplicationSwapChain; uint32_t initFlags, maxInterpolatedFrames, creationNodeMask, visibleNodeMask;
                        uint8_t rest[64]; };
#pragma pack(pop)
static PFN_XefgCreate g_fgCreate; static PFN_XefgInit g_fgInit; static PFN_XefgSetNum g_fgSetNum;
// xefgSwapChainD3D12GetSwapChainPtr(ctx, riid, ppSwapChain): the swap chain the game presents to (ProxyHook)
typedef int32_t (*PFN_XefgGetSwapChainPtr)(void* ctx, REFIID riid, void** sc);
static PFN_XefgGetSwapChainPtr g_fgGetSc;
static volatile LONG g_fgScTries;   // from a context, when the game got its swap chain before the hooks were in
static void ProxyFromContext(void* ctx)
{
    if (g_scVt || !g_fgGetSc || !ctx || g_fgScTries >= 8) return;
    InterlockedIncrement(&g_fgScTries);
    IUnknown* sc = nullptr;
    if (g_fgGetSc(ctx, __uuidof(IDXGISwapChain), (void**) &sc) == 0 && sc) { ProxyHook(sc); sc->Release(); }
}
static int32_t Hook_XefgGetSwapChainPtr(void* ctx, REFIID riid, void** sc)
{
    const int32_t r = g_fgGetSc(ctx, riid, sc);
    if (r == 0 && sc && *sc) ProxyHook((IUnknown*) *sc);
    return r;
}
static PFN_XefgSetLog g_fgSetLog; static PFN_XefgGetProps g_fgGetProps; static PFN_XefgGetInit g_fgGetInit;

static bool g_fgLog = false;
static void FLog(const char* fmt, ...)
{
    if (!g_fgLog) return;
    va_list ap; va_start(ap, fmt); AppendLogLine("C:\\igdext_xefg.log", fmt, ap); va_end(ap);
}

static void XefgLogCb(const char* msg, int32_t level, void*)
{
    if (msg) FLog("XeFG[%d] %s", level, msg);
}

static void XefgReportMax(void* ctx, const char* when)
{
    XefgProps p = {};
    int32_t r = g_fgGetProps ? g_fgGetProps(ctx, &p) : -1;
    FLog("  %s: xefgSwapChainGetProperties -> %d, maxSupportedInterpolations %u", when, r, p.maxSupportedInterpolations);
}

// More than 4x (XESS_FG=5..8): XeSS-FG takes its maximum number of interpolated frames from the Intel Graphics Software
// override (ControlLib.dll answers it; Intel's enum and XeSS-FG's parser end at 4x), and keeps that number in its own
// writable data. It is found by observation, without any knowledge of the library's layout: after the game's context
// creation (override 4x: 3 frames) every 32-bit value 3 in libxess_fg's writable sections is a candidate; a probe
// context created through the public API with the override at 3x (XESS_FG_PROBE=2, read by ControlLib.dll) leaves only
// the value that followed it. That one is set to the wanted number, and the public xefgSwapChainGetProperties of the
// game's context must then report it - otherwise the old value goes back and the game keeps 4x.
typedef int32_t (*PFN_XefgDestroy)(void* ctx);
static PFN_XefgDestroy g_fgDestroy;

static void XefgRaise(void* dev, void* gameCtx)
{
    char b[16];
    if (GetEnvironmentVariableA("XESS_FG", b, sizeof(b)) == 0) return;
    int n = atoi(b);
    if (n <= 4) return;
    uint32_t want = (uint32_t) (n > 8 ? 8 : n) - 1;
    HMODULE m = GetModuleHandleW(L"libxess_fg.dll");
    XefgProps p0 = {};
    if (!m || !g_fgCreate || !g_fgDestroy || !g_fgGetProps || g_fgGetProps(gameCtx, &p0) != 0) { FLog("  XESS_FG=%d: no libxess_fg API, 4x", n); return; }
    if (p0.maxSupportedInterpolations != 3) { FLog("  XESS_FG=%d: maximum is %u, not the 4x override (ControlLib.dll missing?)", n, p0.maxSupportedInterpolations); return; }
    // candidates: every aligned 3 in the writable sections
    uint8_t* base = (uint8_t*) m;
    auto* nt = (IMAGE_NT_HEADERS*) (base + ((IMAGE_DOS_HEADER*) base)->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    static uint32_t* cand[4096]; size_t nc = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i)
    {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
        auto* q = (uint32_t*) (base + sec[i].VirtualAddress);
        size_t words = sec[i].Misc.VirtualSize / 4;
        for (size_t k = 0; k < words; ++k)
            if (q[k] == 3) { if (nc < 4096) cand[nc++] = &q[k]; else { FLog("  XESS_FG=%d: too many candidates, 4x", n); return; } }
    }
    // the probe: the same device, override 3x
    SetEnvironmentVariableA("XESS_FG_PROBE", "2");
    void* probe = nullptr;
    int32_t r = g_fgCreate(dev, &probe);
    SetEnvironmentVariableA("XESS_FG_PROBE", nullptr);
    if (r == 0 && probe) g_fgDestroy(probe);
    uint32_t* hit = nullptr; size_t nh = 0;
    for (size_t k = 0; k < nc; ++k) if (*cand[k] == 2) { hit = cand[k]; ++nh; }
    FLog("  XESS_FG=%d: probe context -> %d, %zu candidate(s), %zu followed the override", n, r, nc, nh);
    if (nh != 1)
    {
        for (size_t k = 0; k < nc; ++k) if (*cand[k] == 2) *cand[k] = 3;   // whatever followed the probe goes back
        FLog("  XESS_FG=%d: not unique, 4x", n);
        return;
    }
    *hit = want;
    XefgProps p1 = {};
    if (g_fgGetProps(gameCtx, &p1) == 0 && p1.maxSupportedInterpolations == want)
    {
        FLog("  XESS_FG=%d: libxess_fg+0x%llx %u -> %u, the game's context now reports %u interpolated frames", n,
             (unsigned long long) ((uint8_t*) hit - base), 3u, want, p1.maxSupportedInterpolations);
        return;
    }
    *hit = 3;
    FLog("  XESS_FG=%d: the context reports %u after the change, put back: 4x", n, p1.maxSupportedInterpolations);
}

// The raise happens once per context, at the first call after its creation: in the creation hook, or - when the
// game created the context before the hooks were in place (the shim is loaded by libxess_fg itself during that call,
// as in Wuthering Waves) - at the first of the calls a game makes next to size and set up frame generation.
static void* g_fgDevice;              // the D3D12 device of the last extension request (the probe needs one)
static void* g_fgDone[16]; static LONG g_fgNDone;
static SRWLOCK g_fgLock = SRWLOCK_INIT;
static volatile DWORD g_fgOwner;      // thread inside XefgRaiseOnce: libxess_fg may call its own exports while it
                                      // creates the probe context, and those calls must not wait for themselves
static void XefgRaiseOnce(void* dev, void* ctx)
{
    if (!ctx || g_fgOwner == GetCurrentThreadId()) return;
    AcquireSRWLockExclusive(&g_fgLock);
    g_fgOwner = GetCurrentThreadId();
    bool seen = false;
    // the last 16 contexts (g_fgNDone keeps counting past the ring's size)
    const LONG nDone = std::min<LONG>(g_fgNDone, 16);
    for (LONG i = 0; i < nDone && !seen; ++i) seen = g_fgDone[i] == ctx;
    if (!seen)
    {
        g_fgDone[g_fgNDone % 16] = ctx; ++g_fgNDone;
        if (dev) XefgRaise(dev, ctx); else FLog("  XESS_FG: no D3D12 device seen yet, 4x");
    }
    g_fgOwner = 0;
    ReleaseSRWLockExclusive(&g_fgLock);
}

typedef int32_t (*PFN_XefgD3D12Props)(void* ctx, const void* init, uint32_t w, uint32_t h, uint32_t fmt, void* props);
typedef int32_t (*PFN_XefgBuild)(void* ctx, void* lib, uint8_t blocking, uint32_t flags);
static PFN_XefgD3D12Props g_fgD3D12Props; static PFN_XefgBuild g_fgBuild;

static int32_t Hook_XefgCreate(void* dev, void** ctx)
{
    int32_t r = g_fgCreate(dev, ctx);
    FLog("xefgSwapChainD3D12CreateContext -> %d ctx %p", r, ctx ? *ctx : nullptr);
    if (r == 0 && ctx && *ctx)
    {
        g_fgDevice = dev;
        XefgRaiseOnce(dev, *ctx);
        if (g_fgLog && g_fgSetLog) FLog("  xefgSwapChainSetLoggingCallback(debug) -> %d", g_fgSetLog(*ctx, 0, XefgLogCb, nullptr));
        XefgReportMax(*ctx, "after creation");
    }
    return r;
}
static int32_t Hook_XefgGetProps(void* ctx, void* props) { XefgRaiseOnce(g_fgDevice, ctx); return g_fgGetProps(ctx, props); }
static int32_t Hook_XefgD3D12Props(void* ctx, const void* init, uint32_t w, uint32_t h, uint32_t fmt, void* props)
{
    XefgRaiseOnce(g_fgDevice, ctx);
    return g_fgD3D12Props(ctx, init, w, h, fmt, props);
}
static int32_t Hook_XefgBuild(void* ctx, void* lib, uint8_t blocking, uint32_t flags)
{
    XefgRaiseOnce(g_fgDevice, ctx);
    return g_fgBuild(ctx, lib, blocking, flags);
}

static void XefgInitBefore(const char* name, void* ctx, const void* params)
{
    XefgRaiseOnce(g_fgDevice, ctx);
    uint32_t want = params ? ((const XefgInitParams*) params)->maxInterpolatedFrames : 0;
    FLog("%s: game asks maxInterpolatedFrames %u%s, init flags 0x%x, ui mode %u", name, want,
         want == 0xFFFFFFFFu ? " (maximum supported)" : "", params ? ((const XefgInitParams*) params)->initFlags : 0u,
         params ? *(const uint32_t*) (((const XefgInitParams*) params)->rest + 40) : 0u);
}
static void XefgInitAfter(void* ctx, int32_t r)
{
    if (!g_fgLog) return;
    XefgInitParams got = {};
    int32_t g = g_fgGetInit ? g_fgGetInit(ctx, &got) : -1;
    FLog("  -> %d; initialization parameters (%d): maxInterpolatedFrames %u", r, g, got.maxInterpolatedFrames);
    XefgReportMax(ctx, "after initialization");
}
// xefgSwapChainD3D12InitFromSwapChain(ctx, queue, params)
static int32_t Hook_XefgInit(void* ctx, void* queue, const void* params)
{
    XefgInitBefore("xefgSwapChainD3D12InitFromSwapChain", ctx, params);
    int32_t r = g_fgInit(ctx, queue, params);
    XefgInitAfter(ctx, r);
    return r;
}
// xefgSwapChainD3D12InitFromSwapChainDesc(ctx, hWnd, desc, fullscreenDesc, queue, dxgiFactory, params)
typedef int32_t (*PFN_XefgInitDesc)(void* ctx, HWND wnd, const void* desc, const void* fsDesc, void* queue, void* factory, const void* params);
static PFN_XefgInitDesc g_fgInitDescFn;
static int32_t Hook_XefgInitDesc(void* ctx, HWND wnd, const void* desc, const void* fsDesc, void* queue, void* factory, const void* params)
{
    XefgInitBefore("xefgSwapChainD3D12InitFromSwapChainDesc", ctx, params);
    int32_t r = g_fgInitDescFn(ctx, wnd, desc, fsDesc, queue, factory, params);
    XefgInitAfter(ctx, r);
    return r;
}

// A game with its own multi-frame choice (Wuthering Waves: 2x/3x/4x in its menu) sets the number while it runs; with
// XESS_FG the override decides, as with Intel Graphics Software, so any number but 0 (off) becomes XESS_FG's. If
// XeSS-FG refuses it (above its maximum, e.g. when raising past 4x did not work), the game's own number goes through.
static int32_t Hook_XefgSetNum(void* ctx, uint32_t n)
{
    char b[8];
    int want = GetEnvironmentVariableA("XESS_FG", b, sizeof(b)) > 0 ? atoi(b) : 0;
    if (n > 0 && want >= 2)
    {
        uint32_t m = (uint32_t) (want > 8 ? 8 : want) - 1;
        if (m != n)
        {
            int32_t r = g_fgSetNum(ctx, m);
            FLog("xefgSwapChainSetNumInterpolatedFrames(%u): XESS_FG=%d -> %u -> %d", n, want, m, r);
            if (r == 0) return r;
        }
    }
    int32_t r = g_fgSetNum(ctx, n);
    FLog("xefgSwapChainSetNumInterpolatedFrames(%u) -> %d", n, r);
    return r;
}

// xefgSwapChainTagFrameConstants(ctx, presentId, constants): with XESS_LOG the camera a game hands to XeSS-FG, two
// frames in a row every 300 (what the header's "row-major" means in practice; FSR_FG=xess builds these itself)
struct XefgFrameConstants { float view[16], proj[16], jitterX, jitterY, mvScaleX, mvScaleY; uint32_t reset; float renderTime; };
typedef int32_t (*PFN_XefgTagConstants)(void* ctx, uint32_t id, const XefgFrameConstants* c);
static PFN_XefgTagConstants g_fgTagConstants;
static int32_t Hook_XefgTagConstants(void* ctx, uint32_t id, const XefgFrameConstants* c)
{
    static volatile LONG n;
    LONG k = InterlockedIncrement(&n);
    if (!g_scVt) ProxyFromContext(ctx);
    if (g_fgLog && c && (k % 300 == 0 || k % 300 == 1))
    {
        char b[1024]; int o = 0;
        for (int i = 0; i < 16; ++i) o += snprintf(b + o, sizeof(b) - o, "%s%.5g", i ? (i % 4 ? " " : " | ") : "", c->view[i]);
        o += snprintf(b + o, sizeof(b) - o, "   proj ");
        for (int i = 0; i < 16; ++i) o += snprintf(b + o, sizeof(b) - o, "%s%.5g", i ? (i % 4 ? " " : " | ") : "", c->proj[i]);
        FLog("constants #%ld id %u: view %s; jitter %.3f,%.3f mvScale %.1f,%.1f reset %u renderTime %.6g", k, id, b,
             c->jitterX, c->jitterY, c->mvScaleX, c->mvScaleY, c->reset, c->renderTime);
    }
    return g_fgTagConstants(ctx, id, c);
}

// xefgSwapChainD3D12TagFrameResource / xefgSwapChainSetUiCompositionState: with XESS_LOG which inputs a game hands to
// XeSS-FG (each new type, size, format and validity once)
struct XefgResData { uint32_t type, validity, baseX, baseY, w, h; ID3D12Resource* res; uint32_t state; };
typedef int32_t (*PFN_XefgTagRes)(void* ctx, void* cl, uint32_t id, const XefgResData* d);
static PFN_XefgTagRes g_fgTagRes;
static int32_t Hook_XefgTagRes(void* ctx, void* cl, uint32_t id, const XefgResData* d)
{
    int32_t r = g_fgTagRes(ctx, cl, id, d);
    if (g_fgLog && d)
    {
        static uint64_t seen[32]; static volatile LONG nSeen;
        uint32_t fmt = 0;
        if (d->res) { D3D12_RESOURCE_DESC rd = d->res->GetDesc(); fmt = (uint32_t) rd.Format; }
        uint64_t key = (uint64_t) d->type | (uint64_t) d->validity << 4 | (uint64_t) fmt << 8 | (uint64_t) d->w << 16 | (uint64_t) d->h << 40 | (uint64_t) (cl != nullptr) << 63;
        bool known = false;
        for (LONG i = 0; i < nSeen && i < 32; ++i) known |= seen[i] == key;
        // the slot is taken first: two threads past the check would otherwise both index from one count
        const LONG slot = !known && nSeen < 32 ? InterlockedIncrement(&nSeen) - 1 : 32;
        if (slot < 32)
        {
            seen[slot] = key;
            FLog("tag resource id %u: type %u validity %u base %u,%u size %ux%u format %u state 0x%x cmdlist %d -> %d", id, d->type,
                 d->validity, d->baseX, d->baseY, d->w, d->h, fmt, d->state, cl != nullptr, r);
        }
    }
    return r;
}
typedef int32_t (*PFN_XefgUiState)(void* ctx, uint32_t state);
static PFN_XefgUiState g_fgUiState;
static int32_t Hook_XefgUiState(void* ctx, uint32_t state)
{
    int32_t r = g_fgUiState(ctx, state);
    FLog("xefgSwapChainSetUiCompositionState(%u) -> %d", state, r);
    return r;
}

// xefgSwapChainSetEnabled(ctx, enable): XeSS-FG's on/off switch, the low latency's source for "frame generation on"
typedef int32_t (*PFN_XefgSetEnabled)(void* ctx, uint32_t enable);
static PFN_XefgSetEnabled g_fgSetEnabled;
static int32_t Hook_XefgSetEnabled(void* ctx, uint32_t enable)
{
    const int32_t r = g_fgSetEnabled(ctx, enable);
    if (r == 0) { g_fgSource = "xefgSwapChainSetEnabled"; InterlockedExchange(&g_fgState, enable != 0); }
    if (r == 0 && enable) ProxyFromContext(ctx);
    FLog("xefgSwapChainSetEnabled(%u) -> %d", enable, r);
    return r;
}

static bool g_fgWanted = false;
static volatile LONG g_fgHooked = 0;
static SRWLOCK g_fgHookLock = SRWLOCK_INIT;
// hooks the exports of the libxess_fg that is loaded now, once per load (Wuthering Waves loads it, unloads it and
// loads it again); returns false while there is none or it is not ready (still loading or being unloaded)
static bool XefgInstallHooks()
{
    HMODULE m = nullptr;
    // pinned while it is patched: an unload in between would leave half a hook table
    if (!GetModuleHandleExW(0, L"libxess_fg.dll", &m) || !m) return false;
    AcquireSRWLockExclusive(&g_fgHookLock);
    // hooked = this load's CreateContext jumps to our hook (a reload at the same address comes back unpatched)
    bool done = false;
    if (const uint8_t* c = (const uint8_t*) GetProcAddress(m, "xefgSwapChainD3D12CreateContext"))
        done = c[0] == 0xE9 && c + 5 + *(const int32_t*) (c + 1) == (const uint8_t*) Hook_XefgCreate;
    struct { const char* name; void* hook; void** orig; void* entry; } hooks[] = {
        { "xefgSwapChainD3D12CreateContext", (void*) Hook_XefgCreate, (void**) &g_fgCreate, nullptr },
        { "xefgSwapChainGetProperties", (void*) Hook_XefgGetProps, (void**) &g_fgGetProps, nullptr },
        { "xefgSwapChainD3D12GetProperties", (void*) Hook_XefgD3D12Props, (void**) &g_fgD3D12Props, nullptr },
        { "xefgSwapChainD3D12BuildPipelines", (void*) Hook_XefgBuild, (void**) &g_fgBuild, nullptr },
        { "xefgSwapChainD3D12InitFromSwapChain", (void*) Hook_XefgInit, (void**) &g_fgInit, nullptr },
        { "xefgSwapChainD3D12InitFromSwapChainDesc", (void*) Hook_XefgInitDesc, (void**) &g_fgInitDescFn, nullptr },
        { "xefgSwapChainSetNumInterpolatedFrames", (void*) Hook_XefgSetNum, (void**) &g_fgSetNum, nullptr },
        { "xefgSwapChainTagFrameConstants", (void*) Hook_XefgTagConstants, (void**) &g_fgTagConstants, nullptr },
        { "xefgSwapChainD3D12TagFrameResource", (void*) Hook_XefgTagRes, (void**) &g_fgTagRes, nullptr },
        { "xefgSwapChainSetUiCompositionState", (void*) Hook_XefgUiState, (void**) &g_fgUiState, nullptr },
    };
    bool ready = !done;
    for (auto& h : hooks)
        if (ready && !(h.entry = (void*) GetProcAddress(m, h.name))) ready = false;   // all of them, or none yet
    if (ready)
    {
        char path[MAX_PATH] = {}; GetModuleFileNameA(m, path, MAX_PATH);
        FLog("libxess_fg.dll at %p: %s", m, path);
        g_fgSetLog = (PFN_XefgSetLog) GetProcAddress(m, "xefgSwapChainSetLoggingCallback");
        g_fgGetInit = (PFN_XefgGetInit) GetProcAddress(m, "xefgSwapChainD3D12GetInitializationParameters");
        g_fgDestroy = (PFN_XefgDestroy) GetProcAddress(m, "xefgSwapChainDestroy");
        for (auto& h : hooks)
        {
            // PatchThunk stores the original before the jmp goes live, so the hook never sees a null original
            bool ok = PatchThunk(h.entry, h.hook, h.orig, h.name);
            FLog("%s %s", h.name, ok ? "hooked" : "NOT hooked");
        }
        // apart from the set above: a library without it keeps the other hooks, and low latency then stays off unless
        // XeLL reports frame generation
        if (void* e = (void*) GetProcAddress(m, "xefgSwapChainSetEnabled"))
            FLog("xefgSwapChainSetEnabled %s", PatchThunk(e, (void*) Hook_XefgSetEnabled, (void**) &g_fgSetEnabled, "xefgSwapChainSetEnabled") ? "hooked" : "NOT hooked");
        // and the swap chain's getter, for the game's questions to the swap chain (ProxyHook); a new library has a
        // new virtual table
        g_fgScTries = 0;
        if (void* e = (void*) GetProcAddress(m, "xefgSwapChainD3D12GetSwapChainPtr"))
        {
            const bool ok = PatchThunk(e, (void*) Hook_XefgGetSwapChainPtr, (void**) &g_fgGetSc, "xefgSwapChainD3D12GetSwapChainPtr");
            if (!ok) g_fgGetSc = (PFN_XefgGetSwapChainPtr) e;   // not hooked: still there to ask (ProxyFromContext)
            FLog("xefgSwapChainD3D12GetSwapChainPtr %s", ok ? "hooked" : "NOT hooked");
        }
        g_fgHooked = 1;
        done = true;
    }
    ReleaseSRWLockExclusive(&g_fgHookLock);
    FreeLibrary(m);
    return done;
}

// every D3D12 extension request: libxess_fg asks for the extension while it creates a context, so this is where the
// hooks go in when the library loaded the shim itself
void XefgNoteDevice(const void* dev)
{
    if (!g_fgWanted) return;
    if (dev) g_fgDevice = const_cast<void*>(dev);
    XefgInstallHooks();
}

static DWORD WINAPI XefgThread(LPVOID)
{
    HANDLE ev = WatchStart(WATCH_XEFG);
    const ULONGLONG deadline = GetTickCount64() + kWatchTotalMs;
    // the install first (the library may have loaded before the notification was in place), and once more after the
    // last wait; a wake that finds it not ready (being unloaded) goes back to waiting for the next load
    while (!XefgInstallHooks() && WatchWait(ev, deadline, 10)) {}
    if (ev) WatchStop();
    if (!g_fgHooked) FLog("libxess_fg.dll never loaded");
    return 0;
}

void StartXefgHook()
{
    // the log (IGDEXT_XEFG_LOG=1), the frame generation override (XESS_FG) or the low latency (its on/off source, after
    // StartXellHook set g_capAuto) needs the hooks
    char b[8];
    g_fgLog = GetEnvironmentVariableA("IGDEXT_XEFG_LOG", b, sizeof(b)) > 0 && atoi(b);
    bool raise = GetEnvironmentVariableA("XESS_FG", b, sizeof(b)) > 0 && atoi(b) >= 2;   // also 2..4: the game's own number
    if (!g_fgLog && !raise && !g_capAuto) return;
    g_fgWanted = true;
    if (XefgInstallHooks()) return;   // already loaded (it loaded the shim): hook now, not later
    if (!StartModuleRefThread(XefgThread)) FLog("XeFG hook thread not started (%lu)", GetLastError());
}

// XellHook.cpp - in-process fix for the frame cap Wuthering Waves hands to Intel XeLL (libxell.dll) when frame generation is enabled.
// XeLL's exports are 5-byte "jmp rel32" thunks; xellSetSleepMode / xellSetFgEnabled / xellSleep are redirected to the functions below.
// The game applies a 33.3 ms frame cap (30 fps, counted per displayed frame) right after it enables frame generation and only
// replaces it when the pause menu is opened and closed. A nonzero interval arriving within 1.5 s of xellSetFgEnabled(1) is replaced by
// the interval that was in effect before.
// The fix is on for Wuthering Waves only (process path contains "Wuthering Waves"), a cap set at that moment in any other game
// is left alone.
//   IGDEXT_XELL_FIXCAP=1    apply the fix in any game;  IGDEXT_XELL_KEEPCAP=1   turn it off everywhere
//   IGDEXT_XELL_LOG=1       append the parameters the game passes and xellSleep timing (real frame rate) to C:\igdext_xell.log
#include "Stdafx.h"
#include "Trace.h"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>

#pragma pack(push, 1)
struct XellSleepParams { uint32_t minimumIntervalUs; uint32_t flags; }; // flags bit0 = bLowLatencyMode, bit1 = bLowLatencyBoost
#pragma pack(pop)
typedef uint32_t (*PFN_XellSet)(void*, const XellSleepParams*);
typedef uint32_t (*PFN_XellSleep)(void*, uint32_t);
typedef uint32_t (*PFN_XellFg)(void*, uint32_t, uint32_t);

static PFN_XellSet g_origSet; static PFN_XellSleep g_origSleep; static PFN_XellFg g_origFg;
static bool g_log = false;
static bool g_fixCap = false; static uint32_t g_effInterval = 0, g_savedInterval = 0; static ULONGLONG g_fgOnTick = 0;
static volatile LONG g_sleepCalls = 0;
typedef uint32_t (*PFN_XellSetLog)(void*, uint32_t, void (*)(const char*, uint32_t));
static PFN_XellSetLog g_setLog;   // xellSetLoggingCallback: XeLL's own messages go to the log with IGDEXT_XELL_LOG
// xellGetFramesReports: per-frame time stamps (ns) XeLL collects; with IGDEXT_XELL_LOG the average time from the start
// of a frame's simulation to the end of its present (the latency XeLL itself works on) is logged every 100 frames
#pragma pack(push, 8)
struct XellFrameReport { uint32_t id; uint64_t simS, simE, rsS, rsE, prS, prE, r1, r2, r3, r4, r5; };
#pragma pack(pop)
typedef uint32_t (*PFN_XellReports)(void*, XellFrameReport*);
static PFN_XellReports g_reports;

static void XLog(const char* fmt, ...)
{
    if (!g_log) return;
    FILE* f = nullptr;
    if (fopen_s(&f, "C:\\igdext_xell.log", "ab") != 0 || !f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "%02d:%02d:%02d.%03d [%lu] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, GetCurrentThreadId());
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}

static void XellLogCb(const char* msg, uint32_t level)
{
    if (msg) XLog("XeLL[%u] %s", level, msg);
}

static uint32_t Hook_SetFg(void* ctx, uint32_t a, uint32_t b)
{
    static int n = 0;
    if (n++ < 12 || (n % 600) == 0) XLog("xellSetFgEnabled ctx=%p a=%u b=%u", ctx, a, b);
    if (a == 1) { g_savedInterval = g_effInterval; g_fgOnTick = GetTickCount64(); }
    return g_origFg(ctx, a, b);
}

static uint32_t Hook_SetSleepMode(void* ctx, const XellSleepParams* p)
{
    if (!p) return g_origSet(ctx, p);
    XellSleepParams q = *p;
    XLog("xellSetSleepMode ctx=%p game: minimumIntervalUs=%u lowLatency=%u boost=%u", ctx, p->minimumIntervalUs, p->flags & 1, (p->flags >> 1) & 1);
    if (g_fixCap && q.minimumIntervalUs != 0 && g_fgOnTick && GetTickCount64() - g_fgOnTick < 1500)
    { XLog("   -> frame cap %u us set right after frame generation was enabled: keeping %u us", q.minimumIntervalUs, g_savedInterval); q.minimumIntervalUs = g_savedInterval; }
    g_effInterval = q.minimumIntervalUs;
    return g_origSet(ctx, &q);
}

static uint32_t Hook_Sleep(void* ctx, uint32_t frame)
{
    LONG n = InterlockedIncrement(&g_sleepCalls);
    if (!g_log) return g_origSleep(ctx, frame);
    if (n == 1 && g_setLog) XLog("xellSetLoggingCallback(debug) -> %u", g_setLog(ctx, 0, XellLogCb));
    if (n < 5 || (n % 600) == 0) XLog("xellSleep call #%ld frame=%u", (long) n, frame);
    LARGE_INTEGER qa, qb, qf; QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&qa);
    uint32_t rr = g_origSleep(ctx, frame);
    QueryPerformanceCounter(&qb);
    static double sum = 0, mx = 0; static int cnt = 0; static LARGE_INTEGER t0;
    double d = (qb.QuadPart - qa.QuadPart) * 1000.0 / qf.QuadPart;
    if (!t0.QuadPart) t0 = qa;
    sum += d; if (d > mx) mx = d; ++cnt;
    if (cnt == 100)
    {
        double wall = (qb.QuadPart - t0.QuadPart) * 1000.0 / qf.QuadPart;
        XLog("xellSleep stats: 100 calls in %.0f ms (%.1f ms/frame), blocked avg %.2f ms max %.2f ms = %.0f%% of wall", wall, wall / 100, sum / 100, mx, 100.0 * sum / wall);
        if (g_reports)
        {
            static XellFrameReport r[64];
            memset(r, 0, sizeof(r));
            if (g_reports(ctx, r) == 0)
            {
                double lat = 0, sub = 0; int k = 0;
                for (int i = 0; i < 64; ++i)
                {
                    if (!r[i].id || !r[i].simS || r[i].prE <= r[i].simS || r[i].prE - r[i].simS > 1000000000ull) continue;
                    lat += (r[i].prE - r[i].simS) / 1e6; sub += (r[i].rsS > r[i].simS ? (r[i].rsS - r[i].simS) / 1e6 : 0); ++k;
                }
                if (k) XLog("xell latency: simulation start -> present end avg %.1f ms (sim -> render submit %.1f ms) over %d frames", lat / k, sub / k, k);
            }
        }
        sum = mx = 0; cnt = 0; t0 = qb;
    }
    return rr;
}

static bool PatchThunk(void* thunk, void* hook, void** orig, const char* name)
{
    uint8_t* p = (uint8_t*) thunk;
    if (p[0] != 0xE9) { XLog("%s: export is not a jmp rel32 thunk (byte %02x), not hooked", name, p[0]); return false; }
    int32_t rel = *(int32_t*) (p + 1);
    *orig = p + 5 + rel;
    intptr_t d = (uint8_t*) hook - (p + 5);
    if (d > INT32_MAX || d < INT32_MIN) { XLog("%s: hook too far from the thunk (%lld), not hooked", name, (long long) d); return false; }
    DWORD old;
    if (!VirtualProtect(p, 8, PAGE_EXECUTE_READWRITE, &old)) { XLog("%s: VirtualProtect failed %lu", name, GetLastError()); return false; }
    uint64_t cur = *(volatile uint64_t*) p;
    uint64_t neu = (cur & ~0xFFFFFFFFFFull) | 0xE9ull | ((uint64_t)(uint32_t)(int32_t) d << 8);
    InterlockedExchange64((volatile LONG64*) p, (LONG64) neu);
    VirtualProtect(p, 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 8);
    XLog("%s hooked: thunk %p -> %p (orig %p)", name, thunk, hook, *orig);
    return true;
}

static DWORD WINAPI XellThread(LPVOID)
{
    HMODULE m = nullptr;
    for (int i = 0; i < 6000 && !(m = GetModuleHandleW(L"libxell.dll")); ++i) Sleep(100);
    if (!m) { XLog("libxell.dll never loaded"); return 0; }
    XLog("libxell.dll found at %p", m);
    Sleep(200);
    auto set = (void*) GetProcAddress(m, "xellSetSleepMode");
    auto slp = (void*) GetProcAddress(m, "xellSleep");
    auto fg = (void*) GetProcAddress(m, "xellSetFgEnabled");
    if (!set || !slp || !fg) { XLog("XeLL exports missing"); return 0; }
    g_setLog = (PFN_XellSetLog) GetProcAddress(m, "xellSetLoggingCallback");
    g_reports = (PFN_XellReports) GetProcAddress(m, "xellGetFramesReports");
    void* dummy = nullptr;
    PatchThunk(slp, (void*) Hook_Sleep, &dummy, "xellSleep"); g_origSleep = (PFN_XellSleep) dummy;
    PatchThunk(set, (void*) Hook_SetSleepMode, &dummy, "xellSetSleepMode"); g_origSet = (PFN_XellSet) dummy;
    PatchThunk(fg, (void*) Hook_SetFg, &dummy, "xellSetFgEnabled"); g_origFg = (PFN_XellFg) dummy;
    return 0;
}

void StartXellHook()
{
    char b[32];
    if (GetEnvironmentVariableA("IGDEXT_XELL_LOG", b, sizeof(b)) > 0) g_log = true;
    {
        char exe[MAX_PATH] = {}; GetModuleFileNameA(nullptr, exe, MAX_PATH);
        g_fixCap = strstr(exe, "Wuthering Waves") != nullptr;
    }
    if (GetEnvironmentVariableA("IGDEXT_XELL_FIXCAP", b, sizeof(b)) > 0 && atoi(b)) g_fixCap = true;
    if (GetEnvironmentVariableA("IGDEXT_XELL_KEEPCAP", b, sizeof(b)) > 0 && atoi(b)) g_fixCap = false;
    if (!g_fixCap && !g_log) return;
    char exe[MAX_PATH] = {}; GetModuleFileNameA(nullptr, exe, MAX_PATH);
    XLog("XellHook start in %s", exe);
    HANDLE h = CreateThread(nullptr, 0, XellThread, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
}

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
static PFN_XefgCreate g_fgCreate; static PFN_XefgInit g_fgInit, g_fgInitDesc; static PFN_XefgSetNum g_fgSetNum;
static PFN_XefgSetLog g_fgSetLog; static PFN_XefgGetProps g_fgGetProps; static PFN_XefgGetInit g_fgGetInit;

static bool g_fgLog = false;
static void FLog(const char* fmt, ...)
{
    if (!g_fgLog) return;
    FILE* f = nullptr;
    if (fopen_s(&f, "C:\\igdext_xefg.log", "ab") != 0 || !f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "%02d:%02d:%02d.%03d [%lu] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, GetCurrentThreadId());
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
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

static int32_t Hook_XefgCreate(void* dev, void** ctx)
{
    int32_t r = g_fgCreate(dev, ctx);
    FLog("xefgSwapChainD3D12CreateContext -> %d ctx %p", r, ctx ? *ctx : nullptr);
    if (r == 0 && ctx && *ctx)
    {
        XefgRaise(dev, *ctx);
        if (g_fgLog && g_fgSetLog) FLog("  xefgSwapChainSetLoggingCallback(debug) -> %d", g_fgSetLog(*ctx, 0, XefgLogCb, nullptr));
        XefgReportMax(*ctx, "after creation");
    }
    return r;
}

static int32_t XefgInitCommon(PFN_XefgInit orig, const char* name, void* ctx, void* queue, const void* params)
{
    uint32_t want = params ? ((const XefgInitParams*) params)->maxInterpolatedFrames : 0;
    FLog("%s: game asks maxInterpolatedFrames %u%s", name, want, want == 0xFFFFFFFFu ? " (maximum supported)" : "");
    int32_t r = orig(ctx, queue, params);
    XefgInitParams got = {};
    int32_t g = g_fgGetInit ? g_fgGetInit(ctx, &got) : -1;
    FLog("  -> %d; initialization parameters (%d): maxInterpolatedFrames %u", r, g, got.maxInterpolatedFrames);
    XefgReportMax(ctx, "after initialization");
    return r;
}
static int32_t Hook_XefgInit(void* ctx, void* queue, const void* params) { return XefgInitCommon(g_fgInit, "xefgSwapChainD3D12InitFromSwapChain", ctx, queue, params); }
static int32_t Hook_XefgInitDesc(void* ctx, void* queue, const void* params) { return XefgInitCommon(g_fgInitDesc, "xefgSwapChainD3D12InitFromSwapChainDesc", ctx, queue, params); }

static int32_t Hook_XefgSetNum(void* ctx, uint32_t n)
{
    int32_t r = g_fgSetNum(ctx, n);
    FLog("xefgSwapChainSetNumInterpolatedFrames(%u) -> %d", n, r);
    return r;
}

static DWORD WINAPI XefgThread(LPVOID)
{
    HMODULE m = nullptr;
    for (int i = 0; i < 60000 && !(m = GetModuleHandleW(L"libxess_fg.dll")); ++i) Sleep(10);
    if (!m) { FLog("libxess_fg.dll never loaded"); return 0; }
    char path[MAX_PATH] = {}; GetModuleFileNameA(m, path, MAX_PATH);
    FLog("libxess_fg.dll at %p: %s", m, path);
    g_fgSetLog = (PFN_XefgSetLog) GetProcAddress(m, "xefgSwapChainSetLoggingCallback");
    g_fgGetProps = (PFN_XefgGetProps) GetProcAddress(m, "xefgSwapChainGetProperties");
    g_fgGetInit = (PFN_XefgGetInit) GetProcAddress(m, "xefgSwapChainD3D12GetInitializationParameters");
    g_fgDestroy = (PFN_XefgDestroy) GetProcAddress(m, "xefgSwapChainDestroy");
    struct { const char* name; void* hook; void** orig; } hooks[] = {
        { "xefgSwapChainD3D12CreateContext", (void*) Hook_XefgCreate, (void**) &g_fgCreate },
        { "xefgSwapChainD3D12InitFromSwapChain", (void*) Hook_XefgInit, (void**) &g_fgInit },
        { "xefgSwapChainD3D12InitFromSwapChainDesc", (void*) Hook_XefgInitDesc, (void**) &g_fgInitDesc },
        { "xefgSwapChainSetNumInterpolatedFrames", (void*) Hook_XefgSetNum, (void**) &g_fgSetNum },
    };
    for (auto& h : hooks)
    {
        void* e = (void*) GetProcAddress(m, h.name);
        if (!e) { FLog("%s: not exported", h.name); continue; }
        // PatchThunk stores the original before the jmp goes live, so the hook never sees a null original
        bool ok = PatchThunk(e, h.hook, h.orig, h.name);
        FLog("%s %s", h.name, ok ? "hooked" : "NOT hooked");
    }
    return 0;
}

void StartXefgHook()
{
    // the log (IGDEXT_XEFG_LOG=1), or more than 4x frame generation (XESS_FG=5..), needs the hooks
    char b[8];
    g_fgLog = GetEnvironmentVariableA("IGDEXT_XEFG_LOG", b, sizeof(b)) > 0 && atoi(b);
    bool raise = GetEnvironmentVariableA("XESS_FG", b, sizeof(b)) > 0 && atoi(b) > 4;
    if (!g_fgLog && !raise) return;
    HANDLE h = CreateThread(nullptr, 0, XefgThread, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
}

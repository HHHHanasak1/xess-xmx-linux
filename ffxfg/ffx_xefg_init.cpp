// ffx_xefg_init.cpp - what amd_fidelityfx_framegeneration_dx12.dll sets up once, at the first FFX call (Init): the
// settings from the environment and ffx_xefg.env (listed at the top of ffx_xefg.cpp; the variables are in ffx_xefg.h),
// the XeSS-FG and XeLL libraries with their entry points. Also PinSelf, and the vendor id that XeSS-FG and XeLL get on
// an Intel GPU that DXVK reports as another vendor's.
#include "ffx_xefg.h"

// A patched vtable (vkd3d's queue, DXVK's adapter) jumps into this module from then on: it must never be unmapped
// (games load, free and load the FidelityFX DLLs again). Pins once; false (logged once) = do not patch.
bool PinSelf()
{
    static std::atomic<int> pinned{0};   // 1 pinned, -1 failed
    if (pinned.load() == 0) {
        HMODULE m = nullptr;
        bool ok = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                     (LPCWSTR)&PinSelf, &m) && m;
        int expect = 0;
        if (pinned.compare_exchange_strong(expect, ok ? 1 : -1) && !ok)
            Log("could not pin this DLL (%lu): the vendor id and queue hooks stay off", GetLastError());
    }
    return pinned.load() > 0;
}

static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static HMODULE g_xefgModule, g_xellModule;

// An Intel GPU that DXVK reports with another vendor id (dxgi.customVendorId, e.g. 1002 for a game that offers FSR only
// on AMD): XeSS-FG and XeLL read the vendor from the DXGI adapter description and without 0x8086 they skip Intel's
// Control Library (no multi-frame generation, 2x at most). The adapter's GetDesc* answer them, and only them, with
// 0x8086 when the description is Intel's; the game keeps seeing the vendor DXVK reports. FFX_XEFG_VENDOR=0 turns it off.
typedef HRESULT(STDMETHODCALLTYPE* PFN_GetDesc)(IDXGIAdapter*, void*);
static PFN_GetDesc g_origGetDesc[4];
static const int kGetDescSlot[4] = {8, 10, 11, 18};  // GetDesc, GetDesc1, GetDesc2, GetDesc3

static bool CalledByXess(void* ret)
{
    HMODULE m = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)ret,
                       &m);
    return m && (m == g_xefgModule || m == g_xellModule);
}

// DXGI_ADAPTER_DESC, DESC1, DESC2 and DESC3 all start with Description[128] followed by VendorId
template <int I> static HRESULT STDMETHODCALLTYPE HookGetDesc(IDXGIAdapter* a, void* desc)
{
    HRESULT hr = g_origGetDesc[I](a, desc);
    if (SUCCEEDED(hr) && desc && CalledByXess(_ReturnAddress())) {
        auto* d = (DXGI_ADAPTER_DESC*)desc;
        if (d->VendorId != 0x8086 && wcsncmp(d->Description, L"Intel", 5) == 0)
            d->VendorId = 0x8086;
    }
    return hr;
}

void IntelVendorForXess(ID3D12Device* dev)
{
    static bool done;
    if (done || EnvInt("FFX_XEFG_VENDOR", 1) == 0)
        return;
    IDXGIFactory4* f = nullptr;
    IDXGIAdapter1* a = nullptr;
    DXGI_ADAPTER_DESC1 d = {};
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f))))
        return;
    if (SUCCEEDED(f->EnumAdapterByLuid(dev->GetAdapterLuid(), IID_PPV_ARGS(&a))) && SUCCEEDED(a->GetDesc1(&d)) &&
        d.VendorId != 0x8086 && wcsncmp(d.Description, L"Intel", 5) == 0) {
        done = true;
        if (!PinSelf()) {
            a->Release();
            f->Release();
            return;
        }
        static void* const hooks[4] = {(void*)HookGetDesc<0>, (void*)HookGetDesc<1>, (void*)HookGetDesc<2>,
                                       (void*)HookGetDesc<3>};
        IDXGIAdapter4* a4 = nullptr;
        int slots = SUCCEEDED(a->QueryInterface(IID_PPV_ARGS(&a4))) ? 4 : 2;  // IDXGIAdapter1 has GetDesc and GetDesc1
        if (a4)
            a4->Release();
        void** vt = *(void***)a;
        for (int i = 0; i < slots; i++) {
            DWORD old;
            void** slot = &vt[kGetDescSlot[i]];
            if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old))
                continue;
            g_origGetDesc[i] = (PFN_GetDesc)*slot;
            *slot = hooks[i];
            VirtualProtect(slot, sizeof(void*), old, &old);
        }
        Log("GPU '%ls' reported with vendor %04x: XeSS-FG and XeLL get 8086", d.Description, d.VendorId);
    }
    if (a)
        a->Release();
    f->Release();
}

static HMODULE LoadBeside(const char* dir, const char* name)
{
    char path[MAX_PATH];
    snprintf(path, MAX_PATH, "%s%s", dir, name);
    HMODULE m = LoadLibraryA(path);
    if (!m)
        m = LoadLibraryA(name);
    Log("load %s -> %p", name, (void*)m);
    return m;
}

static BOOL CALLBACK InitOnce(PINIT_ONCE, PVOID, PVOID*)
{
    char dir[MAX_PATH], env[MAX_PATH];
    InitLogAndEnv("ffx_xefg", "FFX_XEFG_LOG", dir);   // ffx_xefg.env, ffx_xefg.log

    g_mult = EnvInt("FFX_XEFG_MULT", EnvInt("XESS_FG", 2));
    if (g_mult < 2)
        g_mult = 2;
    g_xefgLogLevel = EnvInt("FFX_XEFG_XEFGLOG", 2);
    TR(g_sleep = EnvInt("FFX_XEFG_SLEEP", 1) != 0);
    TR(g_ownWait = EnvInt("FFX_XEFG_OWNWAIT", 1) != 0);
    TR(g_pace = EnvInt("FFX_XEFG_PACE", 0) != 0);
    g_noExclusive = EnvInt("FFX_XEFG_EXCLUSIVE", 0) == 0;
    g_hudless = EnvInt("FFX_XEFG_HUDLESS", 1) != 0;
    TR(g_mvNdc = EnvInt("FFX_XEFG_MVNDC", 0) != 0);
    TR(g_validityNow = EnvInt("FFX_XEFG_VALIDITY", 1) != 0);
    g_oflow = EnvInt("FFX_XEFG_OFLOW", 0);
    g_times = EnvInt("FFX_XEFG_TIMES", 0) != 0;
    if (GetEnvironmentVariableA("FFX_XEFG_CAP", env, MAX_PATH))
        g_capAuto = !_stricmp(env, "auto");
    else
        g_capAuto = g_oflow != 0;
    g_ffxCap = g_capAuto || EnvInt("FFX_XEFG_CAP", 0) > 0;
    {
        // IGDEXT_XELL_CAP, parsed as XellHook.cpp in the shim does: off/0 = off, auto[:ms] = auto, a plain number up to
        // 1000000 = XeLL's fixed cap (set by the shim), anything else = off
        DWORD n = GetEnvironmentVariableA("IGDEXT_XELL_CAP", env, 32);
        if (n > 0 && n < 32) {
            char* end = nullptr;
            unsigned long us = env[0] >= '0' && env[0] <= '9' ? strtoul(env, &end, 10) : 0;
            if (!strncmp(env, "auto", 4) && (env[4] == 0 || env[4] == ':')) {
                g_lat = 1;
                if (env[4] == ':')
                    g_latMarginMs = atof(env + 5);
            } else if (!_stricmp(env, "off") || !strcmp(env, "0")) {
                g_lat = 0;
            } else {
                g_lat = end && *end == 0 && us > 0 && us <= 1000000 ? 2 : 0;
            }
        }   // longer than 31 characters: ignored, as the shim does (auto)
    }
    // With the optical flow too, where the frame cap paces the frames (Resident Evil 4 at 3x, 2026-11-11: with the swap
    // chain's own answers the game ran with an idle GPU part of the time, 30.3 ms a frame with a 95th percentile of
    // 42 ms; with these answers 27.7 ms and 32 ms, under the same cap. At 2x there is no difference.)
    g_pollCache = EnvInt("IGDEXT_XELL_POLLS", g_lat != 0) != 0;
    TR(g_abSec = EnvInt("FFX_XEFG_AB", 0));
    TR(g_abOld = EnvInt("FFX_XEFG_AB_OLD", 0) != 0);
    TR(if (GetEnvironmentVariableA("FFX_XEFG_HOLD", env, MAX_PATH)) g_holdMs = (float)atof(env));
    g_latCheck = EnvInt("IGDEXT_XELL_CHECK", 1) != 0;
    g_hdr10 = EnvInt("FFX_XEFG_HDR10", 0) != 0;
    TR(g_hdr10AbSec = EnvInt("FFX_XEFG_HDR10_AB", 0));
    TR(tr::g_subs = EnvInt("FFX_XEFG_TRACE_SUBS", 0) != 0; if (EnvInt("FFX_XEFG_TRACE", 0)) tr::Start(dir));
    if (GetEnvironmentVariableA("FFX_XEFG_OFLOW_DEPTH", env, MAX_PATH))
        g_oflowDepth = (float)atof(env);
    if (g_oflow) {
        g_oflowLog = Log;
        g_hudless = false;
        Log("optical flow motion vectors (depth %.3f), the game's depth, motion vectors, camera and UI unused; cap %s",
            g_oflowDepth, g_capAuto ? "auto" : "as set");
    }
    snprintf(g_dir, MAX_PATH, "%s", dir);
    Log("=== ffx_xefg start pid %lu, multiplier %dx, sleep %d, own wait %d (paced by the GPU %d), exclusive fullscreen %s",
        GetCurrentProcessId(), g_mult, g_sleep, g_ownWait, g_pace, g_noExclusive ? "blocked" : "allowed");
    Log("low latency (IGDEXT_XELL_CAP): %s; the game's questions to the swap chain answered %s",
        g_lat == 1 && !g_ffxCap ? "auto, the game's frames are held back in Present"
        : g_lat == 1            ? "FFX_XEFG_CAP's XeLL interval"
        : g_lat == 2            ? "XeLL's fixed cap"
                                : "off",
        g_pollCache ? "as of the last Present (no wait)" : "by the swap chain");
    if (g_lat == 1 && !g_ffxCap && g_latMarginMs != 0)
        Log("  each frame starts %.1f ms earlier", g_latMarginMs);
    if (!g_hudless || g_mvNdc || !g_sleep || !g_ownWait || !g_validityNow)
        Log("inputs: HUD-less %d, NDC mv %d, validity only now %d", g_hudless, g_mvNdc, g_validityNow);
    if (g_hdr10AbSec > 0)
        Log("HDR10 back buffers: XeSS-FG is told and not told in turns of %d s (FFX_XEFG_HDR10_AB)", g_hdr10AbSec);
    else if (g_hdr10)
        Log("HDR10 back buffers: XeSS-FG is told (FFX_XEFG_HDR10=1)");
#ifdef FFXFG_DEBUG
    g_camera = EnvInt("FFX_XEFG_CAMERA", 1);
    g_jitter = (float)EnvInt("FFX_XEFG_JITTER", 1);
    g_dumpEvery = EnvInt("FFX_XEFG_DUMP", 0);
    if (GetEnvironmentVariableA("FFX_XEFG_RENDERTIME", env, MAX_PATH))
        g_renderTime = (float)atof(env);
    if (GetEnvironmentVariableA("FFX_XEFG_RENDERSCALE", env, MAX_PATH))
        g_renderScale = (float)atof(env);
    Log("debug build: camera matrices %d, jitter x%.0f, dump every %d, render time %g, x%g", g_camera, g_jitter, g_dumpEvery,
        g_renderTime, g_renderScale);
#endif

    HMODULE fg = LoadBeside(dir, "libxess_fg.dll");
    HMODULE xell = LoadBeside(dir, "libxell.dll");
    g_xefgModule = fg;
    g_xellModule = xell;
    bool ok = fg && xell;
#define LOAD_FG(name, req)                                         \
    p_##name = fg ? (decltype(p_##name))GetProcAddress(fg, #name) : nullptr; \
    if (!p_##name && req) {                                        \
        Log("missing %s", #name);                                  \
        ok = false;                                                \
    }
#define LOAD_XELL(name, req)                                           \
    p_##name = xell ? (decltype(p_##name))GetProcAddress(xell, #name) : nullptr; \
    if (!p_##name && req) {                                            \
        Log("missing %s", #name);                                      \
        ok = false;                                                    \
    }
    XEFG_FUNCS(LOAD_FG)
    XELL_FUNCS(LOAD_XELL)
    g_libsOk = ok;
    if (ok) {
        xefg_swapchain_version_t v = {};
        p_xefgSwapChainGetVersion(&v);
        Log("XeSS-FG %u.%u.%u", v.major, v.minor, v.patch);
    }
    return TRUE;
}

void Init() { InitOnceExecuteOnce(&g_once, InitOnce, nullptr, nullptr); }

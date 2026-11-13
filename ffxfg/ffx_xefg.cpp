// ffx_xefg.cpp - amd_fidelityfx_framegeneration_dx12.dll that implements AMD's FFX frame generation API on top of
// Intel XeSS frame generation (libxess_fg.dll, proxy swap chain) and XeLL (libxell.dll).
//
// The game keeps its FSR 3.1+/4 integration; the FFX loader loads this DLL in place of AMD's frame generation
// provider. Frame generation swap chain contexts (WRAP / NEW / FOR_HWND) create an XeSS-FG proxy swap chain,
// wrapped in FgSwapChain for the XeLL present markers; FRAMEGENERATION contexts tag depth, motion vectors,
// HUD-less color and the camera constants for XeSS-FG. A game that gives a HUD-less color gets XeSS-FG's UI composition
// from it, as AMD's frame generation separates the UI that way. The game's own interpolation dispatch is a no-op:
// XeSS-FG interpolates inside the proxy swap chain.
//
// The same DLL, as ffx_xefg_bridge.dll, serves a game without FSR frame generation (FSR_FG=inject, xefg_bridge.h),
// and with FFX_XEFG_OFLOW=1 (OFLOW_FG=1) any DX12 game: the motion vectors come from FidelityFX optical flow on the
// final image (ffx_oflow.cpp), the bridge makes a frame on every present itself.
//
// Environment:
//   FFX_XEFG_MULT=n   frames shown per rendered frame (2 = one interpolated frame; default: XESS_FG, else 2)
//   FFX_XEFG_LOG=path log file (default ffx_xefg.log next to the DLL); FFX_XEFG_LOG=0 turns it off
//   FFX_XEFG_XEFGLOG=0..3  XeSS-FG library log level forwarded to the log (default 2 = warnings)
//   FFX_XEFG_SLEEP=0  no xellSleep (XeLL stays enabled, as XeSS-FG requires)
//   FFX_XEFG_OWNWAIT=0     hand the game the proxy's frame latency waitable object (deadlocks CONTROL Resonant)
//   FFX_XEFG_PACE=1        release our waitable object when the GPU finished the frame, not when Present returns
//   FFX_XEFG_VALIDITY=0    tag depth and motion vectors UNTIL_NEXT_PRESENT instead of ONLY_NOW (no copy; measuring aid, can flicker)
//   FFX_XEFG_EXCLUSIVE=1   let the game enter exclusive fullscreen (XeSS-FG then refuses to interpolate)
//   FFX_XEFG_OFLOW=1       motion vectors from FidelityFX optical flow on the final image instead of the game's,
//                          constant depth (FFX_XEFG_OFLOW_DEPTH, default 1.0), no camera, no HUD-less color or UI: what
//                          a driver-level frame generation has (ffx_oflow.cpp; tools/proton-hook.py sets it for OFLOW_FG=1)
//   FFX_XEFG_OFLOW_DIV=n   the optical flow sees the image at 1/n size (1..4, default 1)
//   FFX_XEFG_CAP=us|auto|0 the interval of a real frame in microseconds through XeLL, for even frames; auto (the default
//                          with FFX_XEFG_OFLOW) follows the GPU time of a frame; 0 = none. FFX_XEFG_CAP_PAD=ms is auto's
//                          first guess of what a frame needs beyond that (default 1.5, then learned), FFX_XEFG_REFRESH=Hz
//                          the display's refresh rate when Windows reports a wrong one
//   FFX_XEFG_TIMES=1       frame time, time in Present and XeLL's sleep (percentiles) in the log every 300 frames, and
//                          the gaps between the shown frames' Presents ("The shown frames")
//   IGDEXT_XELL_CAP        the low latency, as for a game's own XeSS-FG (read by the XMX shim too): unset or auto[:ms]
//                          = the game stays in Present until its next frame should start, so that the frame reaches
//                          its Present when XeSS-FG can take it instead of waiting there ("Low latency" below);
//                          off = no such wait; <us> = XeLL's fixed cap holds the frame back there instead
//   IGDEXT_XELL_POLLS=0|1  1 (the default with the low latency, with FFX_XEFG_OFLOW too): what a game asks of the
//                          swap chain every frame (GetFrameStatistics, CheckColorSpaceSupport) is answered as of
//                          its last Present and never waits for XeSS-FG's presentation thread; 0: asked from the
//                          swap chain each time (the XMX shim reads it too, for a game's own XeSS-FG)
//   FFX_XEFG_AB=s          measuring aid: the low latency's wait is off and on in turns of s seconds (off first);
//                          with FFX_XEFG_AB_OLD=1 three turns: as before (answers from the swap chain, no wait),
//                          the answers as of the last Present, the wait too. FFX_XEFG_HOLD=ms: a fixed wait in the
//                          place of the one the low latency works out
//   IGDEXT_XELL_CHECK=0    the low latency's wait is not checked (by default it stays only where the frame time with
//                          it is within 1 % of the frame time without it and the latency is lower; the XMX shim
//                          reads it too)
//   FFX_XEFG_HDR10=1       XeSS-FG is told when the game's back buffer is HDR10 (PQ); by default it is not, and
//                          interpolates the PQ values as they are ("HDR10" below). FFX_XEFG_HDR10_AB=s: measuring
//                          aid, told and not told in turns of s seconds (told first)
// The same KEY=VALUE lines can go into ffx_xefg.env next to the DLL (the environment wins). XESS_FG itself belongs in
// the launch options: the XMX shim reads it when it starts, before this DLL is loaded.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <intrin.h>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <algorithm>
#include <vector>

#pragma warning(disable : 4996)
#include "inc/ffx/api/include/ffx_api.h"
#include "inc/ffx/api/include/ffx_api_types.h"
#include "inc/ffx/api/include/dx12/ffx_api_dx12.h"
#include "inc/ffx/framegeneration/include/ffx_framegeneration.h"
#include "inc/ffx/framegeneration/include/dx12/ffx_api_framegeneration_dx12.h"
#include "xefg_bridge.h"
#include "inc/xess/xess_fg/xefg_swapchain.h"
#include "inc/xess/xess_fg/xefg_swapchain_d3d12.h"
#include "inc/xess/xell/xell.h"
#include "inc/xess/xell/xell_d3d12.h"
#include "ffx_oflow.h"
#include "../common/lowlat_check.h"

#pragma comment(lib, "dxgi.lib")

// ---------------------------------------------------------------------------------------------------------------
// Logging

static HMODULE g_self;
static FILE* g_log;
static std::mutex g_logLock;
static LARGE_INTEGER g_qpf, g_qp0;

static void Log(const char* fmt, ...)
{
    if (!g_log)
        return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double ms = (double)(now.QuadPart - g_qp0.QuadPart) * 1000.0 / (double)g_qpf.QuadPart;
    std::lock_guard<std::mutex> lk(g_logLock);
    va_list ap;
    va_start(ap, fmt);
    fprintf(g_log, "%10.3f t%-5lu ", ms, GetCurrentThreadId());
    vfprintf(g_log, fmt, ap);
    fputc('\n', g_log);
    va_end(ap);
    fflush(g_log);
}

// A patched vtable (vkd3d's queue, DXVK's adapter) jumps into this module from then on: it must never be unmapped
// (games load, free and load the FidelityFX DLLs again). Pins once; false (logged once) = do not patch.
static bool PinSelf()
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

static int EnvInt(const char* name, int def)
{
    char buf[32];
    if (!GetEnvironmentVariableA(name, buf, sizeof(buf)))
        return def;
    return atoi(buf);
}

static uint64_t Qpc()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)t.QuadPart;
}
static float QpcMs(uint64_t a, uint64_t b) { return (float)((double)(b - a) * 1000.0 / (double)g_qpf.QuadPart); }

// TR(...): only in a debug build (FFX_XEFG_TRACE)
#ifdef FFXFG_DEBUG
#include "ffx_trace.h"
#define TR(...) __VA_ARGS__
#else
#define TR(...)
#endif

// ---------------------------------------------------------------------------------------------------------------
// XeSS-FG / XeLL entry points, loaded at run time

#define XEFG_FUNCS(X)                                \
    X(xefgSwapChainD3D12CreateContext, true)         \
    X(xefgSwapChainD3D12InitFromSwapChain, true)     \
    X(xefgSwapChainD3D12InitFromSwapChainDesc, true) \
    X(xefgSwapChainD3D12GetSwapChainPtr, true)       \
    X(xefgSwapChainD3D12TagFrameResource, true)      \
    X(xefgSwapChainTagFrameConstants, true)          \
    X(xefgSwapChainSetEnabled, true)                 \
    X(xefgSwapChainSetPresentId, true)               \
    X(xefgSwapChainGetProperties, true)              \
    X(xefgSwapChainGetLastPresentStatus, true)       \
    X(xefgSwapChainSetLoggingCallback, true)         \
    X(xefgSwapChainDestroy, true)                    \
    X(xefgSwapChainSetLatencyReduction, true)        \
    X(xefgSwapChainGetVersion, true)                 \
    X(xefgSwapChainSetNumInterpolatedFrames, false)  \
    X(xefgSwapChainSetUiCompositionState, false)

#define XELL_FUNCS(X)              \
    X(xellD3D12CreateContext, true) \
    X(xellDestroyContext, true)     \
    X(xellSetSleepMode, true)       \
    X(xellSleep, true)              \
    X(xellAddMarkerData, true)

#define DECLARE_FN(name, req) static decltype(&name) p_##name;
XEFG_FUNCS(DECLARE_FN)
XELL_FUNCS(DECLARE_FN)

static bool g_libsOk;
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static int g_mult = 2;
static int g_xefgLogLevel = 2;
static bool g_sleep = true;
static bool g_ownWait = true;
static bool g_pace = false;     // FFX_XEFG_PACE=1: the waitable object is released when the GPU finished the frame, not when Present returns
static bool g_noExclusive = true;
static int g_oflow = 0;          // FFX_XEFG_OFLOW=1 (see the top)
static float g_oflowDepth = 1.0f;
static bool g_times = false;     // FFX_XEFG_TIMES=1
static bool g_capAuto = false;   // FFX_XEFG_CAP=auto (the default with FFX_XEFG_OFLOW)
// The low latency ("Low latency" below), from IGDEXT_XELL_CAP as the XMX shim reads it: 1 = auto (the default): the
// game is kept in Present until its next frame should start; 2 = <us>: no wait of ours, XeLL's frame cap (the shim
// sets it) sleeps there; 0 = off / anything else
static int g_lat = 1;
static double g_latMarginMs = 0;   // IGDEXT_XELL_CAP=auto:<ms>: each frame starts <ms> earlier (later if negative)
// FFX_XEFG_CAP is set (auto, the default with FFX_XEFG_OFLOW, or <us>): its XeLL interval holds the frames back, no
// wait of ours on top (CapAdjust reads XeLL's sleep to tell whether its interval holds frames back; a wait in front
// of that sleep would hide it)
static bool g_ffxCap = false;
static bool g_pollCache = false;    // IGDEXT_XELL_POLLS (see the top)
static int g_abSec = 0;             // FFX_XEFG_AB (see the top)
static bool g_abOld = false;        // FFX_XEFG_AB_OLD
static float g_holdMs = 0;          // FFX_XEFG_HOLD
static bool g_latCheck = true;      // IGDEXT_XELL_CHECK=0: the wait is not checked
static bool g_hdr10 = false;        // FFX_XEFG_HDR10=1: XeSS-FG is told when the back buffer is HDR10 ("HDR10" below)
static int g_hdr10AbSec = 0;        // FFX_XEFG_HDR10_AB
static bool g_hudless = true;   // FFX_XEFG_HUDLESS=0: no HUD-less color, no UI composition
static int g_camera = 1;        // the game's camera as view and projection matrices (FFXFG_DEBUG: FFX_XEFG_CAMERA)
static bool g_mvNdc = false;    // FFX_XEFG_MVNDC=1: motion vectors in NDC (init flag USE_NDC_VELOCITY), as Cyberpunk 2077 passes them
#ifdef FFXFG_DEBUG
// debug build (build.bat with FFXFG_DEBUG=1): change or leave out one of the game's inputs at a time, to find the one
// behind an artifact
//   FFX_XEFG_CAMERA=0: identity view and projection matrices, 2: transposed, 3: projection not inverted, 4: both,
//                      5: all zero, 6: the game's camera ignored (a fixed view, the real projection)
//   FFX_XEFG_JITTER=0: no jitter, -1: jitter negated
//   FFX_XEFG_DUMP=<n>: depth and motion vectors of every n-th frame into files (8 at most)
//   FFX_XEFG_RENDERTIME=<ms>: XeSS-FG's frame render time instead of the game's frame time
//   FFX_XEFG_RENDERSCALE=<f>: the game's frame time times this (0.001: seconds)
//   FFX_XEFG_ORDER=1: the order of the game's calls with their frame ids, for the first 300 after interpolation starts
//   FFX_XEFG_TRACE_SUBS=1 (with FFX_XEFG_TRACE): when the GPU finished each submission to a D3D12 queue, too
//   FFX_XEFG_TRACE=1: the time line of every frame as events in ffx_xefg_trace.txt (ffx_trace.h; tools/fsr-trace.py
//                     reads it); FFX_XEFG_TRACE_STACKS=<n>: where the present thread waits after a Present, n samples
static float g_jitter = 1.0f;
static int g_dumpEvery = 0;
static float g_renderTime = -1;
static float g_renderScale = 1;
#endif
static bool g_validityNow = true;  // FFX_XEFG_VALIDITY=0: depth and motion vectors tagged UNTIL_NEXT_PRESENT (no copy; can flicker)
static char g_dir[MAX_PATH];    // the DLL's folder (the game's)

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

static void IntelVendorForXess(ID3D12Device* dev)
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

// The upscalers the game's FidelityFX loader offers on this GPU, through the public version query; the first one is what
// a game that does not choose (FSR 3.1.0 to 3.1.3 cannot) gets
static void LogUpscalers(ID3D12Device* dev)
{
    static bool done;
    if (done || !dev)
        return;
    done = true;
    HMODULE m = GetModuleHandleA("amd_fidelityfx_loader_dx12.dll");
    if (!m || m == g_self)
        m = GetModuleHandleA("amd_fidelityfx_dx12.dll");
    auto query = m && m != g_self ? (PfnFfxQuery)GetProcAddress(m, "ffxQuery") : nullptr;
    if (!query)
        return;
    uint64_t n = 8, ids[8] = {};
    const char* names[8] = {};
    ffxQueryDescGetVersions q = {};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType = FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_UPSCALE, 0);
    q.device = dev;
    q.outputCount = &n;
    q.versionIds = ids;
    q.versionNames = names;
    ffxReturnCode_t r = query(nullptr, &q.header);
    char list[512] = "";
    for (uint64_t i = 0; r == FFX_API_RETURN_OK && i < n && i < 8; i++)
        snprintf(list + strlen(list), sizeof list - strlen(list), "%s%s", i ? ", " : "", names[i] ? names[i] : "?");
    Log("upscalers on this GPU (%d): %s", (int)r, list);
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
    QueryPerformanceFrequency(&g_qpf);
    QueryPerformanceCounter(&g_qp0);

    char dir[MAX_PATH];
    GetModuleFileNameA(g_self, dir, MAX_PATH);
    char* slash = strrchr(dir, '\\');
    if (slash)
        slash[1] = 0;

    {
        // ffx_xefg.env beside the DLL: KEY=VALUE lines, set into the environment unless already set
        char path[MAX_PATH];
        snprintf(path, MAX_PATH, "%sffx_xefg.env", dir);
        if (FILE* f = fopen(path, "r")) {
            char line[512];
            while (fgets(line, sizeof line, f)) {
                for (size_t n = strlen(line); n && (line[n - 1] == 10 || line[n - 1] == 13);)  // strip CR LF
                    line[--n] = 0;
                char* eq = strchr(line, '=');
                if (line[0] == '#' || !eq)
                    continue;
                *eq = 0;
                if (!GetEnvironmentVariableA(line, nullptr, 0))
                    SetEnvironmentVariableA(line, eq + 1);
            }
            fclose(f);
        }
    }

    char env[MAX_PATH];
    if (GetEnvironmentVariableA("FFX_XEFG_LOG", env, MAX_PATH)) {
        if (strcmp(env, "0") != 0)
            g_log = fopen(env, "a");
    } else {
        char path[MAX_PATH];
        snprintf(path, MAX_PATH, "%sffx_xefg.log", dir);
        g_log = fopen(path, "w");   // the last run only: it stays in the game's folder
    }

    g_mult = EnvInt("FFX_XEFG_MULT", EnvInt("XESS_FG", 2));
    if (g_mult < 2)
        g_mult = 2;
    g_xefgLogLevel = EnvInt("FFX_XEFG_XEFGLOG", 2);
    g_sleep = EnvInt("FFX_XEFG_SLEEP", 1) != 0;
    g_ownWait = EnvInt("FFX_XEFG_OWNWAIT", 1) != 0;
    g_pace = EnvInt("FFX_XEFG_PACE", 0) != 0;
    g_noExclusive = EnvInt("FFX_XEFG_EXCLUSIVE", 0) == 0;
    g_hudless = EnvInt("FFX_XEFG_HUDLESS", 1) != 0;
    g_mvNdc = EnvInt("FFX_XEFG_MVNDC", 0) != 0;
    g_validityNow = EnvInt("FFX_XEFG_VALIDITY", 1) != 0;
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
    g_abSec = EnvInt("FFX_XEFG_AB", 0);
    g_abOld = EnvInt("FFX_XEFG_AB_OLD", 0) != 0;
    if (GetEnvironmentVariableA("FFX_XEFG_HOLD", env, MAX_PATH))
        g_holdMs = (float)atof(env);
    g_latCheck = EnvInt("IGDEXT_XELL_CHECK", 1) != 0;
    g_hdr10 = EnvInt("FFX_XEFG_HDR10", 0) != 0;
    g_hdr10AbSec = EnvInt("FFX_XEFG_HDR10_AB", 0);
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

static void Init() { InitOnceExecuteOnce(&g_once, InitOnce, nullptr, nullptr); }

static void XefgLogCallback(const char* message, xefg_swapchain_logging_level_t level, void*)
{
    Log("xefg[%d] %s", (int)level, message);
}

// ---------------------------------------------------------------------------------------------------------------
// Helpers

static D3D12_RESOURCE_STATES ToD3D12State(uint32_t s)
{
    D3D12_RESOURCE_STATES r = D3D12_RESOURCE_STATE_COMMON;
    if (s & FFX_API_RESOURCE_STATE_UNORDERED_ACCESS)
        r |= D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if (s & FFX_API_RESOURCE_STATE_COMPUTE_READ)
        r |= D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (s & FFX_API_RESOURCE_STATE_PIXEL_READ)
        r |= D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (s & FFX_API_RESOURCE_STATE_COPY_SRC)
        r |= D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (s & FFX_API_RESOURCE_STATE_COPY_DEST)
        r |= D3D12_RESOURCE_STATE_COPY_DEST;
    if (s & FFX_API_RESOURCE_STATE_INDIRECT_ARGUMENT)
        r |= D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    if (s & FFX_API_RESOURCE_STATE_RENDER_TARGET)
        r |= D3D12_RESOURCE_STATE_RENDER_TARGET;
    if (s & FFX_API_RESOURCE_STATE_DEPTH_ATTACHMENT)
        r |= D3D12_RESOURCE_STATE_DEPTH_WRITE;
    return r;
}

static void Mul4(const float* a, const float* b, float* out)
{
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            float s = 0;
            for (int k = 0; k < 4; k++)
                s += a[r * 4 + k] * b[k * 4 + c];
            out[r * 4 + c] = s;
        }
}

// Row-vector (DirectX) view matrix from camera position and basis.
static void BuildView(const float* pos, const float* right, const float* up, const float* fwd, float* m)
{
    auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    const float r[16] = {right[0], up[0], fwd[0], 0, right[1], up[1], fwd[1], 0, right[2], up[2], fwd[2], 0,
                         -dot(pos, right), -dot(pos, up), -dot(pos, fwd), 1};
    memcpy(m, r, sizeof(r));
}

// Left-handed perspective projection, depth 0..1 (XMMatrixPerspectiveFovLH layout), with the FFX depth flags.
static void BuildProjection(float fovY, float aspect, float n, float f, bool inverted, bool infinite, float* m)
{
    memset(m, 0, 16 * sizeof(float));
    // the planes in either order (with inverted depth UE passes near FLT_MAX, far = the near plane), FLT_MAX = infinity
    float lo = std::min(n, f), hi = std::max(n, f);
    if (infinite || hi >= 1e30f)
        hi = INFINITY;
    n = lo;
    f = hi;
    if (!(fovY > 0.0001f) || !(aspect > 0.0001f) || !(n > 0) || n == f) {
        m[0] = m[5] = m[10] = m[15] = 1;
        return;
    }
    if (inverted) {
        float t = n;
        n = f;
        f = t;
    }
    float ys = 1.0f / tanf(fovY * 0.5f);
    m[0] = ys / aspect;
    m[5] = ys;
    m[11] = 1;
    if (std::isinf(f)) {  // far at infinity
        m[10] = 1;
        m[14] = -n;
    } else if (std::isinf(n)) {  // inverted infinite: near plane value at infinity
        m[10] = 0;
        m[14] = f;
    } else {
        m[10] = f / (f - n);
        m[14] = -n * f / (f - n);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Contexts

enum : uint32_t { MAGIC_FG = 0x46474658, MAGIC_SC = 0x43534658 };  // "XFGF", "XFSC"

static const uint64_t kProviderVersionId = 0xf600000000c01006ull;  // what AMD's 4.0.1 DLL reports
static const char* kProviderVersionName = "3.1.6";

struct FgSwapChain;

struct ScState {
    uint32_t magic = MAGIC_SC;
    std::mutex lock;
    std::atomic<int> refs{1};  // the FFX context; the FgSwapChain adds one

    xefg_swapchain_handle_t xefg = nullptr;
    xell_context_handle_t xell = nullptr;
    IDXGISwapChain4* proxy = nullptr;
    ID3D12Device* device = nullptr;
    FgSwapChain* wrapper = nullptr;
    FgSwapChain* contextRef = nullptr;  // the context's own reference to the wrapper, as FFX's context holds one
    Oflow* oflow = nullptr;             // FFX_XEFG_OFLOW
    uint64_t oflowLastQpc = 0;          // FFX_XEFG_OFLOW with FSR_FG=inject: the last present, for the frame time
    uint64_t oflowTags = 0;
    std::atomic<int> colorSpace{-1};    // the last successful SetColorSpace1, -1 = none (FFX_XEFG_OFLOW's encoding)
    // HDR10 (see there), all under hdrLock: what XeSS-FG's interpolation takes the back buffer for
    std::mutex hdrLock;
    bool hdrTold = false;               // for HDR10
    bool hdrSet = false;                // the game set HDR10 since the buffers were last resized
    bool hdrKept = false;               // the real swap chain took the colour space meant to be refused: no more tries
    bool hdrSaid = false;
    uint64_t hdrAbStart = 0;            // FFX_XEFG_HDR10_AB (present thread)
    int hdrAbTurn = -1;                 // 0 told, 1 not told
    // FFX_XEFG_TIMES=1: frame times at the present, reported every 300 frames
    uint64_t tStart = 0, tLastEnd = 0;
    std::vector<float> tFrame, tPresent, tSleep;
    // FFX_XEFG_CAP=auto: the real frame interval follows what the game needs (its frame time without XeLL's sleep)
    std::vector<float> capWork, capFrame, capSleep, capPresent;
    double capFreqSum = 0;    // MHz of the busy samples in this window
    int capFreqN = 0;
    float capFreqRef = 0;     // the clock the GPU reaches when it is kept busy (slowly forgotten)
    float capPad = -1;        // learned extra ms per frame (CapAdjust)
    float capFloor = 0;       // us, the last GPU-bound interval, slowly forgotten
    bool capOff = false;      // an outside limiter paces, no cap of ours
    uint32_t capUs = 0;       // per real frame, 0 = none
    uint64_t capLastEnd = 0;
    int capCalm = 0;          // windows in a row that would allow a shorter interval
    int capRoom = 0;          // windows in a row with the GPU well below capFloor and XeLL sleeping
    uint32_t width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t maxInterpolations = 1;

    // Objects handed to games that drive the interpolation dispatch themselves, made on the first query.
    bool interpMade = false;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* cmdList = nullptr;
    ID3D12Resource* interpTex = nullptr;

    // Per-frame state.
    bool gameEnabled = false;
    bool xefgEnabled = false;
    bool havePrepare = false;     // PrepareV2 seen since the last present
    bool uiComposition = false;
    uint32_t frameId = 0;          // id of the frame being built
    uint64_t presents = 0;
    // XeLL wants, per frame id, xellSleep first and then the six markers once each in order. The game prepares and
    // presents on different threads, some prepare frame N+1 before presenting frame N, and FFX frame ids can repeat
    // or restart, so what went out is kept per id in a small ring (no allocation): a marker asked for out of order
    // fills in the ones before it, a repeated marker is not sent again.
    struct XellFrame {
        uint32_t id = 0;
        bool used = false;
        bool sleeping = false;   // xellSleep(id) in flight outside the lock: markers for the id wait for it
        bool slept = false;      // xellSleep done, or forgone because a marker went out first
        uint8_t wanted = 0, sent = 0;   // bit (1 << xell_latency_marker_type_t)
    };
    XellFrame xellFrames[8];
    unsigned xellNext = 0;   // the slot taken next
    struct FfxApiResource ui = {};
    uint32_t uiFlags = 0;
    // Frame latency waitable object of our own, so the game's wait follows our presents instead of XeSS-FG's
    // presentation thread. FFX_XEFG_PACE=1 (default 0, no gain measured in CONTROL Resonant): released when the GPU has finished the presented frame, like
    // DXGI's own object: after each Present the game's queue signals `fence`, and `paceThread` releases the semaphore
    // when the value is reached, or after 200 ms regardless (a stuck wait is the worst case: CONTROL Resonant
    // deadlocked on the proxy's object). FFX_XEFG_PACE=0, the default: released as soon as Present returns.
    HANDLE latencySem = nullptr;
    UINT maxLatency = 1;
    ID3D12CommandQueue* queue = nullptr;   // the game's, given at creation (XeSS-FG presents on it)
    ID3D12Fence* fence = nullptr;          // made with the thread on the first GetFrameLatencyWaitableObject
    HANDLE paceEvent = nullptr;            // a new signal, a fence completion, or stop
    HANDLE paceThread = nullptr;
    std::atomic<bool> paceStop{false};
    std::mutex paceLock;                   // around signal + publish of fenceValue (presents should not overlap, but)
    std::atomic<uint64_t> fenceValue{0};   // last value signalled on the queue
    uint64_t paceReleased = 0;             // values the thread has released the semaphore for (thread only)
    uint64_t watchdogs = 0;                // releases by the 200 ms watchdog (thread only)
    BOOL fakeFullscreen = FALSE;   // what the game asked for while exclusive fullscreen is blocked
    void* realSc = nullptr;   // the real swap chain XeSS-FG made ("The shown frames")

    // The low latency (LatHold), on the present thread
    struct LatCtl {
        double hold = 0;             // ms the game is kept in Present after the proxy's Present returned
        double prevEarly = 0;        // how much later the frame before could have started (ms)
        int unheld = 0;              // frames in a row that could not have started later
        uint64_t lastEnd = 0;        // the last return of the proxy's Present (QPC)
        double frameMs = 0;          // the interval of those returns, smoothed
        double lateMs = 1.0;         // late wake-ups of the wait (WaitUntil)
        bool on = false;
        uint64_t abStart = 0;        // FFX_XEFG_AB: 0 = as before, 1 = answers as of the last Present, 2 = our wait too
        int abTurn = -1, abSaid = -1;
        // the GPU's side (LatGpu): how long before the GPU gets free a frame's first work reaches it (ms, smoothed;
        // negative: the GPU idles first), the GPU's time for a frame (ms, smoothed), the frames they are from
        double aheadMs = 0, gpuMs = 0;
        int gpuN = 0;
        uint64_t prevFirst = 0, prevStart = 0;   // of the frame before: its first submission, its start
        // The check of the wait (common/lowlat_check.h): whether the frame may wait now, the wait as it was when the
        // check took it away (it comes back as it was), and the wait that was in the frame just measured
        lowlat::Check chk;
        bool chkAllowed = true;
        double holdKept = 0, lastHeld = 0;
        // the log, every 300 presents
        int logN = 0, logWaited = 0, logLatN = 0, logGpuN = 0;
        double logWait = 0, logLat = 0, logBlock = 0, logFrame = 0, logGpuLat = 0;
    } lat;
    // The GPU's side of the low latency: a fence on the game's queue, signalled after each proxy Present (the frame's
    // interpolation is the last thing on that queue), and a thread that notes when the GPU reaches it (LatGpuThread)
    struct LatGpu {
        ID3D12Fence* fence = nullptr;
        HANDLE event = nullptr, thread = nullptr;
        std::atomic<bool> stop{false};
        std::atomic<uint64_t> signalled{0};
        std::atomic<uint64_t> doneAt[16];   // when the GPU reached value v (QPC), at [v % 16]; 0 = not yet
    } latGpu;
    // when the game's frame began: the last return of Present to the game, or of a call of the game into the swap
    // chain that waited there
    std::atomic<uint64_t> frameStart{0};
    // What the game asks of the swap chain every frame, answered as of its last Present call (IGDEXT_XELL_POLLS, see
    // "Low latency"): the frame statistics, and the support of the color spaces it asked about
    struct ColorSpaceAnswer {
        DXGI_COLOR_SPACE_TYPE cs;
        HRESULT hr;
        UINT support;
        bool asked;       // since the last Present: asked again there
        uint32_t idle;    // Presents in a row without a question
    };
    std::mutex pollLock;
    bool pollValid = false;
    DXGI_FRAME_STATISTICS stats = {};
    HRESULT statsHr = E_FAIL;
    ColorSpaceAnswer colorSpaces[8] = {};
    int nColorSpaces = 0;
    std::atomic<int> waitedSaid{0};   // log lines about calls of the game that waited in the swap chain
#ifdef FFXFG_DEBUG
    tr::GpuMark *gpu1 = nullptr, *gpu2 = nullptr, *gpu3 = nullptr;   // FFX_XEFG_TRACE
#endif

    // FSR_FG=inject (xefg_bridge.h): the game knows nothing of frame generation, so the ids are ours. A frame's upscale
    // can be recorded before the previous frame is presented; presents take the upscaled frames in order.
    bool bridge = false;
    uint32_t nextId = 0;
    std::deque<uint32_t> pending;
};

struct FgCtx {
    uint32_t magic = MAGIC_FG;
    uint32_t flags = 0;
    uint32_t displayW = 0, displayH = 0, maxRenderW = 0, maxRenderH = 0;
    uint32_t backBufferFormat = 0;
    ID3D12Device* device = nullptr;
    uint64_t prepares = 0;
};

static std::mutex g_globalLock;
static ScState* g_currentSc;     // the last created live swap chain context
static std::atomic<ID3D12CommandQueue*> g_timedQueue{nullptr};   // FFX_XEFG_CAP=auto (HookEcl)
static std::atomic<ScState*> g_timedSc{nullptr};
static std::mutex g_timedLock;   // g_timedSc against TearDown: HookEcl uses the state under it
static uint32_t g_lastFgFlags;   // creation flags of the last FG context, used for the XeSS-FG init flags
static bool g_haveFgFlags;

// The shown frames (see there): the real swap chain's Presents, on the game's and XeSS-FG's presentation thread
static struct EvenState {
    std::atomic<void*> real{nullptr};       // the swap chain XeSS-FG presents on
    std::atomic<int> mult{2};               // images per rendered frame
    std::atomic<int> image{0};              // the newest image's index
    // FFX_XEFG_TIMES: the Present calls of the shown frames
    std::mutex lock;
    uint64_t lastCall = 0;
    float gMin = 1e9f, gMax = 0;            // gaps in the rendered frame under way
    int gN = 0;
    std::vector<float> gaps, ratios;
    int frames = 0;
} g_ev;

static bool PaceStop(ScState* st);
static bool LatGpuStop(ScState* st);
#ifdef FFXFG_DEBUG
// FFX_XEFG_TRACE: the mark signalled in front of the game's first submission of a frame, and that frame's present number
static std::atomic<tr::GpuMark*> g_tracePre{nullptr};
static std::atomic<uint64_t> g_traceNext{0};
#endif
// the low latency's swap chain: its game queue, and the game's first submission on it since Present returned (QPC,
// 0 = none yet; HookEcl)
static std::atomic<ID3D12CommandQueue*> g_latQueue{nullptr};
static std::atomic<uint64_t> g_latFirstSubmit{0};

// The XMX shim's own low latency (AutoPaceWait in shim/src/dll/XellHook.cpp) leaves an XeLL context to its caller when
// told so; a shim without the call never waits for our contexts anyway (it finds no frame in flight at our xellSleep)
typedef void (*PFN_IgdextXellExternalPacing)(void* xellContext, int on);
static void ShimExternalPacing(void* xell, int on)
{
    HMODULE shim = xell ? GetModuleHandleA("igdext64.dll") : nullptr;
    auto fn = shim ? (PFN_IgdextXellExternalPacing)GetProcAddress(shim, "IgdextXellExternalPacing") : nullptr;
    if (fn)
        fn(xell, on);
}

// The XMX shim answers a game's questions to XeSS-FG's swap chain for a game with its own XeSS-FG (ProxyHook in
// shim/src/dll/XellHook.cpp). This DLL wraps that swap chain and answers its game itself (FgSwapChain), so it tells
// the shim to pass this swap chain's calls on unchanged; a shim without the call does not touch them anyway.
typedef void (*PFN_IgdextXefgProxyOwned)(void* swapChain, int on);
static void ShimProxyOwned(void* proxy, int on)
{
    HMODULE shim = proxy ? GetModuleHandleA("igdext64.dll") : nullptr;
    auto fn = shim ? (PFN_IgdextXefgProxyOwned)GetProcAddress(shim, "IgdextXefgProxyOwned") : nullptr;
    if (fn)
        fn(proxy, on);
}

static void TearDown(ScState* st)
{
    Log("teardown sc %p (xefg %p, presents %llu)", (void*)st, (void*)st->xefg, (unsigned long long)st->presents);
    {
        // first: once g_timedSc is cleared under this lock, HookEcl is done with st and does not find it again
        std::lock_guard<std::mutex> lk(g_timedLock);
        if (g_timedSc.load() == st) {
            g_timedQueue = nullptr;
            g_timedSc = nullptr;
        }
    }
    if (st->realSc && g_ev.real.load() == st->realSc)
        g_ev.real = nullptr;   // its Presents pass through from now on
    if (!PaceStop(st)) {
        Log("!! the pacing thread did not stop: the state stays allocated");
        return;
    }
    if (!LatGpuStop(st)) {
        Log("!! the low latency's GPU thread did not stop: the state stays allocated");
        return;
    }
#ifdef FFXFG_DEBUG
    if (g_tracePre.load() == st->gpu3)
        g_tracePre = nullptr;
    tr::GpuMarkDestroy(st->gpu1);
    tr::GpuMarkDestroy(st->gpu2);
    tr::GpuMarkDestroy(st->gpu3);
    st->gpu1 = st->gpu2 = st->gpu3 = nullptr;
#endif
    ShimExternalPacing(st->xell, 0);
    ShimProxyOwned(st->proxy, 0);
    if (st->proxy) {
        ULONG r = st->proxy->Release();
        Log("  proxy released, refs left %lu", r);
        st->proxy = nullptr;
    }
    if (st->xefg) {
        xefg_swapchain_result_t r = p_xefgSwapChainDestroy(st->xefg);
        Log("  xefgSwapChainDestroy -> %d", (int)r);
    }
    if (st->xell) {
        xell_result_t r = p_xellDestroyContext(st->xell);
        Log("  xellDestroyContext -> %d", (int)r);
    }
    OflowDestroy(st->oflow);
    st->oflow = nullptr;
    if (st->cmdList)
        st->cmdList->Release();
    if (st->alloc)
        st->alloc->Release();
    if (st->interpTex)
        st->interpTex->Release();
    if (st->latencySem)
        CloseHandle(st->latencySem);
    if (st->ui.resource)
        ((ID3D12Resource*)st->ui.resource)->Release();
    if (st->queue)
        st->queue->Release();
    if (st->device)
        st->device->Release();
    delete st;
}

static void ScRelease(ScState* st)
{
    if (--st->refs == 0)
        TearDown(st);
}

// ---------------------------------------------------------------------------------------------------------------
// Present handling (called by FgSwapChain around the proxy's Present)

#ifdef FFXFG_DEBUG
// FFX_XEFG_ORDER=1: the order of the game's calls with their frame ids, for the first 300 after interpolation starts
static std::atomic<int> g_orderLeft{-1};
static void Order(const char* what, uint64_t id)
{
    if (g_orderLeft.load() > 0 && g_orderLeft.fetch_sub(1) > 0)
        Log("order %s %llu", what, (unsigned long long)id);
}
#else
static inline void Order(const char*, uint64_t) {}
#endif

static void SetXefgEnabled(ScState* st, bool want)
{
    if (want == st->xefgEnabled)
        return;
#ifdef FFXFG_DEBUG
    if (want && g_orderLeft.load() == -1 && EnvInt("FFX_XEFG_ORDER", 0))
        g_orderLeft = 300;
#endif
    if (want) {
        BOOL fs = FALSE;
        st->proxy->GetFullscreenState(&fs, nullptr);
        Log("enabling interpolation: proxy fullscreen state %d", fs);
    }
    xefg_swapchain_result_t r = p_xefgSwapChainSetEnabled(st->xefg, want ? 1 : 0);
    Log("xefgSwapChainSetEnabled(%d) frame %u -> %d", want, st->frameId, (int)r);
    if (r == XEFG_SWAPCHAIN_RESULT_SUCCESS)
        st->xefgEnabled = want;
}

// --- XeLL markers (all with st->lock held, except XellStartFrame) ---

static const uint8_t XM_SIM_START = 1 << XELL_SIMULATION_START, XM_PRESENT_END = 1 << XELL_PRESENT_END;

// The ring entry of a frame id. An id whose PRESENT_END went out and that comes up again is a new frame (FFX ids
// repeat when the game restarts its counter); a slot with a sleep in flight is never taken over.
static ScState::XellFrame* XellFrameFor(ScState* st, uint32_t id)
{
    for (auto& f : st->xellFrames)
        if (f.used && f.id == id) {
            if ((f.sent & XM_PRESENT_END) && !f.sleeping) {
                f = ScState::XellFrame();
                f.used = true;
                f.id = id;
            }
            return &f;
        }
    for (unsigned i = 0; i < 8; i++) {
        auto& f = st->xellFrames[(st->xellNext + i) % 8];
        if (f.sleeping)
            continue;
        st->xellNext = (st->xellNext + i + 1) % 8;
        f = ScState::XellFrame();
        f.used = true;
        f.id = id;
        return &f;
    }
    return nullptr;   // not reached: at most one sleep is in flight
}

// Sends the wanted markers that have not gone out, in order. A marker before the sleep would break XeLL's order, so
// once one went out the id gets no sleep any more.
static void XellFlush(ScState* st, ScState::XellFrame* f)
{
    if (f->sleeping)
        return;
    for (int m = XELL_SIMULATION_START; m <= XELL_PRESENT_END; m++) {
        uint8_t bit = (uint8_t)(1u << m);
        if ((f->wanted & bit) && !(f->sent & bit)) {
            f->slept = true;
            p_xellAddMarkerData(st->xell, f->id, (xell_latency_marker_type_t)m);
            f->sent |= bit;
        }
    }
}

// Marker `type` for `id`, with the ones before it that are still missing.
static void XellMarker(ScState* st, uint32_t id, xell_latency_marker_type_t type)
{
    if (auto* f = XellFrameFor(st, id)) {
        f->wanted |= (uint8_t)((2u << type) - 1);
        XellFlush(st, f);
    }
}

// Starts frame `id` for XeLL: xellSleep(id), then SIMULATION_START(id). The sleep can block, so it runs without the
// lock; markers the other thread asks for the id meanwhile wait until it returns. Nothing if the id already started.
static void XellStartFrame(ScState* st, uint32_t id)
{
    {
        std::lock_guard<std::mutex> lk(st->lock);
        auto* f = XellFrameFor(st, id);
        if (!f)
            return;
        f->wanted |= XM_SIM_START;
        if (f->slept || f->sleeping || !g_sleep) {
            XellFlush(st, f);
            return;
        }
        f->sleeping = true;
    }
    TR(const uint64_t ts0 = Qpc());
    p_xellSleep(st->xell, id);
    TR(tr::Put(tr::XSLEEP, id, (uint64_t)(QpcMs(ts0, Qpc()) * 1000.0f)));
    std::lock_guard<std::mutex> lk(st->lock);
    auto* f = XellFrameFor(st, id);   // the same slot: a sleeping slot is not taken over
    if (!f)
        return;
    f->sleeping = false;
    f->slept = true;
    XellFlush(st, f);
}

// --- FFX_XEFG_OFLOW ---

// Our textures for frame `id`, written right before its present (BeforePresent): depth, motion vectors, an identity
// camera, no jitter. Called with st->lock held.
static void OflowTag(ScState* st, uint32_t id, bool reset, float frameTimeMs)
{
    xefg_swapchain_d3d12_resource_data_t od = {};
    od.validity = XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT;
    od.resourceSize = {st->width, st->height};
    od.incomingState = kOflowOutState;
    od.type = XEFG_SWAPCHAIN_RES_DEPTH;
    od.pResource = OflowDepth(st->oflow);
    xefg_swapchain_result_t o1 = p_xefgSwapChainD3D12TagFrameResource(st->xefg, nullptr, id, &od);
    od.type = XEFG_SWAPCHAIN_RES_MOTION_VECTOR;
    od.pResource = OflowMotionVectors(st->oflow);
    xefg_swapchain_result_t o2 = p_xefgSwapChainD3D12TagFrameResource(st->xefg, nullptr, id, &od);
    xefg_swapchain_frame_constant_data_t oc = {};
    for (int i = 0; i < 4; ++i)
        oc.viewMatrix[i * 5] = oc.projectionMatrix[i * 5] = 1.0f;
    oc.motionVectorScaleX = oc.motionVectorScaleY = 1.0f;
    oc.resetHistory = reset ? 1 : 0;
    oc.frameRenderTime = frameTimeMs;
    xefg_swapchain_result_t o3 = p_xefgSwapChainTagFrameConstants(st->xefg, id, &oc);
    st->oflowTags++;
    if (st->oflowTags <= 5 || st->oflowTags % 600 == 0 || o1 || o2 || o3)
        Log("oflow tag #%llu frame %u (%.2f ms) -> depth %d mv %d const %d", (unsigned long long)st->oflowTags, id,
            frameTimeMs, (int)o1, (int)o2, (int)o3);
}

// --- Present ---

// ExecuteCommandLists of the D3D12 queues (vtable slot 10, shared by vkd3d's queues) is patched once, for two things.
// FFX_XEFG_CAP=auto with the optical flow: the GPU time of a frame runs from the game's first submission after a present
// (a time stamp put in front of it here) to our optical flow right before the next present; our own submissions pass
// through. The low latency: when XeSS-FG submitted first and last inside its Present (t_eclSpan, set by the present
// thread around that call): the time between the two is what the frame waited there (LatHold); and when the game
// submitted the first work of a frame (g_latFirstSubmit).
typedef void(STDMETHODCALLTYPE* PFN_ExecuteCommandLists)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
static PFN_ExecuteCommandLists g_origEcl;
static std::atomic<bool> g_wantFrameStart{false};
static thread_local bool t_ownSubmit = false;
static thread_local uint64_t* t_eclSpan = nullptr;   // [0] first, [1] last submission (QPC), [2] their number
#ifdef FFXFG_DEBUG
static std::atomic<ID3D12CommandQueue*> g_traceQueue{nullptr};   // FFX_XEFG_TRACE: the game's queue
#endif

// The state is used under g_timedLock and takes no reference: a reference of the hook could be the last one, and its
// release would run the whole TearDown (XeSS-FG and XeLL destroyed, fence waits) inside the game's submission.
// TearDown clears g_timedSc under the same lock before it frees anything, so it waits at most for the time stamp
// below, which does not block (OflowFrameStart skips a busy slot). The state's lock is taken only if free: the present
// thread may hold it while XeSS-FG submits on this queue, and a missed time stamp costs one sample. (HookQueue takes
// g_timedLock with the state's lock held: no deadlock, as the hook only tries that lock.)
static void STDMETHODCALLTYPE HookEcl(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists)
{
    if (uint64_t* span = t_eclSpan) {
        const uint64_t now = Qpc();
        if (!span[0])
            span[0] = now;
        span[1] = now;
        span[2]++;
    } else if (!t_ownSubmit && !g_latFirstSubmit.load(std::memory_order_relaxed) &&
               q == g_latQueue.load(std::memory_order_relaxed)) {
        uint64_t none = 0;   // the game's first work of its next frame reaches the GPU's queue
        if (g_latFirstSubmit.compare_exchange_strong(none, Qpc())) {
            TR(tr::GpuMarkSignal(g_tracePre.load(), q, g_traceNext.load()));
        }
    }
#ifdef FFXFG_DEBUG
    struct TraceEcl {
        uint64_t t0;
        ID3D12CommandQueue* q;
        UINT n;
        uint64_t sub = 0;   // FFX_XEFG_TRACE_SUBS: the value signalled behind this submission, and its queue's index
        int subQ = 0;
        ~TraceEcl()
        {
            if (t0)
                tr::Put(tr::ECL, n,
                        (q == g_traceQueue.load() ? 1u : 0u) | (t_ownSubmit ? 2u : 0u) | ((uint64_t)subQ << 4) | (sub << 8),
                        (uint16_t)std::min(QpcMs(t0, Qpc()) * 1000.0f, 65535.0f), t0);
        }
    } traceEcl{tr::g_on ? Qpc() : 0, q, n};
#endif
    if (!t_ownSubmit && q == g_timedQueue.load() && g_wantFrameStart.exchange(false)) {
        std::lock_guard<std::mutex> tl(g_timedLock);
        ScState* st = g_timedSc.load();
        if (st) {
            std::unique_lock<std::mutex> lk(st->lock, std::try_to_lock);
            if (lk.owns_lock() && st->oflow) {
                t_ownSubmit = true;
                OflowFrameStart(st->oflow, q);
                t_ownSubmit = false;
            }
        }
    }
    g_origEcl(q, n, lists);
    TR(traceEcl.sub = tr::SubSignal(q, &traceEcl.subQ));
}

static bool HookEclVt(ID3D12CommandQueue* queue)
{
    if (!g_origEcl) {
        void** vt = *(void***)queue;
        DWORD old;
        if (!PinSelf() || !VirtualProtect(&vt[10], sizeof(void*), PAGE_READWRITE, &old))   // a vtable is data
            return false;
        g_origEcl = (PFN_ExecuteCommandLists)vt[10];
        vt[10] = (void*)HookEcl;
        VirtualProtect(&vt[10], sizeof(void*), old, &old);
        Log("ExecuteCommandLists of the D3D12 queues hooked (through queue %p)", (void*)queue);
    }
    return true;
}

// FFX_XEFG_CAP=auto: the game's queue is timed
static void HookQueue(ScState* st)
{
    if (!HookEclVt(st->queue))
        return;
    std::lock_guard<std::mutex> lk(g_timedLock);
    g_timedSc = st;
    g_timedQueue = st->queue;
}

static void TimesReport(ScState* st)
{
    auto pct = [](std::vector<float> v, char* out, size_t n) {
        std::sort(v.begin(), v.end());
        size_t c = v.size();
        double sum = 0;
        for (float x : v)
            sum += x;
        snprintf(out, n, "mean %.2f p50 %.2f p95 %.2f p99 %.2f max %.2f", sum / c, v[c / 2], v[c * 95 / 100],
                 v[c * 99 / 100], v[c - 1]);
    };
    char a[128], b[128], c[128];
    pct(st->tFrame, a, sizeof a);
    pct(st->tPresent, b, sizeof b);
    pct(st->tSleep, c, sizeof c);
    Log("times over %zu frames (ms): frame %s | in Present %s | XeLL sleep %s", st->tFrame.size(), a, b, c);
    st->tFrame.clear(), st->tPresent.clear(), st->tSleep.clear();
}

// How the back buffer is encoded, for the optical flow (FFX_BACKBUFFER_TRANSFER_FUNCTION_*): the colour space the game
// set, else DXGI's default for the format (FP16 = linear scRGB, everything else = sRGB).
static int OflowTransfer(ScState* st, ID3D12Resource* bb)
{
    switch (st->colorSpace.load()) {
    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
        return 1;   // PQ (HDR10)
    case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
        return 2;   // scRGB
    case -1:
        break;
    default:
        return 0;
    }
    DXGI_FORMAT f = bb->GetDesc().Format;
    return f == DXGI_FORMAT_R16G16B16A16_FLOAT || f == DXGI_FORMAT_R16G16B16A16_TYPELESS ? 2 : 0;
}

static uint32_t BeforePresent(ScState* st)
{
    std::lock_guard<std::mutex> lk(st->lock);
    if (st->bridge && st->oflow) {
        st->pending.clear();
        st->frameId = ++st->nextId;
        st->havePrepare = true;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        bool first = !st->oflowLastQpc;
        float ms = first ? 16.7f
                         : (float)((double)(now.QuadPart - st->oflowLastQpc) * 1000.0 / (double)g_qpf.QuadPart);
        st->oflowLastQpc = now.QuadPart;
        XellMarker(st, st->frameId, XELL_SIMULATION_END);
        XellMarker(st, st->frameId, XELL_RENDERSUBMIT_START);
        OflowTag(st, st->frameId, first || ms > 250.0f, ms);
    } else if (st->bridge) {
        st->havePrepare = !st->pending.empty();
        if (st->havePrepare) {
            st->frameId = st->pending.front();
            st->pending.pop_front();
        } else {
            st->frameId = ++st->nextId;
        }
    } else if (!st->havePrepare) {
        st->frameId++;  // no frame data this time (menus, loading): keep the ids moving, no interpolation
    }
    uint32_t id = st->frameId;
    Order(st->havePrepare ? "present" : "present (no prepare)", id);
    SetXefgEnabled(st, st->gameEnabled && st->havePrepare);

    if (st->oflow && st->havePrepare) {
        UINT bi = st->proxy->GetCurrentBackBufferIndex();
        ID3D12Resource* bb = nullptr;
        if (SUCCEEDED(st->proxy->GetBuffer(bi, IID_PPV_ARGS(&bb)))) {
            t_ownSubmit = true;
            OflowRun(st->oflow, st->queue, bb, D3D12_RESOURCE_STATE_PRESENT, OflowTransfer(st, bb), false);
            t_ownSubmit = false;
            bb->Release();
        }
    }
    if (st->havePrepare && st->ui.resource && !st->oflow) {
        xefg_swapchain_d3d12_resource_data_t rd = {};
        rd.type = XEFG_SWAPCHAIN_RES_UI;
        rd.validity = XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT;
        rd.resourceSize = {st->ui.description.width, st->ui.description.height};
        rd.pResource = (ID3D12Resource*)st->ui.resource;
        rd.incomingState = ToD3D12State(st->ui.state);
        p_xefgSwapChainD3D12TagFrameResource(st->xefg, nullptr, id, &rd);
    }

    // SIMULATION_START .. RENDERSUBMIT_START fill in when no prepare sent them (menus, loading)
    XellMarker(st, id, XELL_RENDERSUBMIT_END);
    XellMarker(st, id, XELL_PRESENT_START);
    p_xefgSwapChainSetPresentId(st->xefg, id);
    st->tStart = Qpc();   // "in Present" (FFX_XEFG_TIMES, the outside limiter test) without our own work above
    return id;
}

// FFX_XEFG_CAP=auto, every 30 real frames with st->lock held. The real frame interval = the 90th percentile of the GPU
// time of a frame (or, without the optical flow's timer, of the frame without XeLL's sleep) + FFX_XEFG_CAP_PAD ms (the
// interpolation and the present, default 3) + 3 %. Frames over 3x the median are left out (a paused game). If XeLL hardly
// slept while frames took longer than the interval, the GPU could not keep up: the interval becomes that frame time.
// Longer at once; shorter after 2 windows in a row, 2 % a step at most and not below the last GPU-bound interval, which
// is forgotten at 0.1 % a window (back up to it + 1 %); at least one frame of the display per frame shown.
// The GPU's actual frequency (MHz, 0 while idle) from the xe driver, or from i915 (its per-GT file, else the older
// one on the card), read through Wine's Z: drive. A capped frame
// lets the GPU idle, its governor lowers the clock and the same work takes longer: without this the cap would chase its
// own tail down (2026-11-06 RE4: 49 -> 35 fps).
static int GpuFreqMhz()
{
    static HANDLE h = INVALID_HANDLE_VALUE;
    static bool tried = false;
    if (!tried) {
        tried = true;
        static const char* const kFiles[] = {
            "device\\tile0\\gt0\\freq0\\act_freq",   // xe
            "gt\\gt0\\rps_act_freq_mhz",             // i915
            "gt_act_freq_mhz",                       // i915, also on kernels before the per-GT files
        };
        for (int c = 0; c < 4 && h == INVALID_HANDLE_VALUE; ++c)
            for (int k = 0; k < 3 && h == INVALID_HANDLE_VALUE; ++k) {
                char path[128];
                snprintf(path, sizeof path, "Z:\\sys\\class\\drm\\card%d\\%s", c, kFiles[k]);
                h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                                nullptr);
                if (h != INVALID_HANDLE_VALUE)
                    Log("cap auto: GPU frequency from %s", path);
            }
        if (h == INVALID_HANDLE_VALUE)
            Log("cap auto: no GPU frequency (xe/i915 sysfs not readable), GPU times taken as they are");
    }
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    char b[32] = {};
    DWORD n = 0;
    SetFilePointer(h, 0, nullptr, FILE_BEGIN);
    if (!ReadFile(h, b, sizeof b - 1, &n, nullptr) || !n)
        return -1;
    return atoi(b);
}

#pragma comment(lib, "user32.lib")   // EnumDisplaySettingsA

// The display's refresh rate (FFX_XEFG_REFRESH overrides; Wine reports gamescope's output), checked again every
// 120 calls for a display that changed; 144 when nothing is known.
static double DisplayHz()
{
    static double hz = 0;
    static int calls = 0;
    if (hz > 0 && calls++ % 120)
        return hz;
    double h = 0;
    char e[32];
    if (GetEnvironmentVariableA("FFX_XEFG_REFRESH", e, sizeof e))
        h = atof(e);
    if (h <= 0) {
        DEVMODEA dm = {};
        dm.dmSize = sizeof dm;
        if (EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
            h = dm.dmDisplayFrequency;
    }
    if (h < 24 || h > 600)
        h = 144;
    if (h != hz)
        Log("cap auto: display %.0f Hz", h);
    hz = h;
    return hz;
}

static void CapAdjust(ScState* st)
{
    auto med = [](std::vector<float> v) { std::sort(v.begin(), v.end()); return v[v.size() / 2]; };
    std::vector<float>& w = st->capWork;
    std::sort(w.begin(), w.end());
    float m = w[w.size() / 2];
    std::vector<float> kept;
    for (float x : w)
        if (x <= 3.0f * m)
            kept.push_back(x);
    float p90 = kept[kept.size() * 90 / 100];
    float frameMed = med(st->capFrame), sleepMed = med(st->capSleep), presentMed = med(st->capPresent);
    w.clear(), st->capFrame.clear(), st->capSleep.clear(), st->capPresent.clear();
    int mult = g_mult > 1 ? g_mult : 1;
    static float pad = -1;
    if (pad < 0) {
        char e[32];
        pad = GetEnvironmentVariableA("FFX_XEFG_CAP_PAD", e, sizeof e) ? (float)atof(e) : 1.5f;
    }
    // the GPU time at the clock it reaches when kept busy: p90 x (this window's clock / that clock)
    float fNow = st->capFreqN ? (float)(st->capFreqSum / st->capFreqN) : 0;
    st->capFreqSum = 0, st->capFreqN = 0;
    float scale = 1.0f;
    if (fNow > 0) {
        st->capFreqRef = st->capFreqRef * 0.998f;   // about 6 % a minute at 2 windows a second
        if (fNow > st->capFreqRef)
            st->capFreqRef = fNow;
        scale = fNow / st->capFreqRef;
    }
    float p90n = p90 * scale;
    // what the frame needs beyond the measured GPU time (the interpolation, the present): learned from the windows in
    // which the GPU could not keep up (XeLL hardly slept), starting from FFX_XEFG_CAP_PAD
    if (st->capPad < 0)
        st->capPad = pad;
    // only when the GPU was busy for most of the frame (a menu at the game's own 60 fps has long frames and an idle GPU);
    // with room to spare (XeLL sleeping) it shrinks slowly, so a wrong value cannot stick
    if (st->oflow && st->capUs && sleepMed < 0.3f && frameMed > p90n && p90n > 0.7f * frameMed) {
        float off = frameMed - p90n;
        if (off > 8.0f)
            off = 8.0f;
        st->capPad = 0.7f * st->capPad + 0.3f * off;
    } else if (st->oflow && sleepMed > 2.0f) {
        st->capPad = st->capPad * 0.95f;
        if (st->capPad < 0.5f)
            st->capPad = 0.5f;
    }
    float needMs = st->oflow ? p90n + st->capPad : p90n;
    uint32_t want = (uint32_t)(needMs * 1030.0f);
    // GPU-bound: XeLL hardly slept, frames took longer than the interval and the GPU's own need explains it (in a menu
    // the game's own 60 fps limit gives long frames with an idle GPU)
    if (st->capUs && sleepMed < 0.3f && frameMed * 1000.0f > st->capUs * 1.02f && needMs > 0.85f * frameMed &&
        (uint32_t)(frameMed * 1010.0f) > want) {
        want = (uint32_t)(frameMed * 1010.0f);
        st->capFloor = (float)want;   // where the GPU last fell behind: the interval creeps back down from there only
    }
    // an outside limiter (gamescope's frame limit): the game waits in Present, XeLL hardly sleeps and the GPU has room.
    // XeLL cannot take that wait over (XeSS-FG's second present waits for the limiter anyway) and a cap fighting it made
    // XeSS-FG drop generated frames (2026-11-06 Viewfinder at gamescope 120: dips to 50 every few seconds). So the cap goes
    // off and the limiter paces, until the game stops waiting in Present (it no longer reaches the limit).
    bool outside = st->oflow && sleepMed < 0.3f && presentMed > 0.3f * frameMed && needMs < 0.7f * frameMed;
    if (st->capOff) {
        // still paced from outside while the game waits in Present and the GPU has room; a GPU that needs most of the
        // frame means the game itself is the limit again (XeSS-FG's own Present takes a few ms then, so the wait in
        // Present alone kept the cap off for minutes in RE4 after its menu had looked like an outside limit)
        if (presentMed > 0.15f * frameMed && needMs < 0.8f * frameMed)
            return;
        st->capOff = false;
        st->capUs = 0;
        Log("cap auto: the outside limiter no longer paces (in Present %.2f of %.2f ms, GPU %.2f), cap back on", presentMed,
            frameMed, needMs);
    } else if (outside) {
        st->capOff = true;
        st->capUs = 0;
        st->capFloor = 0;
        xell_sleep_params_t off = {};
        off.bLowLatencyMode = 1;
        xell_result_t ro = st->xell ? p_xellSetSleepMode(st->xell, &off) : XELL_RESULT_SUCCESS;
        Log("cap auto: outside limiter at %.2f ms (in Present %.2f, GPU %.2f), cap off -> %d", frameMed, presentMed,
            needMs, (int)ro);
        return;
    }
    // The floor keeps the interval from swinging straight back into a GPU-bound stretch, and is forgotten slowly
    // (0.999 a window, about 6 % a minute). Once XeLL has slept a quarter of the frame for 4 windows in a row while the
    // GPU needs 10 % less than the floor (a lighter scene, the clock back up), it gives way 10 % a window: in Resident Evil
    // 4 a heavy stretch at 1700 MHz left it at 30.8 ms for minutes while the GPU needed 21 ms (XeLL sleeping 22 ms).
    if (st->capFloor > 0 && sleepMed > 0.25f * frameMed && (float)want < 0.9f * st->capFloor) {
        if (++st->capRoom >= 4)
            st->capFloor *= 0.9f;
    } else {
        st->capRoom = 0;
    }
    st->capFloor *= 0.999f;
    if (want < (uint32_t)st->capFloor)
        want = (uint32_t)st->capFloor;
    uint32_t lo = (uint32_t)(1e6 / DisplayHz() * mult) + 1;
    if (want < lo)
        want = lo;
    if (want > 50000)
        want = 50000;
    uint32_t cap = st->capUs;
    if (!cap || want > cap * 1.04f) {
        cap = want;
        st->capCalm = 0;
    } else if (want < cap * 0.96f) {
        if (++st->capCalm >= 2) {
            cap = want > (uint32_t)(cap * 0.98f) ? want : (uint32_t)(cap * 0.98f);
            st->capCalm = 0;
        }
    } else {
        st->capCalm = 0;
    }
    if (cap == st->capUs)
        return;
    st->capUs = cap;
    xell_sleep_params_t sp = {};
    sp.bLowLatencyMode = 1;
    sp.minimumIntervalUs = cap / mult;   // XeLL counts every frame shown
    xell_result_t r = st->xell ? p_xellSetSleepMode(st->xell, &sp) : XELL_RESULT_SUCCESS;
    static int logged;
    if (logged++ < 300 || logged % 20 == 0)
        Log("cap auto: %s p90 %.2f ms at %.0f MHz (ref %.0f) = %.2f + %.2f, frame %.2f, sleep %.2f -> %u us per real frame "
            "(%.1f fps) -> %d",
            st->oflow ? "GPU" : "CPU", p90, fNow, st->capFreqRef, p90n, st->capPad, frameMed, sleepMed, cap, 1e6 / cap, (int)r);
}

static float LatHold(ScState* st, uint64_t tEnd, float blockMs, uint64_t first, bool ok);
static void FrameDone(ScState* st, HRESULT hr);

// `id` is what BeforePresent returned: a prepare for the next frame may have changed st->frameId during the present.
// `tEnd`: when the proxy's Present returned; `blockMs`: how long the frame was held up in front of it, `first`: when
// its first work was submitted (LatHold).
static void AfterPresent(ScState* st, uint32_t id, HRESULT hr, UINT sync, UINT flags, uint64_t tEnd, float blockMs,
                         uint64_t first)
{
    uint32_t next;
    uint64_t presents;
    {
        std::lock_guard<std::mutex> lk(st->lock);
        XellMarker(st, id, XELL_PRESENT_END);
        presents = ++st->presents;
        // only the first presents: a later present sometimes stalled 560-580 ms around this query (Viewfinder with
        // OFLOW_FG at the display's limit while our cap fought that limit; not seen in Forza Horizon 6 at a frame limit),
        // and the status is only for the log
        if (st->presents <= 10) {
            xefg_swapchain_present_status_t ps = {};
            p_xefgSwapChainGetLastPresentStatus(st->xefg, &ps);
            Log("present #%llu frame %u sync %u flags 0x%x hr 0x%08lx: presented %u fgResult %d fgEnabled %u, %zu waiting",
                (unsigned long long)st->presents, id, sync, flags, (unsigned long)hr, ps.framesPresented, (int)ps.frameGenResult,
                ps.isFrameGenEnabled, st->pending.size());
        }
        if (st->frameId == id)   // else a newer prepare arrived during the present and is kept for the next one
            st->havePrepare = false;
        // the id the next prepare is expected to carry
        next = st->bridge ? st->nextId + 1 : id + 1;
    }
    // without the lock: the sleep can block
    // every 4th present: the window's average needs no more, and each read is a file read through Wine (a window
    // without any sample keeps the GPU times as they are, see CapAdjust)
    if (g_capAuto && st->oflow && presents % 4 == 0) {
        int f = GpuFreqMhz();
        if (f > 0) {
            std::lock_guard<std::mutex> lk(st->lock);
            st->capFreqSum += f;
            st->capFreqN++;
        }
    }
    if (g_capAuto && g_timedQueue.load() == st->queue)
        g_wantFrameStart = true;   // the game's next submission starts the next frame on the GPU (HookEcl)
    // The low latency: the game stays here until its next frame should start. Then the frame latency waitable object
    // is released (a game that waits on it starts there) and the frame starts for XeLL: its sleep (XeLL's own
    // interval, if one is set) and SIMULATION_START.
    const float held = LatHold(st, tEnd, blockMs, first, SUCCEEDED(hr));
    FrameDone(st, hr);
    uint64_t t0 = Qpc();
    XellStartFrame(st, next);
    if (g_capAuto) {
        uint64_t t1 = Qpc();
        std::lock_guard<std::mutex> lk(st->lock);
        float sl = QpcMs(t0, t1);
        if (st->oflow) {
            // the GPU time of each frame, measured on the game's queue (HookEcl .. OflowRun)
            if (g_timedQueue.load() != st->queue)
                HookQueue(st);
            float g[64];
            int n = OflowTakeGpuFrames(st->oflow, g, 64);
            st->capWork.insert(st->capWork.end(), g, g + n);
        } else if (st->capLastEnd) {
            st->capWork.push_back(QpcMs(st->capLastEnd, t1) - sl - held);   // no GPU timer: the frame without the sleep
        }
        if (st->capLastEnd) {
            st->capFrame.push_back(QpcMs(st->capLastEnd, t1));
            st->capSleep.push_back(sl);
            st->capPresent.push_back(QpcMs(st->tStart, tEnd));   // in Present: an outside limiter (gamescope) waits here
        }
        st->capLastEnd = t1;
        if (st->capWork.size() >= 30 && st->capFrame.size() >= 30)
            CapAdjust(st);
        else if (st->capFrame.size() >= 300)   // no GPU times arrive (no timer): nothing to adjust, keep it small
            st->capFrame.clear(), st->capSleep.clear(), st->capPresent.clear(), st->capWork.clear();
    }
    if (g_times) {
        uint64_t t1 = Qpc();
        std::lock_guard<std::mutex> lk(st->lock);
        if (st->tLastEnd) {
            st->tFrame.push_back(QpcMs(st->tLastEnd, tEnd));   // from present to present: what reaches the display
            st->tPresent.push_back(QpcMs(st->tStart, tEnd));
            st->tSleep.push_back(QpcMs(t0, t1));
        }
        st->tLastEnd = tEnd;
        if (st->tFrame.size() >= 300)
            TimesReport(st);
    }
    if (g_latQueue.load() == st->queue)
        g_latFirstSubmit = 0;
    st->frameStart = Qpc();   // the game's next frame starts when this returns
}

// ---------------------------------------------------------------------------------------------------------------
// Frame latency waitable object, paced by the GPU (FFX_XEFG_PACE=1)

static const DWORD kPaceWatchdogMs = 200;

// Releases the semaphore once per signalled value when the fence reaches it. The thread touches only the fence, the
// events, the semaphore and the three counters; never st->lock, so stopping it from TearDown cannot deadlock, and
// TearDown frees nothing before it has stopped.
static DWORD WINAPI PaceThread(void* p)
{
    auto* st = (ScState*)p;
    while (!st->paceStop.load()) {
        uint64_t target = st->fenceValue.load();
        if (st->paceReleased >= target) {
            WaitForSingleObject(st->paceEvent, INFINITE);   // a new signal or stop
            continue;
        }
        uint64_t want = st->paceReleased + 1;
        if (st->fence->GetCompletedValue() < want) {
            st->fence->SetEventOnCompletion(want, st->paceEvent);
            DWORD r = WaitForSingleObject(st->paceEvent, kPaceWatchdogMs);
            if (st->paceStop.load())
                break;
            if (r == WAIT_TIMEOUT && st->fence->GetCompletedValue() < want) {
                // watchdog: the frame is not done after 200 ms; the game must not wait on us for that
                LONG n = (LONG)(st->fenceValue.load() - st->paceReleased);
                ReleaseSemaphore(st->latencySem, n, nullptr);
                st->paceReleased += n;
                st->watchdogs++;
                if (st->watchdogs <= 5 || st->watchdogs % 100 == 0)
                    Log("pace watchdog #%llu: frame %llu not done after %lu ms, %ld released", (unsigned long long)st->watchdogs,
                        (unsigned long long)want, kPaceWatchdogMs, n);
            }
            continue;   // done, or woken by a newer signal: look again
        }
        ReleaseSemaphore(st->latencySem, 1, nullptr);
        st->paceReleased++;
    }
    return 0;
}

// With the lock held, after the semaphore exists. Without fence or thread the semaphore is released at Present.
static void PaceStart(ScState* st)
{
    if (!g_pace || st->fence || !st->queue)
        return;
    ID3D12Fence* fence = nullptr;
    if (FAILED(st->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        Log("pace: no fence, the waitable object is released at Present");
        return;
    }
    st->paceEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    st->paceThread = st->paceEvent ? CreateThread(nullptr, 0, PaceThread, st, CREATE_SUSPENDED, nullptr) : nullptr;
    if (!st->paceThread) {
        Log("pace: no thread (%lu), the waitable object is released at Present", GetLastError());
        if (st->paceEvent)
            CloseHandle(st->paceEvent);
        st->paceEvent = nullptr;
        fence->Release();
        return;
    }
    st->fence = fence;   // last: FrameDone on the present thread reads it without the lock
    SetThreadPriority(st->paceThread, THREAD_PRIORITY_HIGHEST);
    ResumeThread(st->paceThread);
    Log("pace: the waitable object follows the GPU (fence %p, watchdog %lu ms)", (void*)fence, kPaceWatchdogMs);
}

// After a Present that returned: the frame's work, including what XeSS-FG queued on the game's queue inside Present,
// precedes the signal on that queue (a command queue is free-threaded, so the present thread may signal it).
static void FrameDone(ScState* st, HRESULT hr)
{
    if (!st->latencySem || FAILED(hr))
        return;
    ID3D12Fence* fence = st->fence;
    if (!fence) {
        ReleaseSemaphore(st->latencySem, 1, nullptr);
        return;
    }
    // one value per present, each released once by the thread; the signal is queued before the value is published,
    // so the thread never waits for a value that will not come
    std::lock_guard<std::mutex> lk(st->paceLock);
    uint64_t v = st->fenceValue.load() + 1;
    if (SUCCEEDED(st->queue->Signal(fence, v))) {
        st->fenceValue.store(v);
        SetEvent(st->paceEvent);
    } else {
        ReleaseSemaphore(st->latencySem, 1, nullptr);
    }
}

// Stops the thread and frees the fence; false if the thread did not stop (then nothing of st may be freed). Called
// from TearDown only, with no lock held and no present in flight (a present holds a reference to st).
static bool PaceStop(ScState* st)
{
    if (!st->paceThread) {
        if (st->fence)
            st->fence->Release();
        st->fence = nullptr;
        if (st->paceEvent)
            CloseHandle(st->paceEvent);
        st->paceEvent = nullptr;
        return true;
    }
    st->paceStop.store(true);
    SetEvent(st->paceEvent);
    DWORD r = WaitForSingleObject(st->paceThread, 2000);   // its waits are at most the watchdog long
    if (r != WAIT_OBJECT_0)
        return false;
    CloseHandle(st->paceThread);
    st->paceThread = nullptr;
    // a completion the thread armed may still be pending on the event: let the last value land before closing it
    uint64_t last = st->fenceValue.load();
    if (st->fence->GetCompletedValue() < last) {
        st->fence->SetEventOnCompletion(last, st->paceEvent);
        WaitForSingleObject(st->paceEvent, 500);
    }
    Log("pace: thread stopped, %llu frames released, %llu by the watchdog", (unsigned long long)st->paceReleased,
        (unsigned long long)st->watchdogs);
    st->fence->Release();
    st->fence = nullptr;
    CloseHandle(st->paceEvent);
    st->paceEvent = nullptr;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Low latency (IGDEXT_XELL_CAP, auto by default)
//
// What a frame waits for on this route, from the time lines of CONTROL Resonant, Resident Evil Requiem and Forza
// Horizon 6 (FFX_XEFG_TRACE in a debug build, XeSS-FG 1.3.1 at 3x under vkd3d-proton and DXVK's DXGI, 2026-11-11):
//  - XeSS-FG's proxy Present puts the interpolation on the game's queue, waits until its presentation thread is
//    through with the frame before, presents the first of the frame's images itself and returns; the presentation
//    thread presents the others. Those Presents of the real swap chain block (two may be under way): the last one of
//    a frame returns when the GPU has finished the frame's first image. A game that reaches Present before that
//    waits in it, with its frame, and the input it read for it, already made: the queue the low latency is after.
//  - DXVK's swap chain holds its locks for the whole of a Present, and all three games ask the swap chain something
//    right after their Present that takes one of them: IDXGISwapChain::GetFrameStatistics (CONTROL Resonant, Forza
//    Horizon 6), IDXGISwapChain3::CheckColorSpaceSupport (Resident Evil Requiem). If the presentation thread is
//    inside its last Present by then, the game's next frame starts only when the GPU has finished this one: no
//    queue, but the GPU idles while the game works (48 instead of 31.5 ms a frame in Resident Evil Requiem, 45
//    instead of 25 in Forza Horizon 6). Otherwise it starts at once and waits in its next Present (10 to 14 ms).
//    Which of the two is decided by some 100 us, and the games change between them every few seconds.
// So two things are done:
//  1. Those questions no longer wait for the presentation thread (IGDEXT_XELL_POLLS): the game gets the answers as of
//     its last Present call. They are taken right before the proxy's Present, where waiting for that thread costs
//     nothing: the proxy would wait for it anyway. The game's frame then starts when its Present returns, and
//     nowhere else. (Any other call of the game that waits in the swap chain is named in the log, FgSwapChain::Waited.)
//  2. The game is kept in Present, after the proxy's Present has returned, until its next frame should start: so
//     that the frame reaches its Present a little before XeSS-FG can take it (LatHold). How long a frame was held up
//     in front of its present is measured in that Present: the wait for those answers plus the time between
//     XeSS-FG's first and last submission inside its Present (HookEcl; without a wait they are about 0.2 ms apart).
//     The wait before the next frame grows while frames are held up by more than kLatTargetMs
//     (IGDEXT_XELL_CAP=auto:<ms> adds to that) and shrinks while they are not, so the frame time stays what the GPU
//     or the display allows, and a game that is never held up (its CPU is the limit) is not kept at all. The frame
//     latency waitable object, XeLL's sleep and its SIMULATION_START marker follow the wait: the XMX shim's log
//     (IGDEXT_XELL_LOG) shows the time from there to the end of the frame's present, as for a game with its own
//     XeSS-FG.
//     That alone is right while the GPU is what the frames wait for and the game submits a frame's work as it goes
//     (CONTROL Resonant). Two cases need the GPU's side as well (LatGpu: a fence on the game's queue after each proxy
//     Present tells when the GPU finished a frame, HookEcl when the game submitted the first work of the next one).
//     A game that submits a frame's work at its Present only (Resident Evil Requiem) left the GPU idle for about
//     1 ms a frame when it reached Present 2.5 ms before XeSS-FG could take it: XeSS-FG can take a frame a little
//     after the GPU is through with the one before (1.7 % of the frame rate). And when the display is what frames
//     wait for (a menu at the display's frame rate, Forza Horizon 6 at 120 frames shown per second), XeSS-FG's
//     Present still makes a frame wait, for its turn, but the GPU must have finished the frame by then, so the
//     frame must not start later (CONTROL Resonant's menu lost 1 to 4 % of its frame rate that way). So a frame
//     counts as early only as far as its first work also reached the GPU's queue more than kLatAheadMs before the
//     GPU got free: where the GPU idles before the work arrives, the game is not kept at all.
//  3. The wait is checked (IGDEXT_XELL_CHECK, common/lowlat_check.h: the same check as the XMX shim's for a game's
//     own XeSS-FG). The two times of 2. do not show everything a frame can wait for: a game's own render work may
//     wait for its compute queue in the middle of a frame (Resident Evil Requiem, about 4 ms), XeSS-FG's later
//     images for the display, and a frame that waits in front of Present can still be the one the GPU waits for
//     next. So the wait has to show that it costs no frame rate and lowers the latency: now and then it is left out
//     for 100 frames, between two windows with it, and it stays only if the frame time with it is within 1 % and the
//     latency (frame start -> end of its present) lower by at least half the wait. A wait that costs in two checks
//     in a row is shortened and checked again; one that does not help is off for a while.
//     (Measured 2026-11-11 against the release build, separate runs: in the three games the wait costs 0 to 0.2 ms
//     a frame. What a wait that is too long costs, IGDEXT_XELL_CAP=auto:-20 in Resident Evil Requiem: 1.1 to 1.8 ms.)
// IGDEXT_XELL_CAP=off: no wait of ours and the swap chain's own answers, all as before. IGDEXT_XELL_CAP=<us>
// or FFX_XEFG_CAP: XeLL's interval holds the frame back in its sleep, at the same place, instead of a wait of ours.

// A frame should reach Present this long before XeSS-FG can take it, and its first work the GPU's queue kLatAheadMs
// before the GPU gets free. The time a game needs from its start to its Present varies by 1 to 1.5 ms (standard
// deviation) in the three games, and a frame that comes late leaves the GPU idle for that long.
static const double kLatTargetMs = 2.5;
static const double kLatAheadMs = 1.5;
static const float kLatSpanMs = 0.25f;     // XeSS-FG's first to last submission inside its Present, without a wait
static const float kLatPresentMs = 2.0f;   // the proxy's Present without a wait, when its submissions are not seen

typedef LONG(NTAPI* PFN_NtDelayExecution)(BOOLEAN alertable, PLARGE_INTEGER interval);   // ntdll, 100 ns, negative = relative

// Waits until `due` (QPC): NtDelayExecution in 100 ns units, stopping short by the late wake-up seen recently (a maximum
// that decays), the rest yielded away
static void WaitUntil(uint64_t due, double& lateMs)
{
    static PFN_NtDelayExecution delay =
        (PFN_NtDelayExecution)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtDelayExecution");
    for (;;) {
        const uint64_t now = Qpc();
        if (now >= due)
            return;
        const double left = QpcMs(now, due);
        const double slack = std::min(2.0, std::max(0.2, lateMs + 0.2));
        if (left > slack) {
            const double ask = left - slack;
            if (delay) {
                LARGE_INTEGER rel;
                rel.QuadPart = -(LONGLONG)(ask * 10000.0);
                delay(FALSE, &rel);
            } else {
                Sleep((DWORD)ask);
            }
            lateMs = std::max((double)QpcMs(now, Qpc()) - ask, lateMs * 0.98);
        } else {
            SwitchToThread();
        }
    }
}

// Notes when the GPU reaches each value signalled on the fence. It touches the fence, the event and the atomics only,
// never st->lock: TearDown stops it before anything is freed.
static DWORD WINAPI LatGpuThread(void* p)
{
    ScState::LatGpu& g = ((ScState*)p)->latGpu;
    TR(tr::t_own = true);
    uint64_t done = 0;
    while (!g.stop.load()) {
        const uint64_t last = g.signalled.load();
        if (done >= last) {
            WaitForSingleObject(g.event, 200);   // a new signal or stop
            continue;
        }
        if (g.fence->GetCompletedValue() <= done) {
            g.fence->SetEventOnCompletion(done + 1, g.event);
            WaitForSingleObject(g.event, 200);
            if (g.stop.load())
                break;
        }
        const uint64_t reached = g.fence->GetCompletedValue();
        if (reached == UINT64_MAX)   // the device is gone
            break;
        const uint64_t now = Qpc();
        for (uint64_t v = done + 1; v <= reached && v <= last; v++)
            g.doneAt[v % 16].store(now);
        done = std::max(done, std::min(reached, last));
    }
    return 0;
}

static void LatGpuStart(ScState* st)
{
    ScState::LatGpu& g = st->latGpu;
    if (g.thread || FAILED(st->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence))))
        return;
    for (auto& d : g.doneAt)
        d.store(0);
    g.event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    g.thread = g.event ? CreateThread(nullptr, 0, LatGpuThread, st, 0, nullptr) : nullptr;
    if (!g.thread) {
        Log("low latency: no thread for the GPU's side (%lu): the frames' wait in Present alone decides", GetLastError());
        if (g.event)
            CloseHandle(g.event);
        g.event = nullptr;
        g.fence->Release();
        g.fence = nullptr;
    }
}

// false if the thread did not stop (then nothing of st may be freed); from TearDown only
static bool LatGpuStop(ScState* st)
{
    ScState::LatGpu& g = st->latGpu;
    if (g_latQueue.load() == st->queue)
        g_latQueue = nullptr;
    if (!g.thread)
        return true;
    g.stop.store(true);
    SetEvent(g.event);
    if (WaitForSingleObject(g.thread, 2000) != WAIT_OBJECT_0)
        return false;
    CloseHandle(g.thread);
    g.thread = nullptr;
    // a completion the thread asked for may still be pending on the event: let the last value land before closing it
    const uint64_t last = g.signalled.load();
    if (g.fence->GetCompletedValue() < last) {
        g.fence->SetEventOnCompletion(last, g.event);
        WaitForSingleObject(g.event, 500);
    }
    g.fence->Release();
    g.fence = nullptr;
    CloseHandle(g.event);
    g.event = nullptr;
    return true;
}

// right after the proxy's Present returned: the GPU has reached this point when it is through with the frame
static void LatGpuSignal(ScState* st)
{
    ScState::LatGpu& g = st->latGpu;
    if (!g.thread)
        return;
    const uint64_t v = g.signalled.load() + 1;
    g.doneAt[v % 16].store(0);
    TR(const bool own = tr::t_own; tr::t_own = true);
    if (SUCCEEDED(st->queue->Signal(g.fence, v))) {
        g.signalled.store(v);
        SetEvent(g.event);
    }
    TR(tr::t_own = own);
}

// On the present thread, after the proxy's Present returned at `tEnd` (`ok`: it succeeded): keeps the game there
// until its next frame should start, and returns that wait in ms. `blockMs`: how long this frame was held up in
// front of its present; `first`: when the game submitted its first work (0 = not seen: its Present call counts).
static float LatHold(ScState* st, uint64_t tEnd, float blockMs, uint64_t first, bool ok)
{
    ScState::LatCtl& c = st->lat;
    const float dt = c.lastEnd ? QpcMs(c.lastEnd, tEnd) : 0;
    c.lastEnd = tEnd;
    const bool steady = dt > 0 && dt < 250;   // not the first frame, a loading screen or a pause
    bool on = g_lat == 1 && !g_ffxCap && ok && st->xefgEnabled;
    if (on && g_abSec > 0) {
        if (!c.abStart)
            c.abStart = tEnd;
        const uint64_t turn = (uint64_t)(QpcMs(c.abStart, tEnd) / 1000.0f) / (uint64_t)g_abSec;
        c.abTurn = g_abOld ? (int)(turn % 3) : 1 + (int)(turn % 2);
        on = c.abTurn == 2;
    }
    const int state = on ? 2 : g_pollCache && c.abTurn != 0 ? 1 : 0;
    if (on != c.on || state != c.abSaid) {
        c.on = on;
        c.abSaid = state;
        SYSTEMTIME now;   // the time of day: the XMX shim's log (IGDEXT_XELL_LOG) has it
        GetLocalTime(&now);
        Log("low latency: %s (frame generation %s) at %02d:%02d:%02d.%03d",
            state == 2 ? "frames are held back in Present" : state == 1 ? "no wait" : "no wait, the swap chain answers the game",
            st->xefgEnabled ? "on" : "off", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
        TR(tr::Put(tr::NOTE, 1, state));
    }
    // the figure for the log: the frame's start (when the game got back from Present, or from a call into the swap
    // chain that waited) -> the end of its present
    const uint64_t start = st->frameStart.load();
    if (steady && start && tEnd > start && st->xefgEnabled) {
        c.logLat += QpcMs(start, tEnd);
        c.logBlock += blockMs;
        c.logFrame += dt;
        c.logLatN++;
    }
    // The GPU's side: this frame's value was signalled just now; the frame before is through on the GPU by now, or
    // the GPU is still busy with it (then our work was there long before it gets free).
    ScState::LatGpu& gp = st->latGpu;
    double ahead = 1e9;   // this frame's first work reached the GPU's queue this long before the GPU got free (ms)
    if (gp.thread && steady && st->xefgEnabled) {
        const uint64_t v = gp.signalled.load();
        const uint64_t donePrev = v >= 2 ? gp.doneAt[(v - 1) % 16].load() : 0;
        const uint64_t donePrev2 = v >= 3 ? gp.doneAt[(v - 2) % 16].load() : 0;
        if (!first)
            first = st->tStart;   // the Present call
        ahead = !donePrev ? 30.0 : donePrev >= first ? QpcMs(first, donePrev) : -(double)QpcMs(donePrev, first);
        ahead = std::max(-30.0, std::min(ahead, 30.0));
        c.aheadMs = c.gpuN ? c.aheadMs + 0.15 * (ahead - c.aheadMs) : ahead;
        if (donePrev && c.prevFirst) {
            // the frame before: the GPU's time for it, from its first work or from when the GPU got free for it
            const uint64_t from = std::max(c.prevFirst, donePrev2);
            const float gpu = donePrev > from ? QpcMs(from, donePrev) : 0;
            if (gpu > 0 && gpu < 200)
                c.gpuMs = c.gpuMs > 0 ? c.gpuMs + 0.1 * (gpu - c.gpuMs) : gpu;
            if (c.prevStart && donePrev > c.prevStart && QpcMs(c.prevStart, donePrev) < 500) {
                c.logGpuLat += QpcMs(c.prevStart, donePrev);   // the figure: its start -> the GPU has finished it
                c.logGpuN++;
            }
        }
        if (++c.gpuN < 8)
            ahead = -1e9;   // nothing known yet: no wait
        c.prevFirst = first;
        c.prevStart = start;
    } else {
        c.gpuN = 0;
        c.prevFirst = c.prevStart = 0;
    }
    float held = 0;
    if (!on || !steady) {
        c.hold = 0;
        c.prevEarly = 0;
        c.unheld = 0;
        c.frameMs = steady ? dt : 0;
        // the check: all over when frame generation went off; after a pause or a hitch only one under way
        c.chk.Reset(!on);
        c.chkAllowed = true;
        c.holdKept = 0;
    } else {
        c.frameMs = c.frameMs > 0 ? c.frameMs + (dt - c.frameMs) * 0.1 : dt;
        // How much later this frame could have started: by what it was held up in front of its present over
        // kLatTargetMs, and no more than its first work was at the GPU's queue earlier than kLatAheadMs before the
        // GPU got free (where the GPU is watched). A wait that the check found to cost is shorter by chk.shortMs.
        const double early = std::min(blockMs - std::max(0.3, kLatTargetMs + g_latMarginMs + c.chk.shortMs),
                                      ahead - (kLatAheadMs + g_latMarginMs + c.chk.shortMs));
        // The check: what the two times above say is not all a frame can wait for (see "Low latency"), so the wait
        // has to show that it costs no frame time and lowers the latency. FFX_XEFG_AB and FFX_XEFG_HOLD measure
        // without it.
        bool wait = true;
        if (g_latCheck && !g_abSec && g_holdMs <= 0) {
            const double lat = start && tEnd > start ? QpcMs(start, tEnd) : 0;
            wait = c.chk.Frame((double)tEnd / (double)g_qpf.QuadPart, dt, c.lastHeld, lat, !gp.thread || c.gpuN >= 8, Log);
            if (wait != c.chkAllowed) {
                c.chkAllowed = wait;
                if (!wait) {
                    c.holdKept = c.hold;   // left out for some frames: it comes back as it was
                } else {
                    c.hold = c.holdKept;
                    c.prevEarly = 0;       // the frames without it say nothing about it
                    c.unheld = 0;
                }
            }
        }
        if (!wait) {
            c.hold = 0;
            c.prevEarly = 0;
            c.unheld = 0;
        } else if (early > 0) {
            // the next frame starts later, by a part of what both this frame and the one before were early by (one
            // frame alone may be a hitch), 2 ms a frame at most
            const double both = std::min(early, c.prevEarly);
            if (both > 0)
                c.hold += std::min(0.25 * both, 2.0);
            c.unheld = 0;
        } else {
            // late, or just in time: earlier, the faster the longer it lasts
            c.unheld++;
            c.hold -= std::min(0.15 * -early * (1.0 + 0.5 * (c.unheld - 1)), 2.0);
        }
        c.prevEarly = early;
        if (g_holdMs > 0)   // FFX_XEFG_HOLD
            c.hold = g_holdMs;
        c.hold = std::max(0.0, std::min(c.hold, std::min(0.8 * c.frameMs, 100.0)));
        if (c.hold >= 0.05) {
            WaitUntil(tEnd + (uint64_t)(c.hold * (double)g_qpf.QuadPart / 1000.0), c.lateMs);
            held = QpcMs(tEnd, Qpc());
            c.logWaited++;
            c.logWait += held;
        }
    }
    c.lastHeld = held;
    if (++c.logN >= 300) {
        if (c.logLatN && (g_lat == 1 || g_times)) {
            char gpu[160] = "";
            if (c.logGpuN)
                snprintf(gpu, sizeof gpu, ", -> GPU done %.1f ms; GPU: %.1f ms a frame, the frame's first work %.1f ms %s",
                         c.logGpuLat / c.logGpuN, c.gpuMs, std::abs(c.aheadMs),
                         c.aheadMs >= 0 ? "before it gets free" : "after it got free");
            Log("low latency: waited in %d of %d frames, %.1f ms each; frames held up in front of Present %.1f ms, frame "
                "%.2f ms; start -> present end %.1f ms%s (%d frames)",
                c.logWaited, c.logN, c.logWaited ? c.logWait / c.logWaited : 0.0, c.logBlock / c.logLatN,
                c.logFrame / c.logLatN, c.logLat / c.logLatN, gpu, c.logLatN);
        }
        c.logN = c.logWaited = c.logLatN = c.logGpuN = 0;
        c.logWait = c.logLat = c.logBlock = c.logFrame = c.logGpuLat = 0;
    }
    return held;
}

// ---------------------------------------------------------------------------------------------------------------
// The shown frames
//
// At n x, XeSS-FG presents a rendered frame's n images on the real swap chain, and on Windows the Intel driver spaces
// them; here they reach the display one refresh apart and then nothing comes until the next rendered frame (at 3x and
// 31.9 ms a frame, gaps of 7, 7 and 18 ms: Resident Evil 4, OFLOW_FG, 2026-11-11). To see what goes to the display,
// the real swap chain's Present is hooked: XeSS-FG makes that swap chain through the DXGI factory while it sets
// itself up (DXVK's DXGI: one vtable for all its swap chains, this one is told apart by its pointer). Who presents
// what (FFX_XEFG_TRACE, XeSS-FG 1.3.1 under DXVK's DXGI, Resident Evil 4 with OFLOW_FG, 2026-11-13): at 3x the game's
// thread presents image 0 inside the proxy's Present (once the presentation thread is through with the frame before)
// and the presentation thread images 1 and 2; at 2x the game's thread presents both, 0.05 ms apart. With
// FFX_XEFG_TIMES=1 the log has the gaps between those Present calls every 300 rendered frames, and the longest over
// the shortest gap of each rendered frame. FFX_XEFG_TRACE puts every call into the time line (RP_CALL / RP_RET).
// Spacing the images cannot be done here (tried 2026-11-13: the calls came out evenly spaced, the display's gaps did
// not change): vkd3d-proton orders a Present on the game's queue when it is called, and a frame's images become ready
// together when the GPU has finished the frame and its interpolations. It has to come after that point, at
// vkQueuePresentKHR on vkd3d-proton's present thread or in gamescope.

typedef HRESULT(STDMETHODCALLTYPE* PFN_ScPresent)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_ScPresent1)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
static PFN_ScPresent g_origRealPresent;
static PFN_ScPresent1 g_origRealPresent1;
static void** g_realVt = nullptr;
static std::atomic<int> g_realSaid{0};
static thread_local int t_pxImages = -1;   // inside the proxy's Present: the images presented there so far; else -1

static bool PatchSlot(void** vt, int slot, void* hook, void** orig)
{
    if (vt[slot] == hook)
        return true;
    DWORD old;
    if (!VirtualProtect(&vt[slot], sizeof(void*), PAGE_READWRITE, &old))
        return false;
    *orig = vt[slot];
    vt[slot] = hook;
    VirtualProtect(&vt[slot], sizeof(void*), old, &old);
    return true;
}

static void EvenReport()
{
    auto pct = [](std::vector<float>& v, int p) { return v[std::min(v.size() - 1, v.size() * p / 100)]; };
    std::sort(g_ev.gaps.begin(), g_ev.gaps.end());
    std::sort(g_ev.ratios.begin(), g_ev.ratios.end());
    char r[96] = "none";
    if (!g_ev.ratios.empty())
        snprintf(r, sizeof r, "p50 %.2f p95 %.2f", pct(g_ev.ratios, 50), pct(g_ev.ratios, 95));
    Log("shown frames over %d rendered frames (%dx): present gaps ms p5 %.2f p50 %.2f p95 %.2f max %.2f; longest / "
        "shortest gap of a frame %s",
        g_ev.frames, g_ev.mult.load(), pct(g_ev.gaps, 5), pct(g_ev.gaps, 50), pct(g_ev.gaps, 95), g_ev.gaps.back(), r);
    g_ev.gaps.clear(), g_ev.ratios.clear();
    g_ev.frames = 0;
}

// FFX_XEFG_TIMES: image k's Present is called now
static void EvenNote(uint64_t now, int k)
{
    std::lock_guard<std::mutex> lk(g_ev.lock);
    const float gap = g_ev.lastCall ? QpcMs(g_ev.lastCall, now) : 0;
    g_ev.lastCall = now;
    if (gap <= 0 || gap > 250) {   // the first, or after a pause
        g_ev.gN = 0, g_ev.gMin = 1e9f, g_ev.gMax = 0;
        return;
    }
    g_ev.gaps.push_back(gap);
    // a rendered frame's gaps: the n that end at its images 1 .. n-1 and at the next frame's image 0
    if (k == 0) {
        if (g_ev.gN + 1 == g_ev.mult.load() && g_ev.mult.load() > 1)
            g_ev.ratios.push_back(std::max(g_ev.gMax, gap) / std::max(0.01f, std::min(g_ev.gMin, gap)));
        g_ev.gN = 0, g_ev.gMin = 1e9f, g_ev.gMax = 0;
        if (++g_ev.frames >= 300)
            EvenReport();
    } else {
        g_ev.gN++;
        g_ev.gMin = std::min(g_ev.gMin, gap);
        g_ev.gMax = std::max(g_ev.gMax, gap);
    }
}

// In front of each Present of a swap chain of DXVK's DXGI: image 0 is the first one inside the proxy's Present
static void EvenBefore(void* sc)
{
    if (sc != g_ev.real.load(std::memory_order_relaxed) || !g_times)
        return;
    const uint64_t now = Qpc();
    if (t_pxImages >= 0 && t_pxImages++ == 0) {
        g_ev.image.store(0);
        EvenNote(now, 0);
        return;
    }
    EvenNote(now, g_ev.image.fetch_add(1) + 1);
}

static HRESULT STDMETHODCALLTYPE H_RealPresent(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    if (g_realSaid.fetch_add(1) < 5)
        Log("real Present(%u, 0x%x) on %p%s", sync, flags, (void*)sc, t_pxImages >= 0 ? " (in the proxy's Present)" : "");
    if (!(flags & DXGI_PRESENT_TEST))
        EvenBefore(sc);
    TR(tr::Put(tr::RP_CALL, sync, flags));
    HRESULT hr = g_origRealPresent(sc, sync, flags);
    TR(tr::Put(tr::RP_RET, (uint64_t)hr));
    return hr;
}
static HRESULT STDMETHODCALLTYPE H_RealPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp)
{
    if (g_realSaid.fetch_add(1) < 5)
        Log("real Present1(%u, 0x%x) on %p%s", sync, flags, (void*)sc, t_pxImages >= 0 ? " (in the proxy's Present)" : "");
    if (!(flags & DXGI_PRESENT_TEST))
        EvenBefore(sc);
    TR(tr::Put(tr::RP_CALL, sync, flags, 1));
    HRESULT hr = g_origRealPresent1(sc, sync, flags, pp);
    TR(tr::Put(tr::RP_RET, (uint64_t)hr, 0, 1));
    return hr;
}

static void ModuleOf(void* addr, char* out, size_t n)
{   // the file a code address belongs to
    HMODULE m = nullptr;
    out[0] = 0;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &m) && m)
        GetModuleFileNameA(m, out, (DWORD)n);
}

// NVIDIA Streamline (sl.interposer.dll, shipped by CONTROL Resonant) hands out proxies of the DXGI objects; this
// interface id of its public headers returns the object underneath, which is what XeSS-FG presents on
static const GUID kStreamlineBase = {0xadec44e2, 0x61f0, 0x45c3, {0xad, 0x9f, 0x1b, 0x37, 0x37, 0x92, 0x84, 0xff}};
static std::atomic<int> g_capturing{0};
static std::atomic<void*> g_captured{nullptr};   // the swap chain made while capturing
static void** g_ownVt = nullptr;                 // FgSwapChain's: never taken for the real one

static void HookRealSwapChain(IUnknown* sc)
{
    IUnknown* base = nullptr;
    if (SUCCEEDED(sc->QueryInterface(kStreamlineBase, (void**)&base)) && base) {
        Log("swap chain %p is a Streamline proxy of %p", (void*)sc, (void*)base);
        sc = base;
    }
    void** vt = *(void***)sc;
    if (vt != g_ownVt) {
        char mod[MAX_PATH];
        ModuleOf(vt[8], mod, sizeof mod);
        bool a = PatchSlot(vt, 8, (void*)H_RealPresent, (void**)&g_origRealPresent);
        bool b = PatchSlot(vt, 22, (void*)H_RealPresent1, (void**)&g_origRealPresent1);
        g_realVt = vt;
        g_captured = (void*)sc;
        Log("real swap chain %p (vtable %p, %s): Present %s, Present1 %s", (void*)sc, (void*)vt, mod,
            a ? "hooked" : "not hooked", b ? "hooked" : "not hooked");
    }
    if (base)
        base->Release();
}

#ifdef FFXFG_DEBUG
// what the real swap chain's Present slots hold now (somebody may have put a hook of their own over ours)
static void CheckRealVt(const char* when)
{
    if (!tr::g_on || !g_realVt)
        return;
    char m8[MAX_PATH], m22[MAX_PATH], mo[MAX_PATH];
    ModuleOf(g_realVt[8], m8, sizeof m8);
    ModuleOf(g_realVt[22], m22, sizeof m22);
    ModuleOf((void*)g_origRealPresent, mo, sizeof mo);
    Log("trace: %s: real vtable %p slot 8 = %p (%s)%s, slot 22 = %p (%s)%s, original Present %p (%s), calls so far %d", when,
        (void*)g_realVt, g_realVt[8], m8, g_realVt[8] == (void*)H_RealPresent ? " ours" : "", g_realVt[22], m22,
        g_realVt[22] == (void*)H_RealPresent1 ? " ours" : "", (void*)g_origRealPresent, mo, g_realSaid.load());
}
#endif

// while XeSS-FG sets itself up it creates the real swap chain through the DXGI factory: CreateSwapChain (slot 10),
// CreateSwapChainForHwnd (15)
typedef HRESULT(STDMETHODCALLTYPE* PFN_FCreate)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_FForHwnd)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                 const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
static PFN_FCreate g_origFCreate;
static PFN_FForHwnd g_origFForHwnd;

static HRESULT STDMETHODCALLTYPE H_FCreate(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* d, IDXGISwapChain** out)
{
    HRESULT hr = g_origFCreate(f, dev, d, out);
    if (g_capturing.load())
        Log("CreateSwapChain -> 0x%08lx %p", (unsigned long)hr, out ? (void*)*out : nullptr);
    if (SUCCEEDED(hr) && out && *out && g_capturing.load())
        HookRealSwapChain(*out);
    return hr;
}
static HRESULT STDMETHODCALLTYPE H_FForHwnd(IDXGIFactory2* f, IUnknown* dev, HWND w, const DXGI_SWAP_CHAIN_DESC1* d,
                                            const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* o, IDXGISwapChain1** out)
{
    HRESULT hr = g_origFForHwnd(f, dev, w, d, fs, o, out);
    if (g_capturing.load())
        Log("CreateSwapChainForHwnd -> 0x%08lx %p", (unsigned long)hr, out ? (void*)*out : nullptr);
    if (SUCCEEDED(hr) && out && *out && g_capturing.load())
        HookRealSwapChain(*out);
    return hr;
}

// The system's DXGI factory class (DXVK's), not the one the game hands over: that can be a proxy (CONTROL Resonant
// passes NVIDIA Streamline's sl.interposer factory, and XeSS-FG presents on the swap chain underneath its proxy)
static void HookFactory()
{
    static bool done = false;
    IDXGIFactory2* f2 = nullptr;
    if (done || !PinSelf() || FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f2))))
        return;
    done = true;
    void** vt = *(void***)f2;
    char mod[MAX_PATH];
    ModuleOf(vt[15], mod, sizeof mod);
    bool a = PatchSlot(vt, 10, (void*)H_FCreate, (void**)&g_origFCreate);
    bool b = PatchSlot(vt, 15, (void*)H_FForHwnd, (void**)&g_origFForHwnd);
    Log("DXGI factory class of %s: CreateSwapChain %s, CreateSwapChainForHwnd %s", mod, a ? "hooked" : "not hooked",
        b ? "hooked" : "not hooked");
    f2->Release();
}

// ---------------------------------------------------------------------------------------------------------------
// HDR10
//
// XeSS-FG learns how the back buffer is encoded from one call: the game's SetColorSpace1 on the proxy swap chain.
// For DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 (HDR10, PQ) its interpolation takes the back buffer for HDR10, for
// any other value for SDR. The proxy notes that first and then passes the call on to the real swap chain, whatever
// that answers. (XeSS-FG 1.3.1; it forgets HDR10 again whenever it sets its interpolation up anew: a new size, or a
// change between one and several generated frames.)
// Told HDR10, XeSS-FG does not hand the PQ values to its network: it makes the network's images from them with
// 350 nits as white, and everything brighter is one flat white there. A picture with large areas above that leaves
// the network nothing to follow in them. CONTROL Resonant's HDR output goes up to about 1500 nits, and its sunlit
// clouds stuttered at 3x and more while the camera turned (not at 2x, which runs another network, and not with the
// game's HDR off). Not told, XeSS-FG gives the network the PQ values as they are: 350 nits is 0.64 there and
// 1500 nits 0.80, so the bright areas keep their detail. The shown frames are made from the back buffers either
// way and stay PQ.
// So XeSS-FG is not told by default (FFX_XEFG_HDR10=1: it is). The real swap chain still has to get HDR10, and only
// the proxy leads to it. After the game's call the proxy therefore gets a colour space that no swap chain takes
// (kRefusedColorSpace): XeSS-FG is back at SDR, the real swap chain refuses the call and keeps HDR10. Should a swap
// chain take it after all, HDR10 is set again at once, and XeSS-FG stays told from then on.
static const DXGI_COLOR_SPACE_TYPE kHdr10 = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
static const DXGI_COLOR_SPACE_TYPE kRefusedColorSpace = DXGI_COLOR_SPACE_CUSTOM;

// ---------------------------------------------------------------------------------------------------------------
// FgSwapChain: forwards everything to the XeSS-FG proxy, adds the XeLL markers and the present id.

struct FgSwapChain final : IDXGISwapChain4 {
    std::atomic<ULONG> ref{1};
    ScState* st;
    IDXGISwapChain4* sc;

    FgSwapChain(ScState* s) : st(s), sc(s->proxy) { st->refs++; }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv)
            return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIDeviceSubObject) ||
            riid == __uuidof(IDXGISwapChain) || riid == __uuidof(IDXGISwapChain1) ||
            riid == __uuidof(IDXGISwapChain2) || riid == __uuidof(IDXGISwapChain3) ||
            riid == __uuidof(IDXGISwapChain4)) {
            AddRef();
            *ppv = static_cast<IDXGISwapChain4*>(this);
            return S_OK;
        }
        return sc->QueryInterface(riid, ppv);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG r = --ref;
        if (r == 0) {
            Log("FgSwapChain %p released by the game", (void*)this);
            {
                std::lock_guard<std::mutex> lk(st->lock);
                st->wrapper = nullptr;
            }
            if (st->bridge) {   // no FFX context: the game's reference is the last one
                std::lock_guard<std::mutex> lk(g_globalLock);
                if (g_currentSc == st)
                    g_currentSc = nullptr;
            }
            ScRelease(st);
            delete this;
        }
        return r;
    }

    // IDXGIObject
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID g, UINT n, const void* d) override { return sc->SetPrivateData(g, n, d); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID g, const IUnknown* u) override { return sc->SetPrivateDataInterface(g, u); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID g, UINT* n, void* d) override { return sc->GetPrivateData(g, n, d); }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** p) override { return sc->GetParent(riid, p); }
    // IDXGIDeviceSubObject
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** p) override { return sc->GetDevice(riid, p); }

    // IDXGISwapChain
    HRESULT STDMETHODCALLTYPE Present(UINT sync, UINT flags) override
    {
        if (flags & DXGI_PRESENT_TEST)
            return sc->Present(sync, flags);
        return DoPresent(sync, flags, nullptr, false);
    }
    // the game's Present and Present1
    HRESULT DoPresent(UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp, bool one)
    {
#ifdef FFXFG_DEBUG
        const uint64_t n = st->presents + 1;
        tr::g_seq++;
        tr::g_presRet = 0;
        tr::g_stackTid = GetCurrentThreadId();
        tr::Put(tr::GP_CALL, st->frameId, n);
        if (tr::g_on && (n & 255) == 1)
            tr::HookInputs();
        if (n == 1 || n == 3 || n == 600)
            CheckRealVt("at a present");
#endif
        if (g_hdr10AbSec > 0 && st->xefgEnabled)
            HdrAb();
        // The answers to what the game asks of the swap chain until its next Present (see "Low latency"). While
        // XeSS-FG's presentation thread is inside a Present of the real swap chain this waits for it, as the proxy's
        // Present below would: a frame held up in front of its present, as far as the low latency is concerned.
        const uint64_t first = g_latQueue.load() == st->queue ? g_latFirstSubmit.load() : 0;
        float blockMs = 0;
        if (g_pollCache && st->xefgEnabled && st->lat.abTurn != 0)
            blockMs = PollAnswers();
        else
            PollForget();
        uint32_t id = BeforePresent(st);
        g_ev.mult = std::min<int>(g_mult, (int)st->maxInterpolations + 1);
        TR(tr::GpuMarkSignal(st->gpu1, st->queue, n); tr::Put(tr::PX_CALL, id, n));
        uint64_t span[3] = {};
        t_eclSpan = g_origEcl ? span : nullptr;
        t_pxImages = 0;   // the first real Present on this thread is the frame's image 0 (EvenBefore)
        HRESULT hr = one ? sc->Present1(sync, flags, pp) : sc->Present(sync, flags);
        t_pxImages = -1;
        t_eclSpan = nullptr;
        const uint64_t tEnd = Qpc();
        TR(tr::Put(tr::PX_RET, (uint64_t)hr, n, 0, tEnd); tr::GpuMarkSignal(st->gpu2, st->queue, n));
        LatGpuSignal(st);
        // what the frame waited inside XeSS-FG's Present: between its first and last submission, or, when they did
        // not show, the whole call less what it takes without a wait
        if (span[2] >= 2)
            blockMs += std::max(0.0f, QpcMs(span[0], span[1]) - kLatSpanMs);
        else
            blockMs += std::max(0.0f, QpcMs(st->tStart, tEnd) - kLatPresentMs);
        AfterPresent(st, id, hr, sync, flags, tEnd, blockMs, first);
#ifdef FFXFG_DEBUG
        tr::g_seq++;
        tr::Put(tr::GP_RET, (uint64_t)(QpcMs(tEnd, Qpc()) * 1000.0f), n, (uint16_t)std::min(blockMs * 100.0f, 65535.0f));
        if (st->xefgEnabled)   // the stack sampler looks at frames with interpolation only
            tr::g_presRet = Qpc();
        g_traceNext = n + 1;
#endif
        return hr;
    }
    // Takes the answers from the swap chain: the frame statistics and the color spaces asked about since the last
    // time. Returns how long that took (ms): it waits while a Present of the real swap chain is under way.
    float PollAnswers()
    {
        const uint64_t t0 = Qpc();
        DXGI_FRAME_STATISTICS fs = {};
        const HRESULT fhr = sc->GetFrameStatistics(&fs);
        ScState::ColorSpaceAnswer cs[8];
        int n;
        {
            std::lock_guard<std::mutex> lk(st->pollLock);
            n = st->nColorSpaces;
            memcpy(cs, st->colorSpaces, sizeof(cs));
        }
        for (int i = 0; i < n; i++)
            if (cs[i].asked)
                cs[i].hr = sc->CheckColorSpaceSupport(cs[i].cs, &cs[i].support);
        const float ms = QpcMs(t0, Qpc());
        std::lock_guard<std::mutex> lk(st->pollLock);
        st->stats = fs;
        st->statsHr = fhr;
        // the game may have asked about another color space meanwhile, on another thread: by value, not by place
        int keep = 0;
        for (int k = 0; k < st->nColorSpaces; k++) {
            ScState::ColorSpaceAnswer a = st->colorSpaces[k];
            for (int i = 0; i < n; i++)
                if (cs[i].cs == a.cs) {
                    if (cs[i].asked) {
                        a.hr = cs[i].hr;
                        a.support = cs[i].support;
                        a.asked = false;
                        a.idle = 0;
                    } else {
                        a.idle++;
                    }
                }
            if (a.idle < 600)   // one not asked about any more is forgotten
                st->colorSpaces[keep++] = a;
        }
        st->nColorSpaces = keep;
        st->pollValid = true;
        return ms;
    }
    // the swap chain itself answers from here on (interpolation off, a resize, a mode or color space change)
    void PollForget()
    {
        std::lock_guard<std::mutex> lk(st->pollLock);
        st->pollValid = false;
        st->nColorSpaces = 0;
    }
    // A call of the game into the swap chain has returned (started at `t0`). If it waited there while XeSS-FG
    // interpolates, for the presentation thread inside a Present of the real swap chain, the game's frame starts
    // where it returns rather than where its Present returned, and the log names the call: the low latency then
    // finds nothing to wait for (the frame is not held up in front of its next present), as before all this.
    void Waited(const char* what, int id, uint64_t t0)
    {
        const uint64_t t1 = Qpc();
        const float ms = QpcMs(t0, t1);
        TR(tr::Put(tr::SCCALL, id, 0, (uint16_t)std::min(ms * 1000.0f, 65535.0f), t0));
        if (ms < 2.0f || !st->xefgEnabled)
            return;
        st->frameStart = t1;
        if (st->waitedSaid.fetch_add(1) < 8)
            Log("the game's %s waited %.1f ms in the swap chain (for XeSS-FG's presentation thread): its frame starts "
                "there, not where Present returned",
                what, ms);
    }
// a call of the game passed on to the proxy, with Waited
#define SC_FORWARD(what, id, call)  \
    {                               \
        const uint64_t t0 = Qpc();  \
        auto r = call;              \
        Waited(what, id, t0);       \
        return r;                   \
    }
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT i, REFIID riid, void** p) override SC_FORWARD("GetBuffer", 1, sc->GetBuffer(i, riid, p))
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL f, IDXGIOutput* o) override
    {
        HRESULT hr;
        if (g_noExclusive) {
            st->fakeFullscreen = f;
            hr = S_OK;  // stay windowed (borderless under gamescope looks the same)
        } else {
            hr = sc->SetFullscreenState(f, o);
        }
        Log("SetFullscreenState(%d) -> 0x%08lx%s", f, (unsigned long)hr, g_noExclusive ? " (blocked)" : "");
        PollForget();
        Recheck();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* f, IDXGIOutput** o) override
    {
        const uint64_t t0 = Qpc();
        HRESULT hr;
        if (g_noExclusive) {
            hr = sc->GetFullscreenState(nullptr, o);
            if (f)
                *f = st->fakeFullscreen;
        } else {
            hr = sc->GetFullscreenState(f, o);
        }
        Waited("GetFullscreenState", 8, t0);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* d) override
    {
        const uint64_t t0 = Qpc();
        HRESULT hr = sc->GetDesc(d);
        if (SUCCEEDED(hr) && d && g_noExclusive)
            d->Windowed = !st->fakeFullscreen;
        Waited("GetDesc", 4, t0);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl) override
    {
        PollForget();
        HRESULT hr = sc->ResizeBuffers(n, w, h, f, fl);
        Log("ResizeBuffers %u %ux%u fmt %d flags 0x%x -> 0x%08lx", n, w, h, (int)f, fl, (unsigned long)hr);
        if (SUCCEEDED(hr)) {
            Resized();
            HdrResized();
        }
        Recheck();
        return hr;
    }
    // the size the swap chain has now (0x0 in ResizeBuffers means the window's); FFX_XEFG_OFLOW's textures follow it
    void Resized()
    {
        DXGI_SWAP_CHAIN_DESC1 d = {};
        if (FAILED(sc->GetDesc1(&d)))
            return;
        std::lock_guard<std::mutex> lk(st->lock);
        if (d.Width == st->width && d.Height == st->height)
            return;
        st->width = d.Width;
        st->height = d.Height;
        if (g_oflow) {   // also when there was none yet (a swap chain created at 0x0)
            OflowDestroy(st->oflow);
            st->oflow = OflowCreate(st->device, d.Width, d.Height, g_oflowDepth);
            st->oflowLastQpc = 0;   // the next frame resets XeSS-FG's history
            Log("oflow: remade for %ux%u -> %p", d.Width, d.Height, (void*)st->oflow);
        }
    }
    // After a mode change XeSS-FG must be asked again: it checks fullscreen only when interpolation is enabled.
    void Recheck()
    {
        std::lock_guard<std::mutex> lk(st->lock);
        if (st->xefgEnabled) {
            p_xefgSwapChainSetEnabled(st->xefg, 0);
            st->xefgEnabled = false;  // the next present with frame data enables it again
        }
    }
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* m) override
    {
        PollForget();
        return sc->ResizeTarget(m);
    }
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** o) override SC_FORWARD("GetContainingOutput", 7, sc->GetContainingOutput(o))
    // As of the last Present call while XeSS-FG interpolates (IGDEXT_XELL_POLLS, see "Low latency"): the swap chain's
    // own answer waits for XeSS-FG's presentation thread whenever that is inside a Present.
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* s) override
    {
        if (s) {
            std::lock_guard<std::mutex> lk(st->pollLock);
            if (st->pollValid) {
                TR(tr::Put(tr::SCCALL, 5, 1));
                *s = st->stats;
                return st->statsHr;
            }
        }
        SC_FORWARD("GetFrameStatistics", 5, sc->GetFrameStatistics(s))
    }
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* c) override SC_FORWARD("GetLastPresentCount", 11, sc->GetLastPresentCount(c))

    // IDXGISwapChain1
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* d) override SC_FORWARD("GetDesc1", 9, sc->GetDesc1(d))
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* d) override SC_FORWARD("GetFullscreenDesc", 10, sc->GetFullscreenDesc(d))
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND* h) override SC_FORWARD("GetHwnd", 12, sc->GetHwnd(h))
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID riid, void** p) override { return sc->GetCoreWindow(riid, p); }
    HRESULT STDMETHODCALLTYPE Present1(UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp) override
    {
        if (flags & DXGI_PRESENT_TEST)
            return sc->Present1(sync, flags, pp);
        return DoPresent(sync, flags, pp, true);
    }
    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override { return sc->IsTemporaryMonoSupported(); }
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** o) override { return sc->GetRestrictToOutput(o); }
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* c) override { return sc->SetBackgroundColor(c); }
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* c) override { return sc->GetBackgroundColor(c); }
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION r) override { return sc->SetRotation(r); }
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* r) override { return sc->GetRotation(r); }

    // IDXGISwapChain2
    HRESULT STDMETHODCALLTYPE SetSourceSize(UINT w, UINT h) override { return sc->SetSourceSize(w, h); }
    HRESULT STDMETHODCALLTYPE GetSourceSize(UINT* w, UINT* h) override { return sc->GetSourceSize(w, h); }
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT l) override
    {
        HRESULT hr = sc->SetMaximumFrameLatency(l);
        Log("SetMaximumFrameLatency(%u) -> 0x%08lx", l, (unsigned long)hr);
        if (g_ownWait && l >= 1) {
            std::lock_guard<std::mutex> lk(st->lock);
            if (st->latencySem && l > st->maxLatency)
                ReleaseSemaphore(st->latencySem, (LONG)(l - st->maxLatency), nullptr);
            st->maxLatency = l;
            hr = S_OK;
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* l) override
    {
        if (g_ownWait && l) {
            *l = st->maxLatency;
            return S_OK;
        }
        return sc->GetMaximumFrameLatency(l);
    }
    HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override
    {
        HANDLE h = nullptr;
        if (g_ownWait) {
            std::lock_guard<std::mutex> lk(st->lock);
            if (!st->latencySem)
                st->latencySem = CreateSemaphoreA(nullptr, (LONG)st->maxLatency, 64, nullptr);
            if (st->latencySem)
                PaceStart(st);
            // the caller closes the handle it gets, as with DXGI
            DuplicateHandle(GetCurrentProcess(), st->latencySem, GetCurrentProcess(), &h, 0, FALSE,
                            DUPLICATE_SAME_ACCESS);
        } else {
            h = sc->GetFrameLatencyWaitableObject();
        }
        Log("GetFrameLatencyWaitableObject -> %p (own %d)", (void*)h, g_ownWait);
        TR(tr::Put(tr::WAITOBJ));
        return h;
    }
    HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F* m) override { return sc->SetMatrixTransform(m); }
    HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F* m) override { return sc->GetMatrixTransform(m); }

    // IDXGISwapChain3
    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override SC_FORWARD("GetCurrentBackBufferIndex", 2, sc->GetCurrentBackBufferIndex())
    // As of the last Present call while XeSS-FG interpolates, like GetFrameStatistics: a color space the game asks
    // about for the first time goes to the swap chain, and is asked again at each Present from then on.
    HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE c, UINT* s) override
    {
        if (!s)
            return sc->CheckColorSpaceSupport(c, s);
        {
            std::lock_guard<std::mutex> lk(st->pollLock);
            for (int i = 0; st->pollValid && i < st->nColorSpaces; i++)
                if (st->colorSpaces[i].cs == c) {
                    TR(tr::Put(tr::SCCALL, 6, 1));
                    st->colorSpaces[i].asked = true;
                    *s = st->colorSpaces[i].support;
                    return st->colorSpaces[i].hr;
                }
        }
        const uint64_t t0 = Qpc();
        const HRESULT hr = sc->CheckColorSpaceSupport(c, s);
        Waited("CheckColorSpaceSupport", 6, t0);
        std::lock_guard<std::mutex> lk(st->pollLock);
        if (st->pollValid && st->nColorSpaces < 8) {
            bool have = false;
            for (int i = 0; i < st->nColorSpaces; i++)
                have |= st->colorSpaces[i].cs == c;
            if (!have)
                st->colorSpaces[st->nColorSpaces++] = {c, hr, *s, true, 0};
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE c) override
    {
        std::lock_guard<std::mutex> lk(st->hdrLock);
        // HDR10 once more while the real swap chain has it and XeSS-FG is not to know: nothing to pass on (a game
        // that sets its colour space every frame would wait for the real swap chain's lock twice each time)
        if (c == kHdr10 && st->hdrSet && !st->hdrTold && !HdrWant())
            return S_OK;
        PollForget();
        const HRESULT hr = sc->SetColorSpace1(c);
        st->hdrTold = c == kHdr10;   // XeSS-FG notes it whatever the real swap chain answered ("HDR10")
        const bool changed = SUCCEEDED(hr) && st->colorSpace.exchange((int)c) != (int)c;
        if (SUCCEEDED(hr))
            st->hdrSet = c == kHdr10;
        HdrSync();
        if (changed && c == kHdr10)
            Log("SetColorSpace1 %d (HDR10): XeSS-FG is %s", (int)c,
                st->hdrTold ? "told" : "not told and interpolates the PQ values as they are (FFX_XEFG_HDR10=1: told)");
        else if (changed)
            Log("SetColorSpace1 %d", (int)c);   // FFX_XEFG_OFLOW reads the back buffer in this encoding
        return hr;
    }
    // Whether XeSS-FG is to know of an HDR10 back buffer (see "HDR10")
    bool HdrWant() const { return g_hdr10AbSec > 0 ? st->hdrAbTurn <= 0 : g_hdr10; }
    // With st->hdrLock held: XeSS-FG's interpolation takes the back buffer for HDR10 only while the real swap chain
    // has HDR10 and XeSS-FG is to know; the real swap chain keeps its colour space. Only the proxy leads to both.
    void HdrSync()
    {
        const int cs = st->colorSpace.load();   // what the real swap chain has, -1: never set
        const bool want = cs == (int)kHdr10 && HdrWant();
        if (want == st->hdrTold)
            return;
        if (want || (cs >= 0 && cs != (int)kHdr10)) {
            // the real swap chain's own colour space once more: XeSS-FG takes it for what it is (it was told HDR10
            // by a call the real swap chain refused, or forgot HDR10, or is told for the first time)
            sc->SetColorSpace1((DXGI_COLOR_SPACE_TYPE)cs);
            st->hdrTold = want;
            return;
        }
        if (st->hdrKept)
            return;
        const HRESULT hr = sc->SetColorSpace1(kRefusedColorSpace);
        st->hdrTold = false;
        if (SUCCEEDED(hr) && cs == (int)kHdr10) {
            // the real swap chain took it: HDR10 again at once, and no further try
            const HRESULT back = sc->SetColorSpace1(kHdr10);
            st->hdrTold = true;
            st->hdrKept = true;
            Log("!! HDR10: the real swap chain took colour space 0x%x (0x%08lx), HDR10 is set again (0x%08lx): XeSS-FG "
                "stays told, as with FFX_XEFG_HDR10=1",
                (unsigned)kRefusedColorSpace, (unsigned long)hr, (unsigned long)back);
        } else if (!st->hdrSaid) {
            st->hdrSaid = true;
            Log("HDR10: XeSS-FG is back at SDR through colour space 0x%x, which the real swap chain %s (0x%08lx)",
                (unsigned)kRefusedColorSpace, FAILED(hr) ? "refused" : "took", (unsigned long)hr);
        }
    }
    // After the buffers were resized: XeSS-FG forgets HDR10 when it sets its interpolation up again, and the game's
    // next SetColorSpace1 goes to the real swap chain whatever it is
    void HdrResized()
    {
        std::lock_guard<std::mutex> lk(st->hdrLock);
        st->hdrSet = false;
        if (st->hdrTold) {
            st->hdrTold = false;
            HdrSync();
        }
    }
    // FFX_XEFG_HDR10_AB, on the present thread: told and not told in turns while XeSS-FG interpolates
    void HdrAb()
    {
        const uint64_t now = Qpc();
        if (!st->hdrAbStart)
            st->hdrAbStart = now;
        const int turn = (int)(((uint64_t)(QpcMs(st->hdrAbStart, now) / 1000.0f) / (uint64_t)g_hdr10AbSec) % 2);
        if (turn == st->hdrAbTurn)
            return;
        std::lock_guard<std::mutex> lk(st->hdrLock);
        st->hdrAbTurn = turn;
        if (st->colorSpace.load() != (int)kHdr10)
            return;
        HdrSync();
        SYSTEMTIME t;
        GetLocalTime(&t);
        Log("HDR10: XeSS-FG %s at %02d:%02d:%02d.%03d", st->hdrTold ? "told" : "not told", t.wHour, t.wMinute,
            t.wSecond, t.wMilliseconds);
    }
    HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl, const UINT* mask,
                                             IUnknown* const* queues) override
    {
        PollForget();
        HRESULT hr = sc->ResizeBuffers1(n, w, h, f, fl, mask, queues);
        Log("ResizeBuffers1 %u %ux%u fmt %d -> 0x%08lx", n, w, h, (int)f, (unsigned long)hr);
        if (SUCCEEDED(hr)) {
            Resized();
            HdrResized();
        }
        Recheck();
        return hr;
    }

    // IDXGISwapChain4
    HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE t, UINT n, void* d) override { return sc->SetHDRMetaData(t, n, d); }
#undef SC_FORWARD
};

// the current swap chain with a reference, so that a context destroyed on another thread stays valid while in use
static ScState* ScAcquire()
{
    std::lock_guard<std::mutex> lk(g_globalLock);
    if (g_currentSc)
        g_currentSc->refs++;
    return g_currentSc;
}

struct ScRef {
    ScState* st;
    ~ScRef() { if (st) ScRelease(st); }
};

static ScState* ScFromSwapChain(void* p)
{
    std::lock_guard<std::mutex> lk(g_globalLock);
    if (g_currentSc && g_currentSc->wrapper && (void*)static_cast<IDXGISwapChain4*>(g_currentSc->wrapper) == p)
        return g_currentSc;
    return g_currentSc;  // single swap chain: fall back to the current one
}

// ---------------------------------------------------------------------------------------------------------------
// Swap chain context creation

// XeSS-FG takes the depth direction and the kind of motion vectors when the swap chain is created, and games create
// the swap chain before the frame generation context that says what they are. The flags a game gave last time are
// kept per game (C:\users\<user>\AppData\Local\ffx_xefg\<exe>.flags in the prefix); without them inverted depth is
// assumed, as in Unreal Engine, Northlight (CONTROL Resonant) and RE Engine. With the wrong depth direction XeSS-FG takes the
// foreground for the background: moving characters flicker, the more the lower the frame rate.
static const uint32_t kXefgFfxFlags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED |
                                      FFX_FRAMEGENERATION_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION |
                                      FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS;
static uint32_t g_initFfxFlags;   // the FFX flags the XeSS-FG swap chain was set up for

static bool FlagsFile(char* path)
{
    char exe[MAX_PATH], base[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", base, MAX_PATH);
    if (!n || n >= MAX_PATH || !GetModuleFileNameA(nullptr, exe, MAX_PATH))
        return false;
    const char* name = strrchr(exe, '\\');
    name = name ? name + 1 : exe;
    snprintf(path, MAX_PATH, "%s\\ffx_xefg", base);
    CreateDirectoryA(path, nullptr);
    snprintf(path, MAX_PATH, "%s\\ffx_xefg\\%s.flags", base, name);
    return true;
}

static bool LoadFlags(uint32_t* flags)
{
    char path[MAX_PATH];
    if (!FlagsFile(path))
        return false;
    FILE* f = fopen(path, "r");
    if (!f)
        return false;
    bool ok = fscanf(f, "%x", flags) == 1;
    fclose(f);
    return ok;
}

static void SaveFlags(uint32_t flags)
{
    char path[MAX_PATH];
    if (!FlagsFile(path))
        return;
    if (FILE* f = fopen(path, "w")) {
        fprintf(f, "%x\n", flags);
        fclose(f);
    }
}

static uint32_t XefgInitFlags()
{
    uint32_t f = XEFG_SWAPCHAIN_INIT_FLAG_UITEXTURE_NOT_PREMUL_ALPHA;
    if (g_oflow) {   // our display-size motion vectors in pixels, depth not inverted
        Log("XeSS-FG set up for the optical flow inputs (display-size motion vectors)");
        return f | XEFG_SWAPCHAIN_INIT_FLAG_HIGH_RES_MV;
    }
    uint32_t ffx;
    bool have;
    {
        std::lock_guard<std::mutex> lk(g_globalLock);
        have = g_haveFgFlags;
        ffx = g_lastFgFlags;
    }
    if (have)
        Log("XeSS-FG set up with the game's flags 0x%x", ffx);
    else if (LoadFlags(&ffx))
        Log("XeSS-FG set up with the flags the game gave last time (0x%x)", ffx);
    else {
        ffx = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED;
        Log("XeSS-FG set up for inverted depth (the game's flags come later)");
    }
    g_initFfxFlags = ffx & kXefgFfxFlags;
    if (ffx & FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED)
        f |= XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH;
    if (ffx & FFX_FRAMEGENERATION_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION)
        f |= XEFG_SWAPCHAIN_INIT_FLAG_JITTERED_MV;
    if (ffx & FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS)
        f |= XEFG_SWAPCHAIN_INIT_FLAG_HIGH_RES_MV;
    if (g_mvNdc)
        f |= XEFG_SWAPCHAIN_INIT_FLAG_USE_NDC_VELOCITY;
    return f;
}

enum class ScKind { Wrap, New, ForHwnd };

static ffxReturnCode_t CreateSwapChainContext(ffxContext* context, ScKind kind, const ffxApiHeader* desc)
{
    IDXGISwapChain4** out = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    IDXGISwapChain4* appSwapChain = nullptr;
    HWND hwnd = nullptr;
    DXGI_SWAP_CHAIN_DESC1 d1 = {};
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fs = {};
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pfs = nullptr;
    IDXGIFactory* factory = nullptr;

    if (kind == ScKind::Wrap) {
        auto* d = (const ffxCreateContextDescFrameGenerationSwapChainWrapDX12*)desc;
        out = d->swapchain;
        queue = d->gameQueue;
        appSwapChain = out ? *out : nullptr;
        if (!appSwapChain)
            return FFX_API_RETURN_ERROR_PARAMETER;
        appSwapChain->GetDesc1(&d1);
    } else if (kind == ScKind::New) {
        auto* d = (const ffxCreateContextDescFrameGenerationSwapChainNewDX12*)desc;
        out = d->swapchain;
        queue = d->gameQueue;
        factory = d->dxgiFactory;
        if (!d->desc)
            return FFX_API_RETURN_ERROR_PARAMETER;
        const DXGI_SWAP_CHAIN_DESC& o = *d->desc;
        hwnd = o.OutputWindow;
        d1.Width = o.BufferDesc.Width;
        d1.Height = o.BufferDesc.Height;
        d1.Format = o.BufferDesc.Format;
        d1.SampleDesc = o.SampleDesc;
        d1.BufferUsage = o.BufferUsage;
        d1.BufferCount = o.BufferCount;
        d1.Scaling = o.BufferDesc.Scaling == DXGI_MODE_SCALING_UNSPECIFIED ? DXGI_SCALING_STRETCH
                     : o.BufferDesc.Scaling == DXGI_MODE_SCALING_CENTERED  ? DXGI_SCALING_NONE
                                                                           : DXGI_SCALING_STRETCH;
        d1.SwapEffect = o.SwapEffect;
        d1.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        d1.Flags = o.Flags;
        fs.RefreshRate = o.BufferDesc.RefreshRate;
        fs.ScanlineOrdering = o.BufferDesc.ScanlineOrdering;
        fs.Scaling = o.BufferDesc.Scaling;
        fs.Windowed = o.Windowed;
        pfs = &fs;
    } else {
        auto* d = (const ffxCreateContextDescFrameGenerationSwapChainForHwndDX12*)desc;
        out = d->swapchain;
        queue = d->gameQueue;
        factory = d->dxgiFactory;
        hwnd = d->hwnd;
        if (!d->desc)
            return FFX_API_RETURN_ERROR_PARAMETER;
        d1 = *d->desc;
        pfs = d->fullscreenDesc;
    }
    if (!out || !queue)
        return FFX_API_RETURN_ERROR_PARAMETER;

    auto* st = new ScState();
    st->width = d1.Width;
    st->height = d1.Height;
    st->format = d1.Format;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&st->device)))) {
        delete st;
        return FFX_API_RETURN_ERROR_RUNTIME_ERROR;
    }
    queue->AddRef();
    st->queue = queue;

    auto fail = [&](const char* what, int code) {
        Log("swap chain context: %s failed (%d)", what, code);
        st->refs = 1;
        if (st->proxy) {
            st->proxy->Release();
            st->proxy = nullptr;
        }
        TearDown(st);
        return FFX_API_RETURN_ERROR_RUNTIME_ERROR;
    };

    xefg_swapchain_d3d12_init_params_t ip = {};
    uint32_t want = 0;
    IntelVendorForXess(st->device);
    xefg_swapchain_result_t r = p_xefgSwapChainD3D12CreateContext(st->device, &st->xefg);
    if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS) {
        st->xefg = nullptr;
        return fail("xefgSwapChainD3D12CreateContext", r);
    }
    if (g_xefgLogLevel >= 0 && g_xefgLogLevel <= 3)
        p_xefgSwapChainSetLoggingCallback(st->xefg, (xefg_swapchain_logging_level_t)g_xefgLogLevel, XefgLogCallback,
                                          nullptr);

    xell_result_t xr = p_xellD3D12CreateContext(st->device, &st->xell);
    if (xr != XELL_RESULT_SUCCESS) {
        st->xell = nullptr;
        return fail("xellD3D12CreateContext", xr);
    }
    xell_sleep_params_t sp = {};
    sp.bLowLatencyMode = 1;
    // FFX_XEFG_CAP: microseconds per real frame, for a steady real frame rate. XeLL's minimum interval counts every
    // frame shown, the generated ones too (27000 gave 54 ms per real frame at 2x), so it gets the interval per frame shown
    int cap = EnvInt("FFX_XEFG_CAP", 0);
    if (cap > 0) {
        sp.minimumIntervalUs = (uint32_t)(cap / (g_mult > 1 ? g_mult : 1));
        Log("XeLL: one real frame per %d us (%.1f fps), %u us per frame shown", cap, 1e6 / cap, sp.minimumIntervalUs);
    }
    xr = p_xellSetSleepMode(st->xell, &sp);
    if (xr != XELL_RESULT_SUCCESS)
        Log("xellSetSleepMode -> %d", (int)xr);
    r = p_xefgSwapChainSetLatencyReduction(st->xefg, st->xell);
    if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS)
        return fail("xefgSwapChainSetLatencyReduction", r);

    // a swap chain made in exclusive fullscreen: XeSS-FG interpolates only windowed, so it starts windowed and the
    // game is told it is fullscreen (as SetFullscreenState below; under gamescope both look the same)
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fsWin = {};
    if (pfs && !pfs->Windowed && g_noExclusive) {
        fsWin = *pfs;
        fsWin.Windowed = TRUE;
        pfs = &fsWin;
        st->fakeFullscreen = TRUE;
    }
    Log("swap chain %ux%u format %d buffers %u effect %d flags 0x%x scaling %d alpha %d, %s%s", d1.Width, d1.Height,
        (int)d1.Format, d1.BufferCount, (int)d1.SwapEffect, d1.Flags, (int)d1.Scaling, (int)d1.AlphaMode,
        pfs ? (pfs->Windowed ? "windowed" : "exclusive fullscreen") : "no fullscreen desc",
        st->fakeFullscreen ? " (asked for exclusive fullscreen, made windowed)" : "");
    ip.initFlags = XefgInitFlags();
    ip.maxInterpolatedFrames = XEFG_SWAPCHAIN_USE_MAX_SUPPORTED_INTERPOLATED_FRAMES;
    ip.uiMode = XEFG_SWAPCHAIN_UI_MODE_AUTO;
    if ((g_lat == 1 && !g_ffxCap) || g_times) {
        // the low latency measures in XeSS-FG's Present and on the game's queue (HookEcl, LatGpu), and waits for this
        // XeLL context itself
        if (HookEclVt(queue)) {
            g_latFirstSubmit = 0;
            g_latQueue = queue;
            LatGpuStart(st);
        }
        if (g_lat == 1 && !g_ffxCap)
            ShimExternalPacing(st->xell, 1);
    }
    // the real swap chain's Present: the shown frames' figures (and the time line)
    if (g_times TR(|| tr::g_on))
        HookFactory();
    g_captured = nullptr;
    struct Capturing {   // the swap chain XeSS-FG makes now is the real one
        Capturing() { g_capturing++; }
        ~Capturing() { g_capturing--; }
    } capturing;
#ifdef FFXFG_DEBUG
    if (tr::g_on) {
        // the queues' ExecuteCommandLists and Signal, the GPU marks, the input calls
        HookEclVt(queue);
        tr::HookQueueVt(queue);
        g_traceQueue = queue;
        st->gpu1 = tr::GpuMarkCreate(st->device, tr::GPU1);
        st->gpu3 = tr::GpuMarkCreate(st->device, tr::GPU3);
        g_tracePre = st->gpu3;
        st->gpu2 = tr::GpuMarkCreate(st->device, tr::GPU2);
        tr::HookInputs();
    }
#endif

    if (kind == ScKind::Wrap) {
        // XeSS-FG takes the game's reference and recreates the native swap chain, as FFX's wrap does.
        ULONG refsPlusOne = appSwapChain->AddRef();
        appSwapChain->Release();
        Log("wrap: game swap chain %p refs %lu", (void*)appSwapChain, refsPlusOne - 1);
        ip.pApplicationSwapChain = appSwapChain;
        r = p_xefgSwapChainD3D12InitFromSwapChain(st->xefg, queue, &ip);
    } else {
        IDXGIFactory2* f2 = nullptr;
        if (!factory || FAILED(factory->QueryInterface(IID_PPV_ARGS(&f2))))
            return fail("IDXGIFactory2", 0);
        r = p_xefgSwapChainD3D12InitFromSwapChainDesc(st->xefg, hwnd, &d1, pfs, queue, f2, &ip);
        f2->Release();
    }
    Log("xefgSwapChainD3D12InitFromSwapChain(Desc) -> %d", (int)r);
    TR(CheckRealVt("after XeSS-FG's setup"));
    st->realSc = g_captured.exchange(nullptr);
    if (st->realSc)
        g_ev.real = st->realSc;
    if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS)
        return fail("xefgSwapChainD3D12InitFromSwapChain(Desc)", r);
    r = p_xefgSwapChainD3D12GetSwapChainPtr(st->xefg, IID_PPV_ARGS(&st->proxy));
    if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS || !st->proxy)
        return fail("xefgSwapChainD3D12GetSwapChainPtr", r);
    ShimProxyOwned(st->proxy, 1);

    xefg_swapchain_properties_t props = {};
    if (p_xefgSwapChainGetProperties(st->xefg, &props) == XEFG_SWAPCHAIN_RESULT_SUCCESS)
        st->maxInterpolations = props.maxSupportedInterpolations;
    want = (uint32_t)(g_mult - 1);
    if (want > st->maxInterpolations)
        want = st->maxInterpolations;
    if (p_xefgSwapChainSetNumInterpolatedFrames) {
        r = p_xefgSwapChainSetNumInterpolatedFrames(st->xefg, want);
        Log("xefgSwapChainSetNumInterpolatedFrames(%u) -> %d", want, (int)r);
    }

    if (g_oflow && (!st->width || !st->height)) {   // 0x0 in the description: the window's size
        DXGI_SWAP_CHAIN_DESC1 pd = {};
        if (SUCCEEDED(st->proxy->GetDesc1(&pd))) {
            st->width = pd.Width;
            st->height = pd.Height;
        }
    }
    if (g_oflow && !(st->oflow = OflowCreate(st->device, st->width, st->height, g_oflowDepth)))
        Log("oflow: not available, the game's motion vectors are used");
    st->wrapper = new FgSwapChain(st);
    g_ownVt = *(void***)static_cast<IDXGISwapChain4*>(st->wrapper);
    // FFX's context keeps a reference to the swap chain until ffxDestroyContext; Wuthering Waves (FSR 3.1.1) drops
    // its own reference before that and still presents
    st->wrapper->AddRef();
    st->contextRef = st->wrapper;
    *out = st->wrapper;
    {
        std::lock_guard<std::mutex> lk(g_globalLock);
        g_currentSc = st;
    }
    *context = st;
    Log("swap chain context %p: %ux%u fmt %d, init flags 0x%x, max interpolations %u, set %u, proxy %p, wrapper %p",
        (void*)st, st->width, st->height, (int)st->format, ip.initFlags, st->maxInterpolations, want,
        (void*)st->proxy, (void*)st->wrapper);
    return FFX_API_RETURN_OK;
}

// ---------------------------------------------------------------------------------------------------------------
// Frame generation context

#ifdef FFXFG_DEBUG
// FFX_XEFG_DUMP: copies of the game's depth and motion vectors, written some frames later (when the game's command
// list has run) as ffx_xefg_<frame>_<what>.bin: header width, height, row pitch, DXGI format (4 x uint32), rows
struct Dump {
    ID3D12Resource* buf = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    uint32_t frame = 0, writeAt = 0;
    const char* what = "";
    float camera[13] = {};  // fov, position, right, up, forward
};
static Dump g_dumps[16];
static int g_nDumps;

static void DumpCopy(ID3D12Device* dev, ID3D12GraphicsCommandList* cl, ID3D12Resource* res, uint32_t state,
                     uint32_t frame, uint32_t writeAt, const char* what, const float* camera)
{
    if (!dev || !cl || !res || g_nDumps >= 16)
        return;
    D3D12_RESOURCE_DESC rd = res->GetDesc();
    Dump& d = g_dumps[g_nDumps];
    UINT64 total = 0;
    dev->GetCopyableFootprints(&rd, 0, 1, 0, &d.fp, nullptr, nullptr, &total);
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            IID_PPV_ARGS(&d.buf))))
        return;
    D3D12_RESOURCE_STATES s = ToD3D12State(state);
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = s;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (s != D3D12_RESOURCE_STATE_COPY_SOURCE)
        cl->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst = {d.buf, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT}, src = {res};
    dst.PlacedFootprint = d.fp;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    if (s != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        cl->ResourceBarrier(1, &b);
    }
    d.frame = frame;
    d.writeAt = writeAt;
    d.what = what;
    memcpy(d.camera, camera, sizeof(d.camera));
    g_nDumps++;
}

static void DumpWrite(uint32_t now)
{
    for (int i = 0; i < g_nDumps; i++) {
        Dump& d = g_dumps[i];
        if (!d.buf || now < d.writeAt)
            continue;
        void* p = nullptr;
        D3D12_RANGE r = {0, (SIZE_T)d.fp.Footprint.RowPitch * d.fp.Footprint.Height};
        char path[MAX_PATH];
        snprintf(path, MAX_PATH, "%sffx_xefg_%u_%s.bin", g_dir, d.frame, d.what);
        if (SUCCEEDED(d.buf->Map(0, &r, &p))) {
            if (FILE* f = fopen(path, "wb")) {
                uint32_t h[4] = {d.fp.Footprint.Width, d.fp.Footprint.Height, d.fp.Footprint.RowPitch,
                                 (uint32_t)d.fp.Footprint.Format};
                fwrite(h, sizeof(h), 1, f);
                fwrite(d.camera, sizeof(d.camera), 1, f);
                fwrite((const uint8_t*)p + d.fp.Offset, 1, r.End, f);
                fclose(f);
                Log("dump %s: %ux%u format %u", path, h[0], h[1], h[3]);
            }
            d.buf->Unmap(0, nullptr);
        }
        d.buf->Release();
        d.buf = nullptr;
    }
}
#endif

static void TagPrepare(FgCtx* fg, uint64_t frameID, void* cmdList, FfxApiDimensions2D renderSize,
                       FfxApiFloatCoords2D jitter, FfxApiFloatCoords2D mvScale, float frameTimeDelta, bool reset,
                       float cameraNear, float cameraFar, float fovY, const struct FfxApiResource& depth,
                       const struct FfxApiResource& mv, const float* camPos, const float* camUp, const float* camRight,
                       const float* camFwd)
{
    ScState* st = ScAcquire();
    if (!st)
        return;
    ScRef ref{st};   // released after the lock below
    std::lock_guard<std::mutex> lk(st->lock);
    if (st->bridge && st->oflow)   // BeforePresent makes the frames
        return;
    uint32_t id = (uint32_t)frameID;
    if (st->bridge) {
        id = ++st->nextId;
        st->pending.push_back(id);
        if (st->pending.size() > 8)   // upscaled but never presented
            st->pending.pop_front();
    }
    Order("prepare", id);
    TR(tr::Put(tr::PREP, id));

    // SIMULATION_START went out with the sleep after the last present when the id was the expected one, else now
    XellMarker(st, id, XELL_SIMULATION_END);
    XellMarker(st, id, XELL_RENDERSUBMIT_START);

    if (st->oflow) {
        OflowTag(st, id, reset, frameTimeDelta);
        st->frameId = id;
        st->havePrepare = true;
        fg->prepares++;
        return;
    }

    xefg_swapchain_d3d12_resource_data_t rd = {};
    // ONLY_NOW: XeSS-FG records a copy on the game's command list; UNTIL_NEXT_PRESENT (FFX_XEFG_VALIDITY=0, for
    // measuring the copy's cost) reads the game's texture at present time, which the game may have written since
    rd.validity = g_validityNow ? XEFG_SWAPCHAIN_RV_ONLY_NOW : XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT;
    rd.resourceSize = {renderSize.width, renderSize.height};
    rd.type = XEFG_SWAPCHAIN_RES_DEPTH;
    rd.pResource = (ID3D12Resource*)depth.resource;
    rd.incomingState = ToD3D12State(depth.state);
    xefg_swapchain_result_t r1 =
        p_xefgSwapChainD3D12TagFrameResource(st->xefg, (ID3D12CommandList*)cmdList, id, &rd);
    rd.type = XEFG_SWAPCHAIN_RES_MOTION_VECTOR;
    rd.pResource = (ID3D12Resource*)mv.resource;
    rd.incomingState = ToD3D12State(mv.state);
    if (fg->flags & FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS)
        rd.resourceSize = {mv.description.width, mv.description.height};
    xefg_swapchain_result_t r2 =
        p_xefgSwapChainD3D12TagFrameResource(st->xefg, (ID3D12CommandList*)cmdList, id, &rd);

    xefg_swapchain_frame_constant_data_t c = {};
    static const float kPos[3] = {0, 0, 0}, kUp[3] = {0, 1, 0}, kRight[3] = {1, 0, 0}, kFwd[3] = {0, 0, 1};
    bool projInverted = (fg->flags & FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED) != 0;
#ifdef FFXFG_DEBUG
    if (g_camera == 6)   // as if the game gave no camera (a fixed view, the real projection)
        camPos = camRight = camUp = camFwd = nullptr;
    if (g_camera == 3 || g_camera == 4)   // projection not inverted (only the init flag says so)
        projInverted = false;
#endif
    if (g_camera == 5) {
        // debug: all zero, no camera at all
    } else if (g_camera) {
        BuildView(camPos ? camPos : kPos, camRight ? camRight : kRight, camUp ? camUp : kUp, camFwd ? camFwd : kFwd,
                  c.viewMatrix);
        float aspect = renderSize.height ? (float)renderSize.width / (float)renderSize.height : 1.0f;
        BuildProjection(fovY, aspect, cameraNear, cameraFar, projInverted,
                        (fg->flags & FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE) != 0, c.projectionMatrix);
#ifdef FFXFG_DEBUG
        if (g_camera == 2 || g_camera == 4)   // transposed
            for (int r = 0; r < 4; ++r)
                for (int k = r + 1; k < 4; ++k) {
                    std::swap(c.viewMatrix[r * 4 + k], c.viewMatrix[k * 4 + r]);
                    std::swap(c.projectionMatrix[r * 4 + k], c.projectionMatrix[k * 4 + r]);
                }
#endif
    } else {
        for (int i = 0; i < 4; ++i)
            c.viewMatrix[i * 5] = c.projectionMatrix[i * 5] = 1.0f;
    }
    c.jitterOffsetX = jitter.x;
    c.jitterOffsetY = jitter.y;
    // FFX: motion vectors times the scale are pixels of the render size; in NDC that is 2 / size, y up
    c.motionVectorScaleX = g_mvNdc && renderSize.width ? mvScale.x * 2.0f / renderSize.width : mvScale.x;
    c.motionVectorScaleY = g_mvNdc && renderSize.height ? -mvScale.y * 2.0f / renderSize.height : mvScale.y;
    c.resetHistory = reset ? 1 : 0;
    c.frameRenderTime = frameTimeDelta;
#ifdef FFXFG_DEBUG
    c.jitterOffsetX *= g_jitter;
    c.jitterOffsetY *= g_jitter;
    c.frameRenderTime = g_renderTime >= 0 ? g_renderTime : frameTimeDelta * g_renderScale;
#endif
    xefg_swapchain_result_t r3 = p_xefgSwapChainTagFrameConstants(st->xefg, id, &c);

    if (!st->bridge) {
        st->frameId = id;
        st->havePrepare = true;
    }
    fg->prepares++;
#ifdef FFXFG_DEBUG
    static uint32_t dumpFrames;   // frames with interpolation on (menus and loading screens have no depth)
    if (g_dumpEvery > 0) {
        if (st->xefgEnabled)
            dumpFrames++;
        DumpWrite((uint32_t)fg->prepares);
        // two frames in a row, to compare what moved
        if (st->xefgEnabled && dumpFrames > 1 && dumpFrames % g_dumpEvery <= 1 && g_nDumps < 16) {
            float cam[13] = {fovY};
            for (int i = 0; i < 3; i++) {
                cam[1 + i] = camPos ? camPos[i] : 0;
                cam[4 + i] = camRight ? camRight[i] : 0;
                cam[7 + i] = camUp ? camUp[i] : 0;
                cam[10 + i] = camFwd ? camFwd[i] : 0;
            }
            uint32_t at = (uint32_t)fg->prepares + 10;
            auto* cl = (ID3D12GraphicsCommandList*)cmdList;
            DumpCopy(st->device, cl, (ID3D12Resource*)depth.resource, depth.state, id, at, "depth", cam);
            DumpCopy(st->device, cl, (ID3D12Resource*)mv.resource, mv.state, id, at, "mv", cam);
        }
    }
#endif
    if (fg->prepares <= 5 || fg->prepares % 600 == 0 || r1 || r2 || r3)
        Log("prepare #%llu frame %u render %ux%u jitter %.3f,%.3f mvScale %.1f,%.1f dt %.2f reset %d fov %.3f "
            "near %.3f far %.1f -> tag depth %d mv %d const %d",
            (unsigned long long)fg->prepares, id, renderSize.width, renderSize.height, jitter.x, jitter.y, mvScale.x,
            mvScale.y, frameTimeDelta, reset, fovY, cameraNear, cameraFar, (int)r1, (int)r2, (int)r3);
    if (fg->prepares <= 3 || fg->prepares % 600 == 0)
        Log("  camera %s pos %.2f,%.2f,%.2f right %.3f,%.3f,%.3f up %.3f,%.3f,%.3f fwd %.3f,%.3f,%.3f, flags 0x%x, "
            "depth %ux%u fmt %u, mv %ux%u fmt %u",
            camPos ? "given" : "not given", camPos ? camPos[0] : 0, camPos ? camPos[1] : 0, camPos ? camPos[2] : 0,
            camRight ? camRight[0] : 0, camRight ? camRight[1] : 0, camRight ? camRight[2] : 0, camUp ? camUp[0] : 0,
            camUp ? camUp[1] : 0, camUp ? camUp[2] : 0, camFwd ? camFwd[0] : 0, camFwd ? camFwd[1] : 0,
            camFwd ? camFwd[2] : 0, fg->flags, depth.description.width, depth.description.height,
            depth.description.format, mv.description.width, mv.description.height, mv.description.format);
}

static ffxReturnCode_t FgConfigure(FgCtx* fg, const ffxApiHeader* desc)
{
    for (const ffxApiHeader* h = desc; h; h = h->pNext) {
        switch (h->type) {
        case FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION: {
            auto* d = (const ffxConfigureDescFrameGeneration*)h;
            ScState* st = ScAcquire();
            if (!st)
                break;
            ScRef ref{st};   // released after the lock below
            std::lock_guard<std::mutex> lk(st->lock);
            if (st->gameEnabled != d->frameGenerationEnabled)
                Log("game frame generation %s (frame %llu, callbacks present %p fg %p)",
                    d->frameGenerationEnabled ? "on" : "off", (unsigned long long)d->frameID,
                    (void*)d->presentCallback, (void*)d->frameGenerationCallback);
            st->gameEnabled = d->frameGenerationEnabled;
            Order("configure", d->frameID);
            TR(tr::Put(tr::CONF, d->frameID, d->frameGenerationEnabled));
            // only while frame generation is on: RE Requiem configures with frame 0 every frame before that, and
            // XeSS-FG rejects a second tag for the same frame
            if (d->HUDLessColor.resource && d->frameGenerationEnabled && g_hudless) {
                xefg_swapchain_d3d12_resource_data_t rd = {};
                rd.type = XEFG_SWAPCHAIN_RES_HUDLESS_COLOR;
                rd.validity = XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT;
                rd.resourceSize = {d->HUDLessColor.description.width, d->HUDLessColor.description.height};
                rd.pResource = (ID3D12Resource*)d->HUDLessColor.resource;
                rd.incomingState = ToD3D12State(d->HUDLessColor.state);
                p_xefgSwapChainD3D12TagFrameResource(st->xefg, nullptr, (uint32_t)d->frameID, &rd);
                if (!st->uiComposition && p_xefgSwapChainSetUiCompositionState) {
                    xefg_swapchain_result_t r =
                        p_xefgSwapChainSetUiCompositionState(st->xefg, XEFG_SWAPCHAIN_UI_COMPOSITION_STATE_ENABLED);
                    Log("UI composition on (HUD-less color) -> %d", (int)r);
                    st->uiComposition = true;
                }
            }
            break;
        }
        case FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION_KEYVALUE:
        case FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION_REGISTERDISTORTIONRESOURCE:  // XeSS-FG has no distortion input
        case FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG:
            break;
        default:
            if (h == desc)
                return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
        }
    }
    return FFX_API_RETURN_OK;
}

static ffxReturnCode_t FgDispatch(FgCtx* fg, const ffxApiHeader* desc)
{
    switch (desc->type) {
    case FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2: {
        auto* d = (const ffxDispatchDescFrameGenerationPrepareV2*)desc;
        TagPrepare(fg, d->frameID, d->commandList, d->renderSize, d->jitterOffset, d->motionVectorScale,
                   d->frameTimeDelta, d->reset, d->cameraNear, d->cameraFar, d->cameraFovAngleVertical, d->depth,
                   d->motionVectors, d->cameraPosition, d->cameraUp, d->cameraRight, d->cameraForward);
        return FFX_API_RETURN_OK;
    }
    case FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE: {
        auto* d = (const ffxDispatchDescFrameGenerationPrepare*)desc;
        const ffxDispatchDescFrameGenerationPrepareCameraInfo* cam = nullptr;
        for (const ffxApiHeader* h = desc->pNext; h; h = h->pNext)
            if (h->type == FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_CAMERAINFO)
                cam = (const ffxDispatchDescFrameGenerationPrepareCameraInfo*)h;
        TagPrepare(fg, d->frameID, d->commandList, d->renderSize, d->jitterOffset, d->motionVectorScale,
                   d->frameTimeDelta, false, d->cameraNear, d->cameraFar, d->cameraFovAngleVertical, d->depth,
                   d->motionVectors, cam ? cam->cameraPosition : nullptr, cam ? cam->cameraUp : nullptr,
                   cam ? cam->cameraRight : nullptr, cam ? cam->cameraForward : nullptr);
        return FFX_API_RETURN_OK;
    }
    case FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION:
        // The game dispatches the interpolation itself; XeSS-FG does it inside the proxy swap chain instead.
        return FFX_API_RETURN_OK;
    default:
        return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// FSR_FG=inject: a game with FSR upscaling and no FSR frame generation (xefg_bridge.h)

extern "C" __declspec(dllexport) HRESULT XefgBridgeCreateSwapChain(IDXGIFactory* factory, ID3D12CommandQueue* queue, HWND hwnd,
                                                                   const DXGI_SWAP_CHAIN_DESC1* desc,
                                                                   const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen,
                                                                   IDXGISwapChain1** out)
{
    Init();
    if (!g_libsOk)
        return E_FAIL;
    DXGI_SWAP_CHAIN_DESC1 d1 = *desc;
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fs = {};
    if (fullscreen)
        fs = *fullscreen;
    IDXGISwapChain4* sc = nullptr;
    ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 d = {};
    d.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
    d.swapchain = &sc;
    d.hwnd = hwnd;
    d.desc = &d1;
    d.fullscreenDesc = fullscreen ? &fs : nullptr;
    d.dxgiFactory = factory;
    d.gameQueue = queue;
    ffxContext ctx = nullptr;
    if (CreateSwapChainContext(&ctx, ScKind::ForHwnd, &d.header) != FFX_API_RETURN_OK)
        return E_FAIL;
    auto* st = (ScState*)ctx;
    st->bridge = true;
    st->gameEnabled = true;
    // no FFX context here: only the game's reference stays
    st->contextRef = nullptr;
    st->wrapper->Release();
    ScRelease(st);
    *out = sc;
    Log("injected swap chain %p for a game without FSR frame generation", (void*)sc);
    return S_OK;
}

extern "C" __declspec(dllexport) void XefgBridgeFrame(const XefgBridgeFrameDesc* f)
{
    Init();
    ScState* st;
    {
        std::lock_guard<std::mutex> lk(g_globalLock);
        st = g_currentSc;
        if (!st || !st->bridge)
            return;
        st->refs++;
    }
    uint32_t flags = (f->depthInverted ? FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED : 0) |
                     (f->depthInfinite ? FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE : 0) |
                     (f->jitteredMotionVectors ? FFX_FRAMEGENERATION_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION : 0) |
                     (f->displayResolutionMotionVectors ? FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS : 0);
    static FgCtx fg;
    if (!fg.prepares || fg.flags != flags) {
        // the swap chain was made before the first frame: the next start uses these
        uint32_t saved;
        if (!LoadFlags(&saved) || (saved & kXefgFfxFlags) != (flags & kXefgFfxFlags))
            SaveFlags(flags & kXefgFfxFlags);
        if (g_initFfxFlags != (flags & kXefgFfxFlags))
            Log("!! XeSS-FG was set up for flags 0x%x, the game uses 0x%x: wrong until the game starts again", g_initFfxFlags,
                flags & kXefgFfxFlags);
    }
    fg.flags = flags;
    fg.device = st->device;
    TagPrepare(&fg, 0, f->commandList, f->renderSize, f->jitter, f->motionVectorScale, f->frameTimeDelta, f->reset,
               f->cameraNear, f->cameraFar, f->cameraFovY, f->depth, f->motionVectors, nullptr, nullptr, nullptr, nullptr);
    ScRelease(st);
}

// ---------------------------------------------------------------------------------------------------------------
// Exports

// Command list and texture for a game that queries them to dispatch the interpolation itself, made on the first
// query (with the lock held): most games never ask, and FSR_FG=inject never does.
static void InterpObjects(ScState* st)
{
    if (st->interpMade)
        return;
    st->interpMade = true;
    if (SUCCEEDED(st->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&st->alloc))) &&
        SUCCEEDED(st->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, st->alloc, nullptr,
                                                IID_PPV_ARGS(&st->cmdList))))
        st->cmdList->Close();
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = st->width;
    rd.Height = st->height;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = st->format;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(st->device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                   IID_PPV_ARGS(&st->interpTex))))
        st->interpTex = nullptr;
    Log("interpolation command list %p and texture %p for the game's own dispatch", (void*)st->cmdList, (void*)st->interpTex);
}

static bool IsOurType(uint64_t t)
{
    return t == FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION ||
           t == FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12 ||
           t == FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_NEW_DX12 ||
           t == FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
}

extern "C" __declspec(dllexport) ffxReturnCode_t ffxCreateContext(ffxContext* context, ffxCreateContextDescHeader* desc,
                                                                  const ffxAllocationCallbacks*)
{
    Init();
    if (!context || !desc)
        return FFX_API_RETURN_ERROR_PARAMETER;
    if (!IsOurType(desc->type))
        return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
    if (!g_libsOk) {
        Log("create %llx: XeSS-FG/XeLL not available", (unsigned long long)desc->type);
        return FFX_API_RETURN_ERROR_RUNTIME_ERROR;
    }

    if (desc->type == FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION) {
        auto* d = (const ffxCreateContextDescFrameGeneration*)desc;
        auto* fg = new FgCtx();
        fg->flags = d->flags;
        fg->displayW = d->displaySize.width;
        fg->displayH = d->displaySize.height;
        fg->maxRenderW = d->maxRenderSize.width;
        fg->maxRenderH = d->maxRenderSize.height;
        fg->backBufferFormat = d->backBufferFormat;
        for (const ffxApiHeader* h = desc->pNext; h; h = h->pNext)
            if (h->type == FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12)
                fg->device = ((const ffxCreateBackendDX12Desc*)h)->device;
        {
            std::lock_guard<std::mutex> lk(g_globalLock);
            g_lastFgFlags = d->flags;
            g_haveFgFlags = true;
        }
        *context = fg;
        Log("fg context %p: flags 0x%x display %ux%u maxRender %ux%u", (void*)fg, fg->flags, fg->displayW, fg->displayH,
            fg->maxRenderW, fg->maxRenderH);
        uint32_t saved;
        if (!LoadFlags(&saved) || (saved & kXefgFfxFlags) != (d->flags & kXefgFfxFlags))
            SaveFlags(d->flags & kXefgFfxFlags);
        if (ScFromSwapChain(nullptr) && g_initFfxFlags != (d->flags & kXefgFfxFlags))
            Log("!! XeSS-FG was set up for flags 0x%x, the game uses 0x%x (depth direction, motion vectors): wrong until "
                "the game starts again, which then uses the game's",
                g_initFfxFlags, d->flags & kXefgFfxFlags);
        LogUpscalers(fg->device);
        return FFX_API_RETURN_OK;
    }
    ScKind kind = desc->type == FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12  ? ScKind::Wrap
                  : desc->type == FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_NEW_DX12 ? ScKind::New
                                                                                                     : ScKind::ForHwnd;
    return CreateSwapChainContext(context, kind, desc);
}

extern "C" __declspec(dllexport) ffxReturnCode_t ffxDestroyContext(ffxContext* context, const ffxAllocationCallbacks*)
{
    Init();
    if (!context || !*context)
        return FFX_API_RETURN_ERROR_PARAMETER;
    uint32_t magic = *(uint32_t*)*context;
    if (magic == MAGIC_FG) {
        auto* fg = (FgCtx*)*context;
        Log("destroy fg context %p (prepares %llu)", (void*)fg, (unsigned long long)fg->prepares);
        delete fg;
    } else if (magic == MAGIC_SC) {
        auto* st = (ScState*)*context;
        {
            std::lock_guard<std::mutex> lk(g_globalLock);
            if (g_currentSc == st)
                g_currentSc = nullptr;
        }
        FgSwapChain* w = st->contextRef;
        st->contextRef = nullptr;
        ULONG left = w ? w->Release() : 0;
        Log("destroy swap chain context %p (game still holds the swap chain: %s)", (void*)st, left ? "yes" : "no");
        ScRelease(st);
    } else {
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    *context = nullptr;
    return FFX_API_RETURN_OK;
}

extern "C" __declspec(dllexport) ffxReturnCode_t ffxConfigure(ffxContext* context, const ffxConfigureDescHeader* desc)
{
    Init();
    if (!desc)
        return FFX_API_RETURN_ERROR_PARAMETER;
    if (!context || !*context)
        return desc->type == FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG ? FFX_API_RETURN_OK
                                                                     : FFX_API_RETURN_ERROR_PARAMETER;
    uint32_t magic = *(uint32_t*)*context;
    if (magic == MAGIC_FG)
        return FgConfigure((FgCtx*)*context, desc);
    if (magic != MAGIC_SC)
        return FFX_API_RETURN_ERROR_PARAMETER;

    auto* st = (ScState*)*context;
    switch (desc->type) {
    case FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12: {
        auto* d = (const ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12*)desc;
        std::lock_guard<std::mutex> lk(st->lock);
        if (st->ui.resource)
            ((ID3D12Resource*)st->ui.resource)->Release();
        st->ui = d->uiResource;
        st->uiFlags = d->flags;
        if (st->ui.resource) {
            ((ID3D12Resource*)st->ui.resource)->AddRef();
            if (!st->uiComposition && p_xefgSwapChainSetUiCompositionState) {
                p_xefgSwapChainSetUiCompositionState(st->xefg, XEFG_SWAPCHAIN_UI_COMPOSITION_STATE_ENABLED);
                st->uiComposition = true;
            }
        }
        Log("UI resource %p flags 0x%x", st->ui.resource, d->flags);
        return FFX_API_RETURN_OK;
    }
    case FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_KEYVALUE_DX12:
    case FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG:
        return FFX_API_RETURN_OK;
    default:
        return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
    }
}

extern "C" __declspec(dllexport) ffxReturnCode_t ffxQuery(ffxContext* context, ffxQueryDescHeader* desc)
{
    Init();
    if (!desc)
        return FFX_API_RETURN_ERROR_PARAMETER;

    if (desc->type == FFX_API_QUERY_DESC_TYPE_GET_VERSIONS) {
        auto* d = (ffxQueryDescGetVersions*)desc;
        if (!IsOurType(d->createDescType) || !g_libsOk) {
            if (d->outputCount)
                *d->outputCount = 0;
            return FFX_API_RETURN_OK;
        }
        if (d->outputCount) {
            uint64_t cap = *d->outputCount;
            if (cap > 0) {
                if (d->versionIds)
                    d->versionIds[0] = kProviderVersionId;
                if (d->versionNames)
                    d->versionNames[0] = kProviderVersionName;
            }
            *d->outputCount = 1;
        }
        return FFX_API_RETURN_OK;
    }
    if (desc->type == FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION) {
        auto* d = (ffxQueryGetProviderVersion*)desc;
        d->versionId = kProviderVersionId;
        d->versionName = kProviderVersionName;
        return FFX_API_RETURN_OK;
    }
    if (desc->type == FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE) {
        auto* d = (ffxQueryDescFrameGenerationGetGPUMemoryUsage*)desc;
        if (d->gpuMemoryUsageFrameGeneration)
            *d->gpuMemoryUsageFrameGeneration = {};
        return FFX_API_RETURN_OK;
    }
    if (desc->type == FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE_V2) {
        auto* d = (ffxQueryDescFrameGenerationGetGPUMemoryUsageV2*)desc;
        if (d->gpuMemoryUsageFrameGeneration)
            *d->gpuMemoryUsageFrameGeneration = {};
        return FFX_API_RETURN_OK;
    }
    if (desc->type == FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_GPU_MEMORY_USAGE_DX12) {
        auto* d = (ffxQueryFrameGenerationSwapChainGetGPUMemoryUsageDX12*)desc;
        if (d->gpuMemoryUsageFrameGenerationSwapchain)
            *d->gpuMemoryUsageFrameGenerationSwapchain = {};
        return FFX_API_RETURN_OK;
    }
    if (desc->type == FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_GPU_MEMORY_USAGE_DX12_V2) {
        auto* d = (ffxQueryFrameGenerationSwapChainGetGPUMemoryUsageDX12V2*)desc;
        if (d->gpuMemoryUsageFrameGenerationSwapchain)
            *d->gpuMemoryUsageFrameGenerationSwapchain = {};
        return FFX_API_RETURN_OK;
    }

    if (!context || !*context || *(uint32_t*)*context != MAGIC_SC)
        return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
    auto* st = (ScState*)*context;
    if (desc->type == FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_INTERPOLATIONCOMMANDLIST_DX12) {
        auto* d = (ffxQueryDescFrameGenerationSwapChainInterpolationCommandListDX12*)desc;
        std::lock_guard<std::mutex> lk(st->lock);
        InterpObjects(st);
        if (d->pOutCommandList)
            *d->pOutCommandList = st->cmdList;
        return FFX_API_RETURN_OK;
    }
    if (desc->type == FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_INTERPOLATIONTEXTURE_DX12) {
        auto* d = (ffxQueryDescFrameGenerationSwapChainInterpolationTextureDX12*)desc;
        std::lock_guard<std::mutex> lk(st->lock);
        InterpObjects(st);
        if (d->pOutTexture) {
            *d->pOutTexture = {};
            d->pOutTexture->resource = st->interpTex;
            d->pOutTexture->description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
            d->pOutTexture->description.format = FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM;
            if (st->format == DXGI_FORMAT_R8G8B8A8_UNORM)
                d->pOutTexture->description.format = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
            else if (st->format == DXGI_FORMAT_B8G8R8A8_UNORM)
                d->pOutTexture->description.format = FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM;
            else if (st->format == DXGI_FORMAT_R16G16B16A16_FLOAT)
                d->pOutTexture->description.format = FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
            d->pOutTexture->description.width = st->width;
            d->pOutTexture->description.height = st->height;
            d->pOutTexture->description.depth = 1;
            d->pOutTexture->description.mipCount = 1;
            d->pOutTexture->description.usage = FFX_API_RESOURCE_USAGE_UAV;
            d->pOutTexture->state = FFX_API_RESOURCE_STATE_UNORDERED_ACCESS;
        }
        return FFX_API_RETURN_OK;
    }
    return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
}

extern "C" __declspec(dllexport) ffxReturnCode_t ffxDispatch(ffxContext* context, const ffxDispatchDescHeader* desc)
{
    Init();
    if (!desc || !context || !*context)
        return FFX_API_RETURN_ERROR_PARAMETER;
    uint32_t magic = *(uint32_t*)*context;
    if (magic == MAGIC_FG)
        return FgDispatch((FgCtx*)*context, desc);
    if (magic == MAGIC_SC && desc->type == FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WAIT_FOR_PRESENTS_DX12)
        return FFX_API_RETURN_OK;
    return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
    }
    return TRUE;
}

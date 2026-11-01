// ffx_xefg.cpp - amd_fidelityfx_framegeneration_dx12.dll that implements AMD's FFX frame generation API on top of
// Intel XeSS frame generation (libxess_fg.dll, proxy swap chain) and XeLL (libxell.dll).
//
// The game keeps its FSR 3.1+/4 integration; the FFX loader loads this DLL in place of AMD's frame generation
// provider. Frame generation swap chain contexts (WRAP / NEW / FOR_HWND) create an XeSS-FG proxy swap chain,
// wrapped in FgSwapChain for the XeLL present markers; FRAMEGENERATION contexts tag depth, motion vectors,
// HUD-less color and the camera constants for XeSS-FG. The game's own interpolation dispatch is a no-op:
// XeSS-FG interpolates inside the proxy swap chain.
//
// Environment:
//   FFX_XEFG_MULT=n   frames shown per rendered frame (2 = one interpolated frame; default: XESS_FG, else 2)
//   FFX_XEFG_LOG=path log file (default ffx_xefg.log next to the DLL); FFX_XEFG_LOG=0 turns it off
//   FFX_XEFG_XEFGLOG=0..3  XeSS-FG library log level forwarded to the log (default 2 = warnings)
//   FFX_XEFG_SLEEP=0  no xellSleep (XeLL stays enabled, as XeSS-FG requires)
//   FFX_XEFG_OWNWAIT=0     hand the game the proxy's frame latency waitable object (deadlocks CONTROL Resonant)
//   FFX_XEFG_EXCLUSIVE=1   let the game enter exclusive fullscreen (XeSS-FG then refuses to interpolate)
// The same KEY=VALUE lines can go into ffx_xefg.env next to the DLL (the environment wins). XESS_FG itself belongs in
// the launch options: the XMX shim reads it when it starts, before this DLL is loaded.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

#pragma warning(disable : 4996)
#include "inc/ffx/api/include/ffx_api.h"
#include "inc/ffx/api/include/ffx_api_types.h"
#include "inc/ffx/api/include/dx12/ffx_api_dx12.h"
#include "inc/ffx/framegeneration/include/ffx_framegeneration.h"
#include "inc/ffx/framegeneration/include/dx12/ffx_api_framegeneration_dx12.h"
#include "inc/xess/xess_fg/xefg_swapchain.h"
#include "inc/xess/xess_fg/xefg_swapchain_d3d12.h"
#include "inc/xess/xell/xell.h"
#include "inc/xess/xell/xell_d3d12.h"

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

static int EnvInt(const char* name, int def)
{
    char buf[32];
    if (!GetEnvironmentVariableA(name, buf, sizeof(buf)))
        return def;
    return atoi(buf);
}

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
static bool g_noExclusive = true;

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
        g_log = fopen(path, "a");
    }

    g_mult = EnvInt("FFX_XEFG_MULT", EnvInt("XESS_FG", 2));
    if (g_mult < 2)
        g_mult = 2;
    g_xefgLogLevel = EnvInt("FFX_XEFG_XEFGLOG", 2);
    g_sleep = EnvInt("FFX_XEFG_SLEEP", 1) != 0;
    g_ownWait = EnvInt("FFX_XEFG_OWNWAIT", 1) != 0;
    g_noExclusive = EnvInt("FFX_XEFG_EXCLUSIVE", 0) == 0;
    Log("=== ffx_xefg start pid %lu, multiplier %dx, sleep %d, own wait %d, exclusive fullscreen %s", GetCurrentProcessId(), g_mult,
        g_sleep, g_ownWait, g_noExclusive ? "blocked" : "allowed");

    HMODULE fg = LoadBeside(dir, "libxess_fg.dll");
    HMODULE xell = LoadBeside(dir, "libxell.dll");
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
    if (!(fovY > 0.0001f) || !(aspect > 0.0001f) || !(n > 0)) {
        m[0] = m[5] = m[10] = m[15] = 1;
        return;
    }
    if (infinite)
        f = INFINITY;
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
    uint32_t width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t maxInterpolations = 1;

    // Objects handed to games that drive the interpolation dispatch themselves.
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* cmdList = nullptr;
    ID3D12Resource* interpTex = nullptr;

    // Per-frame state.
    bool gameEnabled = false;
    bool xefgEnabled = false;
    bool havePrepare = false;     // PrepareV2 seen since the last present
    bool uiComposition = false;
    uint32_t frameId = 0;          // id of the frame being built
    uint32_t simStartedFor = UINT32_MAX;
    uint64_t presents = 0;
    struct FfxApiResource ui = {};
    uint32_t uiFlags = 0;
    // Frame latency waitable object of our own: released once per Present that returned, so the game's
    // wait follows our Present calls instead of XeSS-FG's presentation thread.
    HANDLE latencySem = nullptr;
    UINT maxLatency = 1;
    BOOL fakeFullscreen = FALSE;   // what the game asked for while exclusive fullscreen is blocked
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
static uint32_t g_lastFgFlags;   // creation flags of the last FG context, used for the XeSS-FG init flags
static bool g_haveFgFlags;

static void TearDown(ScState* st)
{
    Log("teardown sc %p (xefg %p, presents %llu)", (void*)st, (void*)st->xefg, (unsigned long long)st->presents);
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

static void SetXefgEnabled(ScState* st, bool want)
{
    if (want == st->xefgEnabled)
        return;
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

static void BeforePresent(ScState* st)
{
    std::lock_guard<std::mutex> lk(st->lock);
    if (!st->havePrepare)
        st->frameId++;  // no frame data this time (menus, loading): keep the ids moving, no interpolation
    uint32_t id = st->frameId;
    SetXefgEnabled(st, st->gameEnabled && st->havePrepare);

    if (st->havePrepare && st->ui.resource) {
        xefg_swapchain_d3d12_resource_data_t rd = {};
        rd.type = XEFG_SWAPCHAIN_RES_UI;
        rd.validity = XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT;
        rd.resourceSize = {st->ui.description.width, st->ui.description.height};
        rd.pResource = (ID3D12Resource*)st->ui.resource;
        rd.incomingState = ToD3D12State(st->ui.state);
        p_xefgSwapChainD3D12TagFrameResource(st->xefg, nullptr, id, &rd);
    }

    if (st->simStartedFor != id) {
        p_xellAddMarkerData(st->xell, id, XELL_SIMULATION_START);
        p_xellAddMarkerData(st->xell, id, XELL_SIMULATION_END);
        p_xellAddMarkerData(st->xell, id, XELL_RENDERSUBMIT_START);
        st->simStartedFor = id;
    }
    p_xellAddMarkerData(st->xell, id, XELL_RENDERSUBMIT_END);
    p_xellAddMarkerData(st->xell, id, XELL_PRESENT_START);
    p_xefgSwapChainSetPresentId(st->xefg, id);
}

static void AfterPresent(ScState* st, HRESULT hr)
{
    std::lock_guard<std::mutex> lk(st->lock);
    uint32_t id = st->frameId;
    p_xellAddMarkerData(st->xell, id, XELL_PRESENT_END);
    st->presents++;
    if (st->presents <= 10 || st->presents % 600 == 0) {
        xefg_swapchain_present_status_t ps = {};
        p_xefgSwapChainGetLastPresentStatus(st->xefg, &ps);
        Log("present #%llu frame %u hr 0x%08lx: presented %u fgResult %d fgEnabled %u", (unsigned long long)st->presents,
            id, (unsigned long)hr, ps.framesPresented, (int)ps.frameGenResult, ps.isFrameGenEnabled);
    }
    st->havePrepare = false;

    // Start the next frame: XeLL sleep and simulation markers use the id the next PrepareV2 is expected to carry.
    uint32_t next = id + 1;
    if (g_sleep)
        p_xellSleep(st->xell, next);
    p_xellAddMarkerData(st->xell, next, XELL_SIMULATION_START);
    st->simStartedFor = next;
}

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
        BeforePresent(st);
        HRESULT hr = sc->Present(sync, flags);
        if (st->latencySem && SUCCEEDED(hr))
            ReleaseSemaphore(st->latencySem, 1, nullptr);
        AfterPresent(st, hr);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT i, REFIID riid, void** p) override { return sc->GetBuffer(i, riid, p); }
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
        Recheck();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* f, IDXGIOutput** o) override
    {
        if (g_noExclusive) {
            HRESULT hr = sc->GetFullscreenState(nullptr, o);
            if (f)
                *f = st->fakeFullscreen;
            return hr;
        }
        return sc->GetFullscreenState(f, o);
    }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* d) override
    {
        HRESULT hr = sc->GetDesc(d);
        if (SUCCEEDED(hr) && d && g_noExclusive)
            d->Windowed = !st->fakeFullscreen;
        return hr;
    }
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl) override
    {
        HRESULT hr = sc->ResizeBuffers(n, w, h, f, fl);
        Log("ResizeBuffers %u %ux%u fmt %d flags 0x%x -> 0x%08lx", n, w, h, (int)f, fl, (unsigned long)hr);
        Recheck();
        return hr;
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
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* m) override { return sc->ResizeTarget(m); }
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** o) override { return sc->GetContainingOutput(o); }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* s) override { return sc->GetFrameStatistics(s); }
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* c) override { return sc->GetLastPresentCount(c); }

    // IDXGISwapChain1
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* d) override { return sc->GetDesc1(d); }
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* d) override { return sc->GetFullscreenDesc(d); }
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND* h) override { return sc->GetHwnd(h); }
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID riid, void** p) override { return sc->GetCoreWindow(riid, p); }
    HRESULT STDMETHODCALLTYPE Present1(UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp) override
    {
        if (flags & DXGI_PRESENT_TEST)
            return sc->Present1(sync, flags, pp);
        BeforePresent(st);
        HRESULT hr = sc->Present1(sync, flags, pp);
        if (st->latencySem && SUCCEEDED(hr))
            ReleaseSemaphore(st->latencySem, 1, nullptr);
        AfterPresent(st, hr);
        return hr;
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
            // the caller closes the handle it gets, as with DXGI
            DuplicateHandle(GetCurrentProcess(), st->latencySem, GetCurrentProcess(), &h, 0, FALSE,
                            DUPLICATE_SAME_ACCESS);
        } else {
            h = sc->GetFrameLatencyWaitableObject();
        }
        Log("GetFrameLatencyWaitableObject -> %p (own %d)", (void*)h, g_ownWait);
        return h;
    }
    HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F* m) override { return sc->SetMatrixTransform(m); }
    HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F* m) override { return sc->GetMatrixTransform(m); }

    // IDXGISwapChain3
    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override { return sc->GetCurrentBackBufferIndex(); }
    HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE c, UINT* s) override { return sc->CheckColorSpaceSupport(c, s); }
    HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE c) override { return sc->SetColorSpace1(c); }
    HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl, const UINT* mask,
                                             IUnknown* const* queues) override
    {
        HRESULT hr = sc->ResizeBuffers1(n, w, h, f, fl, mask, queues);
        Log("ResizeBuffers1 %u %ux%u fmt %d -> 0x%08lx", n, w, h, (int)f, (unsigned long)hr);
        Recheck();
        return hr;
    }

    // IDXGISwapChain4
    HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE t, UINT n, void* d) override { return sc->SetHDRMetaData(t, n, d); }
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

static uint32_t XefgInitFlags()
{
    uint32_t f = XEFG_SWAPCHAIN_INIT_FLAG_UITEXTURE_NOT_PREMUL_ALPHA;
    uint32_t ffx;
    {
        std::lock_guard<std::mutex> lk(g_globalLock);
        ffx = g_haveFgFlags ? g_lastFgFlags : 0;
    }
    if (ffx & FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED)
        f |= XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH;
    if (ffx & FFX_FRAMEGENERATION_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION)
        f |= XEFG_SWAPCHAIN_INIT_FLAG_JITTERED_MV;
    if (ffx & FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS)
        f |= XEFG_SWAPCHAIN_INIT_FLAG_HIGH_RES_MV;
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
    xr = p_xellSetSleepMode(st->xell, &sp);
    if (xr != XELL_RESULT_SUCCESS)
        Log("xellSetSleepMode -> %d", (int)xr);
    r = p_xefgSwapChainSetLatencyReduction(st->xefg, st->xell);
    if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS)
        return fail("xefgSwapChainSetLatencyReduction", r);

    ip.initFlags = XefgInitFlags();
    ip.maxInterpolatedFrames = XEFG_SWAPCHAIN_USE_MAX_SUPPORTED_INTERPOLATED_FRAMES;
    ip.uiMode = XEFG_SWAPCHAIN_UI_MODE_AUTO;

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
    if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS)
        return fail("xefgSwapChainD3D12InitFromSwapChain(Desc)", r);
    r = p_xefgSwapChainD3D12GetSwapChainPtr(st->xefg, IID_PPV_ARGS(&st->proxy));
    if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS || !st->proxy)
        return fail("xefgSwapChainD3D12GetSwapChainPtr", r);

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

    // Command list and texture for games that query them and dispatch the interpolation themselves.
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

    st->wrapper = new FgSwapChain(st);
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

static void TagPrepare(FgCtx* fg, uint64_t frameID, void* cmdList, FfxApiDimensions2D renderSize,
                       FfxApiFloatCoords2D jitter, FfxApiFloatCoords2D mvScale, float frameTimeDelta, bool reset,
                       float cameraNear, float cameraFar, float fovY, const struct FfxApiResource& depth,
                       const struct FfxApiResource& mv, const float* camPos, const float* camUp, const float* camRight,
                       const float* camFwd)
{
    ScState* st = ScFromSwapChain(nullptr);
    if (!st)
        return;
    std::lock_guard<std::mutex> lk(st->lock);
    uint32_t id = (uint32_t)frameID;

    if (st->simStartedFor != id) {
        p_xellAddMarkerData(st->xell, id, XELL_SIMULATION_START);
        st->simStartedFor = id;
    }
    p_xellAddMarkerData(st->xell, id, XELL_SIMULATION_END);
    p_xellAddMarkerData(st->xell, id, XELL_RENDERSUBMIT_START);

    xefg_swapchain_d3d12_resource_data_t rd = {};
    rd.validity = XEFG_SWAPCHAIN_RV_ONLY_NOW;
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
    BuildView(camPos ? camPos : kPos, camRight ? camRight : kRight, camUp ? camUp : kUp, camFwd ? camFwd : kFwd,
              c.viewMatrix);
    float aspect = renderSize.height ? (float)renderSize.width / (float)renderSize.height : 1.0f;
    BuildProjection(fovY, aspect, cameraNear, cameraFar, (fg->flags & FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED) != 0,
                    (fg->flags & FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE) != 0, c.projectionMatrix);
    c.jitterOffsetX = jitter.x;
    c.jitterOffsetY = jitter.y;
    c.motionVectorScaleX = mvScale.x;
    c.motionVectorScaleY = mvScale.y;
    c.resetHistory = reset ? 1 : 0;
    c.frameRenderTime = frameTimeDelta;
    xefg_swapchain_result_t r3 = p_xefgSwapChainTagFrameConstants(st->xefg, id, &c);

    st->frameId = id;
    st->havePrepare = true;
    fg->prepares++;
    if (fg->prepares <= 5 || fg->prepares % 600 == 0 || r1 || r2 || r3)
        Log("prepare #%llu frame %u render %ux%u jitter %.3f,%.3f mvScale %.1f,%.1f dt %.2f reset %d fov %.3f "
            "near %.3f far %.1f -> tag depth %d mv %d const %d",
            (unsigned long long)fg->prepares, id, renderSize.width, renderSize.height, jitter.x, jitter.y, mvScale.x,
            mvScale.y, frameTimeDelta, reset, fovY, cameraNear, cameraFar, (int)r1, (int)r2, (int)r3);
}

static ffxReturnCode_t FgConfigure(FgCtx* fg, const ffxApiHeader* desc)
{
    for (const ffxApiHeader* h = desc; h; h = h->pNext) {
        switch (h->type) {
        case FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION: {
            auto* d = (const ffxConfigureDescFrameGeneration*)h;
            ScState* st = ScFromSwapChain(d->swapChain);
            if (!st)
                break;
            std::lock_guard<std::mutex> lk(st->lock);
            if (st->gameEnabled != d->frameGenerationEnabled)
                Log("game frame generation %s (frame %llu, callbacks present %p fg %p)",
                    d->frameGenerationEnabled ? "on" : "off", (unsigned long long)d->frameID,
                    (void*)d->presentCallback, (void*)d->frameGenerationCallback);
            st->gameEnabled = d->frameGenerationEnabled;
            // only while frame generation is on: RE Requiem configures with frame 0 every frame before that, and
            // XeSS-FG rejects a second tag for the same frame
            if (d->HUDLessColor.resource && d->frameGenerationEnabled) {
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
// Exports

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
        Log("destroy swap chain context %p (game still holds the swap chain: %s)", (void*)st,
            st->wrapper ? "yes" : "no");
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
            if (cap > 0 || d->versionIds || d->versionNames) {
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
        if (d->pOutCommandList)
            *d->pOutCommandList = st->cmdList;
        return FFX_API_RETURN_OK;
    }
    if (desc->type == FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_INTERPOLATIONTEXTURE_DX12) {
        auto* d = (ffxQueryDescFrameGenerationSwapChainInterpolationTextureDX12*)desc;
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

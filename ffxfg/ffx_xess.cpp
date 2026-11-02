// ffx_xess.cpp - amd_fidelityfx_upscaler_dx12.dll that implements AMD's FFX upscaler API on top of Intel XeSS
// super resolution (libxess.dll), so that a game with FSR 3.1+ upscales with XeSS (on XMX through the shim).
//
// The game keeps its FSR integration; the FFX loader loads this DLL in place of AMD's upscaler provider. An UPSCALE
// context is an XeSS context; each upscale dispatch is an xessD3D12Execute on the game's command list with the same
// inputs. The queries a game makes to set up FSR (render size for a quality mode, the jitter sequence) are answered
// the way AMD's upscaler answers them, so the game renders exactly as it would for FSR.
//
// Environment:
//   FFX_XESS_LOG=path    log file (default ffx_xess.log next to the DLL); FFX_XESS_LOG=0 turns it off
//   FFX_XESS_XESSLOG=0..3  XeSS log level forwarded to the log (default 2 = warnings)
//   FFX_XESS_QUALITY=100..106  the XeSS quality setting (default: the one whose input range holds the game's maximum
//                        render size)
//   FFX_XESS_JITTER=<f>  jitter times this (debug: -1 negates it)
//   FFX_XESS_AUTOEXPOSURE=0  no XeSS auto exposure for a game that asks for it (debug)
// The same KEY=VALUE lines can go into ffx_xess.env next to the DLL (the environment wins).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

#pragma warning(disable : 4996)
#include "inc/ffx/api/include/ffx_api.h"
#include "inc/ffx/api/include/ffx_api_types.h"
#include "inc/ffx/api/include/dx12/ffx_api_dx12.h"
#include "inc/ffx/upscalers/include/ffx_upscale.h"
#define XESS_TYPES_ONLY
#include "inc/xess/xess/xess.h"
#undef XESS_TYPES_ONLY
#include "inc/xess/xess/xess_d3d12.h"
#include "xefg_bridge.h"

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
// XeSS entry points, loaded at run time

typedef xess_result_t (*PFN_xessGetVersion)(xess_version_t*);
typedef xess_result_t (*PFN_xessD3D12CreateContext)(ID3D12Device*, xess_context_handle_t*);
typedef xess_result_t (*PFN_xessD3D12Init)(xess_context_handle_t, const xess_d3d12_init_params_t*);
typedef xess_result_t (*PFN_xessD3D12Execute)(xess_context_handle_t, ID3D12GraphicsCommandList*,
                                              const xess_d3d12_execute_params_t*);
typedef xess_result_t (*PFN_xessGetOptimalInputResolution)(xess_context_handle_t, const xess_2d_t*,
                                                           xess_quality_settings_t, xess_2d_t*, xess_2d_t*, xess_2d_t*);
typedef xess_result_t (*PFN_xessSetVelocityScale)(xess_context_handle_t, float, float);
typedef xess_result_t (*PFN_xessDestroyContext)(xess_context_handle_t);
typedef xess_result_t (*PFN_xessSetLoggingCallback)(xess_context_handle_t, xess_logging_level_t, xess_app_log_callback_t);

static PFN_xessGetVersion p_xessGetVersion;
static PFN_xessD3D12CreateContext p_xessD3D12CreateContext;
static PFN_xessD3D12Init p_xessD3D12Init;
static PFN_xessD3D12Execute p_xessD3D12Execute;
static PFN_xessGetOptimalInputResolution p_xessGetOptimalInputResolution;
static PFN_xessSetVelocityScale p_xessSetVelocityScale;
static PFN_xessDestroyContext p_xessDestroyContext;
static PFN_xessSetLoggingCallback p_xessSetLoggingCallback;

static bool g_libOk;
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static int g_xessLogLevel = 2;
static int g_quality = 0;
static float g_jitterScale = 1.0f;
static bool g_autoExposure = true;

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
        // ffx_xess.env beside the DLL: KEY=VALUE lines, set into the environment unless already set
        char path[MAX_PATH];
        snprintf(path, MAX_PATH, "%sffx_xess.env", dir);
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
    if (GetEnvironmentVariableA("FFX_XESS_LOG", env, MAX_PATH)) {
        if (strcmp(env, "0") != 0)
            g_log = fopen(env, "a");
    } else {
        char path[MAX_PATH];
        snprintf(path, MAX_PATH, "%sffx_xess.log", dir);
        g_log = fopen(path, "a");
    }
    g_xessLogLevel = EnvInt("FFX_XESS_XESSLOG", 2);
    g_quality = EnvInt("FFX_XESS_QUALITY", 0);
    if (GetEnvironmentVariableA("FFX_XESS_JITTER", env, MAX_PATH))
        g_jitterScale = (float)atof(env);
    g_autoExposure = EnvInt("FFX_XESS_AUTOEXPOSURE", 1) != 0;
    Log("=== ffx_xess start pid %lu", GetCurrentProcessId());
    if (g_quality || g_jitterScale != 1.0f || !g_autoExposure)
        Log("debug: quality %d, jitter x%g, auto exposure %d", g_quality, g_jitterScale, g_autoExposure);

    char path[MAX_PATH];
    snprintf(path, MAX_PATH, "%slibxess.dll", dir);
    HMODULE m = LoadLibraryA(path);
    if (!m)
        m = LoadLibraryA("libxess.dll");
    Log("load libxess.dll -> %p", (void*)m);
    bool ok = m != nullptr;
#define LOAD(name)                                                       \
    p_##name = m ? (PFN_##name)GetProcAddress(m, #name) : nullptr;      \
    if (!p_##name) {                                                     \
        Log("missing %s", #name);                                        \
        ok = false;                                                      \
    }
    LOAD(xessGetVersion)
    LOAD(xessD3D12CreateContext)
    LOAD(xessD3D12Init)
    LOAD(xessD3D12Execute)
    LOAD(xessGetOptimalInputResolution)
    LOAD(xessSetVelocityScale)
    LOAD(xessDestroyContext)
    LOAD(xessSetLoggingCallback)
    g_libOk = ok;
    if (ok) {
        xess_version_t v = {};
        p_xessGetVersion(&v);
        Log("XeSS %u.%u.%u", v.major, v.minor, v.patch);
    }
    return TRUE;
}

static void Init() { InitOnceExecuteOnce(&g_once, InitOnce, nullptr, nullptr); }

static void XessLogCallback(const char* message, xess_logging_level_t level) { Log("xess[%d] %s", (int)level, message); }

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

// ---------------------------------------------------------------------------------------------------------------
// What AMD's upscaler answers for the setup queries (FSR 3.1): per-dimension ratios, render size, jitter sequence

static float QualityRatio(uint32_t mode)
{
    switch (mode) {
    case FFX_UPSCALE_QUALITY_MODE_NATIVEAA: return 1.0f;
    case FFX_UPSCALE_QUALITY_MODE_QUALITY: return 1.5f;
    case FFX_UPSCALE_QUALITY_MODE_BALANCED: return 1.7f;
    case FFX_UPSCALE_QUALITY_MODE_PERFORMANCE: return 2.0f;
    case FFX_UPSCALE_QUALITY_MODE_ULTRA_PERFORMANCE: return 3.0f;
    default: return 0.0f;
    }
}

static int JitterPhaseCount(uint32_t renderWidth, uint32_t displayWidth)
{
    const float basePhaseCount = 8.0f;
    float ratio = renderWidth ? (float)displayWidth / (float)renderWidth : 1.0f;
    return (int)(basePhaseCount * ratio * ratio);
}

static float Halton(int index, int base)
{
    float f = 1.0f, result = 0.0f;
    for (int i = index; i > 0;) {
        f /= (float)base;
        result += f * (float)(i % base);
        i = (int)floorf((float)i / (float)base);
    }
    return result;
}

// ---------------------------------------------------------------------------------------------------------------
// Upscale context

enum : uint32_t { MAGIC_UP = 0x53455846 };  // "FXES"

// reported as AMD's FSR upscalers (id 0xF5A5CA1E in the high half, FFX_SDK_MAKE_VERSION in the low): Cyberpunk 2077
// offers its FFX upscaling only for an FSR 4 provider (FSR 2 is built into the game), other games ask for FSR 3.1
static const uint64_t kProviderVersionIds[] = {0xF5A5CA1Eull << 32 | (4u << 22 | 0u << 12 | 2u),
                                               0xF5A5CA1Eull << 32 | (3u << 22 | 1u << 12 | 5u)};
static const char* kProviderVersionNames[] = {"4.0.2", "3.1.5"};
static const uint64_t kProviderVersionId = kProviderVersionIds[0];
static const char* kProviderVersionName = kProviderVersionNames[0];

struct UpCtx {
    uint32_t magic = MAGIC_UP;
    std::mutex lock;
    ID3D12Device* device = nullptr;
    uint32_t flags = 0;
    uint32_t maxRenderW = 0, maxRenderH = 0, maxUpscaleW = 0, maxUpscaleH = 0;
    xess_context_handle_t xess = nullptr;
    // what the XeSS context was initialised for; a change sets it up again
    bool inited = false;
    uint32_t initFlags = 0, outW = 0, outH = 0;
    xess_quality_settings_t quality = XESS_QUALITY_SETTING_AA;
    float velScaleX = 0, velScaleY = 0;
    uint64_t dispatches = 0;
};

// The lowest XeSS quality setting whose optimal input size reaches the game's maximum render size: the setting tunes
// XeSS for its ratio, and XeSS takes any render size inside its range (dynamic resolution). FSR Quality (1.5x) gets
// XeSS Ultra Quality (1.5x).
static xess_quality_settings_t PickQuality(UpCtx* c, uint32_t outW, uint32_t outH)
{
    if (g_quality >= XESS_QUALITY_SETTING_ULTRA_PERFORMANCE && g_quality <= XESS_QUALITY_SETTING_AA)
        return (xess_quality_settings_t)g_quality;
    static const xess_quality_settings_t order[] = {
        XESS_QUALITY_SETTING_ULTRA_PERFORMANCE, XESS_QUALITY_SETTING_PERFORMANCE,  XESS_QUALITY_SETTING_BALANCED,
        XESS_QUALITY_SETTING_QUALITY,           XESS_QUALITY_SETTING_ULTRA_QUALITY, XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS,
        XESS_QUALITY_SETTING_AA};
    uint32_t needW = std::min(c->maxRenderW, outW), needH = std::min(c->maxRenderH, outH);
    xess_2d_t out = {outW, outH};
    for (xess_quality_settings_t q : order) {
        xess_2d_t opt = {}, mn = {}, mx = {};
        if (p_xessGetOptimalInputResolution(c->xess, &out, q, &opt, &mn, &mx) == XESS_RESULT_SUCCESS && opt.x + 1 >= needW &&
            opt.y + 1 >= needH && mx.x >= needW && mx.y >= needH)
            return q;
    }
    return XESS_QUALITY_SETTING_AA;
}

static bool SetUp(UpCtx* c, uint32_t outW, uint32_t outH, uint32_t xflags)
{
    if (c->inited && c->outW == outW && c->outH == outH && c->initFlags == xflags)
        return true;
    xess_d3d12_init_params_t ip = {};
    ip.outputResolution = {outW, outH};
    ip.qualitySetting = PickQuality(c, outW, outH);
    ip.initFlags = xflags;
    xess_result_t r = p_xessD3D12Init(c->xess, &ip);
    Log("xessD3D12Init %ux%u quality %d flags 0x%x (game max render %ux%u, FFX flags 0x%x) -> %d", outW, outH,
        (int)ip.qualitySetting, xflags, c->maxRenderW, c->maxRenderH, c->flags, (int)r);
    c->inited = r == XESS_RESULT_SUCCESS;
    c->outW = outW;
    c->outH = outH;
    c->initFlags = xflags;
    c->quality = ip.qualitySetting;
    c->velScaleX = c->velScaleY = 0;
    return c->inited;
}

// XeSS reads its inputs in NON_PIXEL_SHADER_RESOURCE and writes the output in UNORDERED_ACCESS
struct Barriers {
    D3D12_RESOURCE_BARRIER b[8];
    UINT n = 0;
    void Add(const FfxApiResource& r, D3D12_RESOURCE_STATES want)
    {
        if (!r.resource || n >= 8)
            return;
        D3D12_RESOURCE_STATES s = ToD3D12State(r.state);
        if ((s & want) == want && (want != D3D12_RESOURCE_STATE_UNORDERED_ACCESS || s == want))
            return;
        D3D12_RESOURCE_BARRIER& x = b[n++];
        x = {};
        x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        x.Transition.pResource = (ID3D12Resource*)r.resource;
        x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        x.Transition.StateBefore = s;
        x.Transition.StateAfter = want;
    }
    void Run(ID3D12GraphicsCommandList* cl) { if (n) cl->ResourceBarrier(n, b); }
    void Back(ID3D12GraphicsCommandList* cl)
    {
        for (UINT i = 0; i < n; i++)
            std::swap(b[i].Transition.StateBefore, b[i].Transition.StateAfter);
        Run(cl);
    }
};

static ffxReturnCode_t Upscale(UpCtx* c, const ffxDispatchDescUpscale* d)
{
    std::lock_guard<std::mutex> lk(c->lock);
    auto* cl = (ID3D12GraphicsCommandList*)d->commandList;
    if (!cl || !d->color.resource || !d->motionVectors.resource || !d->output.resource)
        return FFX_API_RETURN_ERROR_PARAMETER;

    uint32_t outW = d->upscaleSize.width ? d->upscaleSize.width : c->maxUpscaleW;
    uint32_t outH = d->upscaleSize.height ? d->upscaleSize.height : c->maxUpscaleH;
    uint32_t xf = 0;
    if (c->flags & FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS)
        xf |= XESS_INIT_FLAG_HIGH_RES_MV;
    if (c->flags & FFX_UPSCALE_ENABLE_DEPTH_INVERTED)
        xf |= XESS_INIT_FLAG_INVERTED_DEPTH;
    if (c->flags & FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION)
        xf |= XESS_INIT_FLAG_JITTERED_MV;
    if (!(c->flags & FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE))
        xf |= XESS_INIT_FLAG_LDR_INPUT_COLOR;
    if ((c->flags & FFX_UPSCALE_ENABLE_AUTO_EXPOSURE) && g_autoExposure)
        xf |= XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE;
    else if (d->exposure.resource)
        xf |= XESS_INIT_FLAG_EXPOSURE_SCALE_TEXTURE;
    if (!SetUp(c, outW, outH, xf))
        return FFX_API_RETURN_ERROR_RUNTIME_ERROR;

    // FFX: pixel motion = motion vector * motionVectorScale; XeSS multiplies its velocity input the same way
    if (d->motionVectorScale.x != c->velScaleX || d->motionVectorScale.y != c->velScaleY) {
        p_xessSetVelocityScale(c->xess, d->motionVectorScale.x, d->motionVectorScale.y);
        c->velScaleX = d->motionVectorScale.x;
        c->velScaleY = d->motionVectorScale.y;
    }

    // FSR_FG=inject: this frame's depth and motion vectors for XeSS-FG, in the states the game gave them
    static PFN_XefgBridgeFrame bridge;
    if (!bridge) {
        if (HMODULE m = GetModuleHandleA(XEFG_BRIDGE_DLL))
            bridge = (PFN_XefgBridgeFrame)GetProcAddress(m, "XefgBridgeFrame");
    }
    if (bridge) {
        XefgBridgeFrameDesc f = {};
        f.commandList = cl;
        f.renderSize = d->renderSize;
        f.jitter = {d->jitterOffset.x * g_jitterScale, d->jitterOffset.y * g_jitterScale};
        f.motionVectorScale = d->motionVectorScale;
        f.frameTimeDelta = d->frameTimeDelta;
        f.reset = d->reset;
        f.cameraNear = d->cameraNear;
        f.cameraFar = d->cameraFar;
        f.cameraFovY = d->cameraFovAngleVertical;
        f.depth = d->depth;
        f.motionVectors = d->motionVectors;
        f.depthInverted = (c->flags & FFX_UPSCALE_ENABLE_DEPTH_INVERTED) != 0;
        f.depthInfinite = (c->flags & FFX_UPSCALE_ENABLE_DEPTH_INFINITE) != 0;
        f.jitteredMotionVectors = (c->flags & FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION) != 0;
        f.displayResolutionMotionVectors = (c->flags & FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS) != 0;
        bridge(&f);
    }

    Barriers br;
    br.Add(d->color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    br.Add(d->motionVectors, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (!(xf & XESS_INIT_FLAG_HIGH_RES_MV))
        br.Add(d->depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (xf & XESS_INIT_FLAG_EXPOSURE_SCALE_TEXTURE)
        br.Add(d->exposure, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    br.Add(d->output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    br.Run(cl);

    xess_d3d12_execute_params_t e = {};
    e.pColorTexture = (ID3D12Resource*)d->color.resource;
    e.pVelocityTexture = (ID3D12Resource*)d->motionVectors.resource;
    e.pDepthTexture = (xf & XESS_INIT_FLAG_HIGH_RES_MV) ? nullptr : (ID3D12Resource*)d->depth.resource;
    e.pExposureScaleTexture =
        (xf & XESS_INIT_FLAG_EXPOSURE_SCALE_TEXTURE) ? (ID3D12Resource*)d->exposure.resource : nullptr;
    e.pOutputTexture = (ID3D12Resource*)d->output.resource;
    e.jitterOffsetX = d->jitterOffset.x * g_jitterScale;
    e.jitterOffsetY = d->jitterOffset.y * g_jitterScale;
    e.exposureScale = 1.0f;
    e.resetHistory = d->reset ? 1 : 0;
    e.inputWidth = d->renderSize.width;
    e.inputHeight = d->renderSize.height;
    xess_result_t r = p_xessD3D12Execute(c->xess, cl, &e);

    br.Back(cl);
    c->dispatches++;
    if (c->dispatches <= 3 || c->dispatches % 600 == 0 || r != XESS_RESULT_SUCCESS)
        Log("upscale #%llu render %ux%u -> %ux%u jitter %.3f,%.3f mvScale %.1f,%.1f reset %d exposure %p "
            "preExposure %.3f sharpen %d -> %d",
            (unsigned long long)c->dispatches, d->renderSize.width, d->renderSize.height, outW, outH,
            d->jitterOffset.x, d->jitterOffset.y, d->motionVectorScale.x, d->motionVectorScale.y, (int)d->reset,
            d->exposure.resource, d->preExposure, (int)d->enableSharpening, (int)r);
    return r == XESS_RESULT_SUCCESS ? FFX_API_RETURN_OK : FFX_API_RETURN_ERROR_RUNTIME_ERROR;
}

// ---------------------------------------------------------------------------------------------------------------
// FFX API

extern "C" __declspec(dllexport) ffxReturnCode_t ffxCreateContext(ffxContext* context, ffxCreateContextDescHeader* desc,
                                                                  const ffxAllocationCallbacks*)
{
    Init();
    if (!context || !desc)
        return FFX_API_RETURN_ERROR_PARAMETER;
    if (desc->type != FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE)
        return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
    if (!g_libOk) {
        Log("create: XeSS not available");
        return FFX_API_RETURN_ERROR_RUNTIME_ERROR;
    }
    auto* d = (const ffxCreateContextDescUpscale*)desc;
    ID3D12Device* dev = nullptr;
    for (const ffxApiHeader* h = desc->pNext; h; h = h->pNext)
        if (h->type == FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12)
            dev = ((const ffxCreateBackendDX12Desc*)h)->device;
    if (!dev)
        return FFX_API_RETURN_ERROR_PARAMETER;
    auto* c = new UpCtx();
    c->device = dev;
    c->flags = d->flags;
    c->maxRenderW = d->maxRenderSize.width;
    c->maxRenderH = d->maxRenderSize.height;
    c->maxUpscaleW = d->maxUpscaleSize.width;
    c->maxUpscaleH = d->maxUpscaleSize.height;
    xess_result_t r = p_xessD3D12CreateContext(dev, &c->xess);
    Log("upscale context %p: flags 0x%x maxRender %ux%u maxUpscale %ux%u, xessD3D12CreateContext -> %d", (void*)c,
        c->flags, c->maxRenderW, c->maxRenderH, c->maxUpscaleW, c->maxUpscaleH, (int)r);
    if (r != XESS_RESULT_SUCCESS) {
        delete c;
        return FFX_API_RETURN_ERROR_RUNTIME_ERROR;
    }
    p_xessSetLoggingCallback(c->xess, (xess_logging_level_t)g_xessLogLevel, XessLogCallback);
    *context = c;
    return FFX_API_RETURN_OK;
}

extern "C" __declspec(dllexport) ffxReturnCode_t ffxDestroyContext(ffxContext* context, const ffxAllocationCallbacks*)
{
    Init();
    if (!context || !*context || *(uint32_t*)*context != MAGIC_UP)
        return FFX_API_RETURN_ERROR_PARAMETER;
    auto* c = (UpCtx*)*context;
    Log("destroy upscale context %p (dispatches %llu)", (void*)c, (unsigned long long)c->dispatches);
    if (c->xess)
        p_xessDestroyContext(c->xess);
    delete c;
    *context = nullptr;
    return FFX_API_RETURN_OK;
}

extern "C" __declspec(dllexport) ffxReturnCode_t ffxConfigure(ffxContext* context, const ffxConfigureDescHeader* desc)
{
    Init();
    if (!desc)
        return FFX_API_RETURN_ERROR_PARAMETER;
    // the key-value tuning of AMD's upscaler (velocity factor, reactiveness, ...) has no XeSS counterpart
    if (desc->type == FFX_API_CONFIGURE_DESC_TYPE_UPSCALE_KEYVALUE || desc->type == FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG)
        return FFX_API_RETURN_OK;
    return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
}

extern "C" __declspec(dllexport) ffxReturnCode_t ffxQuery(ffxContext* context, ffxQueryDescHeader* desc)
{
    Init();
    if (!desc)
        return FFX_API_RETURN_ERROR_PARAMETER;
    switch (desc->type) {
    case FFX_API_QUERY_DESC_TYPE_GET_VERSIONS: {
        auto* d = (ffxQueryDescGetVersions*)desc;
        bool ours = d->createDescType == FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE && g_libOk;
        if (d->outputCount) {
            uint64_t n = ours ? 2 : 0, cap = *d->outputCount;
            for (uint64_t i = 0; i < n && i < cap; ++i) {
                if (d->versionIds)
                    d->versionIds[i] = kProviderVersionIds[i];
                if (d->versionNames)
                    d->versionNames[i] = kProviderVersionNames[i];
            }
            Log("versions asked (room for %llu): %llu", (unsigned long long)cap, (unsigned long long)n);
            *d->outputCount = n;
        }
        return FFX_API_RETURN_OK;
    }
    case FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION: {
        auto* d = (ffxQueryGetProviderVersion*)desc;
        d->versionId = kProviderVersionId;
        d->versionName = kProviderVersionName;
        return FFX_API_RETURN_OK;
    }
    case FFX_API_QUERY_DESC_TYPE_UPSCALE_GETUPSCALERATIOFROMQUALITYMODE: {
        auto* d = (ffxQueryDescUpscaleGetUpscaleRatioFromQualityMode*)desc;
        float r = QualityRatio(d->qualityMode);
        if (!r)
            return FFX_API_RETURN_ERROR_PARAMETER;
        if (d->pOutUpscaleRatio)
            *d->pOutUpscaleRatio = r;
        return FFX_API_RETURN_OK;
    }
    case FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE: {
        auto* d = (ffxQueryDescUpscaleGetRenderResolutionFromQualityMode*)desc;
        float r = QualityRatio(d->qualityMode);
        if (!r)
            return FFX_API_RETURN_ERROR_PARAMETER;
        if (d->pOutRenderWidth)
            *d->pOutRenderWidth = (uint32_t)((float)d->displayWidth / r);
        if (d->pOutRenderHeight)
            *d->pOutRenderHeight = (uint32_t)((float)d->displayHeight / r);
        return FFX_API_RETURN_OK;
    }
    case FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT: {
        auto* d = (ffxQueryDescUpscaleGetJitterPhaseCount*)desc;
        if (d->pOutPhaseCount)
            *d->pOutPhaseCount = JitterPhaseCount(d->renderWidth, d->displayWidth);
        return FFX_API_RETURN_OK;
    }
    case FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET: {
        auto* d = (ffxQueryDescUpscaleGetJitterOffset*)desc;
        if (d->phaseCount <= 0)
            return FFX_API_RETURN_ERROR_PARAMETER;
        int i = ((d->index % d->phaseCount) + d->phaseCount) % d->phaseCount;
        if (d->pOutX)
            *d->pOutX = Halton(i + 1, 2) - 0.5f;
        if (d->pOutY)
            *d->pOutY = Halton(i + 1, 3) - 0.5f;
        return FFX_API_RETURN_OK;
    }
    case FFX_API_QUERY_DESC_TYPE_UPSCALE_GPU_MEMORY_USAGE: {
        auto* d = (ffxQueryDescUpscaleGetGPUMemoryUsage*)desc;
        if (d->gpuMemoryUsageUpscaler)
            *d->gpuMemoryUsageUpscaler = {};
        return FFX_API_RETURN_OK;
    }
    case FFX_API_QUERY_DESC_TYPE_UPSCALE_GPU_MEMORY_USAGE_V2: {
        auto* d = (ffxQueryDescUpscaleGetGPUMemoryUsageV2*)desc;
        if (d->gpuMemoryUsageUpscaler)
            *d->gpuMemoryUsageUpscaler = {};
        return FFX_API_RETURN_OK;
    }
    case FFX_API_QUERY_DESC_TYPE_UPSCALE_GET_RESOURCE_REQUIREMENTS: {
        auto* d = (ffxQueryDescUpscaleGetResourceRequirements*)desc;
        d->required_resources = FFX_API_QUERY_RESOURCE_INPUT_COLOR | FFX_API_QUERY_RESOURCE_INPUT_DEPTH |
                                FFX_API_QUERY_RESOURCE_INPUT_MV;
        d->optional_resources = FFX_API_QUERY_RESOURCE_INPUT_EXPOSURE;
        return FFX_API_RETURN_OK;
    }
    default:
        return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
    }
}

extern "C" __declspec(dllexport) ffxReturnCode_t ffxDispatch(ffxContext* context, const ffxDispatchDescHeader* desc)
{
    Init();
    if (!desc || !context || !*context || *(uint32_t*)*context != MAGIC_UP)
        return FFX_API_RETURN_ERROR_PARAMETER;
    auto* c = (UpCtx*)*context;
    switch (desc->type) {
    case FFX_API_DISPATCH_DESC_TYPE_UPSCALE:
        return Upscale(c, (const ffxDispatchDescUpscale*)desc);
    case FFX_API_DISPATCH_DESC_TYPE_UPSCALE_GENERATEREACTIVEMASK:
        // a reactive mask only AMD's upscaler reads; XeSS does not take one from the game
        return FFX_API_RETURN_OK;
    default:
        return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
    }
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
    }
    return TRUE;
}

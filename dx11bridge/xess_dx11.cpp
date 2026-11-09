// xess_dx11.cpp - libxess_dx11.dll: XeSS super resolution for D3D11 games, run by the D3D12 XeSS of libxess.dll.
//
// Intel's libxess_dx11.dll only forwards to the XeSS of the Intel Windows driver, which does not exist under Proton.
// This one implements the public XeSS D3D11 API (xess_d3d11.h, xess.h) itself: every context owns a D3D12 device on
// the game's adapter (vkd3d-proton) with an XeSS D3D12 context, which runs on XMX through the patched driver like in
// a D3D12 game. Per execute:
//   D3D11 (the game's immediate context): copy the inputs into textures shared with D3D12 (depth through a compute
//     shader into R32_FLOAT, a depth format cannot be shared), signal the shared fence;
//   D3D12: wait for it, xessD3D12Execute, signal;
//   D3D11: wait, copy the result into the game's output texture.
// The functions of xess.h that take a context go to the D3D12 context; xessGetVersion reports libxess.dll's version.
//
// libxess.dll is loaded from the folder of this DLL (the game's folder), else from the DLL search path. XESS_DX11_TRACE=1
// logs every call to stderr and OutputDebugString.
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>

#include "../ffxfg/inc/xess/xess/xess.h"
#include "../ffxfg/inc/xess/xess/xess_d3d11.h"
#include "../ffxfg/inc/xess/xess/xess_d3d12.h"
#include "../ffxfg/inc/xess/xess/xess_debug.h"
#include "depth_copy_cs.h"   // g_depthCopyCs, built by build.bat from depth_copy.hlsl

#define EXPORT extern "C"

// ---- log ------------------------------------------------------------------------------------------------------------

static bool g_trace;
static unsigned g_dump_at; // XESS_DX11_DUMP

static void Log(const char *fmt, ...)
{
   char b[1024];
   int n = snprintf(b, sizeof(b), "[xess-dx11] ");
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(b + n, sizeof(b) - n - 2, fmt, ap);
   va_end(ap);
   strcat(b, "\n");
   fputs(b, stderr);
   OutputDebugStringA(b);
}

#define TRACE(...)                                                                                                     \
   do {                                                                                                                \
      if (g_trace)                                                                                                     \
         Log(__VA_ARGS__);                                                                                             \
   } while (0)

// ---- libxess.dll ----------------------------------------------------------------------------------------------------

static HMODULE g_self, g_xess;
static std::once_flag g_load_once;

#define XESS_FNS(X)                                                                                                    \
   X(xessGetVersion, (xess_version_t *))                                                                               \
   X(xessD3D12CreateContext, (ID3D12Device *, xess_context_handle_t *))                                                \
   X(xessD3D12Init, (xess_context_handle_t, const xess_d3d12_init_params_t *))                                         \
   X(xessD3D12Execute, (xess_context_handle_t, ID3D12GraphicsCommandList *, const xess_d3d12_execute_params_t *))      \
   X(xessDestroyContext, (xess_context_handle_t))                                                                      \
   X(xessGetIntelXeFXVersion, (xess_context_handle_t, xess_version_t *))                                               \
   X(xessGetProperties, (xess_context_handle_t, const xess_2d_t *, xess_properties_t *))                               \
   X(xessGetInputResolution, (xess_context_handle_t, const xess_2d_t *, xess_quality_settings_t, xess_2d_t *))         \
   X(xessGetOptimalInputResolution,                                                                                    \
     (xess_context_handle_t, const xess_2d_t *, xess_quality_settings_t, xess_2d_t *, xess_2d_t *, xess_2d_t *))       \
   X(xessGetJitterScale, (xess_context_handle_t, float *, float *))                                                    \
   X(xessGetVelocityScale, (xess_context_handle_t, float *, float *))                                                  \
   X(xessSetJitterScale, (xess_context_handle_t, float, float))                                                        \
   X(xessSetVelocityScale, (xess_context_handle_t, float, float))                                                      \
   X(xessSetExposureMultiplier, (xess_context_handle_t, float))                                                        \
   X(xessGetExposureMultiplier, (xess_context_handle_t, float *))                                                      \
   X(xessSetMaxResponsiveMaskValue, (xess_context_handle_t, float))                                                    \
   X(xessGetMaxResponsiveMaskValue, (xess_context_handle_t, float *))                                                  \
   X(xessSetLoggingCallback, (xess_context_handle_t, xess_logging_level_t, xess_app_log_callback_t))                   \
   X(xessIsOptimalDriver, (xess_context_handle_t))                                                                     \
   X(xessForceLegacyScaleFactors, (xess_context_handle_t, bool))                                                       \
   X(xessGetPipelineBuildStatus, (xess_context_handle_t))                                                              \
   X(xessSelectNetworkModel, (xess_context_handle_t, xess_network_model_t))                                            \
   X(xessStartDump, (xess_context_handle_t, const xess_dump_parameters_t *))                                           \
   X(xessGetProfilingData, (xess_context_handle_t, xess_profiling_data_t **))                                          \
   /* not in the public headers: a parameter id and a float / a pointer out, as libxess.dll exports them */            \
   X(xessSetContextParameterF, (xess_context_handle_t, uint32_t, float))                                               \
   X(xessGetContextParameterP, (xess_context_handle_t, uint32_t, void **))

#define X(name, args) static xess_result_t(*p_##name) args;
XESS_FNS(X)
#undef X

static void LoadXess()
{
   g_trace = [] {
      const char *e = getenv("XESS_DX11_TRACE");
      return e && *e && strcmp(e, "0");
   }();
   if (const char *d = getenv("XESS_DX11_DUMP"))
      g_dump_at = (unsigned)atoi(d);
   char path[MAX_PATH] = {};
   GetModuleFileNameA(g_self, path, MAX_PATH);
   char *slash = strrchr(path, '\\');
   if (slash)
      strcpy(slash + 1, "libxess.dll");
   g_xess = LoadLibraryA(path);
   if (!g_xess)
      g_xess = LoadLibraryA("libxess.dll");
   if (!g_xess) {
      Log("cannot load libxess.dll (%s or the DLL search path)", path);
      return;
   }
#define X(name, args) p_##name = (decltype(p_##name))GetProcAddress(g_xess, #name);
   XESS_FNS(X)
#undef X
   if (!p_xessD3D12CreateContext || !p_xessD3D12Init || !p_xessD3D12Execute || !p_xessDestroyContext) {
      Log("libxess.dll has no XeSS D3D12 API");
      g_xess = nullptr;
      return;
   }
   xess_version_t v = {};
   if (p_xessGetVersion)
      p_xessGetVersion(&v);
   Log("libxess.dll %u.%u.%u", v.major, v.minor, v.patch);
}

static bool XessLoaded()
{
   std::call_once(g_load_once, LoadXess);
   return g_xess != nullptr;
}

// ---- formats --------------------------------------------------------------------------------------------------------

// The format of a shared copy: the game's, made typed (XeSS creates its views from the resource format) and not sRGB
// (an output must take UAVs). CopySubresourceRegion accepts both: same typeless group.
static DXGI_FORMAT CopyFormat(DXGI_FORMAT f)
{
   switch (f) {
   case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
   case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
   case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
   case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
   case DXGI_FORMAT_R8G8B8A8_TYPELESS:
   case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
   case DXGI_FORMAT_B8G8R8A8_TYPELESS:
   case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
   case DXGI_FORMAT_B8G8R8X8_TYPELESS:
   case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_UNORM;
   case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
   case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
   case DXGI_FORMAT_R8G8_TYPELESS: return DXGI_FORMAT_R8G8_UNORM;
   case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_FLOAT;
   case DXGI_FORMAT_R8_TYPELESS: return DXGI_FORMAT_R8_UNORM;
   default: return f;
   }
}

// The SRV format to read a depth texture with, DXGI_FORMAT_UNKNOWN if the texture is no depth texture.
static DXGI_FORMAT DepthReadFormat(const D3D11_TEXTURE2D_DESC &d)
{
   switch (d.Format) {
   case DXGI_FORMAT_D24_UNORM_S8_UINT:
   case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
   case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
   case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
   case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
   case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_UNORM;
   case DXGI_FORMAT_R32_TYPELESS: return (d.BindFlags & D3D11_BIND_DEPTH_STENCIL) ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_UNKNOWN;
   case DXGI_FORMAT_R16_TYPELESS: return (d.BindFlags & D3D11_BIND_DEPTH_STENCIL) ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_UNKNOWN;
   default: return DXGI_FORMAT_UNKNOWN;
   }
}

// ---- the context ----------------------------------------------------------------------------------------------------

enum Slot { S_COLOR, S_VELOCITY, S_DEPTH, S_EXPOSURE, S_MASK, S_OUTPUT, S_N };
static const char *const SLOT_NAME[S_N] = {"color", "velocity", "depth", "exposure", "mask", "output"};

// a game texture's copy shared by both devices
struct Shared {
   UINT w = 0, h = 0;
   DXGI_FORMAT game_fmt = DXGI_FORMAT_UNKNOWN; // the game texture it was made for
   ID3D11Texture2D *t11 = nullptr;
   ID3D12Resource *t12 = nullptr;
   ID3D11UnorderedAccessView *uav11 = nullptr; // depth: the compute shader's target
};

static const unsigned RING = 3;
static const uint32_t MAGIC = 0x31315844; // "DX11"

struct Ctx {
   uint32_t magic = MAGIC;
   std::mutex m;
   ID3D11Device *d11 = nullptr;
   ID3D11Device5 *d11_5 = nullptr;
   ID3D11DeviceContext *c11 = nullptr;
   ID3D11DeviceContext4 *c11_4 = nullptr;
   ID3D12Device *d12 = nullptr;
   ID3D12CommandQueue *q = nullptr;
   ID3D12CommandAllocator *ca[RING] = {};
   ID3D12GraphicsCommandList *cl[RING] = {};
   uint64_t ring_done[RING] = {}; // the fence value that frees each slot
   unsigned frame = 0;
   ID3D11Fence *f11 = nullptr;
   ID3D12Fence *f12 = nullptr;
   uint64_t fv = 0;
   HANDLE ev = nullptr;
   xess_context_handle_t x = nullptr; // the D3D12 context
   bool inited = false;
   xess_d3d11_init_params_t ip = {};
   Shared s[S_N];
   ID3D11ComputeShader *cs = nullptr;
   ID3D11Buffer *cb = nullptr;
   ID3D11Resource *depth_src = nullptr; // the game texture depth_srv reads
   ID3D11ShaderResourceView *depth_srv = nullptr;
   unsigned executes = 0;
};

static std::mutex g_ctx_m;
static std::set<Ctx *> g_ctxs;

static Ctx *Get(xess_context_handle_t h)
{
   Ctx *c = (Ctx *)h;
   std::lock_guard<std::mutex> l(g_ctx_m);
   return g_ctxs.count(c) ? c : nullptr;
}

template <class T> static void Rel(T *&p)
{
   if (p)
      p->Release();
   p = nullptr;
}

static void FreeShared(Shared &s)
{
   Rel(s.uav11);
   Rel(s.t12);
   Rel(s.t11);
   s = Shared();
}

// waits until the D3D12 queue has done everything submitted
static void WaitIdle(Ctx *c)
{
   if (!c->q || !c->f12)
      return;
   const uint64_t v = ++c->fv;
   c->q->Signal(c->f12, v);
   if (c->f12->GetCompletedValue() < v && SUCCEEDED(c->f12->SetEventOnCompletion(v, c->ev)))
      WaitForSingleObject(c->ev, 10000);
}

static void Destroy(Ctx *c)
{
   WaitIdle(c);
   if (c->x)
      p_xessDestroyContext(c->x);
   for (auto &s : c->s)
      FreeShared(s);
   Rel(c->depth_srv);
   Rel(c->depth_src);
   Rel(c->cb);
   Rel(c->cs);
   for (unsigned i = 0; i < RING; i++) {
      Rel(c->cl[i]);
      Rel(c->ca[i]);
   }
   Rel(c->f12);
   Rel(c->f11);
   Rel(c->q);
   Rel(c->d12);
   Rel(c->c11_4);
   Rel(c->c11);
   Rel(c->d11_5);
   Rel(c->d11);
   if (c->ev)
      CloseHandle(c->ev);
   c->magic = 0;
   delete c;
}

#define FAILR(hr, what, res)                                                                                           \
   do {                                                                                                                \
      HRESULT hr_ = (hr);                                                                                              \
      if (FAILED(hr_)) {                                                                                               \
         Log("%s failed: 0x%08lx", what, (unsigned long)hr_);                                                          \
         return res;                                                                                                   \
      }                                                                                                                \
   } while (0)

static xess_result_t Setup(Ctx *c, ID3D11Device *dev)
{
   c->d11 = dev;
   dev->AddRef();
   FAILR(dev->QueryInterface(IID_PPV_ARGS(&c->d11_5)), "ID3D11Device5 (D3D11 fences)", XESS_RESULT_ERROR_UNSUPPORTED_DEVICE);
   dev->GetImmediateContext(&c->c11);
   FAILR(c->c11->QueryInterface(IID_PPV_ARGS(&c->c11_4)), "ID3D11DeviceContext4", XESS_RESULT_ERROR_UNSUPPORTED_DEVICE);

   IDXGIDevice *xd = nullptr;
   IDXGIAdapter *ad = nullptr;
   FAILR(dev->QueryInterface(IID_PPV_ARGS(&xd)), "IDXGIDevice", XESS_RESULT_ERROR_DEVICE);
   HRESULT hr = xd->GetAdapter(&ad);
   xd->Release();
   FAILR(hr, "IDXGIDevice::GetAdapter", XESS_RESULT_ERROR_DEVICE);
   DXGI_ADAPTER_DESC adesc = {};
   ad->GetDesc(&adesc);
   hr = D3D12CreateDevice(ad, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&c->d12));
   ad->Release();
   FAILR(hr, "D3D12CreateDevice", XESS_RESULT_ERROR_UNSUPPORTED_DEVICE);
   TRACE("D3D12 device on %ls (vendor %04x)", adesc.Description, adesc.VendorId);

   D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT};
   FAILR(c->d12->CreateCommandQueue(&qd, IID_PPV_ARGS(&c->q)), "CreateCommandQueue", XESS_RESULT_ERROR_DEVICE);
   for (unsigned i = 0; i < RING; i++) {
      FAILR(c->d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&c->ca[i])),
            "CreateCommandAllocator", XESS_RESULT_ERROR_DEVICE);
      FAILR(c->d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->ca[i], nullptr, IID_PPV_ARGS(&c->cl[i])),
            "CreateCommandList", XESS_RESULT_ERROR_DEVICE);
      c->cl[i]->Close();
   }

   // one timeline both sides signal and wait on, created by D3D11 and opened by D3D12
   HANDLE fh = nullptr;
   FAILR(c->d11_5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&c->f11)), "ID3D11Device5::CreateFence",
         XESS_RESULT_ERROR_DEVICE);
   FAILR(c->f11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fh), "ID3D11Fence::CreateSharedHandle",
         XESS_RESULT_ERROR_DEVICE);
   hr = c->d12->OpenSharedHandle(fh, IID_PPV_ARGS(&c->f12));
   CloseHandle(fh);
   FAILR(hr, "OpenSharedHandle (fence)", XESS_RESULT_ERROR_DEVICE);
   c->ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);

   FAILR(dev->CreateComputeShader(g_depthCopyCs, sizeof(g_depthCopyCs), nullptr, &c->cs), "CreateComputeShader",
         XESS_RESULT_ERROR_DEVICE);
   D3D11_BUFFER_DESC bd = {16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
   FAILR(dev->CreateBuffer(&bd, nullptr, &c->cb), "CreateBuffer", XESS_RESULT_ERROR_DEVICE);

   xess_result_t r = p_xessD3D12CreateContext(c->d12, &c->x);
   if (r != XESS_RESULT_SUCCESS) {
      Log("xessD3D12CreateContext: %d", (int)r);
      c->x = nullptr;
      return r;
   }
   return XESS_RESULT_SUCCESS;
}

// the shared copy of a game texture for slot i, made (again) when the game texture's size or format changed
static Shared *SharedFor(Ctx *c, Slot i, const D3D11_TEXTURE2D_DESC &gd, bool depth)
{
   Shared &s = c->s[i];
   if (s.t11 && s.w == gd.Width && s.h == gd.Height && s.game_fmt == gd.Format)
      return &s;
   if (s.t11)
      WaitIdle(c); // the D3D12 side may still read the old one
   FreeShared(s);
   D3D11_TEXTURE2D_DESC td = {};
   td.Width = gd.Width;
   td.Height = gd.Height;
   td.MipLevels = 1;
   td.ArraySize = 1;
   td.Format = depth ? DXGI_FORMAT_R32_FLOAT : CopyFormat(gd.Format);
   td.SampleDesc.Count = 1;
   td.Usage = D3D11_USAGE_DEFAULT;
   td.BindFlags = D3D11_BIND_SHADER_RESOURCE | ((i == S_OUTPUT || depth) ? D3D11_BIND_UNORDERED_ACCESS : 0);
   td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
   HRESULT hr = c->d11->CreateTexture2D(&td, nullptr, &s.t11);
   if (FAILED(hr)) {
      Log("%s: shared %ux%u format %d: CreateTexture2D 0x%08lx", SLOT_NAME[i], td.Width, td.Height, (int)td.Format,
          (unsigned long)hr);
      s = Shared();
      return nullptr;
   }
   IDXGIResource1 *r1 = nullptr;
   HANDLE h = nullptr;
   hr = s.t11->QueryInterface(IID_PPV_ARGS(&r1));
   if (SUCCEEDED(hr)) {
      hr = r1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &h);
      r1->Release();
   }
   if (SUCCEEDED(hr)) {
      hr = c->d12->OpenSharedHandle(h, IID_PPV_ARGS(&s.t12));
      CloseHandle(h);
   }
   if (SUCCEEDED(hr) && depth)
      hr = c->d11->CreateUnorderedAccessView(s.t11, nullptr, &s.uav11);
   if (FAILED(hr)) {
      Log("%s: sharing a %ux%u format %d texture failed: 0x%08lx", SLOT_NAME[i], td.Width, td.Height, (int)td.Format,
          (unsigned long)hr);
      FreeShared(s);
      return nullptr;
   }
   s.w = gd.Width;
   s.h = gd.Height;
   s.game_fmt = gd.Format;
   TRACE("%s: shared %ux%u format %d (game format %d)", SLOT_NAME[i], td.Width, td.Height, (int)td.Format,
         (int)gd.Format);
   return &s;
}

static D3D11_BOX Box(xess_coord_t base, UINT w, UINT h, UINT tw, UINT th)
{
   D3D11_BOX b;
   b.left = base.x < tw ? base.x : tw;
   b.top = base.y < th ? base.y : th;
   b.right = b.left + w < tw ? b.left + w : tw;
   b.bottom = b.top + h < th ? b.top + h : th;
   b.front = 0;
   b.back = 1;
   return b;
}

// the D3D11 side of an input: copies the region of the game texture into its shared copy, returns the D3D12 copy
static ID3D12Resource *CopyIn(Ctx *c, Slot i, ID3D11Resource *res, xess_coord_t base, UINT w, UINT h)
{
   if (!res)
      return nullptr;
   ID3D11Texture2D *gt = nullptr;
   if (FAILED(res->QueryInterface(IID_PPV_ARGS(&gt)))) {
      Log("%s: not a 2D texture", SLOT_NAME[i]);
      return nullptr;
   }
   D3D11_TEXTURE2D_DESC gd;
   gt->GetDesc(&gd);
   gt->Release();
   const DXGI_FORMAT dfmt = i == S_DEPTH ? DepthReadFormat(gd) : DXGI_FORMAT_UNKNOWN;
   const bool depth = dfmt != DXGI_FORMAT_UNKNOWN;
   Shared *s = SharedFor(c, i, gd, depth);
   if (!s)
      return nullptr;
   const D3D11_BOX b = Box(base, w, h, gd.Width, gd.Height);
   if (!depth) {
      c->c11->CopySubresourceRegion(s->t11, 0, b.left, b.top, 0, res, 0, &b);
      return s->t12;
   }
   // depth: the compute shader, with the game's compute state restored afterwards
   if (c->depth_src != res) {
      Rel(c->depth_srv);
      Rel(c->depth_src);
      D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
      sd.Format = dfmt;
      sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
      sd.Texture2D.MipLevels = 1;
      HRESULT hr = c->d11->CreateShaderResourceView(res, &sd, &c->depth_srv);
      if (FAILED(hr)) {
         Log("depth: SRV format %d of a format %d texture: 0x%08lx (needs D3D11_BIND_SHADER_RESOURCE)", (int)dfmt,
             (int)gd.Format, (unsigned long)hr);
         return nullptr;
      }
      c->depth_src = res;
      res->AddRef();
   }
   const uint32_t cbv[4] = {b.left, b.top, b.right - b.left, b.bottom - b.top};
   c->c11->UpdateSubresource(c->cb, 0, nullptr, cbv, 0, 0);
   ID3D11ComputeShader *old_cs = nullptr;
   ID3D11ShaderResourceView *old_srv = nullptr;
   ID3D11UnorderedAccessView *old_uav = nullptr;
   ID3D11Buffer *old_cb = nullptr;
   c->c11->CSGetShader(&old_cs, nullptr, nullptr);
   c->c11->CSGetShaderResources(0, 1, &old_srv);
   c->c11->CSGetUnorderedAccessViews(0, 1, &old_uav);
   c->c11->CSGetConstantBuffers(0, 1, &old_cb);
   c->c11->CSSetShader(c->cs, nullptr, 0);
   c->c11->CSSetShaderResources(0, 1, &c->depth_srv);
   c->c11->CSSetUnorderedAccessViews(0, 1, &s->uav11, nullptr);
   c->c11->CSSetConstantBuffers(0, 1, &c->cb);
   c->c11->Dispatch((cbv[2] + 7) / 8, (cbv[3] + 7) / 8, 1);
   const UINT keep = (UINT)-1;
   c->c11->CSSetShader(old_cs, nullptr, 0);
   c->c11->CSSetShaderResources(0, 1, &old_srv);
   c->c11->CSSetUnorderedAccessViews(0, 1, &old_uav, &keep);
   c->c11->CSSetConstantBuffers(0, 1, &old_cb);
   Rel(old_cs);
   Rel(old_srv);
   Rel(old_uav);
   Rel(old_cb);
   return s->t12;
}

static void Transition(ID3D12GraphicsCommandList *cl, ID3D12Resource *const *res, unsigned n, D3D12_RESOURCE_STATES from,
                       D3D12_RESOURCE_STATES to)
{
   D3D12_RESOURCE_BARRIER b[S_N];
   unsigned k = 0;
   for (unsigned i = 0; i < n; i++) {
      if (!res[i])
         continue;
      b[k] = {};
      b[k].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      b[k].Transition.pResource = res[i];
      b[k].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      b[k].Transition.StateBefore = from;
      b[k].Transition.StateAfter = to;
      k++;
   }
   if (k)
      cl->ResourceBarrier(k, b);
}

// XESS_DX11_DUMP=<n>: the output of execute n into xess_dx11_dump.raw next to this DLL (debugging):
// a header "XD11" width height DXGI format row pitch (uint32 each), then the rows
static void Dump(Ctx *c, Shared *out, UINT w, UINT h)
{
   D3D11_TEXTURE2D_DESC sd = {};
   out->t11->GetDesc(&sd);
   sd.Usage = D3D11_USAGE_STAGING;
   sd.BindFlags = 0;
   sd.MiscFlags = 0;
   sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
   ID3D11Texture2D *st = nullptr;
   HRESULT hr = c->d11->CreateTexture2D(&sd, nullptr, &st);
   if (FAILED(hr)) {
      Log("dump: staging texture 0x%08lx", (unsigned long)hr);
      return;
   }
   c->c11->CopyResource(st, out->t11);
   char path[MAX_PATH] = {};
   GetModuleFileNameA(g_self, path, MAX_PATH);
   if (char *slash = strrchr(path, '\\'))
      strcpy(slash + 1, "xess_dx11_dump.raw");
   D3D11_MAPPED_SUBRESOURCE m;
   hr = c->c11->Map(st, 0, D3D11_MAP_READ, 0, &m);
   if (FAILED(hr))
      Log("dump: Map 0x%08lx", (unsigned long)hr);
   if (SUCCEEDED(hr)) {
      FILE *f = fopen(path, "wb");
      if (!f)
         Log("dump: cannot write %s", path);
      if (f) {
         const uint32_t hdr[5] = {0x31314458, sd.Width, sd.Height, (uint32_t)sd.Format, m.RowPitch};
         fwrite(hdr, sizeof(hdr), 1, f);
         fwrite(m.pData, m.RowPitch, sd.Height, f);
         fclose(f);
         Log("dumped execute %u: %ux%u format %d (output region %ux%u)", c->executes, sd.Width, sd.Height,
             (int)sd.Format, w, h);
      }
      c->c11->Unmap(st, 0);
   }
   st->Release();
}

static xess_result_t Execute(Ctx *c, const xess_d3d11_execute_params_t *e)
{
   const uint32_t flags = c->ip.initFlags;
   const UINT iw = e->inputWidth, ih = e->inputHeight;
   const UINT ow = c->ip.outputResolution.x, oh = c->ip.outputResolution.y;
   const bool hires_mv = flags & XESS_INIT_FLAG_HIGH_RES_MV;

   ID3D11Texture2D *ot = nullptr;
   if (FAILED(e->pOutputTexture->QueryInterface(IID_PPV_ARGS(&ot))))
      return XESS_RESULT_ERROR_INVALID_ARGUMENT;
   D3D11_TEXTURE2D_DESC od;
   ot->GetDesc(&od);
   ot->Release();
   Shared *out = SharedFor(c, S_OUTPUT, od, false);

   // D3D11: the inputs
   xess_d3d12_execute_params_t x = {};
   x.pColorTexture = CopyIn(c, S_COLOR, e->pColorTexture, e->inputColorBase, iw, ih);
   x.pVelocityTexture = CopyIn(c, S_VELOCITY, e->pVelocityTexture, e->inputMotionVectorBase, hires_mv ? ow : iw,
                               hires_mv ? oh : ih);
   if (!hires_mv)
      x.pDepthTexture = CopyIn(c, S_DEPTH, e->pDepthTexture, e->inputDepthBase, iw, ih);
   if (flags & XESS_INIT_FLAG_EXPOSURE_SCALE_TEXTURE)
      x.pExposureScaleTexture = CopyIn(c, S_EXPOSURE, e->pExposureScaleTexture, {0, 0}, 1, 1);
   if (flags & XESS_INIT_FLAG_RESPONSIVE_PIXEL_MASK)
      x.pResponsivePixelMaskTexture = CopyIn(c, S_MASK, e->pResponsivePixelMaskTexture, e->inputResponsiveMaskBase, iw, ih);
   x.pOutputTexture = out ? out->t12 : nullptr;
   if (!out || !x.pColorTexture || !x.pVelocityTexture || (!hires_mv && !x.pDepthTexture) ||
       ((flags & XESS_INIT_FLAG_EXPOSURE_SCALE_TEXTURE) && !x.pExposureScaleTexture) ||
       ((flags & XESS_INIT_FLAG_RESPONSIVE_PIXEL_MASK) && !x.pResponsivePixelMaskTexture))
      return XESS_RESULT_ERROR_INVALID_ARGUMENT;
   x.jitterOffsetX = e->jitterOffsetX;
   x.jitterOffsetY = e->jitterOffsetY;
   x.exposureScale = e->exposureScale;
   x.resetHistory = e->resetHistory;
   x.inputWidth = iw;
   x.inputHeight = ih;
   x.inputColorBase = e->inputColorBase;
   x.inputMotionVectorBase = e->inputMotionVectorBase;
   x.inputDepthBase = e->inputDepthBase;
   x.inputResponsiveMaskBase = e->inputResponsiveMaskBase;
   x.reserved0 = e->reserved0;
   x.outputColorBase = e->outputColorBase;
   const uint64_t v_in = ++c->fv;
   c->c11_4->Signal(c->f11, v_in);
   c->c11->Flush();

   // D3D12: XeSS
   const unsigned k = c->frame++ % RING;
   if (c->f12->GetCompletedValue() < c->ring_done[k] && SUCCEEDED(c->f12->SetEventOnCompletion(c->ring_done[k], c->ev)))
      WaitForSingleObject(c->ev, 10000);
   ID3D12GraphicsCommandList *cl = c->cl[k];
   c->ca[k]->Reset();
   cl->Reset(c->ca[k], nullptr);
   ID3D12Resource *ins[] = {x.pColorTexture, x.pVelocityTexture, x.pDepthTexture, x.pExposureScaleTexture,
                            x.pResponsivePixelMaskTexture};
   Transition(cl, ins, 5, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
   Transition(cl, &x.pOutputTexture, 1, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
   xess_result_t r = p_xessD3D12Execute(c->x, cl, &x);
   Transition(cl, ins, 5, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
   Transition(cl, &x.pOutputTexture, 1, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
   cl->Close();
   c->q->Wait(c->f12, v_in);
   ID3D12CommandList *lists[] = {cl};
   c->q->ExecuteCommandLists(1, lists);
   const uint64_t v_out = ++c->fv;
   c->q->Signal(c->f12, v_out);
   c->ring_done[k] = v_out;

   // D3D11: the result into the game's output
   c->c11_4->Wait(c->f11, v_out);
   const D3D11_BOX b = Box(e->outputColorBase, ow, oh, od.Width, od.Height);
   c->c11->CopySubresourceRegion(e->pOutputTexture, 0, b.left, b.top, 0, out->t11, 0, &b);
   if (c->executes++ < 3 || g_trace)
      TRACE("execute %u: %ux%u -> %ux%u, xessD3D12Execute %d", c->executes, iw, ih, ow, oh, (int)r);
   if (c->executes == g_dump_at)
      Dump(c, out, ow, oh);
   return r;
}

// ---- the D3D11 API --------------------------------------------------------------------------------------------------

EXPORT xess_result_t xessD3D11CreateContext(ID3D11Device *device, xess_context_handle_t *phContext)
{
   if (!device || !phContext)
      return XESS_RESULT_ERROR_INVALID_ARGUMENT;
   *phContext = nullptr;
   if (!XessLoaded())
      return XESS_RESULT_ERROR_CANT_LOAD_LIBRARY;
   Ctx *c = new Ctx();
   xess_result_t r = Setup(c, device);
   if (r != XESS_RESULT_SUCCESS) {
      Destroy(c);
      return r;
   }
   {
      std::lock_guard<std::mutex> l(g_ctx_m);
      g_ctxs.insert(c);
   }
   *phContext = (xess_context_handle_t)c;
   TRACE("context %p (D3D12 context %p)", (void *)c, (void *)c->x);
   return XESS_RESULT_SUCCESS;
}

EXPORT xess_result_t xessD3D11Init(xess_context_handle_t hContext, const xess_d3d11_init_params_t *pInitParams)
{
   Ctx *c = Get(hContext);
   if (!c)
      return XESS_RESULT_ERROR_INVALID_CONTEXT;
   if (!pInitParams)
      return XESS_RESULT_ERROR_INVALID_ARGUMENT;
   if (pInitParams->initFlags & XESS_INIT_FLAG_EXTERNAL_DESCRIPTOR_HEAP)
      return XESS_RESULT_ERROR_INVALID_ARGUMENT; // as Intel's: not in D3D11
   std::lock_guard<std::mutex> l(c->m);
   WaitIdle(c); // no XeSS work may be pending while it is initialised again
   xess_d3d12_init_params_t ip = {};
   ip.outputResolution = pInitParams->outputResolution;
   ip.qualitySetting = pInitParams->qualitySetting;
   ip.initFlags = pInitParams->initFlags;
   xess_result_t r = p_xessD3D12Init(c->x, &ip);
   TRACE("init %ux%u quality %d flags 0x%x: %d", ip.outputResolution.x, ip.outputResolution.y, (int)ip.qualitySetting,
         ip.initFlags, (int)r);
   if (r == XESS_RESULT_SUCCESS) {
      c->ip = *pInitParams;
      c->inited = true;
   }
   return r;
}

EXPORT xess_result_t xessD3D11GetInitParams(xess_context_handle_t hContext, xess_d3d11_init_params_t *pInitParams)
{
   Ctx *c = Get(hContext);
   if (!c)
      return XESS_RESULT_ERROR_INVALID_CONTEXT;
   if (!pInitParams)
      return XESS_RESULT_ERROR_INVALID_ARGUMENT;
   if (!c->inited)
      return XESS_RESULT_ERROR_UNINITIALIZED;
   *pInitParams = c->ip;
   return XESS_RESULT_SUCCESS;
}

EXPORT xess_result_t xessD3D11Execute(xess_context_handle_t hContext, const xess_d3d11_execute_params_t *pExecParams)
{
   Ctx *c = Get(hContext);
   if (!c)
      return XESS_RESULT_ERROR_INVALID_CONTEXT;
   if (!pExecParams || !pExecParams->pColorTexture || !pExecParams->pVelocityTexture || !pExecParams->pOutputTexture)
      return XESS_RESULT_ERROR_INVALID_ARGUMENT;
   if (!c->inited)
      return XESS_RESULT_ERROR_UNINITIALIZED;
   std::lock_guard<std::mutex> l(c->m);
   return Execute(c, pExecParams);
}

EXPORT xess_result_t xessDestroyContext(xess_context_handle_t hContext)
{
   Ctx *c = Get(hContext);
   if (!c)
      return XESS_RESULT_ERROR_INVALID_CONTEXT;
   {
      std::lock_guard<std::mutex> l(g_ctx_m);
      g_ctxs.erase(c);
   }
   TRACE("destroy %p after %u executes", (void *)c, c->executes);
   Destroy(c);
   return XESS_RESULT_SUCCESS;
}

// ---- xess.h: to the D3D12 context -----------------------------------------------------------------------------------

EXPORT xess_result_t xessGetVersion(xess_version_t *pVersion)
{
   if (!pVersion)
      return XESS_RESULT_ERROR_INVALID_ARGUMENT;
   if (!XessLoaded() || !p_xessGetVersion)
      return XESS_RESULT_ERROR_CANT_LOAD_LIBRARY;
   return p_xessGetVersion(pVersion);
}

// a function of xess.h called with the D3D12 context in place of ours
#define FWD(name, params, ...)                                                                                         \
   EXPORT xess_result_t name params                                                                                    \
   {                                                                                                                   \
      Ctx *c = Get(hContext);                                                                                          \
      if (!c)                                                                                                          \
         return XESS_RESULT_ERROR_INVALID_CONTEXT;                                                                     \
      if (!p_##name)                                                                                                   \
         return XESS_RESULT_ERROR_NOT_IMPLEMENTED;                                                                     \
      return p_##name(c->x, __VA_ARGS__);                                                                              \
   }
#define FWD0(name)                                                                                                     \
   EXPORT xess_result_t name(xess_context_handle_t hContext)                                                           \
   {                                                                                                                   \
      Ctx *c = Get(hContext);                                                                                          \
      if (!c)                                                                                                          \
         return XESS_RESULT_ERROR_INVALID_CONTEXT;                                                                     \
      if (!p_##name)                                                                                                   \
         return XESS_RESULT_ERROR_NOT_IMPLEMENTED;                                                                     \
      return p_##name(c->x);                                                                                           \
   }

FWD(xessGetIntelXeFXVersion, (xess_context_handle_t hContext, xess_version_t *v), v)
FWD(xessGetProperties, (xess_context_handle_t hContext, const xess_2d_t *o, xess_properties_t *p), o, p)
FWD(xessGetInputResolution, (xess_context_handle_t hContext, const xess_2d_t *o, xess_quality_settings_t q, xess_2d_t *i),
    o, q, i)
FWD(xessGetOptimalInputResolution,
    (xess_context_handle_t hContext, const xess_2d_t *o, xess_quality_settings_t q, xess_2d_t *i, xess_2d_t *mn,
     xess_2d_t *mx),
    o, q, i, mn, mx)
FWD(xessGetJitterScale, (xess_context_handle_t hContext, float *x, float *y), x, y)
FWD(xessGetVelocityScale, (xess_context_handle_t hContext, float *x, float *y), x, y)
FWD(xessSetJitterScale, (xess_context_handle_t hContext, float x, float y), x, y)
FWD(xessSetVelocityScale, (xess_context_handle_t hContext, float x, float y), x, y)
FWD(xessSetExposureMultiplier, (xess_context_handle_t hContext, float s), s)
FWD(xessGetExposureMultiplier, (xess_context_handle_t hContext, float *s), s)
FWD(xessSetMaxResponsiveMaskValue, (xess_context_handle_t hContext, float v), v)
FWD(xessGetMaxResponsiveMaskValue, (xess_context_handle_t hContext, float *v), v)
FWD(xessSetLoggingCallback, (xess_context_handle_t hContext, xess_logging_level_t l, xess_app_log_callback_t cb), l, cb)
FWD0(xessIsOptimalDriver)
FWD(xessForceLegacyScaleFactors, (xess_context_handle_t hContext, bool f), f)
FWD0(xessGetPipelineBuildStatus)
FWD(xessSelectNetworkModel, (xess_context_handle_t hContext, xess_network_model_t n), n)
FWD(xessStartDump, (xess_context_handle_t hContext, const xess_dump_parameters_t *d), d)
FWD(xessGetProfilingData, (xess_context_handle_t hContext, xess_profiling_data_t **p), p)
FWD(xessSetContextParameterF, (xess_context_handle_t hContext, uint32_t id, float v), id, v)
FWD(xessGetContextParameterP, (xess_context_handle_t hContext, uint32_t id, void **p), id, p)

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
   if (reason == DLL_PROCESS_ATTACH) {
      g_self = inst;
      DisableThreadLibraryCalls(inst);
   }
   return TRUE;
}

// bridgetest.cpp - libxess_dx11.dll (next to this program) with the inputs the SDK sample does not use: low-resolution
// motion vectors with a D24S8 depth texture (the bridge's compute shader path) and an R8G8B8A8 output. 60 frames of a
// flat colour; then the output must be that colour, and the compute state the "game" had bound must be unchanged.
// Results in bridgetest.txt next to the program (Proton drops a console program's stdout); exit code 0 when passed.
#include <windows.h>
#include <d3d11_4.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "../../ffxfg/inc/xess/xess/xess.h"
#include "../../ffxfg/inc/xess/xess/xess_d3d11.h"

#define CHECK(x)                                                                                                       \
   do {                                                                                                                \
      HRESULT hr_ = (x);                                                                                               \
      if (FAILED(hr_)) {                                                                                               \
         printf("FAIL %s: 0x%08lx\n", #x, (unsigned long)hr_);                                                         \
         return 1;                                                                                                     \
      }                                                                                                                \
   } while (0)

typedef xess_result_t (*PFN_Create)(ID3D11Device *, xess_context_handle_t *);
typedef xess_result_t (*PFN_Init)(xess_context_handle_t, const xess_d3d11_init_params_t *);
typedef xess_result_t (*PFN_Exec)(xess_context_handle_t, const xess_d3d11_execute_params_t *);
typedef xess_result_t (*PFN_Destroy)(xess_context_handle_t);
typedef xess_result_t (*PFN_InRes)(xess_context_handle_t, const xess_2d_t *, xess_quality_settings_t, xess_2d_t *);

static ID3D11Texture2D *Tex(ID3D11Device *d, UINT w, UINT h, DXGI_FORMAT f, UINT bind, const void *data, UINT pitch)
{
   D3D11_TEXTURE2D_DESC td = {w, h, 1, 1, f, {1, 0}, D3D11_USAGE_DEFAULT, bind, 0, 0};
   D3D11_SUBRESOURCE_DATA sd = {data, pitch, 0};
   ID3D11Texture2D *t = nullptr;
   d->CreateTexture2D(&td, data ? &sd : nullptr, &t);
   return t;
}

int main()
{
   freopen("bridgetest.txt", "w", stdout);
   setvbuf(stdout, nullptr, _IONBF, 0);
   HMODULE m = LoadLibraryA("libxess_dx11.dll");
   if (!m) {
      printf("FAIL no libxess_dx11.dll\n");
      return 1;
   }
   auto create = (PFN_Create)GetProcAddress(m, "xessD3D11CreateContext");
   auto init = (PFN_Init)GetProcAddress(m, "xessD3D11Init");
   auto exec = (PFN_Exec)GetProcAddress(m, "xessD3D11Execute");
   auto destroy = (PFN_Destroy)GetProcAddress(m, "xessDestroyContext");
   auto inres = (PFN_InRes)GetProcAddress(m, "xessGetInputResolution");

   ID3D11Device *d = nullptr;
   ID3D11DeviceContext *c = nullptr;
   D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1;
   CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &d, nullptr, &c));

   xess_context_handle_t x = nullptr;
   xess_result_t r = create(d, &x);
   printf("%s xessD3D11CreateContext %d\n", r == XESS_RESULT_SUCCESS ? "ok" : "FAIL", (int)r);
   if (r != XESS_RESULT_SUCCESS)
      return 1;
   const xess_2d_t out = {1920, 1080};
   xess_2d_t in = {};
   r = inres(x, &out, XESS_QUALITY_SETTING_PERFORMANCE, &in);
   printf("%s input resolution %ux%u (%d)\n", r == XESS_RESULT_SUCCESS ? "ok" : "FAIL", in.x, in.y, (int)r);
   xess_d3d11_init_params_t ip = {out, XESS_QUALITY_SETTING_PERFORMANCE, XESS_INIT_FLAG_NONE};
   r = init(x, &ip);
   printf("%s xessD3D11Init (low-res motion vectors, depth) %d\n", r == XESS_RESULT_SUCCESS ? "ok" : "FAIL", (int)r);
   if (r != XESS_RESULT_SUCCESS)
      return 1;

   // inputs: a flat colour, zero motion, depth 0.5 (written through a DSV, read by the bridge through an SRV)
   const uint32_t colour = 0xff3080c0; // R c0 G 80 B 30 (ABGR in memory: R first)
   std::vector<uint32_t> px(in.x * in.y, colour);
   ID3D11Texture2D *col = Tex(d, in.x, in.y, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, px.data(), in.x * 4);
   std::vector<uint32_t> zero(in.x * in.y, 0);
   ID3D11Texture2D *mv = Tex(d, in.x, in.y, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE, zero.data(), in.x * 4);
   ID3D11Texture2D *dep = Tex(d, in.x, in.y, DXGI_FORMAT_R24G8_TYPELESS,
                              D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE, nullptr, 0);
   ID3D11Texture2D *o = Tex(d, out.x, out.y, DXGI_FORMAT_R8G8B8A8_UNORM,
                            D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, nullptr, 0);
   if (!col || !mv || !dep || !o) {
      printf("FAIL textures\n");
      return 1;
   }
   D3D11_DEPTH_STENCIL_VIEW_DESC dd = {DXGI_FORMAT_D24_UNORM_S8_UINT, D3D11_DSV_DIMENSION_TEXTURE2D};
   ID3D11DepthStencilView *dsv = nullptr;
   CHECK(d->CreateDepthStencilView(dep, &dd, &dsv));
   c->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 0.5f, 0);

   // the "game's" compute state: an SRV in slot 0 that must still be there afterwards
   ID3D11ShaderResourceView *game_srv = nullptr;
   CHECK(d->CreateShaderResourceView(col, nullptr, &game_srv));
   c->CSSetShaderResources(0, 1, &game_srv);

   static const float jit[8][2] = {{0.25f, -0.25f}, {-0.25f, 0.25f}, {0.125f, 0.375f}, {-0.375f, -0.125f},
                                   {0.375f, 0.125f}, {-0.125f, -0.375f}, {0.0625f, -0.4375f}, {-0.4375f, 0.0625f}};
   for (int f = 0; f < 60; f++) {
      xess_d3d11_execute_params_t e = {};
      e.pColorTexture = col;
      e.pVelocityTexture = mv;
      e.pDepthTexture = dep;
      e.pOutputTexture = o;
      e.jitterOffsetX = jit[f % 8][0];
      e.jitterOffsetY = jit[f % 8][1];
      e.exposureScale = 1.0f;
      e.resetHistory = f == 0;
      e.inputWidth = in.x;
      e.inputHeight = in.y;
      r = exec(x, &e);
      if (r != XESS_RESULT_SUCCESS) {
         printf("FAIL xessD3D11Execute frame %d: %d\n", f, (int)r);
         return 1;
      }
   }
   printf("ok 60 executes\n");

   ID3D11ShaderResourceView *now = nullptr;
   c->CSGetShaderResources(0, 1, &now);
   const bool state_ok = now == game_srv;
   printf("%s compute SRV slot 0 of the game kept\n", state_ok ? "ok" : "FAIL");
   if (now)
      now->Release();

   D3D11_TEXTURE2D_DESC sd;
   o->GetDesc(&sd);
   sd.Usage = D3D11_USAGE_STAGING;
   sd.BindFlags = 0;
   sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
   ID3D11Texture2D *st = nullptr;
   CHECK(d->CreateTexture2D(&sd, nullptr, &st));
   c->CopyResource(st, o);
   D3D11_MAPPED_SUBRESOURCE map;
   CHECK(c->Map(st, 0, D3D11_MAP_READ, 0, &map));
   // compare samples across the image with the input colour (each channel within 3)
   int bad = 0, n = 0;
   uint32_t worst = 0;
   for (UINT y = 8; y < out.y; y += 97)
      for (UINT xx = 8; xx < out.x; xx += 113) {
         const uint32_t v = ((const uint32_t *)((const char *)map.pData + y * map.RowPitch))[xx];
         n++;
         for (int ch = 0; ch < 3; ch++) {
            const int a = (v >> (8 * ch)) & 255, b = (colour >> (8 * ch)) & 255;
            if (abs(a - b) > 3) {
               bad++;
               worst = v;
               break;
            }
         }
      }
   const uint32_t centre = ((const uint32_t *)((const char *)map.pData + (out.y / 2) * map.RowPitch))[out.x / 2];
   c->Unmap(st, 0);
   printf("%s output: %d of %d samples off the input colour 0x%08x (centre 0x%08x%s)\n", bad ? "FAIL" : "ok", bad, n,
          colour, centre, bad ? ", e.g. a wrong one" : "");
   if (bad)
      printf("   e.g. 0x%08x\n", worst);
   destroy(x);
   printf("ok xessDestroyContext\n");
   const bool pass = !bad && state_ok;
   printf(pass ? "PASS\n" : "FAILED\n");
   return pass ? 0 : 1;
}

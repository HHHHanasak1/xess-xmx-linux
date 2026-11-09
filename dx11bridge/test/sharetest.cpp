// sharetest.cpp - can a D3D11 device (DXVK) and a D3D12 device (vkd3d-proton) of one process share a texture and a
// fence? D3D11 clears a shared texture to A and signals 1; D3D12 waits, reads it back (expects A), clears it to B and
// signals 2; D3D11 waits and reads it back (expects B). Prints one line per step, exit code 0 when all passed.
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <cstdint>

#define CHECK(x)                                                                                    \
   do {                                                                                             \
      HRESULT hr_ = (x);                                                                            \
      if (FAILED(hr_)) {                                                                            \
         printf("FAIL %s: 0x%08lx\n", #x, (unsigned long)hr_);                                      \
         return 1;                                                                                  \
      }                                                                                             \
   } while (0)

static const UINT W = 64, H = 64;

int main()
{
   // Proton drops a console program's stdout: the results go to sharetest.txt next to the program
   freopen("sharetest.txt", "w", stdout);
   setvbuf(stdout, nullptr, _IONBF, 0);
   ID3D11Device *d11 = nullptr;
   ID3D11DeviceContext *c11 = nullptr;
   D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1;
   CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &d11, nullptr, &c11));
   ID3D11Device5 *d11_5 = nullptr;
   ID3D11DeviceContext4 *c11_4 = nullptr;
   CHECK(d11->QueryInterface(IID_PPV_ARGS(&d11_5)));
   CHECK(c11->QueryInterface(IID_PPV_ARGS(&c11_4)));

   IDXGIDevice *xd = nullptr;
   IDXGIAdapter *ad = nullptr;
   CHECK(d11->QueryInterface(IID_PPV_ARGS(&xd)));
   CHECK(xd->GetAdapter(&ad));
   DXGI_ADAPTER_DESC adesc;
   ad->GetDesc(&adesc);
   printf("adapter %ls vendor %04x LUID %08lx:%08lx\n", adesc.Description, adesc.VendorId,
          (unsigned long)adesc.AdapterLuid.HighPart, (unsigned long)adesc.AdapterLuid.LowPart);

   ID3D12Device *d12 = nullptr;
   CHECK(D3D12CreateDevice(ad, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&d12)));
   printf("ok d3d12 device\n");

   // shared texture, created by D3D11
   D3D11_TEXTURE2D_DESC td = {};
   td.Width = W;
   td.Height = H;
   td.MipLevels = 1;
   td.ArraySize = 1;
   td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
   td.SampleDesc.Count = 1;
   td.Usage = D3D11_USAGE_DEFAULT;
   td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
   td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
   ID3D11Texture2D *t11 = nullptr;
   CHECK(d11->CreateTexture2D(&td, nullptr, &t11));
   IDXGIResource1 *r1 = nullptr;
   HANDLE th = nullptr;
   CHECK(t11->QueryInterface(IID_PPV_ARGS(&r1)));
   CHECK(r1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &th));
   ID3D12Resource *t12 = nullptr;
   CHECK(d12->OpenSharedHandle(th, IID_PPV_ARGS(&t12)));
   D3D12_RESOURCE_DESC rd = t12->GetDesc();
   printf("ok shared texture: d3d12 sees %llux%u format %d flags 0x%x\n", (unsigned long long)rd.Width, rd.Height,
          (int)rd.Format, (unsigned)rd.Flags);

   // shared fence, created by D3D11
   ID3D11Fence *f11 = nullptr;
   HANDLE fh = nullptr;
   CHECK(d11_5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&f11)));
   CHECK(f11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fh));
   ID3D12Fence *f12 = nullptr;
   CHECK(d12->OpenSharedHandle(fh, IID_PPV_ARGS(&f12)));
   printf("ok shared fence\n");

   // D3D11: clear to A, signal 1
   ID3D11UnorderedAccessView *uav = nullptr;
   CHECK(d11->CreateUnorderedAccessView(t11, nullptr, &uav));
   const float A[4] = {1.0f, 0.0f, 0.0f, 1.0f};
   c11->ClearUnorderedAccessViewFloat(uav, A);
   CHECK(c11_4->Signal(f11, 1));
   c11->Flush();

   // D3D12: wait 1, copy to readback, clear to B (via a UAV clear), signal 2
   D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT};
   ID3D12CommandQueue *q = nullptr;
   ID3D12CommandAllocator *ca = nullptr;
   ID3D12GraphicsCommandList *cl = nullptr;
   CHECK(d12->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)));
   CHECK(d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca)));
   CHECK(d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca, nullptr, IID_PPV_ARGS(&cl)));

   D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
   UINT64 total = 0;
   d12->GetCopyableFootprints(&rd, 0, 1, 0, &fp, nullptr, nullptr, &total);
   D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_READBACK};
   D3D12_RESOURCE_DESC bd = {};
   bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
   bd.Width = total;
   bd.Height = 1;
   bd.DepthOrArraySize = 1;
   bd.MipLevels = 1;
   bd.SampleDesc.Count = 1;
   bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
   ID3D12Resource *rb = nullptr;
   CHECK(d12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                      IID_PPV_ARGS(&rb)));

   D3D12_DESCRIPTOR_HEAP_DESC hd_gpu = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1,
                                        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
   D3D12_DESCRIPTOR_HEAP_DESC hd_cpu = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE};
   ID3D12DescriptorHeap *hg = nullptr, *hc = nullptr;
   CHECK(d12->CreateDescriptorHeap(&hd_gpu, IID_PPV_ARGS(&hg)));
   CHECK(d12->CreateDescriptorHeap(&hd_cpu, IID_PPV_ARGS(&hc)));
   d12->CreateUnorderedAccessView(t12, nullptr, nullptr, hg->GetCPUDescriptorHandleForHeapStart());
   d12->CreateUnorderedAccessView(t12, nullptr, nullptr, hc->GetCPUDescriptorHandleForHeapStart());

   D3D12_RESOURCE_BARRIER b = {};
   b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
   b.Transition.pResource = t12;
   b.Transition.Subresource = 0;
   b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
   b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
   cl->ResourceBarrier(1, &b);
   D3D12_TEXTURE_COPY_LOCATION dst = {rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
   dst.PlacedFootprint = fp;
   D3D12_TEXTURE_COPY_LOCATION src = {t12, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
   src.SubresourceIndex = 0;
   cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
   b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
   b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
   cl->ResourceBarrier(1, &b);
   cl->SetDescriptorHeaps(1, &hg);
   const float B[4] = {0.0f, 0.0f, 1.0f, 1.0f};
   cl->ClearUnorderedAccessViewFloat(hg->GetGPUDescriptorHandleForHeapStart(), hc->GetCPUDescriptorHandleForHeapStart(),
                                     t12, B, 0, nullptr);
   b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
   b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
   cl->ResourceBarrier(1, &b);
   CHECK(cl->Close());
   CHECK(q->Wait(f12, 1));
   ID3D12CommandList *lists[] = {cl};
   q->ExecuteCommandLists(1, lists);
   CHECK(q->Signal(f12, 2));

   printf("ok d3d12 work submitted\n");
   // D3D11: wait 2, copy to staging, check B
   CHECK(c11_4->Wait(f11, 2));
   printf("ok d3d11 wait queued\n");
   D3D11_TEXTURE2D_DESC sd = td;
   sd.Usage = D3D11_USAGE_STAGING;
   sd.BindFlags = 0;
   sd.MiscFlags = 0;
   sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
   ID3D11Texture2D *st = nullptr;
   CHECK(d11->CreateTexture2D(&sd, nullptr, &st));
   c11->CopyResource(st, t11);
   D3D11_MAPPED_SUBRESOURCE m;
   CHECK(c11->Map(st, 0, D3D11_MAP_READ, 0, &m));
   const uint32_t got11 = *(const uint32_t *)m.pData;
   c11->Unmap(st, 0);

   // the D3D12 readback of A (complete: D3D11 waited for 2)
   void *p = nullptr;
   D3D12_RANGE rr = {0, (SIZE_T)total};
   CHECK(rb->Map(0, &rr, &p));
   const uint32_t got12 = *(const uint32_t *)((const char *)p + fp.Offset);
   rb->Unmap(0, nullptr);
   const UINT64 fv = f12->GetCompletedValue();

   const bool ok12 = got12 == 0xff0000ffu, ok11 = got11 == 0xffff0000u;
   printf("%s d3d12 read 0x%08x (want 0xff0000ff, the D3D11 clear)\n", ok12 ? "ok" : "FAIL", got12);
   printf("%s d3d11 read 0x%08x (want 0xffff0000, the D3D12 clear)\n", ok11 ? "ok" : "FAIL", got11);
   printf("fence completed %llu\n", (unsigned long long)fv);
   printf(ok11 && ok12 ? "PASS\n" : "FAILED\n");
   return ok11 && ok12 ? 0 : 1;
}

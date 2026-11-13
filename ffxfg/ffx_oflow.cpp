// ffx_oflow.cpp - motion vectors for XeSS-FG from the final image only (FFX_XEFG_OFLOW=1, OFLOW_FG=1), as a
// driver-level frame generation has to work. FidelityFX optical flow (FidelityFX SDK 1.1.4, MIT, linked statically)
// estimates per 8x8 block how the current backbuffer (or a 1/n copy of it, oflow_down.hlsl) moved from the previous
// one; oflow_expand.hlsl expands that into a display-size R16G16_FLOAT motion vector texture (pixels, current ->
// previous, the FFX convention) and writes a constant depth texture. It also times a frame on the GPU for
// FFX_XEFG_CAP=auto (OflowFrameStart). Kept apart from ffx_xefg.cpp because the SDK 1.1 host headers and the FFX API
// headers there must not meet in one translation unit.
#include <windows.h>
#include <d3d12.h>
#include <stdint.h>
#include <stdio.h>
#include <vector>
#include <initializer_list>
#include <stdlib.h>

#include <FidelityFX/host/ffx_opticalflow.h>
#include <FidelityFX/host/backends/dx12/ffx_dx12.h>

#include "ffx_oflow.h"
#include "oflow_expand_cs.h"   // g_oflowExpandCs: DXIL of oflow_expand.hlsl (build.bat)
#include "oflow_down_cs.h"     // g_oflowDownCs: DXIL of oflow_down.hlsl

void (*g_oflowLog)(const char* fmt, ...) = nullptr;
#define Log(...) (g_oflowLog ? g_oflowLog(__VA_ARGS__) : (void)0)

static const UINT kRing = 3;

struct Oflow {
    ID3D12Device* dev = nullptr;
    uint32_t w = 0, h = 0, bw = 0, bh = 0;
    uint32_t div = 1, sw = 0, sh = 0;      // FFX_XEFG_OFLOW_DIV: the optical flow sees the image at 1/div size
    ID3D12Resource* lowres = nullptr;       // that image (div > 1)
    ID3D12PipelineState* down = nullptr;
    std::vector<uint8_t> scratch;
    FfxInterface iface = {};
    FfxOpticalflowContext ctx = {};
    bool ctxMade = false;
    ID3D12Resource *vec = nullptr, *scd = nullptr, *mv = nullptr, *depth = nullptr;
    ID3D12RootSignature* rs = nullptr;
    ID3D12PipelineState* pso = nullptr;
    ID3D12DescriptorHeap* heap = nullptr;
    UINT inc = 0;
    ID3D12CommandAllocator* alloc[kRing] = {};
    UINT64 done[kRing] = {};
    ID3D12GraphicsCommandList* cl = nullptr;
    ID3D12Fence* fence = nullptr;
    UINT64 fenceValue = 0;
    HANDLE event = nullptr;
    UINT slot = 0;
    uint64_t runs = 0;
    float depthValue = 1.0f;
    bool depthWritten = false;   // the constant depth is written by the first run only
    bool times = false;          // FFX_XEFG_TIMES=1: the optical flow's own GPU time in the log
    // GPU time: 3 time stamps per slot (start, after the optical flow, after the expansion)
    ID3D12QueryHeap* qh = nullptr;
    ID3D12Resource* qrb = nullptr;
    bool qpending[kRing] = {};
    double tickMs = 0;
    double sumOf = 0, sumEx = 0, maxAll = 0;
    uint32_t nT = 0;
    // GPU frame time (OflowFrameStart .. OflowRun): 2 time stamps per frame, kFrames frames in flight
    static const UINT kFrames = 8;   // more than a game runs ahead, so OflowFrameStart rarely skips a sample
    ID3D12QueryHeap* tq = nullptr;
    ID3D12Resource* trb = nullptr;
    ID3D12CommandAllocator* tAlloc[kFrames] = {};
    ID3D12GraphicsCommandList* tCl = nullptr;
    UINT64 tWait[kFrames] = {};      // fence value after which frame k's lists and results are done
    bool tPending[kFrames] = {};     // frame k has both time stamps
    uint64_t tFrame = 0;
    int tOpen = -1;                  // the frame started and not yet closed
    bool tFailed = false;            // the timer could not be made: not tried again
    std::vector<float> gpuFrames;
};

namespace {

ID3D12Resource* Tex(ID3D12Device* d, uint32_t w, uint32_t h, DXGI_FORMAT f, D3D12_RESOURCE_STATES s, const wchar_t* n)
{
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w;
    rd.Height = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = f;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* r = nullptr;
    if (FAILED(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, s, nullptr, IID_PPV_ARGS(&r))))
        return nullptr;
    r->SetName(n);
    return r;
}

void Barrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER br = {};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition.pResource = r;
    br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    br.Transition.StateBefore = a;
    br.Transition.StateAfter = b;
    cl->ResourceBarrier(1, &br);
}

// a view format for a swap chain buffer (typeless and sRGB read as UNORM: the optical flow wants the encoded values)
DXGI_FORMAT SrvFormat(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
        return f;
    }
}

struct Transition {
    ID3D12Resource* r;
    D3D12_RESOURCE_STATES a, b;
};

// several transitions in one ResourceBarrier call (vkd3d makes one pipeline barrier of it)
void Barriers(ID3D12GraphicsCommandList* cl, std::initializer_list<Transition> ts)
{
    D3D12_RESOURCE_BARRIER b[4] = {};
    UINT n = 0;
    for (const Transition& t : ts) {
        if (!t.r || t.a == t.b || n == 4)
            continue;
        b[n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[n].Transition.pResource = t.r;
        b[n].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b[n].Transition.StateBefore = t.a;
        b[n].Transition.StateAfter = t.b;
        n++;
    }
    if (n)
        cl->ResourceBarrier(n, b);
}

template <class T> void Rel(T*& p)
{
    if (p)
        p->Release();
    p = nullptr;
}
}  // namespace

// The states the two outputs are left in after each run (what the XeSS-FG tag must say).
const D3D12_RESOURCE_STATES kOflowOutState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

void OflowDestroy(Oflow* o)
{
    if (!o)
        return;
    if (o->fence && o->fenceValue && o->fence->GetCompletedValue() < o->fenceValue) {
        o->fence->SetEventOnCompletion(o->fenceValue, o->event);
        WaitForSingleObject(o->event, 2000);
    }
    if (o->ctxMade)
        ffxOpticalflowContextDestroy(&o->ctx);
    Rel(o->qh), Rel(o->qrb), Rel(o->lowres), Rel(o->down), Rel(o->tq), Rel(o->trb), Rel(o->tCl);
    for (auto& a : o->tAlloc)
        Rel(a);
    Rel(o->vec), Rel(o->scd), Rel(o->mv), Rel(o->depth), Rel(o->rs), Rel(o->pso), Rel(o->heap), Rel(o->cl), Rel(o->fence);
    for (auto& a : o->alloc)
        Rel(a);
    if (o->event)
        CloseHandle(o->event);
    Rel(o->dev);
    delete o;
}

Oflow* OflowCreate(ID3D12Device* dev, uint32_t w, uint32_t h, float depthValue)
{
    auto* o = new Oflow();
    o->dev = dev;
    dev->AddRef();
    o->w = w;
    o->h = h;
    o->depthValue = depthValue;
    char env[32];
    o->times = GetEnvironmentVariableA("FFX_XEFG_TIMES", env, sizeof env) && atoi(env) != 0;
    if (GetEnvironmentVariableA("FFX_XEFG_OFLOW_DIV", env, sizeof env))
        o->div = (uint32_t)atoi(env);
    if (o->div < 1 || o->div > 4)
        o->div = 1;
    o->sw = (w + o->div - 1) / o->div;
    o->sh = (h + o->div - 1) / o->div;
    auto fail = [&](const char* what, long code) {
        Log("oflow: %s failed (0x%lx)", what, (unsigned long)code);
        OflowDestroy(o);
        return (Oflow*)nullptr;
    };

    o->scratch.resize(ffxGetScratchMemorySizeDX12(1));
    FfxErrorCode e = ffxGetInterfaceDX12(&o->iface, ffxGetDeviceDX12(dev), o->scratch.data(), o->scratch.size(), 1);
    if (e != FFX_OK)
        return fail("ffxGetInterfaceDX12", e);
    FfxOpticalflowContextDescription cd = {};
    cd.backendInterface = o->iface;
    cd.resolution = {o->sw, o->sh};
    e = ffxOpticalflowContextCreate(&o->ctx, &cd);
    if (e != FFX_OK)
        return fail("ffxOpticalflowContextCreate", e);
    o->ctxMade = true;

    FfxOpticalflowSharedResourceDescriptions sr = {};
    ffxOpticalflowGetSharedResourceDescriptions(&o->ctx, &sr);
    o->bw = sr.opticalFlowVector.resourceDescription.width;
    o->bh = sr.opticalFlowVector.resourceDescription.height;
    const D3D12_RESOURCE_STATES uav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    o->vec = Tex(dev, o->bw, o->bh, DXGI_FORMAT_R16G16_SINT, uav, L"oflow_vector");
    o->scd = Tex(dev, 3, 1, DXGI_FORMAT_R32_UINT, uav, L"oflow_scd");
    o->mv = Tex(dev, w, h, DXGI_FORMAT_R16G16_FLOAT, kOflowOutState, L"oflow_mv");
    o->depth = Tex(dev, w, h, DXGI_FORMAT_R32_FLOAT, kOflowOutState, L"oflow_depth");
    if (o->div > 1)
        o->lowres = Tex(dev, o->sw, o->sh, DXGI_FORMAT_R16G16B16A16_FLOAT,   // float: scRGB values above 1 stay
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, L"oflow_small");
    if (!o->vec || !o->scd || !o->mv || !o->depth || (o->div > 1 && !o->lowres))
        return fail("CreateCommittedResource", 0);

    // root signature: t0 = block vectors, u0 = motion vectors, u1 = depth (one table), b0 = 4 constants
    D3D12_DESCRIPTOR_RANGE rg[2] = {};
    rg[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
    rg[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 1};
    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable = {2, rg};
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[1].Constants = {0, 0, 8};
    D3D12_ROOT_SIGNATURE_DESC rsd = {2, rp};
    ID3DBlob *blob = nullptr, *err = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
    if (FAILED(hr))
        return fail("D3D12SerializeRootSignature", hr);
    hr = dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&o->rs));
    blob->Release();
    if (FAILED(hr))
        return fail("CreateRootSignature", hr);
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = o->rs;
    pd.CS = {g_oflowExpandCs, sizeof(g_oflowExpandCs)};
    hr = dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&o->pso));
    if (FAILED(hr))
        return fail("CreateComputePipelineState", hr);
    pd.CS = {g_oflowDownCs, sizeof(g_oflowDownCs)};
    if (o->div > 1 && FAILED(hr = dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&o->down))))
        return fail("CreateComputePipelineState (down)", hr);

    D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3 + 3 * kRing,
                                     D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    hr = dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&o->heap));
    if (FAILED(hr))
        return fail("CreateDescriptorHeap", hr);
    o->inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE h0 = o->heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = DXGI_FORMAT_R16G16_SINT;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    dev->CreateShaderResourceView(o->vec, &sv, h0);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {};
    uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uv.Format = DXGI_FORMAT_R16G16_FLOAT;
    h0.ptr += o->inc;
    dev->CreateUnorderedAccessView(o->mv, nullptr, &uv, h0);
    uv.Format = DXGI_FORMAT_R32_FLOAT;
    h0.ptr += o->inc;
    dev->CreateUnorderedAccessView(o->depth, nullptr, &uv, h0);
    uv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    for (UINT i = 0; o->lowres && i < kRing; ++i) {
        D3D12_CPU_DESCRIPTOR_HANDLE hs = o->heap->GetCPUDescriptorHandleForHeapStart();
        hs.ptr += (SIZE_T)(4 + 3 * i) * o->inc;
        dev->CreateUnorderedAccessView(o->lowres, nullptr, &uv, hs);
        hs.ptr += o->inc;
        dev->CreateUnorderedAccessView(o->lowres, nullptr, &uv, hs);
    }

    for (auto& a : o->alloc)
        if (FAILED(hr = dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a))))
            return fail("CreateCommandAllocator", hr);
    hr = dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, o->alloc[0], nullptr, IID_PPV_ARGS(&o->cl));
    if (FAILED(hr))
        return fail("CreateCommandList", hr);
    o->cl->Close();
    if (FAILED(hr = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&o->fence))))
        return fail("CreateFence", hr);
    o->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Log("oflow: %ux%u, optical flow at %ux%u (1/%u), block vectors %ux%u, depth %.3f", w, h, o->sw, o->sh, o->div, o->bw,
        o->bh, depthValue);
    return o;
}

void OflowFrameStart(Oflow* o, ID3D12CommandQueue* q)
{
    if (!o || o->tFailed)
        return;
    const UINT K = Oflow::kFrames;
    if (!o->tq) {
        UINT64 f = 0;
        D3D12_QUERY_HEAP_DESC qd = {D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2 * K, 0};
        D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = 16 * K;
        bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bool ok = SUCCEEDED(q->GetTimestampFrequency(&f)) && f &&
                  SUCCEEDED(o->dev->CreateQueryHeap(&qd, IID_PPV_ARGS(&o->tq))) &&
                  SUCCEEDED(o->dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                            IID_PPV_ARGS(&o->trb)));
        for (UINT i = 0; ok && i < K; ++i)
            ok = SUCCEEDED(o->dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&o->tAlloc[i])));
        ok = ok && SUCCEEDED(o->dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, o->tAlloc[0], nullptr,
                                                       IID_PPV_ARGS(&o->tCl)));
        if (!ok) {
            Log("oflow: no GPU frame timer");
            Rel(o->tq), Rel(o->trb), Rel(o->tCl);
            for (auto& a : o->tAlloc)
                Rel(a);
            o->tFailed = true;
            return;
        }
        o->tCl->Close();
        if (!o->tickMs)
            o->tickMs = 1000.0 / (double)f;
    }
    if (!o->trb || !o->tCl)
        return;
    UINT k = (UINT)(o->tFrame % K);
    // called inside the game's ExecuteCommandLists: never wait there. A slot still in flight (the game runs more than
    // kFrames ahead) skips this frame's sample; the slot is tried again next frame.
    if (o->tWait[k] && o->fence->GetCompletedValue() < o->tWait[k]) {
        o->tOpen = -1;   // OflowRun closes no older start with this frame's end
        return;
    }
    if (o->tPending[k]) {
        UINT64* t = nullptr;
        D3D12_RANGE r = {16 * k, 16 * (k + 1)};
        if (SUCCEEDED(o->trb->Map(0, &r, (void**)&t))) {
            const UINT64* v = t + 2 * k;
            if (v[1] > v[0] && o->gpuFrames.size() < 1024)
                o->gpuFrames.push_back((float)((v[1] - v[0]) * o->tickMs));
            D3D12_RANGE none = {0, 0};
            o->trb->Unmap(0, &none);
        }
        o->tPending[k] = false;
    }
    o->tAlloc[k]->Reset();
    o->tCl->Reset(o->tAlloc[k], nullptr);
    o->tCl->EndQuery(o->tq, D3D12_QUERY_TYPE_TIMESTAMP, 2 * k);
    o->tCl->Close();
    ID3D12CommandList* l = o->tCl;
    q->ExecuteCommandLists(1, &l);
    o->tWait[k] = ++o->fenceValue;
    q->Signal(o->fence, o->fenceValue);
    o->tOpen = (int)k;
    o->tFrame++;
}

int OflowTakeGpuFrames(Oflow* o, float* ms, int max)
{
    if (!o)
        return 0;
    int n = (int)o->gpuFrames.size() < max ? (int)o->gpuFrames.size() : max;
    for (int i = 0; i < n; ++i)
        ms[i] = o->gpuFrames[i];
    o->gpuFrames.erase(o->gpuFrames.begin(), o->gpuFrames.begin() + n);
    return n;
}

ID3D12Resource* OflowMotionVectors(Oflow* o) { return o ? o->mv : nullptr; }
ID3D12Resource* OflowDepth(Oflow* o) { return o ? o->depth : nullptr; }

bool OflowRun(Oflow* o, ID3D12CommandQueue* q, ID3D12Resource* color, D3D12_RESOURCE_STATES colorState, int transfer,
              bool reset)
{
    if (!o || !color)
        return false;
    UINT s = o->slot;
    o->slot = (o->slot + 1) % kRing;
    if (o->done[s] && o->fence->GetCompletedValue() < o->done[s]) {
        o->fence->SetEventOnCompletion(o->done[s], o->event);
        WaitForSingleObject(o->event, 1000);
    }
    if (o->times && !o->qh) {   // made on the first run: the queue gives the tick rate
        UINT64 f = 0;
        D3D12_QUERY_HEAP_DESC qd = {D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 3 * kRing, 0};
        D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = 8 * 3 * kRing;
        bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (SUCCEEDED(q->GetTimestampFrequency(&f)) && f &&
            SUCCEEDED(o->dev->CreateQueryHeap(&qd, IID_PPV_ARGS(&o->qh))) &&
            SUCCEEDED(o->dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                      nullptr, IID_PPV_ARGS(&o->qrb))))
            o->tickMs = 1000.0 / (double)f;
    }
    if (!o->tickMs) {   // the frame timer needs it too (OflowFrameStart sets it as well)
        UINT64 f = 0;
        if (SUCCEEDED(q->GetTimestampFrequency(&f)) && f)
            o->tickMs = 1000.0 / (double)f;
    }
    if (o->qpending[s] && o->qrb) {   // this slot's last run has finished (waited above)
        UINT64* t = nullptr;
        D3D12_RANGE r = {8 * 3 * s, 8 * 3 * (s + 1)};
        if (SUCCEEDED(o->qrb->Map(0, &r, (void**)&t))) {
            const UINT64* v = t + 3 * s;
            if (v[1] >= v[0] && v[2] >= v[1]) {
                double a = (v[1] - v[0]) * o->tickMs, b = (v[2] - v[1]) * o->tickMs;
                o->sumOf += a, o->sumEx += b, o->nT++;
                if (a + b > o->maxAll)
                    o->maxAll = a + b;
            }
            D3D12_RANGE none = {0, 0};
            o->qrb->Unmap(0, &none);
        }
        o->qpending[s] = false;
        if (o->nT >= 300) {
            Log("oflow: GPU time over %u runs: optical flow %.3f ms, expansion %.3f ms, max %.3f ms", o->nT,
                o->sumOf / o->nT, o->sumEx / o->nT, o->maxAll);
            o->sumOf = o->sumEx = o->maxAll = 0;
            o->nT = 0;
        }
    }
    o->alloc[s]->Reset();
    o->cl->Reset(o->alloc[s], nullptr);
    if (o->qh)
        o->cl->EndQuery(o->qh, D3D12_QUERY_TYPE_TIMESTAMP, 3 * s);

    FfxOpticalflowDispatchDescription dd = {};
    dd.commandList = ffxGetCommandListDX12(o->cl);
    if (o->lowres) {
        // the final image at 1/div size, in its own encoding
        D3D12_RESOURCE_DESC cd = color->GetDesc();
        D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Format = SrvFormat(cd.Format);
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE hc = o->heap->GetCPUDescriptorHandleForHeapStart();
        hc.ptr += (SIZE_T)(3 + 3 * s) * o->inc;
        o->dev->CreateShaderResourceView(color, &sv, hc);
        Barriers(o->cl, {{color, colorState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
                         {o->lowres, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS}});
        o->cl->SetDescriptorHeaps(1, &o->heap);
        o->cl->SetComputeRootSignature(o->rs);
        o->cl->SetPipelineState(o->down);
        D3D12_GPU_DESCRIPTOR_HANDLE g = o->heap->GetGPUDescriptorHandleForHeapStart();
        g.ptr += (UINT64)(3 + 3 * s) * o->inc;
        o->cl->SetComputeRootDescriptorTable(0, g);
        UINT c[6] = {o->sw, o->sh, o->div, 0, (UINT)cd.Width, cd.Height};
        o->cl->SetComputeRoot32BitConstants(1, 6, c, 0);
        o->cl->Dispatch((o->sw + 7) / 8, (o->sh + 7) / 8, 1);
        Barriers(o->cl, {{o->lowres, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
                         {color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, colorState}});
        dd.color = ffxGetResourceDX12(o->lowres, ffxGetResourceDescriptionDX12(o->lowres), L"oflow_small",
                                      FFX_RESOURCE_STATE_COMPUTE_READ);
    } else {
        FfxResourceStates cs = colorState == D3D12_RESOURCE_STATE_PRESENT ? FFX_RESOURCE_STATE_PRESENT
                                                                          : FFX_RESOURCE_STATE_COMPUTE_READ;
        dd.color = ffxGetResourceDX12(color, ffxGetResourceDescriptionDX12(color), L"oflow_color", cs);
    }
    dd.opticalFlowVector = ffxGetResourceDX12(o->vec, ffxGetResourceDescriptionDX12(o->vec, FFX_RESOURCE_USAGE_UAV),
                                              L"oflow_vector", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    dd.opticalFlowSCD = ffxGetResourceDX12(o->scd, ffxGetResourceDescriptionDX12(o->scd, FFX_RESOURCE_USAGE_UAV),
                                           L"oflow_scd", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    dd.reset = reset;
    dd.backbufferTransferFunction = transfer;   // 1000 nits = 1.0 for PQ and scRGB
    dd.minMaxLuminance = {0.0f, 1000.0f};
    FfxErrorCode e = ffxOpticalflowContextDispatch(&o->ctx, &dd);

    // expand the block vectors (8x8 blocks) into the display-size motion vectors; the constant depth only once (it
    // stays in kOflowOutState, XeSS-FG only reads it)
    bool writeDepth = !o->depthWritten;
    ID3D12Resource* depthRes = writeDepth ? o->depth : nullptr;
    Barriers(o->cl, {{o->vec, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
                     {o->mv, kOflowOutState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS},
                     {depthRes, kOflowOutState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS}});
    o->cl->SetDescriptorHeaps(1, &o->heap);
    o->cl->SetComputeRootSignature(o->rs);
    o->cl->SetPipelineState(o->pso);
    o->cl->SetComputeRootDescriptorTable(0, o->heap->GetGPUDescriptorHandleForHeapStart());
    float dv = o->depthValue;
    float sc = (float)o->div;
    UINT c[6] = {o->w, o->h, *(UINT*)&dv, 8 * o->div, *(UINT*)&sc, writeDepth ? 1u : 0u};
    o->cl->SetComputeRoot32BitConstants(1, 6, c, 0);
    if (o->qh)
        o->cl->EndQuery(o->qh, D3D12_QUERY_TYPE_TIMESTAMP, 3 * s + 1);
    o->cl->Dispatch((o->w + 7) / 8, (o->h + 7) / 8, 1);
    Barriers(o->cl, {{o->vec, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS},
                     {o->mv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kOflowOutState},
                     {depthRes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kOflowOutState}});
    int tk = o->tOpen;
    if (tk >= 0 && o->tq) {   // the frame's end on the GPU: the game's work and ours are done
        o->cl->EndQuery(o->tq, D3D12_QUERY_TYPE_TIMESTAMP, 2 * tk + 1);
        o->cl->ResolveQueryData(o->tq, D3D12_QUERY_TYPE_TIMESTAMP, 2 * tk, 2, o->trb, 16 * tk);
        o->tOpen = -1;
    } else {
        tk = -1;
    }
    if (o->qh) {
        o->cl->EndQuery(o->qh, D3D12_QUERY_TYPE_TIMESTAMP, 3 * s + 2);
        o->cl->ResolveQueryData(o->qh, D3D12_QUERY_TYPE_TIMESTAMP, 3 * s, 3, o->qrb, 8 * 3 * s);
        o->qpending[s] = true;
    }
    HRESULT hr = o->cl->Close();
    if (FAILED(hr)) {
        Log("oflow: command list close failed 0x%lx (dispatch %d)", (unsigned long)hr, (int)e);
        return false;
    }
    ID3D12CommandList* l = o->cl;
    q->ExecuteCommandLists(1, &l);
    o->depthWritten = true;
    o->done[s] = ++o->fenceValue;
    q->Signal(o->fence, o->fenceValue);
    if (tk >= 0) {
        o->tWait[tk] = o->fenceValue;
        o->tPending[tk] = true;
    }
    if (++o->runs <= 3 || o->runs % 600 == 0 || e != FFX_OK)
        Log("oflow: run #%llu color %p state 0x%x transfer %d -> dispatch %d", (unsigned long long)o->runs,
            (void*)color, (unsigned)colorState, transfer, (int)e);
    return e == FFX_OK;
}

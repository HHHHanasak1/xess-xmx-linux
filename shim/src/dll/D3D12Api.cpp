#include "Stdafx.h"
#include "Trace.h"
#include <string>
#include <vector>
#include <atomic>

static const unsigned char kDummyCS[] = {
#include "dummy_cs_bytes.inc"
};
static const unsigned char kDummyUAV[] = {
#include "dummy_uav_bytes.inc"
};
#include "xess_dummies.inc"
static unsigned long long Fnv1a64(const void* a, size_t na, const void* b, size_t nb)
{
    unsigned long long h = 14695981039346656037ull;
    const unsigned char* p = (const unsigned char*)a; for (size_t i = 0; i < na; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    p = (const unsigned char*)b; for (size_t i = 0; i < nb; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}
static ID3D12Device* DevOf(INTCExtensionContext* c) { return (c && c->m_pD3D12ExtensionContext) ? c->m_pD3D12ExtensionContext->m_pAppDevice.Get() : nullptr; }


// frame generation pacing hints (XeFG calls these every frame): IGDEXT_FGPACE_OK=1 answers S_OK instead of E_NOTIMPL, IGDEXT_FGPACE_LOG=1 logs the values to C:\igdext_fgpace.log
static void FgPaceLog(const char* what, unsigned v)
{
    static int on = -1; static LONG n = 0;
    if (on < 0) { char b[8]; on = GetEnvironmentVariableA("IGDEXT_FGPACE_LOG", b, sizeof(b)) > 0; }
    static LONG zeros = 0;
    if (!on) return;
    if (v == 0 && InterlockedIncrement(&zeros) > 30) return;
    if (InterlockedIncrement(&n) > 1500) return;
    FILE* f = nullptr; if (fopen_s(&f, "C:\\igdext_fgpace.log", "ab") != 0 || !f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "%02d:%02d:%02d.%03d [%lu] %s %u\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, GetCurrentThreadId(), what, v);
    fclose(f);
}
static bool FgPaceOk() { static int ok = -1; if (ok < 0) { char b[8]; ok = GetEnvironmentVariableA("IGDEXT_FGPACE_OK", b, sizeof(b)) > 0; } return ok > 0; }

// Software implementation of the Intel driver's frame generation pacing. XeFG issues numGenerated+1 presents per application frame, all tagged with the
// same present sequence number (the application thread first, then the XeSS-FG thread for the generated ones) and expects the driver to space them evenly
// over the frame interval. IGDEXT_FGPACE_SLEEP=1: the k-th present of a sequence waits until t0 + k * T / (numGenerated + 1).
static bool FgPaceSleepOn() { static int on = -1; if (on < 0) { char b[8]; on = GetEnvironmentVariableA("IGDEXT_FGPACE_SLEEP", b, sizeof(b)) > 0 && atoi(b); } return on > 0; }
static double NowMs() { static LARGE_INTEGER f; if (!f.QuadPart) QueryPerformanceFrequency(&f); LARGE_INTEGER c; QueryPerformanceCounter(&c); return c.QuadPart * 1000.0 / f.QuadPart; }
static CRITICAL_SECTION g_fgCs; static INIT_ONCE g_fgOnce = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK FgInit(PINIT_ONCE, PVOID, PVOID*) { InitializeCriticalSection(&g_fgCs); return TRUE; }
struct FgSeq { UINT seq; double t0; int k; };
static FgSeq g_fgRing[8]; static UINT g_fgLast = 0; static double g_fgT = 0; static UINT g_fgNumGen = 0;
static void FgPaceWait(UINT seq)
{
    InitOnceExecuteOnce(&g_fgOnce, FgInit, nullptr, nullptr);
    double now = NowMs(); int k = 0; double t0 = now, T = 0; UINT ng = 0;
    EnterCriticalSection(&g_fgCs);
    FgSeq& e = g_fgRing[seq & 7];
    if (e.seq != seq || e.t0 == 0) {
        e.seq = seq; e.t0 = now; e.k = 0;
        if (g_fgLast && seq == g_fgLast + 1) {
            const FgSeq& p = g_fgRing[g_fgLast & 7];
            double dt = now - p.t0;
            if (p.seq == g_fgLast && dt > 3.0 && dt < 250.0) g_fgT = g_fgT > 0 ? 0.8 * g_fgT + 0.2 * dt : dt; else if (dt >= 250.0) g_fgT = 0;
        } else g_fgT = 0;
        g_fgLast = seq;
    } else k = ++e.k;
    t0 = e.t0; T = g_fgT; ng = g_fgNumGen;
    LeaveCriticalSection(&g_fgCs);
    if (k <= 0 || T <= 0 || ng == 0) return;
    double target = t0 + k * T / (ng + 1);
    for (;;) {
        double rem = target - NowMs();
        if (rem <= 0) break;
        if (rem > 1.5) Sleep(1); else YieldProcessor();
    }
}

extern "C" {

void _INTC_D3D12_BuildRaytracingAccelerationStructure(INTCExtensionContext* pExtensionContext, ID3D12GraphicsCommandList* pCommandList, const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* pDesc, UINT NumPostbuildInfoDescs, const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* pPostbuildInfoDescs, const INTC_D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC_INSTANCE_COMPARISON_DATA* pComparisonDataDesc)
{
    TraceF("BuildRaytracingAccelerationStructure (not implemented)");
}

void _INTC_D3D12_BuildRaytracingAccelerationStructure_Host(INTCExtensionContext* pExtensionContext, const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* pDesc, const D3D12_GPU_VIRTUAL_ADDRESS* pInstanceGPUVAs, UINT NumInstances)
{
    TraceF("BuildRaytracingAccelerationStructure_Host (not implemented)");
}

void _INTC_D3D12_CopyRaytracingAccelerationStructure_Host(INTCExtensionContext* pExtensionContext, void* DestAccelerationStructureData, const void* SourceAccelerationStructureData, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE Mode)
{
    TraceF("CopyRaytracingAccelerationStructure_Host (not implemented)");
}

HRESULT _INTC_D3D12_CreateHostRTASResource(INTCExtensionContext* pExtensionContext, size_t SizeInBytes, DWORD Flags, REFIID riidResource, void** ppvResource)
{
    TraceF("CreateHostRTASResource (not implemented)");
    return E_NOTIMPL;
}

HRESULT _INTC_D3D12_CreateStateObject(INTCExtensionContext* pExtensionContext, const INTC_D3D12_STATE_OBJECT_DESC* pDesc, REFIID riid, void** ppPipelineState)
{
    TraceF("CreateStateObject (not implemented)");
    return E_NOTIMPL;
}

void _INTC_D3D12_EmitRaytracingAccelerationStructurePostbuildInfo_Host(INTCExtensionContext* pExtensionContext, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_TYPE InfoType, void* DestBuffer, const void* SourceRTAS)
{
    TraceF("EmitRaytracingAccelerationStructurePostbuildInfo_Host (not implemented)");
}

HRESULT _INTC_D3D12_GetCachedBlob(INTCExtensionContext* pExtensionContext, ID3D12GraphicsCommandList* pCommandList, ID3D12PipelineState* pPipelineState, ID3DBlob** ppBlob, INTC_D3D12_CACHED_BLOB_FLAGS flags)
{
    TraceF("GetCachedBlob (not implemented)");
    return E_NOTIMPL;
}

uint64_t _INTC_D3D12_GetCommandListHandle(INTCExtensionContext* pExtensionContext, void* pCommandList)
{
    TraceF("GetCommandListHandle (not implemented)");
    return {};
}

HRESULT _INTC_D3D12_GetCurrentShaderHeapUsage(INTCExtensionContext* pExtensionContext, UINT64* pShaderHeapUsage)
{
    TraceF("GetCurrentShaderHeapUsage (not implemented)");
    return E_NOTIMPL;
}

HRESULT _INTC_D3D12_GetDisplayTelemetry(INTCExtensionContext* pExtensionContext, void* pTelemetryData, UINT size)
{
    TraceF("GetDisplayTelemetry (not implemented)");
    return E_NOTIMPL;
}

void _INTC_D3D12_GetRaytracingAccelerationStructurePrebuildInfo(INTCExtensionContext* pExtensionContext, const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS* pDesc, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO* pInfo, const INTC_D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC_INSTANCE_COMPARISON_DATA* pComparisonDataDesc)
{
    TraceF("GetRaytracingAccelerationStructurePrebuildInfo (not implemented)");
}

void _INTC_D3D12_GetRaytracingAccelerationStructurePrebuildInfo_Host(INTCExtensionContext* pExtensionContext, const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS* pDesc, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO* pInfo)
{
    TraceF("GetRaytracingAccelerationStructurePrebuildInfo_Host (not implemented)");
}

HRESULT _INTC_D3D12_LatencyReductionExt(INTCExtensionContext* pExtensionContext, uint32_t version, BOOL latencyReductionEnabled, BOOL renderSubmitTimingsEnabled, uint32_t timingSlots)
{
    TraceF("LatencyReductionExt (not implemented)");
    return E_NOTIMPL;
}

HRESULT _INTC_D3D12_LatencyReductionGetRenderSubmitTimingsBuffers(INTCExtensionContext* pExtensionContext, void** ppRenderSubmitCpuTimings, void** ppRenderSubmitGpuTimings)
{
    TraceF("LatencyReductionGetRenderSubmitTimingsBuffers (not implemented)");
    return E_NOTIMPL;
}

HRESULT _INTC_D3D12_RegisterApplicationCallbacks1(const INTC_D3D12_API_CALLBACKS1* pCallbacks)
{
    TraceF("RegisterApplicationCallbacks1 (not implemented)");
    return E_NOTIMPL;
}

HRESULT _INTC_D3D12_RenderSubmitStart(INTCExtensionContext* pExtensionContext, uint32_t frameId)
{
    TraceF("RenderSubmitStart (not implemented)");
    return E_NOTIMPL;
}

HRESULT _INTC_D3D12_SetDeviceParams(INTCDeviceParams* pDeviceParams)
{
    TraceF("SetDeviceParams (not implemented)");
    return E_NOTIMPL;
}

void _INTC_D3D12_SetDriverEventMetadata(INTCExtensionContext* pExtensionContext, ID3D12GraphicsCommandList* pCommandList, UINT64 Metadata)
{
    TraceF("SetDriverEventMetadata (not implemented)");
}

void _INTC_D3D12_SetEventMarker(INTCExtensionContext* pExtensionContext, ID3D12GraphicsCommandList* pCommandList, uint32_t eventType, const void* marker, uint32_t markerSize)
{
    TraceF("SetEventMarker (not implemented)");
}

HRESULT _INTC_D3D12_SetNumGeneratedFrames(INTCExtensionContext* pExtensionContext, UINT NumFrames)
{
    TraceF("SetNumGeneratedFrames (not implemented)");
    FgPaceLog("SetNumGeneratedFrames", NumFrames);
    if (FgPaceSleepOn()) { g_fgNumGen = NumFrames; return S_OK; }
    return FgPaceOk() ? S_OK : E_NOTIMPL;
}

HRESULT _INTC_D3D12_SetPresentSequenceNumber(INTCExtensionContext* pExtensionContext, UINT PresentSequenceNumber)
{
    TraceF("SetPresentSequenceNumber (not implemented)");
    FgPaceLog("SetPresentSequenceNumber", PresentSequenceNumber);
    if (FgPaceSleepOn()) { FgPaceWait(PresentSequenceNumber); return S_OK; }
    return FgPaceOk() ? S_OK : E_NOTIMPL;
}

void _INTC_D3D12_TransferHostRTAS(INTCExtensionContext* pExtensionContext, ID3D12GraphicsCommandList* pCommandList, D3D12_GPU_VIRTUAL_ADDRESS DestAccelerationStructureData, D3D12_GPU_VIRTUAL_ADDRESS SrcAccelerationStructureData, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE Mode)
{
    TraceF("TransferHostRTAS (not implemented)");
}


HRESULT _INTC_D3D12_CreateComputePipelineState(INTCExtensionContext* ctx, const INTC_D3D12_COMPUTE_PIPELINE_STATE_DESC* d, REFIID riid, void** pp)
{
    static std::atomic<int> counter{0};
    const int id = counter++;
    char b1[400], b2[400];
    TraceF("CreateComputePipelineState #%d: inputType=%d csLen=%zu csPtr=%p d3d12desc=%p", id, (int)d->ShaderInputType,
           (size_t)d->CS.BytecodeLength, d->CS.pShaderBytecode, d->pD3D12Desc);
    TraceF("  CompileOptions : %s", SafeStr(d->CompileOptions, b1, sizeof(b1)));
    TraceF("  InternalOptions: %s", SafeStr(d->InternalOptions, b2, sizeof(b2)));
    if (d->pD3D12Desc)
        TraceF("  rootSig=%p nodeMask=%u cachedBlobLen=%zu flags=0x%x", d->pD3D12Desc->pRootSignature, d->pD3D12Desc->NodeMask,
               (size_t)d->pD3D12Desc->CachedPSO.CachedBlobSizeInBytes, (unsigned)d->pD3D12Desc->Flags);
    char name[96];
    snprintf(name, sizeof(name), "cs_%04d_type%d.bin", id, (int)d->ShaderInputType);
    if (d->CS.pShaderBytecode && d->CS.BytecodeLength) DumpBlob(name, d->CS.pShaderBytecode, d->CS.BytecodeLength);
    if (d->CompileOptions) { snprintf(name, sizeof(name), "cs_%04d_options.txt", id); DumpBlob(name, d->CompileOptions, strlen((const char*)d->CompileOptions)); }
    if (d->CS.pShaderBytecode && d->CS.BytecodeLength >= 4)
    {
        const unsigned char* p = (const unsigned char*)d->CS.pShaderBytecode;
        TraceF("  first bytes: %02x %02x %02x %02x  %02x %02x %02x %02x", p[0], p[1], p[2], p[3],
               d->CS.BytecodeLength > 7 ? p[4] : 0, d->CS.BytecodeLength > 7 ? p[5] : 0, d->CS.BytecodeLength > 7 ? p[6] : 0, d->CS.BytecodeLength > 7 ? p[7] : 0);
    }
    ID3D12Device* dev = DevOf(ctx);
    if (dev && d->pD3D12Desc && (d->ShaderInputType == NONE || d->ShaderInputType == HLSL))
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc = *d->pD3D12Desc;
        if (d->CS.pShaderBytecode) desc.CS = d->CS;
        TraceF("  -> pass-through to the native D3D12 device");
        return dev->CreateComputePipelineState(&desc, riid, pp);
    }
    if (dev && d->pD3D12Desc)
    {
        // experiment: hand back a no-op pipeline (same root signature) so XeSS keeps requesting the rest of its kernels
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc = *d->pD3D12Desc;
        desc.CS.pShaderBytecode = kDummyUAV; desc.CS.BytecodeLength = sizeof(kDummyUAV);
        {
            const char* opts = d->CompileOptions ? (const char*)d->CompileOptions : "";
            const unsigned long long h = Fnv1a64(d->CS.pShaderBytecode, d->CS.BytecodeLength, opts, strlen(opts));
            for (const XessDummy& xd : kXessDummies)
                if (xd.hash == h) { desc.CS.pShaderBytecode = xd.bytes; desc.CS.BytecodeLength = xd.size; TraceF("  kernel hash %016llx -> dummy id %d (workgroup %d,%d,5)", h, xd.id, 1 + xd.id % 16, 1 + xd.id / 16); break; }
        }
        HRESULT hr = dev->CreateComputePipelineState(&desc, riid, pp);
        TraceF("  -> DUMMY uav(u0) pipeline returned (hr=0x%08x)", (unsigned)hr);
        if (FAILED(hr))
        {
            desc.CS.pShaderBytecode = kDummyCS; desc.CS.BytecodeLength = sizeof(kDummyCS);
            hr = dev->CreateComputePipelineState(&desc, riid, pp);
            TraceF("  -> DUMMY no-op pipeline returned (hr=0x%08x)", (unsigned)hr);
        }
        return hr;
    }
    TraceF("  -> NOT SUPPORTED (returning E_NOTIMPL)");
    return E_NOTIMPL;
}

HRESULT _INTC_D3D12_CreateCommandQueue(INTCExtensionContext* ctx, const INTC_D3D12_COMMAND_QUEUE_DESC* d, REFIID riid, void** pp)
{
    ID3D12Device* dev = DevOf(ctx);
    TraceF("CreateCommandQueue: throttle=%d type=%d -> native", d ? (int)d->CommandThrottlePolicy : -1, (d && d->pD3D12Desc) ? (int)d->pD3D12Desc->Type : -1);
    if (!dev || !d || !d->pD3D12Desc) return E_INVALIDARG;
    return dev->CreateCommandQueue(d->pD3D12Desc, riid, pp);
}

HRESULT _INTC_D3D12_CheckFeatureSupport(INTCExtensionContext* ctx, INTC_D3D12_FEATURES f, void* data, UINT size)
{
    TraceF("CheckFeatureSupport feature=%d size=%u", (int)f, size);
    if (!data) return E_INVALIDARG;
    if (f == INTC_D3D12_FEATURE_D3D12_OPTIONS1 && size >= sizeof(INTC_D3D12_FEATURE_DATA_D3D12_OPTIONS1))
    {
        auto* o = (INTC_D3D12_FEATURE_DATA_D3D12_OPTIONS1*)data;
        int xmx = 1, dl = 1, em = 0;
        { char b[32]; if (GetEnvironmentVariableA("IGDEXT_OPTIONS1", b, sizeof(b)) > 0) sscanf(b, "%d,%d,%d", &xmx, &dl, &em); }
        o->XMXEnabled = xmx ? TRUE : FALSE; o->DLBoostEnabled = dl ? TRUE : FALSE; o->EmulatedTyped64bitAtomics = em ? TRUE : FALSE;
        TraceF("  OPTIONS1 -> XMXEnabled=%d DLBoostEnabled=%d EmulatedTyped64bitAtomics=%d", xmx, dl, em);
        return S_OK;
    }
    if (f == INTC_D3D12_FEATURE_D3D12_OPTIONS2 && size >= sizeof(INTC_D3D12_FEATURE_DATA_D3D12_OPTIONS2))
    {
        auto* o = (INTC_D3D12_FEATURE_DATA_D3D12_OPTIONS2*)data;
        // IGDEXT_OPTIONS2=<simd16>,<lsc>,<legacy> overrides the answer (experiment: which kernel variants XeSS then picks)
        // XeSS SR (extension context requested as HW level 3 / API 10): SIMD16Required=1 -> XeSS ships its SIMD16 + LSC-typed kernel
        // variant, the one Xe2/Xe3 can run natively (the SIMD8/legacy-typed variant produced the stippled motion ghost).
        // XeSS-FG (HW level 5 / API 11): SIMD16Required=0 keeps its 24 CM kernels (with 1 it silently switches to a non-CM path).
        const bool fgCtx = ctx && ctx->m_pD3D12ExtensionContext && ctx->m_pD3D12ExtensionContext->m_SupportedExtVersion.HWFeatureLevel >= 5;
        int simd16 = fgCtx ? 0 : 1, lsc = 1, legacy = 0;
        { char b[32]; if (GetEnvironmentVariableA(fgCtx ? "IGDEXT_OPTIONS2_FG" : "IGDEXT_OPTIONS2", b, sizeof(b)) > 0) sscanf(b, "%d,%d,%d", &simd16, &lsc, &legacy); }
        o->SIMD16Required = simd16 ? TRUE : FALSE; o->LSCSupported = lsc ? TRUE : FALSE; o->LegacyTranslationRequired = legacy ? TRUE : FALSE;
        TraceF("  OPTIONS2 (%s ctx) -> SIMD16Required=%d LSCSupported=%d LegacyTranslationRequired=%d", fgCtx ? "FG" : "SR", simd16, lsc, legacy);
        return S_OK;
    }
    memset(data, 0, size);
    return S_OK;
}

HRESULT _INTC_D3D12_SetFeatureSupport(INTCExtensionContext*, INTC_D3D12_FEATURE* f)
{
    TraceF("SetFeatureSupport EmulatedTyped64bitAtomics=%d", f ? (int)f->EmulatedTyped64bitAtomics : -1);
    return S_OK;
}

HRESULT _INTC_D3D12_CreateReservedResource(INTCExtensionContext* ctx, const INTC_D3D12_RESOURCE_DESC* d, D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE* c, REFIID riid, void** pp)
{
    TraceF("CreateReservedResource -> native");
    ID3D12Device* dev = DevOf(ctx);
    return (dev && d && d->pD3D12Desc) ? dev->CreateReservedResource(d->pD3D12Desc, s, c, riid, pp) : E_INVALIDARG;
}

HRESULT _INTC_D3D12_CreateCommittedResource(INTCExtensionContext* ctx, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf, const INTC_D3D12_RESOURCE_DESC_0001* d, D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE* c, REFIID riid, void** pp)
{
    TraceF("CreateCommittedResource -> native");
    ID3D12Device* dev = DevOf(ctx);
    return (dev && d && d->pD3D12Desc) ? dev->CreateCommittedResource(hp, hf, d->pD3D12Desc, s, c, riid, pp) : E_INVALIDARG;
}

HRESULT _INTC_D3D12_CreateCommittedResource1(INTCExtensionContext* ctx, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf, const INTC_D3D12_RESOURCE_DESC_0002* d, D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE* c, REFIID riid, void** pp)
{
    TraceF("CreateCommittedResource1 -> native");
    ID3D12Device* dev = DevOf(ctx);
    return (dev && d && d->pD3D12Desc) ? dev->CreateCommittedResource(hp, hf, d->pD3D12Desc, s, c, riid, pp) : E_INVALIDARG;
}

HRESULT _INTC_D3D12_CreateHeap(INTCExtensionContext* ctx, const INTC_D3D12_HEAP_DESC* d, REFIID riid, void** pp)
{
    TraceF("CreateHeap -> native");
    ID3D12Device* dev = DevOf(ctx);
    return (dev && d && d->pD3D12Desc) ? dev->CreateHeap(d->pD3D12Desc, riid, pp) : E_INVALIDARG;
}

HRESULT _INTC_D3D12_CreatePlacedResource(INTCExtensionContext* ctx, ID3D12Heap* heap, UINT64 off, const INTC_D3D12_RESOURCE_DESC_0001* d, D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE* c, REFIID riid, void** pp)
{
    TraceF("CreatePlacedResource -> native");
    ID3D12Device* dev = DevOf(ctx);
    return (dev && d && d->pD3D12Desc) ? dev->CreatePlacedResource(heap, off, d->pD3D12Desc, s, c, riid, pp) : E_INVALIDARG;
}

D3D12_RESOURCE_ALLOCATION_INFO _INTC_D3D12_GetResourceAllocationInfo(INTCExtensionContext* ctx, UINT mask, UINT n, const INTC_D3D12_RESOURCE_DESC_0001* descs)
{
    TraceF("GetResourceAllocationInfo n=%u -> native", n);
    ID3D12Device* dev = DevOf(ctx);
    D3D12_RESOURCE_ALLOCATION_INFO r = {};
    if (!dev || !descs || !n) return r;
    std::vector<D3D12_RESOURCE_DESC> v(n);
    for (UINT i = 0; i < n; i++) v[i] = *descs[i].pD3D12Desc;
    return dev->GetResourceAllocationInfo(mask, n, v.data());
}

HRESULT _INTC_D3D12_AddShaderBinariesPath(INTCExtensionContext*, const wchar_t* p)   { char b[260] = {}; WideCharToMultiByte(CP_UTF8, 0, p ? p : L"", -1, b, sizeof(b) - 1, nullptr, nullptr); TraceF("AddShaderBinariesPath '%s'", b); return S_OK; }
HRESULT _INTC_D3D12_RemoveShaderBinariesPath(INTCExtensionContext*, const wchar_t* p) { char b[260] = {}; WideCharToMultiByte(CP_UTF8, 0, p ? p : L"", -1, b, sizeof(b) - 1, nullptr, nullptr); TraceF("RemoveShaderBinariesPath '%s'", b); return S_OK; }
HRESULT _INTC_D3D12_SetApplicationInfo(INTCExtensionAppInfo1*)                          { TraceF("SetApplicationInfo"); return S_OK; }
HRESULT _INTC_D3D12_GetLatencyReductionStatus(INTCExtensionContext*, INTC_D3D12_LATENCY_REDUCTION_STATUS* st) { TraceF("GetLatencyReductionStatus"); if (st) *st = {}; return S_OK; }
void    _INTC_D3D12_QueryCpuVisibleVidmem(INTCExtensionContext*, UINT64* total, UINT64* freeb) { TraceF("QueryCpuVisibleVidmem"); if (total) *total = 0; if (freeb) *freeb = 0; }

} // extern "C"

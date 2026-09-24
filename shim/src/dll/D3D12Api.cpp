#include "Stdafx.h"
#include "Trace.h"
#include "GpuInfo.h"
#include <string>
#include <vector>
#include <atomic>

static const unsigned char kDummyCS[] = {
#include "dummy_cs_bytes.inc"
};
static const unsigned char kDummyUAV[] = {
#include "dummy_uav_bytes.inc"
};
#ifdef XMX_STATIC_TABLE   // developer builds only (cmake -DXMX_STATIC_TABLE=ON)
#include "xess_dummies.inc"
#endif
#include "dyn_dummies.inc"
static unsigned long long Fnv1a64(const void* a, size_t na, const void* b, size_t nb)
{
    unsigned long long h = 14695981039346656037ull;
    const unsigned char* p = (const unsigned char*)a; for (size_t i = 0; i < na; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    p = (const unsigned char*)b; for (size_t i = 0; i < nb; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

// Every CM kernel gets a persistent id per prefix (C:\igdext_kernels\map.txt: "<id> <hash>"), its SPIR-V and compile options
// are written next to it as dyn_<id>.spv / dyn_<id>.opt, and the placeholder pipeline for that id is returned. The patched
// ANV finds the files through $WINEPREFIX/drive_c/igdext_kernels, looks the hash up in ~/.cache/xess-xmx/kernels and
// compiles the kernel there on a miss. When all ids are taken the map starts over (the compiled kernels are keyed by hash,
// so nothing is lost but the id assignment).
static const char* kDynDir = "C:\\igdext_kernels";
static CRITICAL_SECTION g_dynCs; static INIT_ONCE g_dynOnce = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK DynInit(PINIT_ONCE, PVOID, PVOID*) { InitializeCriticalSection(&g_dynCs); return TRUE; }
static bool WriteFileOnce(const char* path, const void* data, size_t size)
{
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return true;
    char tmp[MAX_PATH]; snprintf(tmp, sizeof(tmp), "%s.%lu.tmp", path, GetCurrentProcessId());
    FILE* f = nullptr; if (fopen_s(&f, tmp, "wb") != 0 || !f) return false;
    const bool ok = fwrite(data, 1, size, f) == size; fclose(f);
    if (!ok || !MoveFileExA(tmp, path, 0)) { DeleteFileA(tmp); return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES; }
    return true;
}
static int DynKernelId(unsigned long long hash, const void* spv, size_t spvLen, const char* opts)
{
    InitOnceExecuteOnce(&g_dynOnce, DynInit, nullptr, nullptr);
    EnterCriticalSection(&g_dynCs);
    CreateDirectoryA(kDynDir, nullptr);
    char mapPath[MAX_PATH]; snprintf(mapPath, sizeof(mapPath), "%s\\map.txt", kDynDir);
    int id = -1, maxId = -1;
    if (FILE* f = nullptr; fopen_s(&f, mapPath, "rb") == 0 && f)
    {
        int i; unsigned long long h; char line[128];
        while (fgets(line, sizeof(line), f))
            if (sscanf(line, "%d %llx", &i, &h) == 2) { if (i > maxId) maxId = i; if (h == hash) id = i; }
        fclose(f);
    }
    const int nDyn = (int)(sizeof(kDynDummies) / sizeof(kDynDummies[0]));
    if (id < 0)
    {
        id = maxId + 1;
        if (id >= nDyn)
        {
            // all placeholders in use: start a new map (old one kept as map.old.txt); ids are only names for this prefix
            char oldPath[MAX_PATH]; snprintf(oldPath, sizeof(oldPath), "%s\\map.old.txt", kDynDir);
            MoveFileExA(mapPath, oldPath, MOVEFILE_REPLACE_EXISTING);
            for (int i = 0; i < nDyn; ++i)
            {
                char q[MAX_PATH];
                snprintf(q, sizeof(q), "%s\\dyn_%d.spv", kDynDir, i); DeleteFileA(q);
                snprintf(q, sizeof(q), "%s\\dyn_%d.opt", kDynDir, i); DeleteFileA(q);
            }
            TraceF("  all %d placeholder ids used: map restarted", nDyn);
            id = 0;
        }
        if (FILE* f = nullptr; fopen_s(&f, mapPath, "ab") == 0 && f) { fprintf(f, "%d %016llx\n", id, hash); fclose(f); }
    }
    char p[MAX_PATH];
    snprintf(p, sizeof(p), "%s\\dyn_%d.spv", kDynDir, id); WriteFileOnce(p, spv, spvLen);
    snprintf(p, sizeof(p), "%s\\dyn_%d.opt", kDynDir, id); WriteFileOnce(p, opts, strlen(opts));
    LeaveCriticalSection(&g_dynCs);
    return id;
}
static ID3D12Device* DevOf(INTCExtensionContext* c) { return (c && c->m_pD3D12ExtensionContext) ? c->m_pD3D12ExtensionContext->m_pAppDevice.Get() : nullptr; }


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

HRESULT _INTC_D3D12_RegisterApplicationCallbacks1(const INTC_D3D12_API_CALLBACKS1* pCallbacks)
{
    TraceF("RegisterApplicationCallbacks1 (not implemented)");
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
    return E_NOTIMPL;
}

HRESULT _INTC_D3D12_SetPresentSequenceNumber(INTCExtensionContext* pExtensionContext, UINT PresentSequenceNumber)
{
    TraceF("SetPresentSequenceNumber (not implemented)");
    return E_NOTIMPL;
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
    TraceCallers("CreateComputePipelineState");
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
            bool known = false;
#ifdef XMX_STATIC_TABLE
            // IGDEXT_STATIC=1: kernels of the prebuilt table use their static ids (needs the kernels/ directory of a
            // developer build); by default every kernel goes through the run-time path
            static int useStatic = -1;
            if (useStatic < 0) { char b[8]; useStatic = GetEnvironmentVariableA("IGDEXT_STATIC", b, sizeof(b)) > 0 && atoi(b); }
            if (useStatic) for (const XessDummy& xd : kXessDummies)
                if (xd.hash == h) { desc.CS.pShaderBytecode = xd.bytes; desc.CS.BytecodeLength = xd.size; TraceF("  kernel hash %016llx -> dummy id %d (workgroup %d,%d,5)", h, xd.id, 1 + xd.id % 16, 1 + xd.id / 16); known = true; break; }
#endif
            if (!known && d->ShaderInputType == CM_SPIRV)
            {
                const int dyn = DynKernelId(h, d->CS.pShaderBytecode, d->CS.BytecodeLength, opts);
                if (dyn >= 0) { desc.CS.pShaderBytecode = kDynDummies[dyn].bytes; desc.CS.BytecodeLength = kDynDummies[dyn].size; TraceF("  kernel hash %016llx -> dynamic id %d", h, dyn); }
            }
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

// true when XeSS frame generation (libxess_fg.dll, whoever loaded it: the game or OptiScaler) is on the call stack.
// XeFG creates its extension context like XeSS SR does, so the context alone cannot tell the two apart.
static bool CalledFromXeFG()
{
    void* frames[32];
    const USHORT n = RtlCaptureStackBackTrace(1, 32, frames, nullptr);
    for (USHORT i = 0; i < n; ++i)
    {
        HMODULE m = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)frames[i], &m) || !m) continue;
        char path[MAX_PATH] = {};
        if (!GetModuleFileNameA(m, path, MAX_PATH)) continue;
        const char* base = strrchr(path, '\\'); base = base ? base + 1 : path;
        if (_strnicmp(base, "libxess_fg", 10) == 0) return true;
    }
    return false;
}

// Self-test, once per process, before XeSS is told to use CM kernels: a placeholder pipeline with workgroup (1, 1, 17)
// is created on the game's device; the patched ANV recognises it (magic value) and writes C:\igdext_kernels\anv_canary.
// If the file is not newer than this process, placeholders would not be replaced - the stock driver is in use (the
// session check fell back, the variables are missing) or vkd3d-proton changed how placeholders reach the driver - and
// XeSS must use its DP4a path instead of producing noise. IGDEXT_SKIP_SELFTEST=1 skips it.
static bool SelfTestOk(ID3D12Device* dev)
{
    static int result = -1;
    if (result >= 0) return result == 1;
    char b[8];
    if (GetEnvironmentVariableA("IGDEXT_SKIP_SELFTEST", b, sizeof(b)) > 0 && atoi(b)) { result = 1; return true; }
    result = 0;
    if (!dev) { TraceF("  self-test: no device"); return false; }
    typedef HRESULT (WINAPI *PFN_Serialize)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
    HMODULE d3d12 = GetModuleHandleA("d3d12.dll");
    auto serialize = d3d12 ? (PFN_Serialize)GetProcAddress(d3d12, "D3D12SerializeRootSignature") : nullptr;
    if (!serialize) { TraceF("  self-test: D3D12SerializeRootSignature not found"); return false; }
    D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0 };
    D3D12_ROOT_PARAMETER param = {};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param.DescriptorTable.NumDescriptorRanges = 1; param.DescriptorTable.pDescriptorRanges = &range;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 1, &param, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ID3DBlob* blob = nullptr; ID3DBlob* err = nullptr;
    ID3D12RootSignature* rs = nullptr; ID3D12PipelineState* pso = nullptr;
    HRESULT hr = serialize(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
    if (SUCCEEDED(hr)) hr = dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature), (void**)&rs);
    if (SUCCEEDED(hr))
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = rs; pd.CS.pShaderBytecode = kCanaryDummy; pd.CS.BytecodeLength = sizeof(kCanaryDummy);
        hr = dev->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState), (void**)&pso);
    }
    if (pso) pso->Release();
    if (rs) rs->Release();
    if (blob) blob->Release();
    if (err) err->Release();
    if (FAILED(hr)) { TraceF("  self-test: creating the test pipeline failed (hr=0x%08lx)", (unsigned long)hr); return false; }
    WIN32_FILE_ATTRIBUTE_DATA fa;
    FILETIME created, t1, t2, t3;
    if (!GetFileAttributesExA("C:\\igdext_kernels\\anv_canary", GetFileExInfoStandard, &fa) ||
        !GetProcessTimes(GetCurrentProcess(), &created, &t1, &t2, &t3))
    { TraceF("  self-test: no C:\\igdext_kernels\\anv_canary from the driver"); return false; }
    ULARGE_INTEGER m, c;
    m.LowPart = fa.ftLastWriteTime.dwLowDateTime; m.HighPart = fa.ftLastWriteTime.dwHighDateTime;
    c.LowPart = created.dwLowDateTime; c.HighPart = created.dwHighDateTime;
    const bool fresh = m.QuadPart + 2ull * 10000000ull >= c.QuadPart;   // 2 s slack for file time granularity
    TraceF("  self-test: %s", fresh ? "the patched driver recognised the test placeholder" : "anv_canary is older than this process");
    result = fresh ? 1 : 0;
    return fresh;
}

// Fallback decision, made when XeSS / XeFG create their extension context: if IGDEXT_FORCE_FALLBACK=1, a kernel compile
// failed earlier in this prefix (C:\igdext_kernels\compile_failed.txt, retried by the driver) or the self-test fails, the
// context is declined - exactly what XeSS sees on a system without the Intel extension (DP4a super resolution, generic
// frame generation). Answering LSCSupported=0 instead would switch XeSS frame generation off altogether.
// IGDEXT_IGNORE_FAILED=1 ignores the compile marker.
// The compile-failure marker (written and retried by the driver) is dropped when the game's XeSS libraries are newer than
// it: a game update brings new kernels, so the old failure says nothing about them.
static bool CompileFailedStillValid()
{
    WIN32_FILE_ATTRIBUTE_DATA mk;
    if (!GetFileAttributesExA("C:\\igdext_kernels\\compile_failed.txt", GetFileExInfoStandard, &mk)) return false;
    const char* libs[] = { "libxess.dll", "libxess_fg.dll" };
    for (const char* name : libs)
    {
        HMODULE m = GetModuleHandleA(name);
        char path[MAX_PATH];
        WIN32_FILE_ATTRIBUTE_DATA la;
        if (!m || !GetModuleFileNameA(m, path, MAX_PATH) || !GetFileAttributesExA(path, GetFileExInfoStandard, &la)) continue;
        if (CompareFileTime(&la.ftLastWriteTime, &mk.ftLastWriteTime) > 0)
        {
            TraceF("  %s is newer than compile_failed.txt (game update): marker removed, kernels are tried again", name);
            DeleteFileA("C:\\igdext_kernels\\compile_failed.txt");
            return false;
        }
    }
    return true;
}

bool XmxFallbackActive(ID3D12Device* dev)
{
    static int decided = -1;
    if (decided >= 0) return decided == 1;
    char b[8];
    decided = 0;
    if (GetEnvironmentVariableA("IGDEXT_FORCE_FALLBACK", b, sizeof(b)) > 0 && atoi(b))
    { TraceF("  fallback: IGDEXT_FORCE_FALLBACK"); decided = 1; }
    else if (CompileFailedStillValid() && GetEnvironmentVariableA("IGDEXT_IGNORE_FAILED", b, sizeof(b)) == 0)
    { TraceF("  fallback: kernels failed to compile earlier (C:\\igdext_kernels\\compile_failed.txt)"); decided = 1; }
    else if (!SelfTestOk(dev))
    { TraceF("  fallback: self-test failed (patched driver not active in this process, or placeholders not recognised)"); decided = 1; }
    return decided == 1;
}

HRESULT _INTC_D3D12_CheckFeatureSupport(INTCExtensionContext* ctx, INTC_D3D12_FEATURES f, void* data, UINT size)
{
    TraceF("CheckFeatureSupport feature=%d size=%u", (int)f, size);
    if (!data) return E_INVALIDARG;
    if (f == INTC_D3D12_FEATURE_D3D12_OPTIONS1 && size >= sizeof(INTC_D3D12_FEATURE_DATA_D3D12_OPTIONS1))
    {
        auto* o = (INTC_D3D12_FEATURE_DATA_D3D12_OPTIONS1*)data;
        int xmx = XmxDetectGpu().xmx ? 1 : 0, dl = xmx, em = 0;
        { char b[32]; if (GetEnvironmentVariableA("IGDEXT_OPTIONS1", b, sizeof(b)) > 0) sscanf(b, "%d,%d,%d", &xmx, &dl, &em); }
        o->XMXEnabled = xmx ? TRUE : FALSE; o->DLBoostEnabled = dl ? TRUE : FALSE; o->EmulatedTyped64bitAtomics = em ? TRUE : FALSE;
        TraceF("  OPTIONS1 -> XMXEnabled=%d DLBoostEnabled=%d EmulatedTyped64bitAtomics=%d", xmx, dl, em);
        return S_OK;
    }
    if (f == INTC_D3D12_FEATURE_D3D12_OPTIONS2 && size >= sizeof(INTC_D3D12_FEATURE_DATA_D3D12_OPTIONS2))
    {
        auto* o = (INTC_D3D12_FEATURE_DATA_D3D12_OPTIONS2*)data;
        // SIMD16Required=1 (Xe2 and later) makes XeSS and XeFG hand out their SIMD16 + LSC-typed kernel variant, the only
        // one those GPUs can compile (the SIMD8 variant uses legacy typed messages); Xe-HPG gets the SIMD8 variant.
        // The same answer goes to every caller; the fallbacks happen at context creation (XmxFallbackActive).
        // Overrides: IGDEXT_OPTIONS2=<simd16>,<lsc>,<legacy>, IGDEXT_OPTIONS2_FG=... for calls from libxess_fg.dll.
        TraceCallers("OPTIONS2");
        const bool fgCtx = CalledFromXeFG();
        int simd16 = XmxDetectGpu().simd16 ? 1 : 0, lsc = 1, legacy = 0;
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
void    _INTC_D3D12_QueryCpuVisibleVidmem(INTCExtensionContext*, UINT64* total, UINT64* freeb) { TraceF("QueryCpuVisibleVidmem"); if (total) *total = 0; if (freeb) *freeb = 0; }

} // extern "C"

#include "Stdafx.h"
#include "Trace.h"
#include "GpuInfo.h"
#include <string>
#include <vector>
#include <atomic>
#include <mutex>

static const unsigned char kDummyCS[] = {
#include "dummy_cs_bytes.inc"
};
static const unsigned char kDummyUAV[] = {
#include "dummy_uav_bytes.inc"
};
#include "placeholders.inc"
#include "DxbcChecksum.h"

// Keep in step with vkwrap/vkwrap.c (vulkan-1.dll, XeSS in a Vulkan game), which names and writes a kernel's files the
// same way (fnv1a, write_once, w_csm): the hash is FNV-1a 64 over the SPIR-V and then the compile options, the files
// are C:\igdext_kernels\<hash as 16 hex digits>.spv / .opt, each written under a temporary name and renamed. If the two
// differ, the same kernel gets two names, and the driver and the layer compile and cache it twice. Only the temporary
// name is not the same: <file>.<process id>.tmp here, where g_dynMutex lets one thread write at a time, and
// <file>.<process id>.<thread id>.tmp there, where XeSS creates the same module from several threads at once.
static unsigned long long Fnv1a64(const void* a, size_t na, const void* b, size_t nb)
{
    unsigned long long h = 14695981039346656037ull;
    const unsigned char* p = (const unsigned char*)a; for (size_t i = 0; i < na; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    p = (const unsigned char*)b; for (size_t i = 0; i < nb; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

// Every CM kernel's SPIR-V and compile options are written to C:\igdext_kernels\<hash>.spv / <hash>.opt (hash: FNV-1a of
// both, 16 hex digits), and a placeholder pipeline that carries the hash is returned. The patched ANV reads the hash
// from the placeholder, finds the files through $WINEPREFIX/drive_c/igdext_kernels, looks the kernel up in
// ~/.cache/xess-xmx/kernels and compiles it there on a miss.
static const char* kDynDir = "C:\\igdext_kernels";
static std::mutex g_dynMutex;
static std::vector<unsigned long long> g_dynWritten;   // hashes whose files this process has confirmed on disk

static bool WriteFileOnce(const char* path, const void* data, size_t size)
{
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return true;
    char tmp[MAX_PATH]; snprintf(tmp, sizeof(tmp), "%s.%lu.tmp", path, GetCurrentProcessId());
    FILE* f = nullptr; if (fopen_s(&f, tmp, "wb") != 0 || !f) return false;
    const bool ok = fwrite(data, 1, size, f) == size; fclose(f);
    if (!ok || !MoveFileExA(tmp, path, 0)) { DeleteFileA(tmp); return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES; }
    return true;
}

static bool DynKernelFiles(unsigned long long hash, const void* spv, size_t spvLen, const char* opts)
{
    std::lock_guard<std::mutex> lock(g_dynMutex);
    for (unsigned long long h : g_dynWritten) if (h == hash) return true;
    CreateDirectoryA(kDynDir, nullptr);
    char p[MAX_PATH];
    snprintf(p, sizeof(p), "%s\\%016llx.spv", kDynDir, hash); const bool a = WriteFileOnce(p, spv, spvLen);
    snprintf(p, sizeof(p), "%s\\%016llx.opt", kDynDir, hash); const bool b = WriteFileOnce(p, opts, strlen(opts));
    if (a && b) g_dynWritten.push_back(hash);
    return a && b;
}

// A placeholder: a copy of one of the templates of placeholders.inc (tools/gen_placeholders.py) with lo and hi in its
// store (low dword first) and the container checksum recomputed for it.
static std::vector<unsigned char> MakePlaceholder(const unsigned char* tmpl, size_t size, size_t at, uint32_t lo, uint32_t hi)
{
    std::vector<unsigned char> b(tmpl, tmpl + size);
    dxbc_checksum::Put32(&b[at], lo);
    dxbc_checksum::Put32(&b[at + 4], hi);
    dxbc_checksum::Update(b.data(), b.size());
    return b;
}

// the self-test values (the driver's ANV_CM_CANARY_LO / _HI; tools/check_placeholders.py compares them)
static const uint32_t kCanaryLo = 0x00414e43u;
static const uint32_t kCanaryHi = 0x7fa15952u;

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

// _INTC_D3D12_GetDisplayTelemetry: XellDriver.cpp (XeLL calls it from its latency-reduction path)

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

// _INTC_D3D12_SetNumGeneratedFrames, _INTC_D3D12_SetPresentSequenceNumber: XellDriver.cpp (XeLL / XeFG pacing calls)

void _INTC_D3D12_TransferHostRTAS(INTCExtensionContext* pExtensionContext, ID3D12GraphicsCommandList* pCommandList, D3D12_GPU_VIRTUAL_ADDRESS DestAccelerationStructureData, D3D12_GPU_VIRTUAL_ADDRESS SrcAccelerationStructureData, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE Mode)
{
    TraceF("TransferHostRTAS (not implemented)");
}


HRESULT _INTC_D3D12_CreateComputePipelineState(INTCExtensionContext* ctx, const INTC_D3D12_COMPUTE_PIPELINE_STATE_DESC* d, REFIID riid, void** pp)
{
    if (!d || !pp) return E_INVALIDARG;
    static std::atomic<int> counter{0};
    const int id = counter++;
    if (TraceOn())
    {
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
        // a CM kernel: the pipeline handed back is the placeholder that carries the kernel's hash (same root signature),
        // which the patched driver replaces by the compiled kernel; anything else gets a no-op so XeSS keeps requesting
        // its kernels
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc = *d->pD3D12Desc;
        desc.CS.pShaderBytecode = kDummyUAV; desc.CS.BytecodeLength = sizeof(kDummyUAV);
        std::vector<unsigned char> placeholder;   // must outlive CreateComputePipelineState
        {
            const char* opts = d->CompileOptions ? (const char*)d->CompileOptions : "";
            const unsigned long long h = Fnv1a64(d->CS.pShaderBytecode, d->CS.BytecodeLength, opts, strlen(opts));
            if (d->ShaderInputType == CM_SPIRV)
            {
                if (DynKernelFiles(h, d->CS.pShaderBytecode, d->CS.BytecodeLength, opts))
                {
                    placeholder = MakePlaceholder(kPlaceholderKernel, sizeof(kPlaceholderKernel), kPlaceholderKernelHashAt,
                                                  (uint32_t)h, (uint32_t)(h >> 32));
                    desc.CS.pShaderBytecode = placeholder.data(); desc.CS.BytecodeLength = placeholder.size();
                    TraceF("  kernel hash %016llx -> placeholder", h);
                }
                else
                    TraceF("  WARNING: cannot write %s\\%016llx.spv: this kernel runs as a no-op", kDynDir, h);
            }
            else
                TraceF("  WARNING: shader input type %d is not handled (only CM SPIR-V is): this kernel runs as a no-op", (int)d->ShaderInputType);
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

// Self-test, once per process, before XeSS is told to use CM kernels: a placeholder pipeline with workgroup (1, 1, 19)
// that carries the test values kCanaryLo / kCanaryHi where a kernel placeholder carries its hash is created on the
// game's device; the patched ANV recognises it (magic value and both test values) and writes
// C:\igdext_kernels\anv_canary. If the file is not newer than this process, placeholders would not be replaced - the
// stock driver is in use (the session check fell back, the variables are missing), the driver is one of an older
// package that does not read hashes, or vkd3d-proton changed how placeholders reach the driver - and XeSS must use its
// DP4a path instead of producing noise. IGDEXT_SKIP_SELFTEST=1 skips it.
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
        const std::vector<unsigned char> canary =
            MakePlaceholder(kPlaceholderCanary, sizeof(kPlaceholderCanary), kPlaceholderCanaryHashAt, kCanaryLo, kCanaryHi);
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = rs; pd.CS.pShaderBytecode = canary.data(); pd.CS.BytecodeLength = canary.size();
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
// *xessLoaded tells whether the check could look at a loaded XeSS library at all: the game itself may open an extension
// context before it loads XeSS (Wuthering Waves does), and a game update is only visible once libxess.dll is loaded.
static bool CompileFailedStillValid(bool* xessLoaded)
{
    *xessLoaded = false;
    WIN32_FILE_ATTRIBUTE_DATA mk;
    if (!GetFileAttributesExA("C:\\igdext_kernels\\compile_failed.txt", GetFileExInfoStandard, &mk)) return false;
    const char* libs[] = { "libxess.dll", "libxess_fg.dll" };
    for (const char* name : libs)
    {
        HMODULE m = GetModuleHandleA(name);
        char path[MAX_PATH];
        WIN32_FILE_ATTRIBUTE_DATA la;
        if (!m || !GetModuleFileNameA(m, path, MAX_PATH) || !GetFileAttributesExA(path, GetFileExInfoStandard, &la)) continue;
        *xessLoaded = true;
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
    // XeSS and XeFG open their contexts from different threads; the decision (and the self-test inside it) is made once,
    // under a lock, so that a second caller neither sees "decided: no fallback" while the test is still running nor
    // runs its own test and reads the first one's in-progress state as a failure.
    static std::mutex mtx;
    static int decided = -1;
    std::lock_guard<std::mutex> lock(mtx);
    if (decided >= 0) return decided == 1;
    char b[8];
    bool xessLoaded = false;
    decided = 0;
    if (GetEnvironmentVariableA("IGDEXT_FORCE_FALLBACK", b, sizeof(b)) > 0 && atoi(b))
    { TraceF("  fallback: IGDEXT_FORCE_FALLBACK"); decided = 1; }
    else if (!XmxDetectGpu().known && XmxDetectGpu().deviceId != 0 && GetEnvironmentVariableA("IGDEXT_GMD", b, sizeof(b)) == 0)
    {
        // an Intel GPU the table does not know (Xe-LP, or newer than this build): the kernels XeSS would request for
        // the assumed family may not run on it. Decline like a system without the extension (DP4a, generic frame
        // generation). IGDEXT_GMD=<arch>,<release> declares the device and lifts this. (No Intel entry in the registry
        // at all keeps the historical assumption, Panther Lake, and the trace says so.)
        TraceF("  fallback: unknown Intel device id 8086:%04x (set IGDEXT_GMD to declare it)", XmxDetectGpu().deviceId); decided = 1;
    }
    else if (CompileFailedStillValid(&xessLoaded) && GetEnvironmentVariableA("IGDEXT_IGNORE_FAILED", b, sizeof(b)) == 0)
    {
        if (!xessLoaded)
        {
            // not decided yet: the game-update check needs XeSS loaded; decline this context, look again at the next one
            TraceF("  fallback for this context: compile_failed.txt present, XeSS not loaded yet (checked again later)");
            decided = -1;
            return true;
        }
        TraceF("  fallback: kernels failed to compile earlier (C:\\igdext_kernels\\compile_failed.txt)"); decided = 1;
    }
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
        // Overrides: IGDEXT_OPTIONS2=<simd16>,<lsc>,<legacy>, IGDEXT_OPTIONS2_FG=... for calls from libxess_fg.dll; an FG
        // caller takes only the _FG one (the default otherwise), so with either override set the caller must be known.
        TraceCallers("OPTIONS2");
        char sr[32], fg[32];
        const bool haveSr = GetEnvironmentVariableA("IGDEXT_OPTIONS2", sr, sizeof(sr)) > 0;
        const bool haveFg = GetEnvironmentVariableA("IGDEXT_OPTIONS2_FG", fg, sizeof(fg)) > 0;
        // CalledFromXeFG walks the stack and resolves every frame's module: only when its answer is used
        const bool fgCtx = (haveSr || haveFg || TraceOn()) && CalledFromXeFG();
        int simd16 = XmxDetectGpu().simd16 ? 1 : 0, lsc = 1, legacy = 0;
        if (fgCtx ? haveFg : haveSr) sscanf(fgCtx ? fg : sr, "%d,%d,%d", &simd16, &lsc, &legacy);
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
    for (UINT i = 0; i < n; i++) { if (!descs[i].pD3D12Desc) return r; v[i] = *descs[i].pD3D12Desc; }
    return dev->GetResourceAllocationInfo(mask, n, v.data());
}

HRESULT _INTC_D3D12_AddShaderBinariesPath(INTCExtensionContext*, const wchar_t* p)   { char b[260] = {}; WideCharToMultiByte(CP_UTF8, 0, p ? p : L"", -1, b, sizeof(b) - 1, nullptr, nullptr); TraceF("AddShaderBinariesPath '%s'", b); return S_OK; }
HRESULT _INTC_D3D12_RemoveShaderBinariesPath(INTCExtensionContext*, const wchar_t* p) { char b[260] = {}; WideCharToMultiByte(CP_UTF8, 0, p ? p : L"", -1, b, sizeof(b) - 1, nullptr, nullptr); TraceF("RemoveShaderBinariesPath '%s'", b); return S_OK; }
HRESULT _INTC_D3D12_SetApplicationInfo(INTCExtensionAppInfo1*)                          { TraceF("SetApplicationInfo"); return S_OK; }
void    _INTC_D3D12_QueryCpuVisibleVidmem(INTCExtensionContext*, UINT64* total, UINT64* freeb) { TraceF("QueryCpuVisibleVidmem"); if (total) *total = 0; if (freeb) *freeb = 0; }

} // extern "C"

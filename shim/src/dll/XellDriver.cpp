// XellDriver.cpp - driver side of Intel XeLL's latency-reduction extension (_INTC_D3D12_*LatencyReduction*, RenderSubmitStart,
// SetNumGeneratedFrames, SetPresentSequenceNumber, GetDisplayTelemetry), and what it takes for libxell.dll to use it.
//
// How libxell.dll (XeSS SDK 2.x, 1.3.x) decides between driver timings and its own cross-vendor pacing, from the code of
// 1.3.2.10 (SDK bin/libxell.dll; whether the 1.3.0.5 that games ship has the same check shows in its strings,
// `strings libxell.dll | grep wine_get_version`, and in the probe's "hid wine_get_version" line):
//
//  1. At context creation it takes the DXGI adapter of the device and sets an "Intel GPU" flag only if VendorId == 0x8086
//     AND GetProcAddress(GetModuleHandleA("ntdll.dll"), "wine_get_version") returns nothing. Under Wine that export
//     exists, so the flag is never set and libxell goes straight to "[XeLL] Using built-in context" without loading
//     igdext64.dll at all: no GetSupportedVersions, no LatencyReductionExt, nothing for the shim to see. Nothing in
//     Proton hides that export; the shim does it for libxell alone (below).
//  2. With the flag set it loads igdext64.dll (INTC_ALT_DRIVER_EXTENSIONS_PATH, the driver folder, the current folder),
//     asks GetSupportedVersions for HW feature level 5 or 4 with API version 13 (exact match on both; the shim lists
//     5/13 and 4/13), creates an extension context, and requires
//       - the driver version string >= 31.0.101.6252 (the shim says 32.0.101.9999), else "unsupported driver";
//       - INTCDeviceInfo::GTGeneration to be an XeSS-capable Intel PRODUCT_FAMILY: 1270 DG2, 1272 MTL, 1273 ARL,
//         1274 BMG, 1275 LNL, 1276, 1300 PTL or anything >= 1340 (gmmlib igfxfmid.h), else "unsupported device".
//         The shim reports 30 on Panther Lake; with the probe on it reports the family id (GpuInfo.cpp, IGDEXT_FAMILY).
//  3. Only then "Intel Extensions initialized." and "Latency reduction enabled." (else "... - Cross Vendor."): it polls the
//     render-submit timing buffers for the CPU and GPU timestamps of each frame's submission ("[Driver] Driver timestamps
//     not available for frame", "GetRenderStartTimestamp failed Map") and reports the present sequence / generated-frame
//     count through the two Set* calls. XELL_FORCE_CROSS_VENDOR=1 (also HKLM\SOFTWARE\Intel\XeLL) forces the fallback;
//     XELL_FORCE_LOGS_TO_FILE=1 writes libxell.log in 1.3.2.10 (1.3.0.5 only has OutputDebugString: the Proton log).
//     igxell.dll is a different, optional mechanism (XeLL's "driver DLL context", a newer XeLL carried by the Windows
//     driver) and is not needed for driver timings.
//
// What XeLL does with the data (1.3.2.10; the author's reading of 1.3.0.5 on a B390 matches):
//  - GetDisplayTelemetry(block, 928): 16 records of 56 bytes at 0: frame id (u32) at +0x08, flags at +0x20 (bit 2 =
//    vsync on), the time the flip was queued at +0x28 and the vsync it was shown on at +0x30 ("Flip in queue" is their
//    difference, "Last used vsync" the second); then at 0x388 a reference vsync time, at 0x390/0x398 that time plus the
//    shortest and the longest refresh interval (equal: fixed refresh; different: VRR). All times in QPC ticks (XeLL
//    multiplies by 100: Windows' QPC runs at 10 MHz, Wine's too). All three trailing values must be non-zero with
//    0x398 >= 0x390 or the frame is skipped ("Display telemetry corrupted."). Two consecutive records with the same
//    frame id tell XeLL that frame generation is on.
//  - CPU timing buffer: 8 slots of 16 bytes, the second qword of slot (frameId & 7) = CPU time of the render submit in
//    XeLL's own clock, std::chrono::steady_clock, i.e. QPC converted to nanoseconds (it is used raw against XeLL's
//    other CPU times). GPU timing buffer: 8 slots of 8 bytes, slot (frameId & 7) = GPU timestamp of the submit in the
//    D3D12 queue's timestamp ticks (XeLL divides by ID3D12CommandQueue::GetTimestampFrequency of its own queue). XeLL
//    zeroes both after reading. A frame whose timings did not arrive is not paced at all ("Driver timings not
//    available for last visible frame"), so driver mode must deliver every frame or not run.
//
// IGDEXT_XELL_PROBE=<version> turns the driver mode on: the shim hides wine_get_version from libxell.dll (an
// import-table hook of GetProcAddress in libxell.dll only, installed when the DLL loads), reports the product family,
// claims that latency-reduction interface version and provides the three things above:
//  - RenderSubmitStart(frameId): CPU time = QPC in ns; GPU time = the GPU clock read with GetClockCalibration on a
//    direct queue the shim creates on the game's device (same clock as every direct queue), i.e. "the GPU could start
//    this submit now" - the queue depth is not known to the shim (first version);
//  - display telemetry: a vsync model from the current display mode's refresh rate (IGDEXT_XELL_HZ overrides, and
//    IGDEXT_XELL_VRR=<min>,<max> describes a VRR range), records from SetPresentSequenceNumber calls (one per present,
//    flip queued = call time, shown = the next vsync after it);
// and logs what XeLL does with it (XELLPROBE lines). IGDEXT_XELL_TELEMETRY=0 answers GetDisplayTelemetry with
// E_NOTIMPL instead (A/B test); IGDEXT_XELL_GUARD=1 puts the timing buffers on guard pages and logs every access XeLL
// makes to them (slot, offset, code address), as the first probe did. Without IGDEXT_XELL_PROBE nothing here is
// active and every function answers "not supported", as before.
#include "Stdafx.h"
#include "Trace.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <intrin.h>
#include <winternl.h>
#include <mutex>
#include "ExtensionContext.h"

namespace
{
int g_probeVersion = -1;           // -1: not read yet, 0: probe off
uint32_t g_used = 0, g_slots = 0;
BOOL g_enabled = FALSE, g_timings = FALSE;
uint8_t* g_cpuBuf = nullptr;
uint8_t* g_gpuBuf = nullptr;
size_t g_bufSize = 0;
volatile LONG g_accessLogged = 0;
thread_local void* t_rearm = nullptr;   // page this thread must re-guard after its single step (faults can overlap across threads)

int ProbeVersion()
{
    if (g_probeVersion < 0)
    {
        char b[16];
        g_probeVersion = GetEnvironmentVariableA("IGDEXT_XELL_PROBE", b, sizeof(b)) > 0 ? atoi(b) : 0;
    }
    return g_probeVersion;
}

void ModuleOffset(void* addr, char* out, size_t cap)
{
    HMODULE m = nullptr; char path[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &m) && m)
        GetModuleFileNameA(m, path, MAX_PATH);
    const char* base = strrchr(path, '\\'); base = base ? base + 1 : path;
    snprintf(out, cap, "%s+%#llx", base, (unsigned long long)((uint8_t*)addr - (uint8_t*)m));
}

LONG CALLBACK GuardHandler(EXCEPTION_POINTERS* ep)
{
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == STATUS_GUARD_PAGE_VIOLATION)
    {
        const ULONG_PTR kind = ep->ExceptionRecord->ExceptionInformation[0];
        uint8_t* a = (uint8_t*)ep->ExceptionRecord->ExceptionInformation[1];
        const char* which = nullptr; size_t off = 0;
        if (g_cpuBuf && a >= g_cpuBuf && a < g_cpuBuf + g_bufSize) { which = "cpu"; off = a - g_cpuBuf; }
        else if (g_gpuBuf && a >= g_gpuBuf && a < g_gpuBuf + g_bufSize) { which = "gpu"; off = a - g_gpuBuf; }
        if (!which) return EXCEPTION_CONTINUE_SEARCH;
        if (InterlockedIncrement(&g_accessLogged) <= 4000)
        {
            char where[160]; ModuleOffset((void*)ep->ContextRecord->Rip, where, sizeof(where));
            TraceF("XELLPROBE access %s %s +%#zx (slot %zu, in-slot %#zx) at %s", kind == 1 ? "write" : "read", which, off,
                   g_slots ? off / (g_bufSize / g_slots) : 0, g_slots ? off % (g_bufSize / g_slots) : off, where);
        }
        t_rearm = (void*)((uintptr_t)a & ~(uintptr_t)0xfff);
        ep->ContextRecord->EFlags |= 0x100;   // single step, then guard the page again
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_SINGLE_STEP && t_rearm)
    {
        DWORD old; VirtualProtect(t_rearm, 1, PAGE_READWRITE | PAGE_GUARD, &old);
        t_rearm = nullptr;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

uint8_t* GuardedBuffer(size_t size)
{
    uint8_t* p = (uint8_t*)VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!p) return nullptr;
    memset(p, 0, size);
    DWORD old; VirtualProtect(p, size, PAGE_READWRITE | PAGE_GUARD, &old);
    return p;
}
}


// ---- hiding wine_get_version from libxell.dll ----------------------------------------------------------------------
namespace
{
typedef FARPROC (WINAPI* PFN_GetProcAddress)(HMODULE, LPCSTR);
PFN_GetProcAddress g_realGetProcAddress = nullptr;
volatile LONG g_wineHidden = 0;

FARPROC WINAPI XellGetProcAddress(HMODULE m, LPCSTR name)
{
    if (((uintptr_t)name >> 16) != 0 && strcmp(name, "wine_get_version") == 0)
    {
        if (InterlockedIncrement(&g_wineHidden) == 1) TraceF("XELLPROBE hid wine_get_version from libxell.dll");
        SetLastError(ERROR_PROC_NOT_FOUND);
        return nullptr;
    }
    return g_realGetProcAddress(m, name);
}

// Replace one entry of a module's import address table. Walks the PE import directory of the mapped image.
bool PatchImport(HMODULE mod, const char* dllName, const char* funcName, void* hook, void** orig)
{
    auto base = (uint8_t*)mod;
    auto dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    for (auto desc = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); desc->Name; ++desc)
    {
        if (_stricmp((const char*)(base + desc->Name), dllName) != 0) continue;
        auto names = (IMAGE_THUNK_DATA64*)(base + (desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk));
        auto iat = (IMAGE_THUNK_DATA64*)(base + desc->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat)
        {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            auto byName = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            if (strcmp((const char*)byName->Name, funcName) != 0) continue;
            DWORD old;
            if (!VirtualProtect(iat, sizeof(*iat), PAGE_READWRITE, &old)) return false;
            if (orig) *orig = (void*)iat->u1.Function;
            iat->u1.Function = (ULONGLONG)hook;
            VirtualProtect(iat, sizeof(*iat), old, &old);
            return true;
        }
    }
    return false;
}

void HideWineFrom(HMODULE libxell)
{
    if (!libxell) return;
    void* orig = nullptr;
    if (PatchImport(libxell, "KERNEL32.dll", "GetProcAddress", (void*)XellGetProcAddress, &orig))
    {
        if (orig == (void*)XellGetProcAddress) return;   // this mapping was hooked already
        // a reloaded libxell.dll (same or another address) has a fresh import table and is hooked again; the real
        // GetProcAddress is the same kernel32 export each time
        g_realGetProcAddress = (PFN_GetProcAddress)orig;
        TraceF("XELLPROBE libxell.dll %p: GetProcAddress import hooked (wine_get_version will be hidden)", libxell);
    }
    else TraceF("XELLPROBE libxell.dll %p: GetProcAddress import not found, cannot hide wine_get_version", libxell);
}

// LdrRegisterDllNotification: called by the loader for every DLL as it is mapped, before the game can call into it.
// Not in the SDK headers; the structures are documented in the Windows SDK (ntddk) and implemented by Wine.
struct LDR_DLL_LOADED_NOTIFICATION_DATA_ { ULONG Flags; const UNICODE_STRING* FullDllName; const UNICODE_STRING* BaseDllName; PVOID DllBase; ULONG SizeOfImage; };
typedef VOID (CALLBACK* PFN_LdrDllNotification)(ULONG reason, const LDR_DLL_LOADED_NOTIFICATION_DATA_* data, PVOID ctx);
typedef NTSTATUS (NTAPI* PFN_LdrRegisterDllNotification)(ULONG flags, PFN_LdrDllNotification fn, PVOID ctx, PVOID* cookie);

VOID CALLBACK OnDllLoaded(ULONG reason, const LDR_DLL_LOADED_NOTIFICATION_DATA_* data, PVOID)
{
    if (reason != 1 /* LDR_DLL_NOTIFICATION_REASON_LOADED */ || !data || !data->BaseDllName || !data->BaseDllName->Buffer) return;
    if (_wcsnicmp(data->BaseDllName->Buffer, L"libxell.dll", data->BaseDllName->Length / sizeof(wchar_t)) == 0 &&
        data->BaseDllName->Length == sizeof(L"libxell.dll") - sizeof(wchar_t))
        HideWineFrom((HMODULE)data->DllBase);
}
}

// Called once from DllMain (Trace.cpp). With IGDEXT_XELL_PROBE set: hook libxell.dll now if it is already loaded, and
// watch for it loading later. If the game created its XeLL context before the shim was loaded, that context stays
// cross-vendor; toggling frame generation off and on in the game recreates it.
void StartXellDriverProbe()
{
    if (ProbeVersion() <= 0) return;
    HideWineFrom(GetModuleHandleW(L"libxell.dll"));
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto reg = ntdll ? (PFN_LdrRegisterDllNotification)GetProcAddress(ntdll, "LdrRegisterDllNotification") : nullptr;
    PVOID cookie = nullptr;
    if (!reg || reg(0, OnDllLoaded, nullptr, &cookie) != 0)
        TraceF("XELLPROBE LdrRegisterDllNotification unavailable: libxell.dll must already be loaded when the shim loads");
}

static ID3D12Device* DevOf(INTCExtensionContext* c) { return (c && c->m_pD3D12ExtensionContext) ? c->m_pD3D12ExtensionContext->m_pAppDevice.Get() : nullptr; }

extern "C" {

// ---- the provider ---------------------------------------------------------------------------------------------------
namespace
{
constexpr uint32_t kSlots = 8;
constexpr size_t kCpuSlot = 16, kGpuSlot = 8;

struct PresentRecord { uint32_t frameId; uint64_t queued, shown; };   // QPC ticks

LARGE_INTEGER g_qpf = {};
ID3D12CommandQueue* g_queue = nullptr;     // shim-owned direct queue for GetClockCalibration
INTCExtensionContext* g_ctx = nullptr;
std::mutex g_mtx;
PresentRecord g_presents[16] = {};
uint32_t g_nPresents = 0, g_lastPresentId = 0;
uint64_t g_vsyncAnchor = 0;                // QPC ticks of a vsync (modelled)
uint64_t g_periodMin = 0, g_periodMax = 0; // QPC ticks
bool g_vsyncOn = true;
int g_telemetryMode = -1;                  // -1 unread, 0 E_NOTIMPL, 1 provide

uint64_t Qpc() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return (uint64_t)t.QuadPart; }
uint64_t QpcToNs(uint64_t q) { const uint64_t f = (uint64_t)g_qpf.QuadPart; return q / f * 1000000000ull + (q % f) * 1000000000ull / f; }

int TelemetryMode()
{
    if (g_telemetryMode < 0)
    {
        char b[8];
        g_telemetryMode = GetEnvironmentVariableA("IGDEXT_XELL_TELEMETRY", b, sizeof(b)) > 0 ? (atoi(b) != 0) : 1;
    }
    return g_telemetryMode;
}

// refresh interval(s) of the display, in QPC ticks: IGDEXT_XELL_VRR=<minHz>,<maxHz>, IGDEXT_XELL_HZ=<hz>, else the mode
void InitDisplayModel()
{
    if (g_periodMin) return;
    QueryPerformanceFrequency(&g_qpf);
    double hzMin = 0, hzMax = 0;
    char b[32];
    if (GetEnvironmentVariableA("IGDEXT_XELL_VRR", b, sizeof(b)) > 0 && sscanf(b, "%lf,%lf", &hzMin, &hzMax) == 2 && hzMin > 0 && hzMax >= hzMin) {}
    else if (GetEnvironmentVariableA("IGDEXT_XELL_HZ", b, sizeof(b)) > 0 && (hzMax = atof(b)) > 0) hzMin = hzMax;
    else
    {
        DEVMODEW dm = {}; dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) hzMin = hzMax = dm.dmDisplayFrequency;
        else hzMin = hzMax = 60;
    }
    g_periodMax = (uint64_t)((double)g_qpf.QuadPart / hzMin + 0.5);   // longest interval = lowest rate
    g_periodMin = (uint64_t)((double)g_qpf.QuadPart / hzMax + 0.5);
    if (GetEnvironmentVariableA("IGDEXT_XELL_TEARING", b, sizeof(b)) > 0 && atoi(b)) g_vsyncOn = false;
    g_vsyncAnchor = Qpc();
    TraceF("XELLPROBE display model: %.2f-%.2f Hz (period %llu-%llu QPC ticks of %lld/s), vsync %s", hzMin, hzMax,
           (unsigned long long)g_periodMin, (unsigned long long)g_periodMax, (long long)g_qpf.QuadPart, g_vsyncOn ? "on" : "off (tearing)");
}

// the next modelled vsync strictly after t (fixed refresh: the anchor advanced in whole periods)
uint64_t NextVsyncAfter(uint64_t t)
{
    const uint64_t p = g_periodMax;
    if (t < g_vsyncAnchor) return g_vsyncAnchor;
    const uint64_t n = (t - g_vsyncAnchor) / p + 1;
    return g_vsyncAnchor + n * p;
}

bool EnsureQueue(INTCExtensionContext* ctx)
{
    if (g_queue) return true;
    ID3D12Device* dev = DevOf(ctx);
    if (!dev) { TraceF("XELLPROBE no device on the extension context: no GPU clock"); return false; }
    D3D12_COMMAND_QUEUE_DESC qd = {}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    HRESULT hr = dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&g_queue);
    if (FAILED(hr) || !g_queue) { TraceF("XELLPROBE CreateCommandQueue for the GPU clock failed (hr=0x%08lx)", (unsigned long)hr); g_queue = nullptr; return false; }
    UINT64 f = 0; g_queue->GetTimestampFrequency(&f);
    TraceF("XELLPROBE GPU clock queue created: timestamp frequency %llu Hz", (unsigned long long)f);
    return true;
}

// write the CPU and GPU time of a render submit into the slot of frameId
void StampSubmit(uint32_t frameId)
{
    if (!g_cpuBuf || !g_gpuBuf) return;
    UINT64 gpu = 0, cpu = 0;
    if (g_queue && FAILED(g_queue->GetClockCalibration(&gpu, &cpu))) gpu = 0;
    if (!cpu) cpu = Qpc();
    if (gpu < 2) gpu = 2;          // XeLL ignores slots with gpu <= 1 (and would never pace the frame)
    const uint32_t slot = frameId & (kSlots - 1);
    *(uint64_t*)(g_cpuBuf + slot * kCpuSlot + 8) = QpcToNs(cpu);
    *(uint64_t*)(g_gpuBuf + slot * kGpuSlot) = gpu;
}

void RecordPresent(uint32_t seq)
{
    const uint64_t now = Qpc();
    std::lock_guard<std::mutex> lock(g_mtx);
    PresentRecord& r = g_presents[g_nPresents % 16];
    r.frameId = seq; r.queued = now; r.shown = NextVsyncAfter(now);
    g_nPresents++; g_lastPresentId = seq;
}

// fill XeLL's 928-byte telemetry block
void FillTelemetry(uint8_t* t, UINT size)
{
    memset(t, 0, size);
    std::lock_guard<std::mutex> lock(g_mtx);
    const uint32_t n = g_nPresents < 16 ? g_nPresents : 16;
    // oldest first; XeLL takes the max frame id as the latest presented frame and the earliest times per id
    for (uint32_t i = 0; i < n; i++)
    {
        const PresentRecord& r = g_presents[(g_nPresents - n + i) % 16];
        uint8_t* rec = t + i * 56;
        *(uint32_t*)(rec + 0x08) = r.frameId;
        *(uint32_t*)(rec + 0x20) = g_vsyncOn ? 4u : 0u;
        *(uint64_t*)(rec + 0x28) = r.queued;
        *(uint64_t*)(rec + 0x30) = r.shown;
    }
    // reference vsync: the last modelled vsync before now
    const uint64_t now = Qpc();
    const uint64_t last = NextVsyncAfter(now) - g_periodMax;
    *(uint64_t*)(t + 0x388) = last;
    *(uint64_t*)(t + 0x390) = last + g_periodMin;
    *(uint64_t*)(t + 0x398) = last + g_periodMax;
}
}

HRESULT _INTC_D3D12_GetLatencyReductionStatus(INTCExtensionContext*, INTC_D3D12_LATENCY_REDUCTION_STATUS* st)
{
    const int v = ProbeVersion();
    if (st)
    {
        *st = {};
        if (v > 0) { st->driverSupportedVersion = (uint32_t)v; st->latencyReductionEnabled = g_enabled; st->renderSubmitTimingsEnabled = g_timings; st->usedVersion = g_used; }
    }
    TraceF("XELLPROBE GetLatencyReductionStatus -> supported %d enabled %d timings %d used %u", v, g_enabled, g_timings, g_used);
    return S_OK;
}

HRESULT _INTC_D3D12_LatencyReductionExt(INTCExtensionContext* ctx, uint32_t version, BOOL latencyReductionEnabled, BOOL renderSubmitTimingsEnabled, uint32_t timingSlots)
{
    TraceF("XELLPROBE LatencyReductionExt(version %u, enabled %d, timings %d, slots %u)", version, latencyReductionEnabled, renderSubmitTimingsEnabled, timingSlots);
    if (ProbeVersion() <= 0) return E_NOTIMPL;
    g_used = version; g_enabled = latencyReductionEnabled; g_timings = renderSubmitTimingsEnabled;
    g_ctx = ctx;
    InitDisplayModel();
    EnsureQueue(ctx);
    if (renderSubmitTimingsEnabled && timingSlots && !g_cpuBuf)
    {
        g_slots = timingSlots;
        // XeLL 1.3 addresses the slots as (frameId & 7) regardless of the count it asks for; a page each is plenty
        g_bufSize = 0x1000;
        char b[8];
        const bool guard = GetEnvironmentVariableA("IGDEXT_XELL_GUARD", b, sizeof(b)) > 0 && atoi(b);
        if (guard)
        {
            AddVectoredExceptionHandler(1, GuardHandler);
            g_cpuBuf = GuardedBuffer(g_bufSize);
            g_gpuBuf = GuardedBuffer(g_bufSize);
        }
        else
        {
            g_cpuBuf = (uint8_t*)VirtualAlloc(nullptr, g_bufSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            g_gpuBuf = (uint8_t*)VirtualAlloc(nullptr, g_bufSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        }
        TraceF("XELLPROBE buffers cpu %p gpu %p (%zu bytes each, %s)", g_cpuBuf, g_gpuBuf, g_bufSize, guard ? "guard pages" : "plain");
    }
    return S_OK;
}

HRESULT _INTC_D3D12_LatencyReductionGetRenderSubmitTimingsBuffers(INTCExtensionContext*, void** ppCpu, void** ppGpu)
{
    TraceF("XELLPROBE GetRenderSubmitTimingsBuffers -> %p %p", g_cpuBuf, g_gpuBuf);
    if (ProbeVersion() <= 0 || !g_cpuBuf) return E_NOTIMPL;
    if (ppCpu) *ppCpu = g_cpuBuf;
    if (ppGpu) *ppGpu = g_gpuBuf;
    return S_OK;
}

HRESULT _INTC_D3D12_RenderSubmitStart(INTCExtensionContext*, uint32_t frameId)
{
    static LONG n = 0;
    static LARGE_INTEGER prev = {};
    LARGE_INTEGER now, freq; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&freq);
    const double sincePrevMs = prev.QuadPart ? (now.QuadPart - prev.QuadPart) * 1000.0 / freq.QuadPart : 0.0;
    prev = now;
    const LONG k = InterlockedIncrement(&n);
    if (ProbeVersion() <= 0) return E_NOTIMPL;
    StampSubmit(frameId);
    if (k <= 40 || (k % 600) == 0)
    {
        char where[160]; ModuleOffset(_ReturnAddress(), where, sizeof(where));
        const uint32_t slot = frameId & (kSlots - 1);
        TraceF("XELLPROBE RenderSubmitStart(frame %u) #%ld +%.2f ms from %s -> slot %u cpu %llu ns gpu %llu", frameId, (long)k, sincePrevMs, where, slot,
               g_cpuBuf ? (unsigned long long)*(uint64_t*)(g_cpuBuf + slot * kCpuSlot + 8) : 0ull,
               g_gpuBuf ? (unsigned long long)*(uint64_t*)(g_gpuBuf + slot * kGpuSlot) : 0ull);
    }
    return S_OK;
}

// XeLL / XeSS-FG pacing information. On Windows the driver uses these to pace generated frames and to match
// present sequence numbers against render-submit timings. SetPresentSequenceNumber is one call per present: it is
// the source of the display telemetry's frame records.
HRESULT _INTC_D3D12_SetNumGeneratedFrames(INTCExtensionContext*, UINT NumFrames)
{
    static LONG n = 0; static UINT last = ~0u;
    const LONG k = InterlockedIncrement(&n);
    if (NumFrames != last || k <= 8) { TraceF("XELLPROBE SetNumGeneratedFrames(%u) #%ld", NumFrames, (long)k); last = NumFrames; }
    return ProbeVersion() > 0 ? S_OK : E_NOTIMPL;
}

HRESULT _INTC_D3D12_SetPresentSequenceNumber(INTCExtensionContext*, UINT PresentSequenceNumber)
{
    static LONG n = 0;
    const LONG k = InterlockedIncrement(&n);
    if (ProbeVersion() <= 0) return E_NOTIMPL;
    if (g_periodMin) RecordPresent(PresentSequenceNumber);
    if (k <= 40 || (k % 600) == 0) TraceF("XELLPROBE SetPresentSequenceNumber(%u) #%ld", PresentSequenceNumber, (long)k);
    return S_OK;
}

HRESULT _INTC_D3D12_GetDisplayTelemetry(INTCExtensionContext*, void* pTelemetryData, UINT size)
{
    static LONG n = 0;
    const LONG k = InterlockedIncrement(&n);
    const bool provide = ProbeVersion() > 0 && TelemetryMode() && pTelemetryData && size >= 0x3a0 && g_periodMin;
    if (k <= 8 || (k % 600) == 0)
    {
        char where[160]; ModuleOffset(_ReturnAddress(), where, sizeof(where));
        TraceF("XELLPROBE GetDisplayTelemetry(size %u) #%ld from %s -> %s (presents recorded %u, last id %u)", size, (long)k, where,
               provide ? "provided" : "E_NOTIMPL", g_nPresents, g_lastPresentId);
    }
    if (!provide) return E_NOTIMPL;
    FillTelemetry((uint8_t*)pTelemetryData, size);
    return S_OK;
}

} // extern "C"

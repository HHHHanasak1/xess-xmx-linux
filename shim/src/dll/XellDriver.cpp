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
//  - RenderSubmitStart(frameId): CPU time = QPC in ns, provisionally the marker time and replaced by the time of the
//    frame's first ExecuteCommandLists (what the driver's "render submit timings" mean: XeLL's queue wait is GPU start
//    minus this, and it logs marker->execute as "RenderSubmitStart to Execute delay"); GPU time = provisionally the GPU clock read with
//    GetClockCalibration on a direct queue the shim creates, replaced by a measured value: a timestamp query the shim
//    executes in front of the frame's first ExecuteCommandLists from the game (vtable hook, see "real GPU start
//    timestamps" below; IGDEXT_XELL_GPUSTART=0 keeps the provisional value);
//  - display telemetry: a vsync model from the current display mode's refresh rate (IGDEXT_XELL_HZ overrides, and
//    IGDEXT_XELL_VRR=<min>,<max> describes a VRR range), records from SetPresentSequenceNumber calls (one per present,
//    flip queued = call time). Shown = the first vsync at which the frame can be on screen: after its GPU work is done
//    (a timestamp the shim executes on the game's queue at that call), after the frame before it was shown, and not
//    before now while it is still in flight. That is the present queue a real driver reports ("Flip in queue"): with
//    the GPU the limit, frames wait there, and that wait is what XeLL's sleep is for. Under vkd3d-proton the game
//    thread otherwise blocks on the full swapchain inside its own frame, so XeLL sees a slow CPU instead.
//    IGDEXT_XELL_FLIPQ=0 goes back to "shown at the next vsync after the call" (A/B);
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

struct PresentRecord { uint32_t frameId; uint64_t queued, shown, gpuDone; };   // QPC ticks; gpuDone 0 = not known yet

LARGE_INTEGER g_qpf = {};
ID3D12CommandQueue* g_queue = nullptr;     // shim-owned direct queue for GetClockCalibration
UINT64 g_tsFreq = 0;                       // its timestamp frequency (Hz), for readable queue-wait numbers
INTCExtensionContext* g_ctx = nullptr;
std::mutex g_mtx;
PresentRecord g_presents[16] = {};
uint32_t g_nPresents = 0, g_lastPresentId = 0;
uint64_t g_vsyncAnchor = 0;                // QPC ticks of a vsync (modelled)
uint64_t g_periodMin = 0, g_periodMax = 0; // QPC ticks
bool g_vsyncOn = true;
int g_telemetryMode = -1;                  // -1 unread, 0 E_NOTIMPL, 1 provide
int g_flipqMode = -1;                      // -1 unread, 0 shown = next vsync after the call, 1 present-queue model
uint64_t g_calGpu = 0, g_calCpu = 0;       // latest GetClockCalibration pair (GPU ticks, QPC ticks)

uint64_t Qpc() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return (uint64_t)t.QuadPart; }

bool FlipQueueModel()
{
    if (g_flipqMode < 0) { char b[8]; g_flipqMode = GetEnvironmentVariableA("IGDEXT_XELL_FLIPQ", b, sizeof(b)) > 0 ? (atoi(b) != 0) : 1; }
    return g_flipqMode == 1;
}

// a GPU timestamp of the D3D12 queues in QPC ticks, through the latest calibration pair (0 if there is none)
uint64_t GpuToQpc(uint64_t gpu)
{
    if (!g_calGpu || !g_calCpu || !g_tsFreq || !g_qpf.QuadPart) return 0;
    const double dq = ((double)(int64_t)(gpu - g_calGpu)) * (double)g_qpf.QuadPart / (double)g_tsFreq;
    const double q = (double)g_calCpu + dq;
    return q > 0 ? (uint64_t)q : 0;
}
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
    UINT64 f = 0; g_queue->GetTimestampFrequency(&f); g_tsFreq = f;
    TraceF("XELLPROBE GPU clock queue created: timestamp frequency %llu Hz", (unsigned long long)f);
    return true;
}

// ---- real GPU start timestamps --------------------------------------------------------------------------------------
// XeLL wants the GPU time at which the GPU began the frame's render submit. The shim cannot see the game's command
// lists through the extension API, but it can see them go by: every vkd3d-proton command queue shares one vtable, so
// hooking ExecuteCommandLists in the vtable of the shim's own queue hooks the game's queues too. After
// RenderSubmitStart(frameId), the first ExecuteCommandLists on a direct queue from the game (not from XeLL / XeSS / the
// shim) gets a tiny pre-recorded command list executed in front of it: one timestamp query resolved into a readback
// buffer, and a fence signal right behind it. The timestamp is written as soon as the GPU has passed that point; when
// the fence shows it is done, the value replaces the provisional "GPU clock at submit" in the slot, unless XeLL has
// already consumed the slot (it zeroes it). IGDEXT_XELL_GPUSTART=0 switches this off (A/B).
namespace
{
constexpr uint32_t kRing = 8;      // one per timing slot
typedef HRESULT (STDMETHODCALLTYPE* PFN_ExecuteCommandLists)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
PFN_ExecuteCommandLists g_origECL = nullptr;
ID3D12QueryHeap* g_qheap = nullptr;
ID3D12Resource* g_readback = nullptr;
uint64_t* g_readbackMap = nullptr;
ID3D12CommandAllocator* g_alloc[kRing] = {};
ID3D12GraphicsCommandList* g_list[kRing] = {};
ID3D12Fence* g_fence = nullptr;
uint64_t g_fenceValue = 0;
struct Pending { uint32_t frameId; uint64_t fenceValue; bool armed, inflight; uint64_t cpuAtQuery; };
Pending g_pending[kRing] = {};
volatile LONG g_waitingFrame = -1;    // frame id whose first game submit is awaited, -1 none
uint64_t g_markerQpc[kSlots] = {};    // QPC at RenderSubmitStart per slot, for marker->execute

// end of each presented frame's GPU work: a timestamp executed on the game's direct queue at SetPresentSequenceNumber
constexpr uint32_t kEndRing = 16;     // one per present record
ID3D12QueryHeap* g_qheapEnd = nullptr;
ID3D12Resource* g_readbackEnd = nullptr;
uint64_t* g_readbackEndMap = nullptr;
ID3D12CommandAllocator* g_allocEnd[kEndRing] = {};
ID3D12GraphicsCommandList* g_listEnd[kEndRing] = {};
struct PendingEnd { uint32_t present, frameId; uint64_t fenceValue; bool inflight; };   // present: index into g_presents
PendingEnd g_pendingEnd[kEndRing] = {};
ID3D12CommandQueue* volatile g_gameQueue = nullptr;   // the game's direct queue (the last one it submitted to)
ID3D12Fence* g_fenceEnd = nullptr;    // its own fence: the start stamps may go to another queue
uint64_t g_fenceEndValue = 0;
std::mutex g_endMtx;
int g_gpuStartMode = -1;
HMODULE g_self = nullptr;
LONG g_hookLogged = 0;

bool GpuStartEnabled()
{
    if (g_gpuStartMode < 0) { char b[8]; g_gpuStartMode = GetEnvironmentVariableA("IGDEXT_XELL_GPUSTART", b, sizeof(b)) > 0 ? (atoi(b) != 0) : 1; }
    return g_gpuStartMode == 1;
}

bool CallerIsGame(void* ret)
{
    HMODULE m = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)ret, &m) || !m) return true;
    if (m == g_self) return false;
    char path[MAX_PATH] = {}; GetModuleFileNameA(m, path, MAX_PATH);
    const char* base = strrchr(path, '\\'); base = base ? base + 1 : path;
    return _stricmp(base, "libxell.dll") != 0 && _stricmp(base, "libxess_fg.dll") != 0 && _stricmp(base, "libxess.dll") != 0 &&
           _stricmp(base, "igxell.dll") != 0;
}

// a fence value reached: the timestamp of that ring entry is in the readback buffer
void HarvestGpuStarts()
{
    if (!g_fence) return;
    const uint64_t done = g_fence->GetCompletedValue();
    for (uint32_t i = 0; i < kRing; i++)
    {
        Pending& p = g_pending[i];
        if (!p.inflight || p.fenceValue > done) continue;
        p.inflight = false;
        const uint64_t gpu = g_readbackMap ? g_readbackMap[i] : 0;
        const uint32_t slot = p.frameId & (kSlots - 1);
        uint64_t* dst = (uint64_t*)(g_gpuBuf + slot * kGpuSlot);
        static LONG n = 0; const LONG k = InterlockedIncrement(&n);
        if (*dst != 0 && gpu > 1)
        {
            // not consumed yet: replace the provisional submit-time value
            const int64_t deltaTicks = (int64_t)gpu - (int64_t)*dst;
            *dst = gpu;
            // logged every frame: the probe script takes the median as the render-queue wait XeLL sees (GPU start minus
            // the time the work was handed in) and sets it against XeLL's own sim->submit figure
            TraceF("XELLPROBE gpu start frame %u: measured %llu (%+lld ticks = %+.2f ms after submit)", p.frameId, (unsigned long long)gpu,
                   (long long)deltaTicks, g_tsFreq ? deltaTicks * 1000.0 / (double)g_tsFreq : 0.0);
        }
        else if (k <= 40 || (k % 600) == 0)
            TraceF("XELLPROBE gpu start frame %u: measured %llu but slot already consumed by XeLL (provisional value was used)", p.frameId, (unsigned long long)gpu);
    }
}

// presented frames whose GPU work is done: their completion time goes into the present record
void HarvestGpuEnds()
{
    if (!g_fenceEnd || !g_readbackEndMap) return;
    const uint64_t done = g_fenceEnd->GetCompletedValue();
    std::lock_guard<std::mutex> lock(g_endMtx);
    for (uint32_t i = 0; i < kEndRing; i++)
    {
        PendingEnd& p = g_pendingEnd[i];
        if (!p.inflight || p.fenceValue > done) continue;
        p.inflight = false;
        const uint64_t q = GpuToQpc(g_readbackEndMap[i]);
        std::lock_guard<std::mutex> l2(g_mtx);
        PresentRecord& r = g_presents[p.present % 16];
        // the record is still this present's (16 presents later it is reused)
        if (g_nPresents - p.present <= 16 && r.frameId == p.frameId && q) r.gpuDone = q > r.queued ? q : r.queued;
    }
}

HRESULT STDMETHODCALLTYPE HookedExecuteCommandLists(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists)
{
    if (g_listEnd[0] && n > 0 && lists && q != g_gameQueue && q->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT && CallerIsGame(_ReturnAddress()))
    {
        // the queue the game renders on, for the end-of-frame timestamps (queues live as long as the device)
        q->AddRef();
        ID3D12CommandQueue* old = (ID3D12CommandQueue*)InterlockedExchangePointer((PVOID volatile*)&g_gameQueue, q);
        if (old) old->Release();
    }
    const LONG frame = g_waitingFrame;
    if (frame >= 0 && n > 0 && lists && g_fence)
    {
        D3D12_COMMAND_QUEUE_DESC qd = q->GetDesc();
        if (qd.Type == D3D12_COMMAND_LIST_TYPE_DIRECT && CallerIsGame(_ReturnAddress()) &&
            InterlockedCompareExchange(&g_waitingFrame, -1, frame) == frame)
        {
            const uint32_t i = (uint32_t)frame & (kRing - 1);
            Pending& p = g_pending[i];
            if (!p.inflight && g_list[i])
            {
                ID3D12CommandList* mine[1] = { g_list[i] };
                g_origECL(q, 1, mine);
                const uint64_t fv = InterlockedIncrement64((volatile LONG64*)&g_fenceValue);
                q->Signal(g_fence, fv);
                p.frameId = (uint32_t)frame; p.fenceValue = fv; p.inflight = true;
                // the CPU slot means "when the work was handed to the GPU", not the marker: XeLL subtracts it from the
                // GPU start to get the render-queue wait and logs marker->execute separately ("RenderSubmitStart to
                // Execute delay"). Replace the provisional marker time unless XeLL already consumed the slot.
                uint64_t* cslot = (uint64_t*)(g_cpuBuf + ((uint32_t)frame & (kSlots - 1)) * kCpuSlot + 8);
                const uint64_t ex = Qpc(), mk = g_markerQpc[(uint32_t)frame & (kSlots - 1)];
                if (*cslot) *cslot = QpcToNs(ex);
                // XeLL's "RenderSubmitStart to Execute delay" (its own message never leaves the DLL): logged every frame
                if (mk && ex >= mk && g_qpf.QuadPart)
                    TraceF("XELLPROBE execute delay frame %u: %.2f ms", (unsigned)frame, (ex - mk) * 1000.0 / (double)g_qpf.QuadPart);
                if (InterlockedIncrement(&g_hookLogged) <= 8)
                {
                    char where[160]; ModuleOffset(_ReturnAddress(), where, sizeof(where));
                    TraceF("XELLPROBE timestamp query queued ahead of frame %u's first submit (%u list(s), queue %p, from %s)", (unsigned)frame, n, q, where);
                }
            }
        }
    }
    return g_origECL(q, n, lists);
}

bool SetupGpuStart(ID3D12Device* dev)
{
    if (g_fence || !GpuStartEnabled()) return g_fence != nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&SetupGpuStart, &g_self);
    D3D12_QUERY_HEAP_DESC qh = {}; qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; qh.Count = kRing;
    if (FAILED(dev->CreateQueryHeap(&qh, __uuidof(ID3D12QueryHeap), (void**)&g_qheap))) { TraceF("XELLPROBE gpu start: CreateQueryHeap failed"); return false; }
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd = {}; rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = kRing * sizeof(uint64_t); rd.Height = 1; rd.DepthOrArraySize = 1;
    rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), (void**)&g_readback)))
    { TraceF("XELLPROBE gpu start: readback buffer failed"); return false; }
    D3D12_RANGE all = { 0, kRing * sizeof(uint64_t) };
    if (FAILED(g_readback->Map(0, &all, (void**)&g_readbackMap))) { TraceF("XELLPROBE gpu start: Map failed"); return false; }
    for (uint32_t i = 0; i < kRing; i++)
    {
        if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&g_alloc[i])) ||
            FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc[i], nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&g_list[i])))
        { TraceF("XELLPROBE gpu start: command list %u failed", i); return false; }
        // recorded once, executed many times: a closed list can be re-executed as long as it is never reset
        g_list[i]->EndQuery(g_qheap, D3D12_QUERY_TYPE_TIMESTAMP, i);
        g_list[i]->ResolveQueryData(g_qheap, D3D12_QUERY_TYPE_TIMESTAMP, i, 1, g_readback, i * sizeof(uint64_t));
        if (FAILED(g_list[i]->Close())) { TraceF("XELLPROBE gpu start: Close failed"); return false; }
    }
    if (FlipQueueModel())
    {
        D3D12_QUERY_HEAP_DESC qe = {}; qe.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; qe.Count = kEndRing;
        D3D12_RESOURCE_DESC re = rd; re.Width = kEndRing * sizeof(uint64_t);
        D3D12_RANGE allEnd = { 0, kEndRing * sizeof(uint64_t) };
        bool ok = SUCCEEDED(dev->CreateQueryHeap(&qe, __uuidof(ID3D12QueryHeap), (void**)&g_qheapEnd)) &&
                  SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &re, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), (void**)&g_readbackEnd)) &&
                  SUCCEEDED(g_readbackEnd->Map(0, &allEnd, (void**)&g_readbackEndMap));
        for (uint32_t i = 0; ok && i < kEndRing; i++)
        {
            ok = SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&g_allocEnd[i])) &&
                 SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_allocEnd[i], nullptr, __uuidof(ID3D12GraphicsCommandList), (void**)&g_listEnd[i]));
            if (!ok) break;
            g_listEnd[i]->EndQuery(g_qheapEnd, D3D12_QUERY_TYPE_TIMESTAMP, i);
            g_listEnd[i]->ResolveQueryData(g_qheapEnd, D3D12_QUERY_TYPE_TIMESTAMP, i, 1, g_readbackEnd, i * sizeof(uint64_t));
            ok = SUCCEEDED(g_listEnd[i]->Close());
        }
        ok = ok && SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&g_fenceEnd));
        if (ok) TraceF("XELLPROBE flip queue: end-of-frame timestamps on the game's queue; shown = first vsync after the GPU is done");
        if (!ok)
        {
            TraceF("XELLPROBE flip queue: end-of-frame timestamps unavailable, shown = next vsync after the present");
            for (uint32_t i = 0; i < kEndRing; i++) g_listEnd[i] = nullptr;   // (leaked: a failed setup happens once)
            g_readbackEndMap = nullptr;
        }
    }
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void**)&g_fence))) { TraceF("XELLPROBE gpu start: CreateFence failed"); return false; }
    // hook ExecuteCommandLists through the vtable shared by every command queue of this D3D12 implementation
    void** vtbl = *(void***)g_queue;
    DWORD old;
    if (!VirtualProtect(&vtbl[10], sizeof(void*), PAGE_READWRITE, &old)) { TraceF("XELLPROBE gpu start: cannot write the command queue vtable"); return false; }
    g_origECL = (PFN_ExecuteCommandLists)vtbl[10];
    vtbl[10] = (void*)HookedExecuteCommandLists;
    VirtualProtect(&vtbl[10], sizeof(void*), old, &old);
    TraceF("XELLPROBE gpu start: ExecuteCommandLists hooked (vtable %p, original %p); timestamp queries go ahead of each frame's first submit", vtbl, (void*)g_origECL);
    return true;
}
}

// at a present: a timestamp behind the frame's GPU work on the game's queue; when it is done, the record learns when
// the frame could first be shown
void StampPresentEnd(uint32_t present, uint32_t frameId)
{
    ID3D12CommandQueue* q = g_gameQueue;
    if (!q || !g_listEnd[0] || !g_origECL || !g_fenceEnd) return;
    const uint32_t i = present % kEndRing;
    std::lock_guard<std::mutex> lock(g_endMtx);
    PendingEnd& p = g_pendingEnd[i];
    if (p.inflight) return;           // 16 presents behind: the GPU is far behind, leave this one unknown
    ID3D12CommandList* mine[1] = { g_listEnd[i] };
    g_origECL(q, 1, mine);
    const uint64_t fv = ++g_fenceEndValue;     // under g_endMtx
    q->Signal(g_fenceEnd, fv);
    p.present = present; p.frameId = frameId; p.fenceValue = fv; p.inflight = true;
}

// write the CPU and GPU time of a render submit into the slot of frameId
void StampSubmit(uint32_t frameId)
{
    if (!g_cpuBuf || !g_gpuBuf) return;
    UINT64 gpu = 0, cpu = 0;
    if (g_queue && FAILED(g_queue->GetClockCalibration(&gpu, &cpu))) gpu = 0;
    if (gpu > 1 && cpu) { g_calGpu = gpu; g_calCpu = cpu; }
    if (!cpu) cpu = Qpc();
    if (gpu < 2) gpu = 2;          // XeLL ignores slots with gpu <= 1 (and would never pace the frame)
    const uint32_t slot = frameId & (kSlots - 1);
    HarvestGpuStarts();
    *(uint64_t*)(g_cpuBuf + slot * kCpuSlot + 8) = QpcToNs(cpu);
    g_markerQpc[slot] = cpu;
    *(uint64_t*)(g_gpuBuf + slot * kGpuSlot) = gpu;   // provisional; replaced by the measured GPU start when it arrives
    if (g_fence) InterlockedExchange(&g_waitingFrame, (LONG)frameId);
}

void RecordPresent(uint32_t seq)
{
    const uint64_t now = Qpc();
    uint32_t idx;
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        idx = g_nPresents;
        PresentRecord& r = g_presents[idx % 16];
        r.frameId = seq; r.queued = now; r.shown = NextVsyncAfter(now); r.gpuDone = 0;
        g_nPresents++; g_lastPresentId = seq;
    }
    if (FlipQueueModel()) StampPresentEnd(idx, seq);
}

// fill XeLL's 928-byte telemetry block
void FillTelemetry(uint8_t* t, UINT size)
{
    memset(t, 0, size);
    HarvestGpuEnds();
    const uint64_t now = Qpc();
    std::lock_guard<std::mutex> lock(g_mtx);
    const uint32_t n = g_nPresents < 16 ? g_nPresents : 16;
    const bool fq = FlipQueueModel() && g_readbackEndMap && g_fenceEnd && g_gameQueue;
    uint64_t prevShown = 0, inQueue = 0;
    // oldest first; XeLL takes the max frame id as the latest presented frame and the earliest times per id
    for (uint32_t i = 0; i < n; i++)
    {
        PresentRecord& r = g_presents[(g_nPresents - n + i) % 16];
        if (fq)
        {
            // ready when its GPU work is done (still in flight: not before now), one flip per vsync after the one before
            const uint64_t ready = r.gpuDone ? r.gpuDone : (now > r.queued ? now : r.queued);
            uint64_t shown = g_vsyncOn ? NextVsyncAfter(ready) : ready;
            if (g_vsyncOn && prevShown && shown <= prevShown) shown = prevShown + g_periodMin;
            r.shown = shown;
            prevShown = shown;
            if (!r.gpuDone || shown > now) inQueue++;
        }
        uint8_t* rec = t + i * 56;
        *(uint32_t*)(rec + 0x08) = r.frameId;
        *(uint32_t*)(rec + 0x20) = g_vsyncOn ? 4u : 0u;
        *(uint64_t*)(rec + 0x28) = r.queued;
        *(uint64_t*)(rec + 0x30) = r.shown;
    }
    if (fq && n)
    {
        // logged every call: the probe script takes the median as the present queue XeLL is told about
        const PresentRecord& last = g_presents[(g_nPresents - 1) % 16];
        TraceF("XELLPROBE flip queue: %llu frame(s) waiting, last flip in queue %.2f ms", (unsigned long long)inQueue,
               last.shown > last.queued ? (last.shown - last.queued) * 1000.0 / (double)g_qpf.QuadPart : 0.0);
    }
    // reference vsync: the last modelled vsync before now
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
    if (EnsureQueue(ctx)) SetupGpuStart(DevOf(ctx));
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
    HarvestGpuStarts();   // XeLL reads the timing slots right after this call
    FillTelemetry((uint8_t*)pTelemetryData, size);
    return S_OK;
}

} // extern "C"

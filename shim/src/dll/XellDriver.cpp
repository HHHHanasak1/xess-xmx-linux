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
// IGDEXT_XELL_PROBE=<version> makes the shim hide wine_get_version from libxell.dll (an import-table hook of GetProcAddress
// in libxell.dll only, installed when the DLL loads), claim support for that latency-reduction interface version, answer
// the Set* calls with S_OK, stamp a QPC timestamp per RenderSubmitStart, and log every call plus every access XeLL makes
// to the render-submit timing buffers (guard pages), to work out the undocumented buffer layout. IGDEXT_XELL_TELEMETRY=1
// additionally answers GetDisplayTelemetry with a zeroed buffer instead of E_NOTIMPL. Without IGDEXT_XELL_PROBE nothing
// here is active and every function answers "not supported", as before.
#include "Stdafx.h"
#include "Trace.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <intrin.h>
#include <winternl.h>

namespace
{
int g_probeVersion = -1;           // -1: not read yet, 0: probe off
uint32_t g_used = 0, g_slots = 0;
BOOL g_enabled = FALSE, g_timings = FALSE;
uint8_t* g_cpuBuf = nullptr;
uint8_t* g_gpuBuf = nullptr;
size_t g_bufSize = 0;
volatile LONG g_accessLogged = 0;
void* g_rearm = nullptr;          // page to re-guard after the single step

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
        g_rearm = (void*)((uintptr_t)a & ~(uintptr_t)0xfff);
        ep->ContextRecord->EFlags |= 0x100;   // single step, then guard the page again
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_SINGLE_STEP && g_rearm)
    {
        DWORD old; VirtualProtect(g_rearm, 1, PAGE_READWRITE | PAGE_GUARD, &old);
        g_rearm = nullptr;
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
    static HMODULE done = nullptr;
    if (!libxell || libxell == done) return;
    void* orig = nullptr;
    if (PatchImport(libxell, "KERNEL32.dll", "GetProcAddress", (void*)XellGetProcAddress, &orig))
    {
        if (!g_realGetProcAddress) g_realGetProcAddress = (PFN_GetProcAddress)orig;
        done = libxell;
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

extern "C" {

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

HRESULT _INTC_D3D12_LatencyReductionExt(INTCExtensionContext*, uint32_t version, BOOL latencyReductionEnabled, BOOL renderSubmitTimingsEnabled, uint32_t timingSlots)
{
    TraceF("XELLPROBE LatencyReductionExt(version %u, enabled %d, timings %d, slots %u)", version, latencyReductionEnabled, renderSubmitTimingsEnabled, timingSlots);
    if (ProbeVersion() <= 0) return E_NOTIMPL;
    g_used = version; g_enabled = latencyReductionEnabled; g_timings = renderSubmitTimingsEnabled;
    if (renderSubmitTimingsEnabled && timingSlots && !g_cpuBuf)
    {
        g_slots = timingSlots;
        g_bufSize = ((size_t)timingSlots * 512 + 0xfff) & ~(size_t)0xfff;
        AddVectoredExceptionHandler(1, GuardHandler);
        g_cpuBuf = GuardedBuffer(g_bufSize);
        g_gpuBuf = GuardedBuffer(g_bufSize);
        TraceF("XELLPROBE buffers cpu %p gpu %p (%zu bytes each)", g_cpuBuf, g_gpuBuf, g_bufSize);
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
    if (k <= 40 || (k % 600) == 0)
    {
        char where[160]; ModuleOffset(_ReturnAddress(), where, sizeof(where));
        TraceF("XELLPROBE RenderSubmitStart(frame %u) #%ld +%.2f ms from %s", frameId, (long)k, sincePrevMs, where);
    }
    return ProbeVersion() > 0 ? S_OK : E_NOTIMPL;
}

// XeLL / XeSS-FG pacing information. On Windows the driver uses these to pace generated frames and to match
// present sequence numbers against render-submit timings. The probe accepts them (S_OK) so that a failed call here
// cannot be the reason XeLL stays in cross-vendor mode; the values are logged to see how XeLL uses them.
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
    if (k <= 40 || (k % 600) == 0) TraceF("XELLPROBE SetPresentSequenceNumber(%u) #%ld", PresentSequenceNumber, (long)k);
    return ProbeVersion() > 0 ? S_OK : E_NOTIMPL;
}

HRESULT _INTC_D3D12_GetDisplayTelemetry(INTCExtensionContext*, void* pTelemetryData, UINT size)
{
    static LONG n = 0;
    static int telemetry = -1;
    if (telemetry < 0) { char b[8]; telemetry = GetEnvironmentVariableA("IGDEXT_XELL_TELEMETRY", b, sizeof(b)) > 0 ? atoi(b) : 0; }
    const LONG k = InterlockedIncrement(&n);
    if (k <= 8 || (k % 600) == 0)
    {
        char where[160]; ModuleOffset(_ReturnAddress(), where, sizeof(where));
        TraceF("XELLPROBE GetDisplayTelemetry(size %u) #%ld from %s -> %s", size, (long)k, where, telemetry ? "zeroed S_OK" : "E_NOTIMPL");
    }
    if (!telemetry || ProbeVersion() <= 0) return E_NOTIMPL;
    if (pTelemetryData && size) memset(pTelemetryData, 0, size);
    return S_OK;
}

} // extern "C"

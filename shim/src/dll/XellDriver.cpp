// XellDriver.cpp - driver side of Intel XeLL's latency-reduction extension (_INTC_D3D12_*LatencyReduction*, RenderSubmitStart,
// SetNumGeneratedFrames, SetPresentSequenceNumber, GetDisplayTelemetry).
//
// What libxell.dll (XeSS SDK 2.x) does with these, from its own strings: it loads igdext64.dll itself (env var
// INTC_ALT_DRIVER_EXTENSIONS_PATH, the device's driver folder, or the current folder), creates an extension context,
// and decides between "Latency reduction enabled." (driver timings) and "Latency reduction enabled - Cross Vendor."
// (its own CPU-side pacing). In driver mode it polls the render-submit timing buffers for the CPU and GPU timestamps of
// each frame's submission ("[Driver] Driver timestamps not available for frame", "[RenderSubmitTimingsTimestamps]
// GetRenderStartTimestamp failed Map") and reports the present sequence / generated-frame count through the two Set*
// calls. XELL_FORCE_LOGS_TO_FILE=1 makes it write that decision path to libxell.log; XELL_FORCE_CROSS_VENDOR=1 forces the
// fallback. There is no separate driver DLL in this path (igxell.dll is XeLL's optional "driver DLL context", a newer
// XeLL carried by the Windows driver, and is not needed for driver timings).
//
// IGDEXT_XELL_PROBE=<version> makes the shim claim support for that interface version, answer the Set* calls with S_OK,
// stamp a QPC timestamp per RenderSubmitStart, and log every call plus every access XeLL makes to the render-submit
// timing buffers (guard pages), to work out the undocumented buffer layout. IGDEXT_XELL_TELEMETRY=1 additionally
// answers GetDisplayTelemetry with a zeroed buffer instead of E_NOTIMPL (to see whether XeLL requires it for driver
// mode). Without IGDEXT_XELL_PROBE every function answers "not supported", as before.
#include "Stdafx.h"
#include "Trace.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <intrin.h>

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

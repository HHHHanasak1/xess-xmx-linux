// XellHook.h - what the source files of the shim's hooks on libxell.dll and libxess_fg.dll share:
//   XellHook.cpp      libxell.dll: the frame cap fix, the low latency's wait (AutoPaceWait), IGDEXT_XELL_LOG; the
//                     environment variables are listed at its top
//   XefgAnswers.cpp   a game's questions to XeSS-FG's swap chain, answered as of its last Present (IGDEXT_XELL_POLLS)
//   XefgHook.cpp      libxess_fg.dll: IGDEXT_XEFG_LOG, XESS_FG above 4x, frame generation on / off for the low latency
//   HookUtil.cpp      what the hooks are put in with: PatchThunk, the wait for a library the game loads later, the
//                     thread that keeps this module mapped meanwhile
// State that several of these files use is defined here, as C++17 inline variables (one for the DLL); what one file
// alone uses is static, or in an unnamed namespace, in that file.
#pragma once
#include "Stdafx.h"
#include "Trace.h"

// IGDEXT_XELL_LOG=1: lines for C:\igdext_xell.log
inline bool g_log = false;
static void XLog(const char* fmt, ...)
{
    if (!g_log) return;
    va_list ap; va_start(ap, fmt); AppendLogLine("C:\\igdext_xell.log", fmt, ap); va_end(ap);
}

// The low latency's switches, set by StartXellHook (XellHook.cpp) before any hook is in: IGDEXT_XELL_CAP is auto
// (the default), not off or a fixed cap
inline bool g_capAuto = true;
inline bool g_polls = true;   // IGDEXT_XELL_POLLS=0: the swap chain answers the game itself
// Whether frame generation is on, from two places: XeLL's xellSetFgEnabled (g_mfg; missed when a game enables frame
// generation before XellThread's hooks are in, 200 ms after the library maps) and XeSS-FG's own switch
// xefgSwapChainSetEnabled (Hook_XefgSetEnabled). The newer call of the two decides (Wuthering Waves switches frame
// generation off in its menu through XeLL only, CONTROL Resonant's FSR route through XeSS-FG only). Neither seen = off: a game
// that loads libxess_fg.dll but keeps frame generation off never calls either.
inline volatile LONG g_fgState = -1;   // -1 not seen, 0 off, 1 on: the newer of the two calls
inline volatile const char* g_fgSource = "not seen";

// Up to four pointers, added and removed with compare-exchange and looked up without a lock (on every xellSleep, on
// every call of the swap chain's methods): the XeLL contexts and the swap chains that the package's own frame
// generation DLL tells the shim to leave to it (IgdextXellExternalPacing, IgdextXefgProxyOwned).
struct PtrSlots
{
    void* volatile p[4];
    bool Has(void* x) const
    {
        for (int i = 0; i < 4; ++i) if (p[i] == x) return true;
        return false;
    }
    int Add(void* x)   // 1: added, 0: was there already, -1: no free slot
    {
        if (Has(x)) return 0;
        for (int i = 0; i < 4; ++i) if (InterlockedCompareExchangePointer(&p[i], x, nullptr) == nullptr) return 1;
        return -1;
    }
    bool Remove(void* x)
    {
        for (int i = 0; i < 4; ++i) if (InterlockedCompareExchangePointer(&p[i], nullptr, x) == x) return true;
        return false;
    }
};

// this module; flags as for GetModuleHandleExW (0 takes a reference, GET_MODULE_HANDLE_EX_FLAG_PIN keeps it loaded until
// the process exits, GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT neither)
static HMODULE SelfModule(DWORD flags)
{
    HMODULE self = nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | flags, (LPCWSTR) &SelfModule, &self) ? self : nullptr;
}

// XefgAnswers.cpp: a game's questions to XeSS-FG's swap chain
inline void** volatile g_scVt;          // the virtual table the hooks are in
void ProxyHook(IUnknown* unk);

// HookUtil.cpp
bool PatchThunk(void* thunk, void* hook, void** orig, const char* name);
enum { WATCH_XELL = 0, WATCH_XEFG = 1, WATCH_COUNT = 2 };
const DWORD kWatchTotalMs = 600000;   // 10 minutes, as the polling loops before (6000 x 100 ms, 60000 x 10 ms)
HANDLE WatchStart(int which);
void WatchStop();
bool WatchWait(HANDLE ev, ULONGLONG deadline, DWORD pollMs);
bool StartModuleRefThread(LPTHREAD_START_ROUTINE body);

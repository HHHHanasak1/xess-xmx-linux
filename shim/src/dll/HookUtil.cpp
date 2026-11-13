// HookUtil.cpp - what the shim's hooks on libxell.dll (XellHook.cpp) and libxess_fg.dll (XefgHook.cpp) are put in with:
// PatchThunk redirects an export, WatchStart / WatchWait / WatchStop wait for a library that the game loads later, and
// StartModuleRefThread runs that wait on a thread that keeps this module mapped.
#include "XellHook.h"
#include "LdrNotify.h"
#include <algorithm>
#include <cstdint>

bool PatchThunk(void* thunk, void* hook, void** orig, const char* name)
{
    // a patched export jumps into this module from then on: it must never be unmapped (games load, free and load the
    // shim again at startup)
    static volatile LONG pinned = 0;
    if (!pinned && !SelfModule(GET_MODULE_HANDLE_EX_FLAG_PIN)) { XLog("%s: could not pin igdext64 (%lu), not hooked", name, GetLastError()); return false; }
    pinned = 1;
    uint8_t* p = (uint8_t*) thunk;
    if (p[0] != 0xE9) { XLog("%s: export is not a jmp rel32 thunk (byte %02x), not hooked", name, p[0]); return false; }
    int32_t rel = *(int32_t*) (p + 1);
    *orig = p + 5 + rel;
    intptr_t d = (uint8_t*) hook - (p + 5);
    if (d > INT32_MAX || d < INT32_MIN) { XLog("%s: hook too far from the thunk (%lld), not hooked", name, (long long) d); return false; }
    // only the rel32 (bytes 1-4) changes. When it lies inside one aligned 8-byte word (10 of 16 thunk positions), that
    // word is swapped (compare-exchange: the neighbouring thunks' bytes stay as they are); otherwise the 8 bytes from the
    // thunk on, unaligned as before (atomic on x86-64, a split lock when they cross a cache line)
    const uintptr_t r = (uintptr_t) (p + 1);
    uint8_t* w = (r & 7) <= 4 ? (uint8_t*) (r & ~(uintptr_t) 7) : p;
    uint8_t* lo = w < p ? w : p;
    const SIZE_T span = (SIZE_T) (std::max(w, p) + 8 - lo);
    DWORD old;
    if (!VirtualProtect(lo, span, PAGE_EXECUTE_READWRITE, &old)) { XLog("%s: VirtualProtect failed %lu", name, GetLastError()); return false; }
    if (w != p)
    {
        const unsigned sh = (unsigned) (r & 7) * 8;
        const uint64_t mask = 0xFFFFFFFFull << sh, newRel = (uint64_t) (uint32_t) (int32_t) d << sh;
        uint64_t cur = *(volatile uint64_t*) w;
        for (;;)
        {
            const uint64_t prev = (uint64_t) InterlockedCompareExchange64((volatile LONG64*) w, (LONG64) ((cur & ~mask) | newRel), (LONG64) cur);
            if (prev == cur) break;
            cur = prev;
        }
    }
    else
    {
        uint64_t cur = *(volatile uint64_t*) p;
        uint64_t neu = (cur & ~0xFFFFFFFFFFull) | 0xE9ull | ((uint64_t)(uint32_t)(int32_t) d << 8);
        InterlockedExchange64((volatile LONG64*) p, (LONG64) neu);
    }
    VirtualProtect(lo, span, old, &old);
    FlushInstructionCache(GetCurrentProcess(), lo, span);
    XLog("%s hooked: thunk %p -> %p (orig %p)", name, thunk, hook, *orig);
    return true;
}

// The hook threads wait for a library the game loads later. The loader tells them when it is mapped
// (LdrRegisterDllNotification, LdrNotify.h): the callback runs under the loader lock and does nothing but signal the
// thread's event. The install code (PatchThunk pins this module, XefgInstallHooks calls FreeLibrary) runs on the
// thread, never in the callback. One registration serves both threads; the last one done removes it before its module
// reference goes, so the callback is never called into an unmapped module. Without the notification (registration
// refused) the threads poll as they used to.
namespace
{
const wchar_t* const kWatchedDll[WATCH_COUNT] = { L"libxell.dll", L"libxess_fg.dll" };
SRWLOCK g_watchLock = SRWLOCK_INIT;
HANDLE g_watchEvent[WATCH_COUNT];     // auto-reset, one per waiter: a load of the other library wakes nobody
PVOID g_watchCookie = nullptr;
int g_watchUsers = 0;
PFN_LdrUnregisterDllNotification g_ldrUnregister = nullptr;

VOID CALLBACK OnWatchedDllLoaded(ULONG reason, const LdrDllNotificationData* data, PVOID)
{
    if (reason != kLdrDllLoaded || !data) return;
    for (int i = 0; i < WATCH_COUNT; ++i)
        if (LdrDllNameIs(data->BaseDllName, kWatchedDll[i])) SetEvent(g_watchEvent[i]);
}

}

// the event the loader signals when the library is mapped; nullptr when there is no notification (the caller polls).
// The caller checks for a library loaded before this call returns, and ends with WatchStop.
HANDLE WatchStart(int which)
{
    AcquireSRWLockExclusive(&g_watchLock);
    if (g_watchUsers == 0)
    {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        auto reg = ntdll ? (PFN_LdrRegisterDllNotification) GetProcAddress(ntdll, "LdrRegisterDllNotification") : nullptr;
        g_ldrUnregister = ntdll ? (PFN_LdrUnregisterDllNotification) GetProcAddress(ntdll, "LdrUnregisterDllNotification") : nullptr;
        bool ok = reg && g_ldrUnregister;
        for (HANDLE& e : g_watchEvent) ok = (e = CreateEventW(nullptr, FALSE, FALSE, nullptr)) != nullptr && ok;
        if (ok && reg(0, OnWatchedDllLoaded, nullptr, &g_watchCookie) != 0) { g_watchCookie = nullptr; ok = false; }
        if (!ok) for (HANDLE& e : g_watchEvent) { if (e) CloseHandle(e); e = nullptr; }
    }
    HANDLE ev = g_watchCookie ? g_watchEvent[which] : nullptr;
    if (ev) ++g_watchUsers;
    ReleaseSRWLockExclusive(&g_watchLock);
    return ev;
}

void WatchStop()
{
    AcquireSRWLockExclusive(&g_watchLock);
    if (--g_watchUsers == 0)
    {
        g_ldrUnregister(g_watchCookie);   // takes the loader lock: no callback runs after it
        g_watchCookie = nullptr;
        for (HANDLE& e : g_watchEvent) { CloseHandle(e); e = nullptr; }
    }
    ReleaseSRWLockExclusive(&g_watchLock);
}

// one wait of the remaining time: the event, or a poll interval without one; false once the total is used up
bool WatchWait(HANDLE ev, ULONGLONG deadline, DWORD pollMs)
{
    const ULONGLONG now = GetTickCount64();
    if (now >= deadline) return false;
    const ULONGLONG left = deadline - now;
    if (ev) WaitForSingleObject(ev, left > 0xFFFFFFFEull ? 0xFFFFFFFE : (DWORD) left); else Sleep(pollMs);
    return true;
}

// A thread that waits for a library holds its own reference to this module, so its code stays mapped while the game
// frees and loads the shim again at startup, and drops it only when it is done.
static DWORD WINAPI ModuleRefThread(LPVOID body)
{
    ((LPTHREAD_START_ROUTINE) body)(nullptr);
    FreeLibraryAndExitThread(SelfModule(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT), 0);
}

bool StartModuleRefThread(LPTHREAD_START_ROUTINE body)
{
    HMODULE self = SelfModule(0);
    if (!self) return false;
    HANDLE h = CreateThread(nullptr, 0, ModuleRefThread, (LPVOID) body, 0, nullptr);
    if (!h) { FreeLibrary(self); return false; }
    CloseHandle(h);
    return true;
}

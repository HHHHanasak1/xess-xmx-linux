// XellHook.cpp - in-process fix for the frame cap Wuthering Waves hands to Intel XeLL (libxell.dll) when frame generation is enabled.
// XeLL's exports are 5-byte "jmp rel32" thunks; xellSetSleepMode / xellSetFgEnabled / xellSleep are redirected to the functions below.
// The game applies a 33.3 ms frame cap (30 fps, counted per displayed frame) right after it enables frame generation and only
// replaces it when the pause menu is opened and closed. A nonzero interval arriving within 1.5 s of xellSetFgEnabled(1) is replaced by
// the interval that was in effect before.
//   IGDEXT_XELL_KEEPCAP=1   turn that fix off
//   IGDEXT_XELL_LOG=1       append the parameters the game passes and xellSleep timing (real frame rate) to C:\igdext_xell.log
#include "Stdafx.h"
#include "Trace.h"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>

#pragma pack(push, 1)
struct XellSleepParams { uint32_t minimumIntervalUs; uint32_t flags; }; // flags bit0 = bLowLatencyMode, bit1 = bLowLatencyBoost
#pragma pack(pop)
typedef uint32_t (*PFN_XellSet)(void*, const XellSleepParams*);
typedef uint32_t (*PFN_XellSleep)(void*, uint32_t);
typedef uint32_t (*PFN_XellFg)(void*, uint32_t, uint32_t);

static PFN_XellSet g_origSet; static PFN_XellSleep g_origSleep; static PFN_XellFg g_origFg;
static bool g_log = false;
static bool g_fixCap = true; static uint32_t g_effInterval = 0, g_savedInterval = 0; static ULONGLONG g_fgOnTick = 0;
static volatile LONG g_sleepCalls = 0;

static void XLog(const char* fmt, ...)
{
    if (!g_log) return;
    FILE* f = nullptr;
    if (fopen_s(&f, "C:\\igdext_xell.log", "ab") != 0 || !f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "%02d:%02d:%02d.%03d [%lu] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, GetCurrentThreadId());
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}

static uint32_t Hook_SetFg(void* ctx, uint32_t a, uint32_t b)
{
    static int n = 0;
    if (n++ < 12 || (n % 600) == 0) XLog("xellSetFgEnabled ctx=%p a=%u b=%u", ctx, a, b);
    if (a == 1) { g_savedInterval = g_effInterval; g_fgOnTick = GetTickCount64(); }
    return g_origFg(ctx, a, b);
}

static uint32_t Hook_SetSleepMode(void* ctx, const XellSleepParams* p)
{
    if (!p) return g_origSet(ctx, p);
    XellSleepParams q = *p;
    XLog("xellSetSleepMode ctx=%p game: minimumIntervalUs=%u lowLatency=%u boost=%u", ctx, p->minimumIntervalUs, p->flags & 1, (p->flags >> 1) & 1);
    if (g_fixCap && q.minimumIntervalUs != 0 && g_fgOnTick && GetTickCount64() - g_fgOnTick < 1500)
    { XLog("   -> frame cap %u us set right after frame generation was enabled: keeping %u us", q.minimumIntervalUs, g_savedInterval); q.minimumIntervalUs = g_savedInterval; }
    g_effInterval = q.minimumIntervalUs;
    return g_origSet(ctx, &q);
}

static uint32_t Hook_Sleep(void* ctx, uint32_t frame)
{
    LONG n = InterlockedIncrement(&g_sleepCalls);
    if (!g_log) return g_origSleep(ctx, frame);
    if (n < 5 || (n % 600) == 0) XLog("xellSleep call #%ld frame=%u", (long) n, frame);
    LARGE_INTEGER qa, qb, qf; QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&qa);
    uint32_t rr = g_origSleep(ctx, frame);
    QueryPerformanceCounter(&qb);
    static double sum = 0, mx = 0; static int cnt = 0; static LARGE_INTEGER t0;
    double d = (qb.QuadPart - qa.QuadPart) * 1000.0 / qf.QuadPart;
    if (!t0.QuadPart) t0 = qa;
    sum += d; if (d > mx) mx = d; ++cnt;
    if (cnt == 100)
    {
        double wall = (qb.QuadPart - t0.QuadPart) * 1000.0 / qf.QuadPart;
        XLog("xellSleep stats: 100 calls in %.0f ms (%.1f ms/frame), blocked avg %.2f ms max %.2f ms = %.0f%% of wall", wall, wall / 100, sum / 100, mx, 100.0 * sum / wall);
        sum = mx = 0; cnt = 0; t0 = qb;
    }
    return rr;
}

static bool PatchThunk(void* thunk, void* hook, void** orig, const char* name)
{
    uint8_t* p = (uint8_t*) thunk;
    if (p[0] != 0xE9) { XLog("%s: export is not a jmp rel32 thunk (byte %02x), not hooked", name, p[0]); return false; }
    int32_t rel = *(int32_t*) (p + 1);
    *orig = p + 5 + rel;
    intptr_t d = (uint8_t*) hook - (p + 5);
    if (d > INT32_MAX || d < INT32_MIN) { XLog("%s: hook too far from the thunk (%lld), not hooked", name, (long long) d); return false; }
    DWORD old;
    if (!VirtualProtect(p, 8, PAGE_EXECUTE_READWRITE, &old)) { XLog("%s: VirtualProtect failed %lu", name, GetLastError()); return false; }
    uint64_t cur = *(volatile uint64_t*) p;
    uint64_t neu = (cur & ~0xFFFFFFFFFFull) | 0xE9ull | ((uint64_t)(uint32_t)(int32_t) d << 8);
    InterlockedExchange64((volatile LONG64*) p, (LONG64) neu);
    VirtualProtect(p, 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 8);
    XLog("%s hooked: thunk %p -> %p (orig %p)", name, thunk, hook, *orig);
    return true;
}

static DWORD WINAPI XellThread(LPVOID)
{
    HMODULE m = nullptr;
    for (int i = 0; i < 6000 && !(m = GetModuleHandleW(L"libxell.dll")); ++i) Sleep(100);
    if (!m) { XLog("libxell.dll never loaded"); return 0; }
    XLog("libxell.dll found at %p", m);
    Sleep(200);
    auto set = (void*) GetProcAddress(m, "xellSetSleepMode");
    auto slp = (void*) GetProcAddress(m, "xellSleep");
    auto fg = (void*) GetProcAddress(m, "xellSetFgEnabled");
    if (!set || !slp || !fg) { XLog("XeLL exports missing"); return 0; }
    void* dummy = nullptr;
    PatchThunk(slp, (void*) Hook_Sleep, &dummy, "xellSleep"); g_origSleep = (PFN_XellSleep) dummy;
    PatchThunk(set, (void*) Hook_SetSleepMode, &dummy, "xellSetSleepMode"); g_origSet = (PFN_XellSet) dummy;
    PatchThunk(fg, (void*) Hook_SetFg, &dummy, "xellSetFgEnabled"); g_origFg = (PFN_XellFg) dummy;
    return 0;
}

void StartXellHook()
{
    char b[32];
    if (GetEnvironmentVariableA("IGDEXT_XELL_LOG", b, sizeof(b)) > 0) g_log = true;
    if (GetEnvironmentVariableA("IGDEXT_XELL_KEEPCAP", b, sizeof(b)) > 0 && atoi(b)) g_fixCap = false;
    if (!g_fixCap && !g_log) return;
    char exe[MAX_PATH] = {}; GetModuleFileNameA(nullptr, exe, MAX_PATH);
    XLog("XellHook start in %s", exe);
    HANDLE h = CreateThread(nullptr, 0, XellThread, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
}

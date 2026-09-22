// FgHook.cpp - diagnostic/tuning hook for the XeSS-FG software present pacer (libxess_fg.dll 1.3.1.78, Wuthering Waves build).
// The pacer waits in a "precise wait" helper at RVA 0x225580: fn(this, out, lastNs, remaining): returns at once when remaining <= 0,
// otherwise sleeps most of it and spins on QPC for the rest.
//   IGDEXT_FGWAIT_LOG=1       log every requested wait to C:\igdext_fgwait.log
//   IGDEXT_FGWAIT_SCALE=<p>   scale the requested wait to p percent (0 = never wait)
#include "Stdafx.h"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <cstring>

static const char kFgLogPath[] = "C:\\igdext_fgwait.log";
typedef uint64_t* (*PFN_FgWait)(void*, uint64_t*, uint64_t, int64_t);
static PFN_FgWait g_origFgWait; static bool g_fgLog = false; static int g_fgScale = 100;

static void FgNote(const char* msg)
{
    FILE* fp = nullptr;
    if (fopen_s(&fp, kFgLogPath, "ab") != 0 || !fp) return;
    fputs(msg, fp); fputc('\n', fp); fclose(fp);
}
static uint64_t* Hook_FgWait(void* self, uint64_t* out, uint64_t lastNs, int64_t remaining)
{
    static LARGE_INTEGER f; static LONG n = 0;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER a; QueryPerformanceCounter(&a);
    int64_t use = remaining;
    if (g_fgScale != 100 && use > 0) use = use * g_fgScale / 100;
    uint64_t* r = g_origFgWait(self, out, lastNs, use);
    if (g_fgLog && InterlockedIncrement(&n) < 6000)
    {
        LARGE_INTEGER b; QueryPerformanceCounter(&b);
        FILE* fp = nullptr;
        if (fopen_s(&fp, kFgLogPath, "ab") == 0 && fp)
        {
            fprintf(fp, "%.2f [%lu] remaining %lld last %llu waited %.3f ms\n", fmod(a.QuadPart * 1000.0 / f.QuadPart, 1e6), GetCurrentThreadId(), (long long) remaining, (unsigned long long) lastNs, (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart);
            fclose(fp);
        }
    }
    return r;
}
// xefgSwapChainTagFrameConstants(hSwapChain, presentId, constants): constants->frameRenderTime (float, ms) sits at offset 148.
//   IGDEXT_FG_FRAMETIME=<ms>  override the value the game passes (0 = "not available"); IGDEXT_FG_DEBUGLOG=1 installs XeFG's debug log callback
typedef uint32_t (*PFN_FgTag)(void*, uint32_t, const void*);
typedef void (*PFN_FgLogCb)(const char*, uint32_t, void*);
typedef uint32_t (*PFN_FgSetLog)(void*, uint32_t, PFN_FgLogCb, void*);
static PFN_FgTag g_origTag; static PFN_FgSetLog g_fgSetLog; static float g_fgFrameTime = -1.0f; static bool g_fgDebugLog = false;
static void FgLogCb(const char* msg, uint32_t level, void*)
{
    static LONG n = 0;
    if (!msg || InterlockedIncrement(&n) > 3000) return;
    char b[700]; snprintf(b, sizeof(b), "XeFG[%u] %s", level, msg); FgNote(b);
}
static uint32_t Hook_FgTag(void* sc, uint32_t presentId, const void* constants)
{
    static LONG n = 0; LONG k = InterlockedIncrement(&n);
    if (k == 1 && g_fgDebugLog && g_fgSetLog) { uint32_t r = g_fgSetLog(sc, 0, FgLogCb, nullptr); char b[80]; snprintf(b, sizeof(b), "xefgSwapChainSetLoggingCallback(debug) -> %u", r); FgNote(b); }
    if (!constants) return g_origTag(sc, presentId, constants);
    uint8_t copy[152]; memcpy(copy, constants, sizeof(copy));
    float* ft = (float*) (copy + 148);
    if (g_fgLog && (k < 6 || (k % 300) == 0)) { char b[160]; snprintf(b, sizeof(b), "TagFrameConstants presentId=%u frameRenderTime=%.3f ms resetHistory=%u", presentId, *ft, *(uint32_t*) (copy + 144)); FgNote(b); }
    if (g_fgFrameTime >= 0.0f) *ft = g_fgFrameTime;
    return g_origTag(sc, presentId, copy);
}
// xefgSwapChainSetEnabled(hSwapChain, enable): IGDEXT_FG_KEEP=1 swallows disable requests (diagnostic)
typedef uint32_t (*PFN_FgEnable)(void*, uint32_t);
static PFN_FgEnable g_origEnable; static bool g_fgKeep = false;
static uint32_t Hook_FgEnable(void* sc, uint32_t enable)
{
    static LONG n = 0;
    if (InterlockedIncrement(&n) < 200)
    {
        SYSTEMTIME st; GetLocalTime(&st); char b[160];
        snprintf(b, sizeof(b), "%02d:%02d:%02d.%03d [%lu] xefgSwapChainSetEnabled(%u)%s", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, GetCurrentThreadId(), enable, (!enable && g_fgKeep) ? " -> ignored" : "");
        FgNote(b);
    }
    if (!enable && g_fgKeep) return 0;
    return g_origEnable(sc, enable);
}
static bool FgPatchThunk(void* thunk, void* hook, void** orig)
{
    uint8_t* p = (uint8_t*) thunk;
    if (!p || p[0] != 0xE9) return false;
    *orig = p + 5 + *(int32_t*) (p + 1);
    intptr_t d = (uint8_t*) hook - (p + 5);
    if (d > INT32_MAX || d < INT32_MIN) return false;
    DWORD old; if (!VirtualProtect(p, 8, PAGE_EXECUTE_READWRITE, &old)) return false;
    uint64_t cur = *(volatile uint64_t*) p;
    uint64_t neu = (cur & ~0xFFFFFFFFFFull) | 0xE9ull | ((uint64_t) (uint32_t) (int32_t) d << 8);
    InterlockedExchange64((volatile LONG64*) p, (LONG64) neu);
    VirtualProtect(p, 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 8);
    return true;
}
static DWORD WINAPI FgThread(LPVOID)
{
    HMODULE m = nullptr;
    for (int i = 0; i < 6000 && !(m = GetModuleHandleW(L"libxess_fg.dll")); ++i) Sleep(100);
    if (!m) return 0;
    Sleep(300);
    {
        void* tag = (void*) GetProcAddress(m, "xefgSwapChainTagFrameConstants"); void* o = nullptr;
        void* sl = (void*) GetProcAddress(m, "xefgSwapChainSetLoggingCallback");
        if (sl) { uint8_t* p = (uint8_t*) sl; g_fgSetLog = (PFN_FgSetLog) ((p[0] == 0xE9) ? (void*) (p + 5 + *(int32_t*) (p + 1)) : sl); }
        void* en = (void*) GetProcAddress(m, "xefgSwapChainSetEnabled"); void* oe = nullptr;
        if (en && FgPatchThunk(en, (void*) Hook_FgEnable, &oe)) { g_origEnable = (PFN_FgEnable) oe; FgNote("xefgSwapChainSetEnabled hooked"); }
        if (tag && FgPatchThunk(tag, (void*) Hook_FgTag, &o)) { g_origTag = (PFN_FgTag) o; FgNote("xefgSwapChainTagFrameConstants hooked"); }
    }
    uint8_t* fn = (uint8_t*) m + 0x225580;
    // mov [rsp+20h],rbx ; push rdi ; push r14 ; push r15 ; sub rsp,20h   (14 bytes, position independent)
    static const uint8_t prologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x20, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x20 };
    if (memcmp(fn, prologue, sizeof(prologue)) != 0) { FgNote("unexpected libxess_fg build: pacer wait not hooked"); return 0; }
    uint8_t* tr = (uint8_t*) VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) return 0;
    memcpy(tr, prologue, sizeof(prologue));
    tr[14] = 0xFF; tr[15] = 0x25; *(uint32_t*) (tr + 16) = 0; *(uint64_t*) (tr + 20) = (uint64_t) (fn + sizeof(prologue));
    g_origFgWait = (PFN_FgWait) tr;
    intptr_t d = (uint8_t*) Hook_FgWait - (fn + 5);
    if (d > INT32_MAX || d < INT32_MIN) { FgNote("hook out of rel32 range"); return 0; }
    DWORD old; VirtualProtect(fn, 16, PAGE_EXECUTE_READWRITE, &old);
    // the first instruction is exactly 5 bytes: replace it atomically enough (threads are not in the pacer during start-up)
    uint8_t patch[5] = { 0xE9 }; *(int32_t*) (patch + 1) = (int32_t) d;
    memcpy(fn, patch, 5);
    VirtualProtect(fn, 16, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, 16);
    FgNote("XeFG pacer wait hooked");
    return 0;
}
void StartFgHook()
{
    char b[16];
    if (GetEnvironmentVariableA("IGDEXT_FGWAIT_LOG", b, sizeof(b)) > 0) g_fgLog = true;
    if (GetEnvironmentVariableA("IGDEXT_FGWAIT_SCALE", b, sizeof(b)) > 0) g_fgScale = atoi(b);
    if (GetEnvironmentVariableA("IGDEXT_FG_FRAMETIME", b, sizeof(b)) > 0) g_fgFrameTime = (float) atof(b);
    if (GetEnvironmentVariableA("IGDEXT_FG_KEEP", b, sizeof(b)) > 0 && atoi(b)) { g_fgKeep = true; g_fgLog = true; }
    if (GetEnvironmentVariableA("IGDEXT_FG_DEBUGLOG", b, sizeof(b)) > 0) { g_fgDebugLog = true; g_fgLog = true; }
    if (!g_fgLog && g_fgScale == 100 && g_fgFrameTime < 0.0f) return;
    HANDLE h = CreateThread(nullptr, 0, FgThread, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
}

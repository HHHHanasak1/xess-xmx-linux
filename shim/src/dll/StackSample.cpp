// StackSample.cpp - diagnostic: IGDEXT_STACKSAMPLE=1 makes the shim sample the stacks of the game's render/RHI/XeSS-FG/main threads.
// Sampling starts when the file C:\igdext_sample.go exists (and stops after ~4 s); every sample lists the modules whose code addresses appear on the
// first 3 KB of the (suspended) thread's stack, innermost first. Output: C:\igdext_stack.log.
#include "Stdafx.h"
#include <tlhelp32.h>
#include <psapi.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>

struct ModRange { uintptr_t lo, hi; char name[40]; };
static std::vector<ModRange> g_mods;
static void LoadMods()
{
    g_mods.clear();
    HMODULE m[1024]; DWORD need = 0;
    if (!EnumProcessModules(GetCurrentProcess(), m, sizeof(m), &need)) return;
    for (DWORD i = 0; i < need / sizeof(HMODULE) && i < 1024; ++i)
    {
        MODULEINFO mi; char nm[MAX_PATH];
        if (!GetModuleInformation(GetCurrentProcess(), m[i], &mi, sizeof(mi))) continue;
        GetModuleFileNameA(m[i], nm, sizeof(nm));
        const char* b = strrchr(nm, '\\'); b = b ? b + 1 : nm;
        ModRange r; r.lo = (uintptr_t) mi.lpBaseOfDll; r.hi = r.lo + mi.SizeOfImage; strncpy_s(r.name, sizeof(r.name), b, _TRUNCATE);
        g_mods.push_back(r);
    }
    std::sort(g_mods.begin(), g_mods.end(), [](const ModRange& a, const ModRange& b) { return a.lo < b.lo; });
}
static const ModRange* FindMod(uintptr_t v)
{
    size_t lo = 0, hi = g_mods.size();
    while (lo < hi) { size_t mid = (lo + hi) / 2; if (g_mods[mid].hi <= v) lo = mid + 1; else hi = mid; }
    return (lo < g_mods.size() && g_mods[lo].lo <= v && v < g_mods[lo].hi) ? &g_mods[lo] : nullptr;
}
typedef HRESULT (WINAPI *PFN_GetThreadDescription)(HANDLE, PWSTR*);

extern volatile unsigned long g_gameThreadId;
struct Tgt { DWORD tid; HANDLE h; std::string name; };
static DWORD WINAPI SamplerThread(LPVOID)
{
    for (;;)
    {
        if (GetFileAttributesA("C:\\igdext_sample.go") != INVALID_FILE_ATTRIBUTES) break;
        Sleep(500);
    }
    DeleteFileA("C:\\igdext_sample.go");
    auto getDesc = (PFN_GetThreadDescription) GetProcAddress(GetModuleHandleA("kernelbase.dll"), "GetThreadDescription");
    if (!getDesc) getDesc = (PFN_GetThreadDescription) GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription");
    LoadMods();
    std::vector<Tgt> tg;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te; te.dwSize = sizeof(te);
    DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    FILE* f = nullptr; fopen_s(&f, "C:\\igdext_stack.log", "wb");
    if (!f) return 0;
    if (snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te))
    {
        do {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (!h) continue;
            std::string nm = "?";
            if (getDesc) { PWSTR w = nullptr; if (SUCCEEDED(getDesc(h, &w)) && w) { char b[80]; WideCharToMultiByte(CP_UTF8, 0, w, -1, b, sizeof(b), nullptr, nullptr); nm = b; LocalFree(w); } }
            bool want = nm.find("RHIThread") != std::string::npos || nm.find("RenderThread") != std::string::npos || nm.find("XeSS") != std::string::npos || nm.find("GameThread") != std::string::npos || te.th32ThreadID == g_gameThreadId;
            if (te.th32ThreadID == g_gameThreadId) nm = "GAME";
            fprintf(f, "thread %lu name=%s want=%d\n", te.th32ThreadID, nm.c_str(), want ? 1 : 0);
            if (want) tg.push_back({ te.th32ThreadID, h, nm }); else CloseHandle(h);
        } while (Thread32Next(snap, &te));
    }
    if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
    LARGE_INTEGER qf, q0, qn; QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&q0);
    int n = 0;
    for (;;)
    {
        QueryPerformanceCounter(&qn);
        double t = (qn.QuadPart - q0.QuadPart) * 1000.0 / qf.QuadPart;
        if (t > 4000 || n > 6000) break;
        for (auto& x : tg)
        {
            if (SuspendThread(x.h) == (DWORD) -1) continue;
            CONTEXT c; memset(&c, 0, sizeof(c)); c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            char line[1500]; int o = 0; bool ok = false;
            if (GetThreadContext(x.h, &c))
            {
                ok = true;
                uintptr_t sp = (uintptr_t) c.Rsp;
                static uintptr_t words[4096]; SIZE_T got = 0;
                // chunked read: a shallow stack ends before the scan window does
                for (SIZE_T off = 0; off < sizeof(words); off += 256) { SIZE_T g2 = 0; if (!ReadProcessMemory(GetCurrentProcess(), (void*) (sp + off), (char*) words + off, 256, &g2) || g2 != 256) break; got = off + 256; }
                const ModRange* mr = FindMod((uintptr_t) c.Rip);
                o += snprintf(line + o, sizeof(line) - o, "%.1f %lu %s rip=%s+%llx |", fmod(qn.QuadPart * 1000.0 / qf.QuadPart, 1e6), x.tid, x.name.c_str(), mr ? mr->name : "?", mr ? (unsigned long long) ((uintptr_t) c.Rip - mr->lo) : (unsigned long long) c.Rip);
                std::string last; int hits = 0;
                for (SIZE_T i = 0; i < got / 8 && hits < 28 && o < 1400; ++i)
                {
                    const ModRange* m = FindMod(words[i]);
                    if (!m) continue;
                    if (hits > 3 && (!strcmp(m->name, "ntdll.dll") || !strcmp(m->name, "kernelbase.dll"))) continue;
                    // suppress runs of ntdll/kernelbase noise: keep first of a run
                    o += snprintf(line + o, sizeof(line) - o, " %s+%llx", m->name, (unsigned long long) (words[i] - m->lo)); ++hits;
                }
            }
            ResumeThread(x.h);
            if (ok) { fputs(line, f); fputc('\n', f); ++n; }
        }
        Sleep(3);
    }
    fclose(f);
    return 0;
}

void StartStackSampler()
{
    char b[8];
    if (GetEnvironmentVariableA("IGDEXT_STACKSAMPLE", b, sizeof(b)) == 0) return;
    HANDLE h = CreateThread(nullptr, 0, SamplerThread, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
}

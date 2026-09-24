#include "Stdafx.h"
#include "Trace.h"
#include <cstdio>
#include <cstring>
#include <mutex>

static std::mutex g_lock;

static bool TraceOn()
{
    static int on = -1;
    if (on < 0) { char b[8]; on = GetEnvironmentVariableA("IGDEXT_TRACE", b, sizeof(b)) > 0 ? 1 : 0; }
    return on == 1;
}

void TraceF(const char* fmt, ...)
{
    if (!TraceOn()) return;
    std::lock_guard<std::mutex> g(g_lock);
    FILE* f = nullptr;
    if (fopen_s(&f, "C:\\igdext_trace.log", "ab") != 0 || !f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "%02d:%02d:%02d.%03d [%lu] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, GetCurrentThreadId());
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}

bool DumpBlob(const char* name, const void* data, size_t size)
{
    if (!TraceOn()) return false;
    CreateDirectoryA("C:\\igdext_dump", nullptr);
    char path[MAX_PATH]; snprintf(path, sizeof(path), "C:\\igdext_dump\\%s", name);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || !f) return false;
    if (data && size) fwrite(data, 1, size, f);
    fclose(f);
    return true;
}

const char* SafeStr(const void* p, char* buf, size_t cap)
{
    if (!p) { snprintf(buf, cap, "(null)"); return buf; }
    __try {
        const char* s = (const char*)p; size_t i = 0;
        for (; i + 1 < cap && s[i]; ++i) buf[i] = (s[i] >= 32 && s[i] < 127) ? s[i] : '.';
        buf[i] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { snprintf(buf, cap, "(unreadable)"); }
    return buf;
}

void StartXellHook();
BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        char exe[MAX_PATH] = {}; GetModuleFileNameA(nullptr, exe, MAX_PATH);
        TraceF("==== igdext64 (tracing build) attached to %s", exe);
        StartXellHook();
    }
    return TRUE;
}

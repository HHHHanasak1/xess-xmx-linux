// ffx_common.h - what the two FidelityFX provider DLLs, amd_fidelityfx_upscaler_dx12.dll (ffx_xess.cpp) and
// amd_fidelityfx_framegeneration_dx12.dll (ffx_xefg.cpp), have in common: the log, the settings (the environment and
// <name>.env beside the DLL) and the FFX resource states. Included once by each, after <windows.h>, <d3d12.h> and the
// FFX API headers; each DLL gets its own copy of the state below.
#pragma once
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

static HMODULE g_self;   // set in DllMain
static FILE* g_log;
static std::mutex g_logLock;
static LARGE_INTEGER g_qpf, g_qp0;

// a line "<ms since the DLL's start> t<thread id> <text>"
static void Log(const char* fmt, ...)
{
    if (!g_log)
        return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double ms = (double)(now.QuadPart - g_qp0.QuadPart) * 1000.0 / (double)g_qpf.QuadPart;
    std::lock_guard<std::mutex> lk(g_logLock);
    va_list ap;
    va_start(ap, fmt);
    fprintf(g_log, "%10.3f t%-5lu ", ms, GetCurrentThreadId());
    vfprintf(g_log, fmt, ap);
    fputc('\n', g_log);
    va_end(ap);
    fflush(g_log);
}

static int EnvInt(const char* name, int def)
{
    char buf[32];
    if (!GetEnvironmentVariableA(name, buf, sizeof(buf)))
        return def;
    return atoi(buf);
}

// The start of a DLL's InitOnce. `name` is ffx_xess or ffx_xefg. Starts the log's clock; puts the DLL's folder, with
// its last backslash, into `dir`; sets the KEY=VALUE lines of <name>.env in that folder into the environment, unless
// the variable is set already; opens the log: <logEnv>=<path> appends to that file, <logEnv>=0 is no log, and without
// the variable it is <name>.log in the DLL's folder.
static void InitLogAndEnv(const char* name, const char* logEnv, char dir[MAX_PATH])
{
    QueryPerformanceFrequency(&g_qpf);
    QueryPerformanceCounter(&g_qp0);

    GetModuleFileNameA(g_self, dir, MAX_PATH);
    char* slash = strrchr(dir, '\\');
    if (slash)
        slash[1] = 0;

    char path[MAX_PATH];
    snprintf(path, MAX_PATH, "%s%s.env", dir, name);
    if (FILE* f = fopen(path, "r")) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            for (size_t n = strlen(line); n && (line[n - 1] == 10 || line[n - 1] == 13);)  // strip CR LF
                line[--n] = 0;
            char* eq = strchr(line, '=');
            if (line[0] == '#' || !eq)
                continue;
            *eq = 0;
            if (!GetEnvironmentVariableA(line, nullptr, 0))
                SetEnvironmentVariableA(line, eq + 1);
        }
        fclose(f);
    }

    char env[MAX_PATH];
    if (GetEnvironmentVariableA(logEnv, env, MAX_PATH)) {
        if (strcmp(env, "0") != 0)
            g_log = fopen(env, "a");
    } else {
        snprintf(path, MAX_PATH, "%s%s.log", dir, name);
        g_log = fopen(path, "w");   // the last run only: it stays in the game's folder
    }
}

static D3D12_RESOURCE_STATES ToD3D12State(uint32_t s)
{
    D3D12_RESOURCE_STATES r = D3D12_RESOURCE_STATE_COMMON;
    if (s & FFX_API_RESOURCE_STATE_UNORDERED_ACCESS)
        r |= D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if (s & FFX_API_RESOURCE_STATE_COMPUTE_READ)
        r |= D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (s & FFX_API_RESOURCE_STATE_PIXEL_READ)
        r |= D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (s & FFX_API_RESOURCE_STATE_COPY_SRC)
        r |= D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (s & FFX_API_RESOURCE_STATE_COPY_DEST)
        r |= D3D12_RESOURCE_STATE_COPY_DEST;
    if (s & FFX_API_RESOURCE_STATE_INDIRECT_ARGUMENT)
        r |= D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    if (s & FFX_API_RESOURCE_STATE_RENDER_TARGET)
        r |= D3D12_RESOURCE_STATE_RENDER_TARGET;
    if (s & FFX_API_RESOURCE_STATE_DEPTH_ATTACHMENT)
        r |= D3D12_RESOURCE_STATE_DEPTH_WRITE;
    return r;
}

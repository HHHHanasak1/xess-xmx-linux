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
void StartStackSampler();
void StartFgHook();
BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        char exe[MAX_PATH] = {}; GetModuleFileNameA(nullptr, exe, MAX_PATH);
        TraceF("==== igdext64 (tracing build) attached to %s", exe);
        StartXellHook();
        StartStackSampler();
        StartFgHook();
    }
    return TRUE;
}

// ---- ID3D12Device::CreateRootSignature vtable hook (index 16): dumps every serialized root signature ----
#include <d3d12.h>
typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateRootSignature)(ID3D12Device*, UINT, const void*, SIZE_T, REFIID, void**);
static PFN_CreateRootSignature g_origCRS = nullptr;
static volatile LONG g_rsCount = 0;
static HRESULT STDMETHODCALLTYPE HookCreateRootSignature(ID3D12Device* d, UINT mask, const void* blob, SIZE_T len, REFIID riid, void** out)
{
    TraceF("CreateRootSignature ENTER len=%zu out=%p", (size_t)len, (void*)out);
    HRESULT hr = g_origCRS(d, mask, blob, len, riid, out);
    TraceF("CreateRootSignature orig returned hr=0x%08lx", (unsigned long)hr);
    LONG n = InterlockedIncrement(&g_rsCount) - 1;
    void* obj = (SUCCEEDED(hr) && out) ? *out : nullptr;
    char name[64]; snprintf(name, sizeof(name), "rootsig_%04ld_%p.bin", n, obj);
    bool okd = DumpBlob(name, blob, len);
    TraceF("CreateRootSignature dump ok=%d", (int)okd);
    TraceF("CreateRootSignature #%ld len=%zu -> %p (hr=0x%08lx) dumped %s", n, (size_t)len, obj, (unsigned long)hr, name);
    return hr;
}
static void PatchVtable(void* obj, const char* what)
{
    void** vtbl = *reinterpret_cast<void***>(obj);
    if (vtbl[16] == reinterpret_cast<void*>(&HookCreateRootSignature)) return;       // already patched
    DWORD old;
    if (!VirtualProtect(&vtbl[16], sizeof(void*), PAGE_READWRITE, &old)) { TraceF("rootsig hook: VirtualProtect failed (%s)", what); return; }
    void* orig = vtbl[16];
    if (!g_origCRS) g_origCRS = reinterpret_cast<PFN_CreateRootSignature>(orig);
    else if (orig != reinterpret_cast<void*>(g_origCRS)) TraceF("rootsig hook: %s has a DIFFERENT original (%p vs %p)", what, orig, (void*)g_origCRS);
    vtbl[16] = reinterpret_cast<void*>(&HookCreateRootSignature);
    VirtualProtect(&vtbl[16], sizeof(void*), old, &old);
    TraceF("rootsig hook installed on %s vtable %p (orig %p)", what, (void*)vtbl, orig);
}
void InstallRootSignatureHook(void* device)
{
    return; // disabled: hooking CreateRootSignature hangs the game (OptiScaler layer)
    if (!device) return;
    PatchVtable(device, "ID3D12Device (as passed)");
    IUnknown* unk = reinterpret_cast<IUnknown*>(device);
    struct { const IID* iid; const char* name; } list[] = {
        { &__uuidof(ID3D12Device),  "ID3D12Device"  }, { &__uuidof(ID3D12Device1), "ID3D12Device1" }, { &__uuidof(ID3D12Device2), "ID3D12Device2" },
        { &__uuidof(ID3D12Device3), "ID3D12Device3" }, { &__uuidof(ID3D12Device4), "ID3D12Device4" }, { &__uuidof(ID3D12Device5), "ID3D12Device5" },
        { &__uuidof(ID3D12Device6), "ID3D12Device6" }, { &__uuidof(ID3D12Device7), "ID3D12Device7" }, { &__uuidof(ID3D12Device8), "ID3D12Device8" },
        { &__uuidof(ID3D12Device9), "ID3D12Device9" }, { &__uuidof(ID3D12Device10), "ID3D12Device10" },
    };
    for (auto& e : list)
    {
        void* p = nullptr;
        if (SUCCEEDED(unk->QueryInterface(*e.iid, &p)) && p) { PatchVtable(p, e.name); unk->Release(); }
    }
}




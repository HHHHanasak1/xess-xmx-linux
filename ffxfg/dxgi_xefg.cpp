// dxgi_xefg.cpp - dxgi.dll for the game's folder (FSR_FG=inject): passes everything to the prefix's dxgi.dll and gives
// a D3D12 swap chain created through it the XeSS-FG proxy swap chain, for a game whose FSR upscaling can feed XeSS-FG
// but which has no FSR frame generation of its own (see xefg_bridge.h). It sits below anything the game loads on top
// of DXGI and changes none of the game's files. Without ffx_xefg_bridge.dll, or if XeSS-FG fails, the swap chain is the
// plain one.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#pragma warning(disable : 4996)
#pragma comment(lib, "dxguid.lib")
#pragma comment(linker, "/export:CreateDXGIFactory=P_CreateDXGIFactory")
#pragma comment(linker, "/export:CreateDXGIFactory1=P_CreateDXGIFactory1")
#pragma comment(linker, "/export:CreateDXGIFactory2=P_CreateDXGIFactory2")
#pragma comment(linker, "/export:DXGIGetDebugInterface1=P_DXGIGetDebugInterface1")
#pragma comment(linker, "/export:DXGIDeclareAdapterRemovalSupport=P_DXGIDeclareAdapterRemovalSupport")

static HMODULE g_real;
static FILE* g_log;
static char g_dir[MAX_PATH];

static void Log(const char* fmt, ...)
{
    if (!g_log)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    fputc('\n', g_log);
    va_end(ap);
    fflush(g_log);
}

static void* Real(const char* name)
{
    if (!g_real) {
        char sys[MAX_PATH], path[MAX_PATH];
        GetSystemDirectoryA(sys, MAX_PATH);
        snprintf(path, MAX_PATH, "%s\\dxgi.dll", sys);
        g_real = LoadLibraryA(path);
        Log("load %s -> %p", path, (void*)g_real);
    }
    return g_real ? (void*)GetProcAddress(g_real, name) : nullptr;
}

// ---------------------------------------------------------------------------------------------------------------
// The swap chain

typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSwapChain)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSwapChainForHwnd)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                               const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                                               IDXGISwapChain1**);
typedef HRESULT (*PFN_XefgBridgeCreateSwapChain)(IDXGIFactory*, ID3D12CommandQueue*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                 const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGISwapChain1**);

static PFN_CreateSwapChain g_origCreate;
static PFN_CreateSwapChainForHwnd g_origForHwnd;
static std::atomic<int> g_creating{0};   // XeSS-FG creates the real swap chain through the same factory

static HRESULT Bridge(IDXGIFactory* f, IUnknown* dev, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* d,
                      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGISwapChain1** out)
{
    ID3D12CommandQueue* queue = nullptr;
    if (g_creating.load() || !dev || !d || !out || FAILED(dev->QueryInterface(IID_PPV_ARGS(&queue))))
        return E_NOINTERFACE;
    HRESULT hr = E_NOINTERFACE;
    Log("swap chain %ux%u format %d flags 0x%x%s: to XeSS-FG", d->Width, d->Height, (int)d->Format, d->Flags,
        fs && !fs->Windowed ? ", exclusive fullscreen" : "");
    char path[MAX_PATH];
    snprintf(path, MAX_PATH, "%sffx_xefg_bridge.dll", g_dir);
    HMODULE m = LoadLibraryA(path);
    auto create = m ? (PFN_XefgBridgeCreateSwapChain)GetProcAddress(m, "XefgBridgeCreateSwapChain") : nullptr;
    if (create) {
        g_creating++;
        hr = create(f, queue, hwnd, d, fs, out);
        g_creating--;
    }
    Log("swap chain %ux%u format %d flags 0x%x: XeSS-FG (%p) -> 0x%08lx", d->Width, d->Height, (int)d->Format, d->Flags,
        (void*)create, (unsigned long)hr);
    queue->Release();
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookForHwnd(IDXGIFactory2* f, IUnknown* dev, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* d,
                                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* o, IDXGISwapChain1** out)
{
    if (!o && SUCCEEDED(Bridge(f, dev, hwnd, d, fs, out)))
        return S_OK;
    return g_origForHwnd(f, dev, hwnd, d, fs, o, out);
}

static HRESULT STDMETHODCALLTYPE HookCreate(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* o, IDXGISwapChain** out)
{
    if (o && out) {
        DXGI_SWAP_CHAIN_DESC1 d = {};
        d.Width = o->BufferDesc.Width;
        d.Height = o->BufferDesc.Height;
        d.Format = o->BufferDesc.Format;
        d.SampleDesc = o->SampleDesc;
        d.BufferUsage = o->BufferUsage;
        d.BufferCount = o->BufferCount;
        d.Scaling = o->BufferDesc.Scaling == DXGI_MODE_SCALING_CENTERED ? DXGI_SCALING_NONE : DXGI_SCALING_STRETCH;
        d.SwapEffect = o->SwapEffect;
        d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        d.Flags = o->Flags;
        DXGI_SWAP_CHAIN_FULLSCREEN_DESC fs = {};
        fs.RefreshRate = o->BufferDesc.RefreshRate;
        fs.ScanlineOrdering = o->BufferDesc.ScanlineOrdering;
        fs.Scaling = o->BufferDesc.Scaling;
        fs.Windowed = o->Windowed;
        IDXGISwapChain1* sc = nullptr;
        if (SUCCEEDED(Bridge(f, dev, o->OutputWindow, &d, &fs, &sc))) {
            *out = sc;
            return S_OK;
        }
    }
    return g_origCreate(f, dev, o, out);
}

// the factories of one DXGI share a vtable: CreateSwapChain is slot 10, CreateSwapChainForHwnd slot 15. A patched slot
// jumps into this module from then on, so it is pinned first (never unmapped); if that fails, nothing is patched.
static void Patch(void** vt, int slot, void* hook, void** orig)
{
    static bool pinned;
    if (vt[slot] == hook)
        return;
    if (!pinned) {
        HMODULE m = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                (LPCWSTR)&Patch, &m) || !m) {
            Log("could not pin dxgi.dll (%lu): swap chains are not wrapped", GetLastError());
            return;
        }
        pinned = true;
    }
    DWORD old;
    if (!VirtualProtect(&vt[slot], sizeof(void*), PAGE_READWRITE, &old))
        return;
    if (!*orig)
        *orig = vt[slot];
    vt[slot] = hook;
    VirtualProtect(&vt[slot], sizeof(void*), old, &old);
}

static HRESULT Patched(HRESULT hr, REFIID riid, void** out)
{
    if (FAILED(hr) || !out || !*out)
        return hr;
    IDXGIFactory2* f = nullptr;
    if (SUCCEEDED(((IUnknown*)*out)->QueryInterface(IID_PPV_ARGS(&f)))) {
        void** vt = *(void***)f;
        Patch(vt, 10, (void*)HookCreate, (void**)&g_origCreate);
        Patch(vt, 15, (void*)HookForHwnd, (void**)&g_origForHwnd);
        f->Release();
    }
    return hr;
}

// ---------------------------------------------------------------------------------------------------------------
// Exports

extern "C" HRESULT WINAPI P_CreateDXGIFactory(REFIID riid, void** out)
{
    auto f = (HRESULT(WINAPI*)(REFIID, void**))Real("CreateDXGIFactory");
    return f ? Patched(f(riid, out), riid, out) : E_FAIL;
}

extern "C" HRESULT WINAPI P_CreateDXGIFactory1(REFIID riid, void** out)
{
    auto f = (HRESULT(WINAPI*)(REFIID, void**))Real("CreateDXGIFactory1");
    return f ? Patched(f(riid, out), riid, out) : E_FAIL;
}

extern "C" HRESULT WINAPI P_CreateDXGIFactory2(UINT flags, REFIID riid, void** out)
{
    auto f = (HRESULT(WINAPI*)(UINT, REFIID, void**))Real("CreateDXGIFactory2");
    return f ? Patched(f(flags, riid, out), riid, out) : E_FAIL;
}

extern "C" HRESULT WINAPI P_DXGIGetDebugInterface1(UINT flags, REFIID riid, void** out)
{
    auto f = (HRESULT(WINAPI*)(UINT, REFIID, void**))Real("DXGIGetDebugInterface1");
    return f ? f(flags, riid, out) : E_NOINTERFACE;
}

extern "C" HRESULT WINAPI P_DXGIDeclareAdapterRemovalSupport()
{
    auto f = (HRESULT(WINAPI*)())Real("DXGIDeclareAdapterRemovalSupport");
    return f ? f() : S_OK;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        GetModuleFileNameA(inst, g_dir, MAX_PATH);
        if (char* slash = strrchr(g_dir, '\\'))
            slash[1] = 0;
        char path[MAX_PATH];
        snprintf(path, MAX_PATH, "%sdxgi_xefg.log", g_dir);
        g_log = fopen(path, "w");   // the last run only: it stays in the game's folder
    }
    return TRUE;
}

// ffx_xefg_shown.cpp - the shown frames of amd_fidelityfx_framegeneration_dx12.dll: the Present of the real swap chain
// under XeSS-FG is hooked, to see what goes to the display (FFX_XEFG_TIMES, FFX_XEFG_TRACE).
#include "ffx_xefg.h"

// ---------------------------------------------------------------------------------------------------------------
// The shown frames
//
// At n x, XeSS-FG presents a rendered frame's n images on the real swap chain, and on Windows the Intel driver spaces
// them; here they reach the display one refresh apart and then nothing comes until the next rendered frame (at 3x and
// 31.9 ms a frame, gaps of 7, 7 and 18 ms: Resident Evil 4, OFLOW_FG, 2026-11-11). To see what goes to the display,
// the real swap chain's Present is hooked: XeSS-FG makes that swap chain through the DXGI factory while it sets
// itself up (DXVK's DXGI: one vtable for all its swap chains, this one is told apart by its pointer). Who presents
// what (FFX_XEFG_TRACE, XeSS-FG 1.3.1 under DXVK's DXGI, Resident Evil 4 with OFLOW_FG, 2026-11-13): at 3x the game's
// thread presents image 0 inside the proxy's Present (once the presentation thread is through with the frame before)
// and the presentation thread images 1 and 2; at 2x the game's thread presents both, 0.05 ms apart. With
// FFX_XEFG_TIMES=1 the log has the gaps between those Present calls every 300 rendered frames, and the longest over
// the shortest gap of each rendered frame. FFX_XEFG_TRACE puts every call into the time line (RP_CALL / RP_RET).
// Spacing the images cannot be done here (tried 2026-11-13: the calls came out evenly spaced, the display's gaps did
// not change): vkd3d-proton orders a Present on the game's queue when it is called, and a frame's images become ready
// together when the GPU has finished the frame and its interpolations. It has to come after that point, at
// vkQueuePresentKHR on vkd3d-proton's present thread or in gamescope.

typedef HRESULT(STDMETHODCALLTYPE* PFN_ScPresent)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_ScPresent1)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
static PFN_ScPresent g_origRealPresent;
static PFN_ScPresent1 g_origRealPresent1;
static void** g_realVt = nullptr;
static std::atomic<int> g_realSaid{0};

static bool PatchSlot(void** vt, int slot, void* hook, void** orig)
{
    if (vt[slot] == hook)
        return true;
    DWORD old;
    if (!VirtualProtect(&vt[slot], sizeof(void*), PAGE_READWRITE, &old))
        return false;
    *orig = vt[slot];
    vt[slot] = hook;
    VirtualProtect(&vt[slot], sizeof(void*), old, &old);
    return true;
}

static void EvenReport()
{
    auto pct = [](std::vector<float>& v, int p) { return v[std::min(v.size() - 1, v.size() * p / 100)]; };
    std::sort(g_ev.gaps.begin(), g_ev.gaps.end());
    std::sort(g_ev.ratios.begin(), g_ev.ratios.end());
    char r[96] = "none";
    if (!g_ev.ratios.empty())
        snprintf(r, sizeof r, "p50 %.2f p95 %.2f", pct(g_ev.ratios, 50), pct(g_ev.ratios, 95));
    Log("shown frames over %d rendered frames (%dx): present gaps ms p5 %.2f p50 %.2f p95 %.2f max %.2f; longest / "
        "shortest gap of a frame %s",
        g_ev.frames, g_ev.mult.load(), pct(g_ev.gaps, 5), pct(g_ev.gaps, 50), pct(g_ev.gaps, 95), g_ev.gaps.back(), r);
    g_ev.gaps.clear(), g_ev.ratios.clear();
    g_ev.frames = 0;
}

// FFX_XEFG_TIMES: image k's Present is called now
static void EvenNote(uint64_t now, int k)
{
    std::lock_guard<std::mutex> lk(g_ev.lock);
    const float gap = g_ev.lastCall ? QpcMs(g_ev.lastCall, now) : 0;
    g_ev.lastCall = now;
    if (gap <= 0 || gap > 250) {   // the first, or after a pause
        g_ev.gN = 0, g_ev.gMin = 1e9f, g_ev.gMax = 0;
        return;
    }
    g_ev.gaps.push_back(gap);
    // a rendered frame's gaps: the n that end at its images 1 .. n-1 and at the next frame's image 0
    if (k == 0) {
        if (g_ev.gN + 1 == g_ev.mult.load() && g_ev.mult.load() > 1)
            g_ev.ratios.push_back(std::max(g_ev.gMax, gap) / std::max(0.01f, std::min(g_ev.gMin, gap)));
        g_ev.gN = 0, g_ev.gMin = 1e9f, g_ev.gMax = 0;
        if (++g_ev.frames >= 300)
            EvenReport();
    } else {
        g_ev.gN++;
        g_ev.gMin = std::min(g_ev.gMin, gap);
        g_ev.gMax = std::max(g_ev.gMax, gap);
    }
}

// In front of each Present of a swap chain of DXVK's DXGI: image 0 is the first one inside the proxy's Present
static void EvenBefore(void* sc)
{
    if (sc != g_ev.real.load(std::memory_order_relaxed) || !g_times)
        return;
    const uint64_t now = Qpc();
    if (t_pxImages >= 0 && t_pxImages++ == 0) {
        g_ev.image.store(0);
        EvenNote(now, 0);
        return;
    }
    EvenNote(now, g_ev.image.fetch_add(1) + 1);
}

static HRESULT STDMETHODCALLTYPE H_RealPresent(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    if (g_realSaid.fetch_add(1) < 5)
        Log("real Present(%u, 0x%x) on %p%s", sync, flags, (void*)sc, t_pxImages >= 0 ? " (in the proxy's Present)" : "");
    if (!(flags & DXGI_PRESENT_TEST))
        EvenBefore(sc);
    TR(tr::Put(tr::RP_CALL, sync, flags));
    HRESULT hr = g_origRealPresent(sc, sync, flags);
    TR(tr::Put(tr::RP_RET, (uint64_t)hr));
    return hr;
}
static HRESULT STDMETHODCALLTYPE H_RealPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp)
{
    if (g_realSaid.fetch_add(1) < 5)
        Log("real Present1(%u, 0x%x) on %p%s", sync, flags, (void*)sc, t_pxImages >= 0 ? " (in the proxy's Present)" : "");
    if (!(flags & DXGI_PRESENT_TEST))
        EvenBefore(sc);
    TR(tr::Put(tr::RP_CALL, sync, flags, 1));
    HRESULT hr = g_origRealPresent1(sc, sync, flags, pp);
    TR(tr::Put(tr::RP_RET, (uint64_t)hr, 0, 1));
    return hr;
}

static void ModuleOf(void* addr, char* out, size_t n)
{   // the file a code address belongs to
    HMODULE m = nullptr;
    out[0] = 0;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &m) && m)
        GetModuleFileNameA(m, out, (DWORD)n);
}

// NVIDIA Streamline (sl.interposer.dll, shipped by CONTROL Resonant) hands out proxies of the DXGI objects; this
// interface id of its public headers returns the object underneath, which is what XeSS-FG presents on
static const GUID kStreamlineBase = {0xadec44e2, 0x61f0, 0x45c3, {0xad, 0x9f, 0x1b, 0x37, 0x37, 0x92, 0x84, 0xff}};

static void HookRealSwapChain(IUnknown* sc)
{
    IUnknown* base = nullptr;
    if (SUCCEEDED(sc->QueryInterface(kStreamlineBase, (void**)&base)) && base) {
        Log("swap chain %p is a Streamline proxy of %p", (void*)sc, (void*)base);
        sc = base;
    }
    void** vt = *(void***)sc;
    if (vt != g_ownVt) {
        char mod[MAX_PATH];
        ModuleOf(vt[8], mod, sizeof mod);
        bool a = PatchSlot(vt, 8, (void*)H_RealPresent, (void**)&g_origRealPresent);
        bool b = PatchSlot(vt, 22, (void*)H_RealPresent1, (void**)&g_origRealPresent1);
        g_realVt = vt;
        g_captured = (void*)sc;
        Log("real swap chain %p (vtable %p, %s): Present %s, Present1 %s", (void*)sc, (void*)vt, mod,
            a ? "hooked" : "not hooked", b ? "hooked" : "not hooked");
    }
    if (base)
        base->Release();
}

#ifdef FFXFG_DEBUG
// what the real swap chain's Present slots hold now (somebody may have put a hook of their own over ours)
void CheckRealVt(const char* when)
{
    if (!tr::g_on || !g_realVt)
        return;
    char m8[MAX_PATH], m22[MAX_PATH], mo[MAX_PATH];
    ModuleOf(g_realVt[8], m8, sizeof m8);
    ModuleOf(g_realVt[22], m22, sizeof m22);
    ModuleOf((void*)g_origRealPresent, mo, sizeof mo);
    Log("trace: %s: real vtable %p slot 8 = %p (%s)%s, slot 22 = %p (%s)%s, original Present %p (%s), calls so far %d", when,
        (void*)g_realVt, g_realVt[8], m8, g_realVt[8] == (void*)H_RealPresent ? " ours" : "", g_realVt[22], m22,
        g_realVt[22] == (void*)H_RealPresent1 ? " ours" : "", (void*)g_origRealPresent, mo, g_realSaid.load());
}
#endif

// while XeSS-FG sets itself up it creates the real swap chain through the DXGI factory: CreateSwapChain (slot 10),
// CreateSwapChainForHwnd (15)
typedef HRESULT(STDMETHODCALLTYPE* PFN_FCreate)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_FForHwnd)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                 const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
static PFN_FCreate g_origFCreate;
static PFN_FForHwnd g_origFForHwnd;

static HRESULT STDMETHODCALLTYPE H_FCreate(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* d, IDXGISwapChain** out)
{
    HRESULT hr = g_origFCreate(f, dev, d, out);
    if (g_capturing.load())
        Log("CreateSwapChain -> 0x%08lx %p", (unsigned long)hr, out ? (void*)*out : nullptr);
    if (SUCCEEDED(hr) && out && *out && g_capturing.load())
        HookRealSwapChain(*out);
    return hr;
}
static HRESULT STDMETHODCALLTYPE H_FForHwnd(IDXGIFactory2* f, IUnknown* dev, HWND w, const DXGI_SWAP_CHAIN_DESC1* d,
                                            const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* o, IDXGISwapChain1** out)
{
    HRESULT hr = g_origFForHwnd(f, dev, w, d, fs, o, out);
    if (g_capturing.load())
        Log("CreateSwapChainForHwnd -> 0x%08lx %p", (unsigned long)hr, out ? (void*)*out : nullptr);
    if (SUCCEEDED(hr) && out && *out && g_capturing.load())
        HookRealSwapChain(*out);
    return hr;
}

// The system's DXGI factory class (DXVK's), not the one the game hands over: that can be a proxy (CONTROL Resonant
// passes NVIDIA Streamline's sl.interposer factory, and XeSS-FG presents on the swap chain underneath its proxy)
void HookFactory()
{
    static bool done = false;
    IDXGIFactory2* f2 = nullptr;
    if (done || !PinSelf() || FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f2))))
        return;
    done = true;
    void** vt = *(void***)f2;
    char mod[MAX_PATH];
    ModuleOf(vt[15], mod, sizeof mod);
    bool a = PatchSlot(vt, 10, (void*)H_FCreate, (void**)&g_origFCreate);
    bool b = PatchSlot(vt, 15, (void*)H_FForHwnd, (void**)&g_origFForHwnd);
    Log("DXGI factory class of %s: CreateSwapChain %s, CreateSwapChainForHwnd %s", mod, a ? "hooked" : "not hooked",
        b ? "hooked" : "not hooked");
    f2->Release();
}

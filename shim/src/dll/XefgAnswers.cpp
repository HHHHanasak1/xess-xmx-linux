// XefgAnswers.cpp - the shim answers what a game asks XeSS-FG's swap chain every frame (IGDEXT_XELL_POLLS, part of the
// low latency of XellHook.cpp): hooks in the virtual table of the swap chain that libxess_fg.dll hands the game, put
// in by ProxyHook, which XefgHook.cpp calls when it learns of that swap chain.
#include "XellHook.h"

// ---------------------------------------------------------------------------------------------------------------
// A game's questions to XeSS-FG's swap chain (IGDEXT_XELL_POLLS, on with the low latency)
//
// XeSS-FG's proxy swap chain passes IDXGISwapChain::GetFrameStatistics and IDXGISwapChain3::CheckColorSpaceSupport on
// to the real swap chain. DXVK's swap chain holds its lock for the whole of a Present, and XeSS-FG's presentation
// thread is inside one for most of a frame: a game that asks every frame starts its next frame only when that
// Present returns, and the GPU idles meanwhile (ffxfg/ffx_xefg*.cpp, "Low latency", has the time lines of three games
// on the FSR routes: Resident Evil Requiem with its HDR output on, CONTROL Resonant, Forza Horizon 6).
// So, while the low latency is on and frame generation runs, a game that asks gets the answers as of its last
// Present. They are taken right before the proxy's Present, where the proxy waits for that thread anyway. Only what
// the game has asked about is taken, and what it no longer asks about is dropped after 600 Presents: a game that
// never asks is not touched. A resize, a mode or a color space change forgets the answers.
// The proxy's methods are reached through its virtual table (in libxess_fg's read-only data), which is patched once
// the game's swap chain is known: at xefgSwapChainD3D12GetSwapChainPtr, or from the context of a later call when the
// game got its swap chain before the hooks were in. A swap chain that the package's own frame generation DLL wraps
// (the FSR routes, OFLOW_FG) is left alone: that DLL answers its game itself and says so (IgdextXefgProxyOwned).
namespace
{
enum { SC_PRESENT = 8, SC_SETFS = 10, SC_RESIZE = 13, SC_STATS = 16, SC_PRESENT1 = 22, SC_INDEX = 36, SC_CHECKCS = 37, SC_SETCS = 38, SC_RESIZE1 = 39, SC_SLOTS = 40 };
typedef HRESULT (STDMETHODCALLTYPE* PFN_ScPresent)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE* PFN_ScPresent1)(IDXGISwapChain*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
typedef HRESULT (STDMETHODCALLTYPE* PFN_ScStats)(IDXGISwapChain*, DXGI_FRAME_STATISTICS*);
typedef HRESULT (STDMETHODCALLTYPE* PFN_ScCheckCs)(IDXGISwapChain*, DXGI_COLOR_SPACE_TYPE, UINT*);
typedef HRESULT (STDMETHODCALLTYPE* PFN_ScSetCs)(IDXGISwapChain*, DXGI_COLOR_SPACE_TYPE);
typedef HRESULT (STDMETHODCALLTYPE* PFN_ScSetFs)(IDXGISwapChain*, BOOL, IDXGIOutput*);
typedef HRESULT (STDMETHODCALLTYPE* PFN_ScResize)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef HRESULT (STDMETHODCALLTYPE* PFN_ScResize1)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*, IUnknown* const*);
typedef UINT (STDMETHODCALLTYPE* PFN_ScIndex)(IDXGISwapChain3*);

void* g_scOrig[SC_SLOTS];        // its own entries
PtrSlots g_scOwned;              // swap chains whose wrapper answers its game itself (IgdextXefgProxyOwned)

struct ScAnswers
{
    SRWLOCK lock;
    void* sc;                    // the swap chain they are of
    bool statsAsked, statsHave; int statsIdle; DXGI_FRAME_STATISTICS stats; HRESULT statsHr;
    struct Cs { UINT cs; HRESULT hr; UINT support; bool asked, have; int idle; } cs[8]; int nCs;
    LONG saidStats, saidCs, saidWaited;
} g_ans = { SRWLOCK_INIT };

bool ScOwned(void* sc) { return g_scOwned.Has(sc); }

bool AnswersOn(void* sc) { return g_capAuto && g_polls && g_fgState == 1 && !ScOwned(sc); }

// the answers belong to one swap chain; with the lock held
void AnswersOf(void* sc)
{
    if (g_ans.sc == sc) return;
    g_ans.sc = sc; g_ans.statsAsked = g_ans.statsHave = false; g_ans.statsIdle = 0; g_ans.nCs = 0;
}

void AnswersForget()
{
    AcquireSRWLockExclusive(&g_ans.lock);
    AnswersOf(nullptr);
    ReleaseSRWLockExclusive(&g_ans.lock);
}

// right before the proxy's Present: what the game has asked about is taken from the swap chain (that waits while a
// Present of the real swap chain is under way; the proxy's Present would wait for the same thread)
void AnswersTake(IDXGISwapChain* sc)
{
    if (!AnswersOn(sc)) { if (g_ans.sc) AnswersForget(); return; }
    bool wantStats; UINT css[8]; int n = 0;
    AcquireSRWLockExclusive(&g_ans.lock);
    AnswersOf(sc);
    wantStats = g_ans.statsAsked || g_ans.statsHave;
    for (int i = 0; i < g_ans.nCs; ++i) css[n++] = g_ans.cs[i].cs;
    ReleaseSRWLockExclusive(&g_ans.lock);
    if (!wantStats && !n) return;
    DXGI_FRAME_STATISTICS fs = {}; HRESULT fhr = E_FAIL; HRESULT chr[8] = {}; UINT sup[8] = {};
    if (wantStats) fhr = ((PFN_ScStats) g_scOrig[SC_STATS])(sc, &fs);
    for (int i = 0; i < n; ++i) chr[i] = ((PFN_ScCheckCs) g_scOrig[SC_CHECKCS])(sc, (DXGI_COLOR_SPACE_TYPE) css[i], &sup[i]);
    AcquireSRWLockExclusive(&g_ans.lock);
    if (g_ans.sc == sc)
    {
        if (wantStats)
        {
            g_ans.stats = fs; g_ans.statsHr = fhr; g_ans.statsHave = true;
            g_ans.statsIdle = g_ans.statsAsked ? 0 : g_ans.statsIdle + 1; g_ans.statsAsked = false;
            if (g_ans.statsIdle >= 600) g_ans.statsHave = false;   // no longer asked about
        }
        // the game may have asked about another color space meanwhile, on another thread: by value, not by place
        int keep = 0;
        for (int k = 0; k < g_ans.nCs; ++k)
        {
            ScAnswers::Cs a = g_ans.cs[k];
            for (int i = 0; i < n; ++i)
                if (css[i] == a.cs) { a.hr = chr[i]; a.support = sup[i]; a.have = true; a.idle = a.asked ? 0 : a.idle + 1; a.asked = false; }
            if (a.idle < 600) g_ans.cs[keep++] = a;
        }
        g_ans.nCs = keep;
    }
    ReleaseSRWLockExclusive(&g_ans.lock);
}

// a question the swap chain answered itself: if it waited there while frame generation runs, the log names it
void ScWaited(const char* what, const LARGE_INTEGER& t0)
{
    if (!g_log || g_fgState != 1) return;
    LARGE_INTEGER t1, qf; QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&qf);
    const double ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / qf.QuadPart;
    if (ms >= 2.0 && InterlockedIncrement(&g_ans.saidWaited) <= 8)
        XLog("the game's %s waited %.1f ms in the swap chain (for XeSS-FG's presentation thread)", what, ms);
}

// the thread inside the proxy's Present (one at a time presents to a swap chain): the proxy's Present1 may call its
// own Present, and the proxy may ask the swap chain itself in there; those calls pass through
volatile DWORD g_presentTid;
inline bool InPresent() { return g_presentTid == GetCurrentThreadId(); }

// IGDEXT_XEFG_ASYNC=1: while frame generation runs, the game's Present hands the frame to a thread of the
// shim, which calls the proxy's Present (where XeSS-FG holds the frame for its pacing), and returns at once; the next
// Present waits until that is done (one frame in flight). Meanwhile the game is told the back buffer index the proxy
// will have after that Present. A resize, a mode or a colour space change waits for the frame in flight first.
struct AsyncState
{
    SRWLOCK lock;
    CONDITION_VARIABLE cv;
    bool have, busy;
    IDXGISwapChain* sc;          // the frame in flight's swap chain (a reference is held until its Present is done)
    UINT sync, flags; bool one, havePp; DXGI_PRESENT_PARAMETERS pp;
    HRESULT hr;                  // the last Present's result, handed to the game one frame late
    void* indexSc; UINT index; bool indexValid; UINT buffers; void* buffersSc;
    HANDLE thread;
    LONG mismatch, said;
} g_as = { SRWLOCK_INIT, CONDITION_VARIABLE_INIT };

void AsyncWaitLocked() { while (g_as.busy) SleepConditionVariableSRW(&g_as.cv, &g_as.lock, INFINITE, 0); }
void AsyncFlush()
{
    if (!g_xefgAsync) return;
    AcquireSRWLockExclusive(&g_as.lock);
    AsyncWaitLocked();
    g_as.indexValid = false;
    ReleaseSRWLockExclusive(&g_as.lock);
}

DWORD WINAPI AsyncThread(void*)
{
    for (;;)
    {
        AcquireSRWLockExclusive(&g_as.lock);
        while (!g_as.have) SleepConditionVariableSRW(&g_as.cv, &g_as.lock, INFINITE, 0);
        IDXGISwapChain* sc = g_as.sc; const UINT sync = g_as.sync, flags = g_as.flags; const bool one = g_as.one;
        DXGI_PRESENT_PARAMETERS pp = g_as.pp; const bool havePp = g_as.havePp;
        g_as.have = false;
        ReleaseSRWLockExclusive(&g_as.lock);
        AnswersTake(sc);
        g_presentTid = GetCurrentThreadId();
        const HRESULT hr = one ? ((PFN_ScPresent1) g_scOrig[SC_PRESENT1])(sc, sync, flags, havePp ? &pp : nullptr)
                               : ((PFN_ScPresent) g_scOrig[SC_PRESENT])(sc, sync, flags);
        g_presentTid = 0;
        const UINT now = ((PFN_ScIndex) g_scOrig[SC_INDEX])((IDXGISwapChain3*) sc);
        AcquireSRWLockExclusive(&g_as.lock);
        if (now != g_as.index && InterlockedIncrement(&g_as.mismatch) <= 8)
            XLog("!! IGDEXT_XEFG_ASYNC: back buffer %u after the Present, the game was told %u", now, g_as.index);
        g_as.hr = hr;
        g_as.busy = false;
        ReleaseSRWLockExclusive(&g_as.lock);
        WakeAllConditionVariable(&g_as.cv);
        sc->Release();
    }
}

// the game's Present: true and *hr set when it went to the thread
bool AsyncPresent(IDXGISwapChain* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp, bool one, HRESULT* hr)
{
    if (!g_xefgAsync) return false;
    const bool take = g_fgState == 1 && !ScOwned(sc) && !(flags & DXGI_PRESENT_TEST) &&
                      (!pp || (!pp->DirtyRectsCount && !pp->pScrollRect && !pp->pScrollOffset));
    AcquireSRWLockExclusive(&g_as.lock);
    AsyncWaitLocked();   // the frame before is through the proxy's Present
    if (!take || (!g_as.thread && !(g_as.thread = CreateThread(nullptr, 0, AsyncThread, nullptr, 0, nullptr))))
    {
        g_as.indexValid = false;
        ReleaseSRWLockExclusive(&g_as.lock);
        return false;
    }
    if (g_as.buffersSc != sc)
    {
        DXGI_SWAP_CHAIN_DESC1 d = {};
        IDXGISwapChain1* s1 = nullptr;
        g_as.buffers = 2;
        if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), (void**) &s1)) && s1)
        {
            if (SUCCEEDED(s1->GetDesc1(&d)) && d.BufferCount) g_as.buffers = d.BufferCount;
            s1->Release();
        }
        g_as.buffersSc = sc;
        if (InterlockedIncrement(&g_as.said) == 1)
            XLog("IGDEXT_XEFG_ASYNC: XeSS-FG's Present runs on a thread of the shim (%u buffers)", g_as.buffers);
    }
    const UINT cur = ((PFN_ScIndex) g_scOrig[SC_INDEX])((IDXGISwapChain3*) sc);
    sc->AddRef();
    g_as.sc = sc; g_as.sync = sync; g_as.flags = flags; g_as.one = one;
    g_as.havePp = pp != nullptr; g_as.pp = pp ? *pp : DXGI_PRESENT_PARAMETERS{};
    g_as.indexSc = sc; g_as.index = (cur + 1) % g_as.buffers; g_as.indexValid = true;
    g_as.have = g_as.busy = true;
    *hr = g_as.hr;
    ReleaseSRWLockExclusive(&g_as.lock);
    WakeAllConditionVariable(&g_as.cv);
    return true;
}

UINT STDMETHODCALLTYPE Hook_ScIndex(IDXGISwapChain3* sc)
{
    if (g_xefgAsync && !InPresent())
    {
        AcquireSRWLockShared(&g_as.lock);
        const bool v = g_as.indexValid && g_as.indexSc == sc; const UINT i = g_as.index;
        ReleaseSRWLockShared(&g_as.lock);
        if (v) return i;
    }
    return ((PFN_ScIndex) g_scOrig[SC_INDEX])(sc);
}

HRESULT STDMETHODCALLTYPE Hook_ScPresent(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    const bool outer = !InPresent();
    HRESULT ahr;
    if (outer && AsyncPresent(sc, sync, flags, nullptr, false, &ahr)) return ahr;
    if (outer) { AnswersTake(sc); g_presentTid = GetCurrentThreadId(); }
    const HRESULT hr = ((PFN_ScPresent) g_scOrig[SC_PRESENT])(sc, sync, flags);
    if (outer) g_presentTid = 0;
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_ScPresent1(IDXGISwapChain* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp)
{
    const bool outer = !InPresent();
    HRESULT ahr;
    if (outer && AsyncPresent(sc, sync, flags, pp, true, &ahr)) return ahr;
    if (outer) { AnswersTake(sc); g_presentTid = GetCurrentThreadId(); }
    const HRESULT hr = ((PFN_ScPresent1) g_scOrig[SC_PRESENT1])(sc, sync, flags, pp);
    if (outer) g_presentTid = 0;
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_ScStats(IDXGISwapChain* sc, DXGI_FRAME_STATISTICS* s)
{
    if (s && !InPresent() && AnswersOn(sc))
    {
        AcquireSRWLockExclusive(&g_ans.lock);
        AnswersOf(sc);
        g_ans.statsAsked = true;
        if (g_ans.statsHave)
        {
            *s = g_ans.stats; const HRESULT hr = g_ans.statsHr;
            ReleaseSRWLockExclusive(&g_ans.lock);
            if (InterlockedIncrement(&g_ans.saidStats) == 1)
                XLog("low latency: the game asks the swap chain for its frame statistics: answered as of its last Present from here on");
            return hr;
        }
        ReleaseSRWLockExclusive(&g_ans.lock);
    }
    LARGE_INTEGER t0; QueryPerformanceCounter(&t0);
    const HRESULT hr = ((PFN_ScStats) g_scOrig[SC_STATS])(sc, s);
    if (!InPresent() && !ScOwned(sc)) ScWaited("GetFrameStatistics", t0);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_ScCheckCs(IDXGISwapChain* sc, DXGI_COLOR_SPACE_TYPE c, UINT* s)
{
    const bool on = s && !InPresent() && AnswersOn(sc);
    if (on)
    {
        AcquireSRWLockExclusive(&g_ans.lock);
        AnswersOf(sc);
        for (int i = 0; i < g_ans.nCs; ++i)
            if (g_ans.cs[i].cs == (UINT) c)
            {
                g_ans.cs[i].asked = true;
                if (!g_ans.cs[i].have) break;
                *s = g_ans.cs[i].support; const HRESULT hr = g_ans.cs[i].hr;
                ReleaseSRWLockExclusive(&g_ans.lock);
                if (InterlockedIncrement(&g_ans.saidCs) == 1)
                    XLog("low latency: the game asks the swap chain about a color space (%u): answered as of its last Present from here on", (UINT) c);
                return hr;
            }
        ReleaseSRWLockExclusive(&g_ans.lock);
    }
    LARGE_INTEGER t0; QueryPerformanceCounter(&t0);
    const HRESULT hr = ((PFN_ScCheckCs) g_scOrig[SC_CHECKCS])(sc, c, s);
    if (!InPresent() && !ScOwned(sc)) ScWaited("CheckColorSpaceSupport", t0);
    if (on)
    {
        // asked about for the first time: taken at each Present from here on
        AcquireSRWLockExclusive(&g_ans.lock);
        if (g_ans.sc == sc)
        {
            bool have = false;
            for (int i = 0; i < g_ans.nCs; ++i) have |= g_ans.cs[i].cs == (UINT) c;
            if (!have && g_ans.nCs < 8) g_ans.cs[g_ans.nCs++] = { (UINT) c, hr, *s, true, false, 0 };
        }
        ReleaseSRWLockExclusive(&g_ans.lock);
    }
    return hr;
}

// the swap chain changes: its own answers again, until the next Present
HRESULT STDMETHODCALLTYPE Hook_ScSetCs(IDXGISwapChain* sc, DXGI_COLOR_SPACE_TYPE c)
{
    AsyncFlush();
    AnswersForget();
    return ((PFN_ScSetCs) g_scOrig[SC_SETCS])(sc, c);
}
HRESULT STDMETHODCALLTYPE Hook_ScSetFs(IDXGISwapChain* sc, BOOL f, IDXGIOutput* o)
{
    AsyncFlush();
    AnswersForget();
    return ((PFN_ScSetFs) g_scOrig[SC_SETFS])(sc, f, o);
}
HRESULT STDMETHODCALLTYPE Hook_ScResize(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags)
{
    AsyncFlush();
    AnswersForget();
    return ((PFN_ScResize) g_scOrig[SC_RESIZE])(sc, n, w, h, f, flags);
}
HRESULT STDMETHODCALLTYPE Hook_ScResize1(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags, const UINT* masks, IUnknown* const* queues)
{
    AsyncFlush();
    AnswersForget();
    return ((PFN_ScResize1) g_scOrig[SC_RESIZE1])(sc, n, w, h, f, flags, masks, queues);
}

}

// the hooks go into the virtual table of XeSS-FG's swap chain, once per table (a libxess_fg that is loaded again
// comes with a fresh one)
void ProxyHook(IUnknown* unk)
{
    if (!unk || ((!g_capAuto || !g_polls) && !g_xefgAsync)) return;
    IDXGISwapChain3* sc = nullptr;
    if (FAILED(unk->QueryInterface(__uuidof(IDXGISwapChain3), (void**) &sc)) || !sc) return;
    void** vt = *(void***) sc;
    static SRWLOCK lock = SRWLOCK_INIT;
    AcquireSRWLockExclusive(&lock);
    if (vt[SC_PRESENT] != (void*) Hook_ScPresent)
    {
        static const struct { int slot; void* hook; } hooks[] = {
            { SC_PRESENT, (void*) Hook_ScPresent }, { SC_PRESENT1, (void*) Hook_ScPresent1 }, { SC_STATS, (void*) Hook_ScStats },
            { SC_CHECKCS, (void*) Hook_ScCheckCs }, { SC_SETCS, (void*) Hook_ScSetCs }, { SC_SETFS, (void*) Hook_ScSetFs },
            { SC_RESIZE, (void*) Hook_ScResize }, { SC_RESIZE1, (void*) Hook_ScResize1 }, { SC_INDEX, (void*) Hook_ScIndex },
        };
        // only a table of libxess_fg's own; this module is pinned: the table points into it from here on
        HMODULE m = nullptr, fg = GetModuleHandleW(L"libxess_fg.dll");
        DWORD old;
        if (!fg || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR) vt, &m) || m != fg)
            XLog("swap chain %p: its virtual table %p is not libxess_fg's, the game's questions go to the swap chain", sc, vt);
        else if (!SelfModule(GET_MODULE_HANDLE_EX_FLAG_PIN) || !VirtualProtect(vt, SC_SLOTS * sizeof(void*), PAGE_READWRITE, &old))
            XLog("swap chain %p: virtual table %p not writable (%lu), the game's questions go to the swap chain", sc, vt, GetLastError());
        else
        {
            for (auto& h : hooks) g_scOrig[h.slot] = vt[h.slot];   // all of them before the first hook is live
            for (auto& h : hooks) InterlockedExchangePointer(&vt[h.slot], h.hook);
            VirtualProtect(vt, SC_SLOTS * sizeof(void*), old, &old);
            g_scVt = vt;
            XLog("XeSS-FG's swap chain %p: virtual table %p hooked (the game's questions to it)", sc, vt);
        }
    }
    ReleaseSRWLockExclusive(&lock);
    sc->Release();
}

// The package's own frame generation DLL (ffxfg/ffx_xefg*.cpp) wraps XeSS-FG's swap chain and answers its game itself:
// the hooks above pass every call of such a swap chain on unchanged.
extern "C" void IgdextXefgProxyOwned(void* swapChain, int on)
{
    if (!swapChain) return;
    if (!on) { g_scOwned.Remove(swapChain); return; }
    const int added = g_scOwned.Add(swapChain);
    if (added > 0) XLog("swap chain %p: its wrapper answers the game itself", swapChain);
    else if (added < 0) XLog("swap chain %p: no free slot, its calls are answered here as for a game's own XeSS-FG", swapChain);
}

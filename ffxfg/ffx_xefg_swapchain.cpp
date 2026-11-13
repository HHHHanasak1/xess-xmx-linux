// ffx_xefg_swapchain.cpp - FgSwapChain, the swap chain that a game gets from amd_fidelityfx_framegeneration_dx12.dll
// (the class is in ffx_xefg.h): the game's Present around the XeSS-FG proxy's, the answers to what a game asks of the
// swap chain every frame (IGDEXT_XELL_POLLS), exclusive fullscreen, the frame latency waitable object and HDR10.
#include "ffx_xefg.h"

// ---------------------------------------------------------------------------------------------------------------
// HDR10
//
// XeSS-FG learns how the back buffer is encoded from one call: the game's SetColorSpace1 on the proxy swap chain.
// For DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 (HDR10, PQ) its interpolation takes the back buffer for HDR10, for
// any other value for SDR. The proxy notes that first and then passes the call on to the real swap chain, whatever
// that answers. (XeSS-FG 1.3.1; it forgets HDR10 again whenever it sets its interpolation up anew: a new size, or a
// change between one and several generated frames.)
// Told HDR10, XeSS-FG does not hand the PQ values to its network: it makes the network's images from them with
// 350 nits as white, and everything brighter is one flat white there. A picture with large areas above that leaves
// the network nothing to follow in them. CONTROL Resonant's HDR output goes up to about 1500 nits, and its sunlit
// clouds stuttered at 3x and more while the camera turned (not at 2x, which runs another network, and not with the
// game's HDR off). Not told, XeSS-FG gives the network the PQ values as they are: 350 nits is 0.64 there and
// 1500 nits 0.80, so the bright areas keep their detail. The shown frames are made from the back buffers either
// way and stay PQ.
// So XeSS-FG is not told by default (FFX_XEFG_HDR10=1: it is). The real swap chain still has to get HDR10, and only
// the proxy leads to it. After the game's call the proxy therefore gets a colour space that no swap chain takes
// (kRefusedColorSpace): XeSS-FG is back at SDR, the real swap chain refuses the call and keeps HDR10. Should a swap
// chain take it after all, HDR10 is set again at once, and XeSS-FG stays told from then on.
static const DXGI_COLOR_SPACE_TYPE kHdr10 = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
static const DXGI_COLOR_SPACE_TYPE kRefusedColorSpace = DXGI_COLOR_SPACE_CUSTOM;

// The low latency (ffx_xefg_lowlat.cpp): how long a frame was held up inside the proxy's Present (DoPresent)
static const float kLatSpanMs = 0.25f;     // XeSS-FG's first to last submission inside its Present, without a wait
static const float kLatPresentMs = 2.0f;   // the proxy's Present without a wait, when its submissions are not seen

// ---------------------------------------------------------------------------------------------------------------
// FgSwapChain: forwards everything to the XeSS-FG proxy, adds the XeLL markers and the present id.

// IUnknown
HRESULT STDMETHODCALLTYPE FgSwapChain::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv)
        return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIDeviceSubObject) ||
        riid == __uuidof(IDXGISwapChain) || riid == __uuidof(IDXGISwapChain1) ||
        riid == __uuidof(IDXGISwapChain2) || riid == __uuidof(IDXGISwapChain3) ||
        riid == __uuidof(IDXGISwapChain4)) {
        AddRef();
        *ppv = static_cast<IDXGISwapChain4*>(this);
        return S_OK;
    }
    return sc->QueryInterface(riid, ppv);
}
ULONG STDMETHODCALLTYPE FgSwapChain::Release()
{
    ULONG r = --ref;
    if (r == 0) {
        Log("FgSwapChain %p released by the game", (void*)this);
        {
            std::lock_guard<std::mutex> lk(st->lock);
            st->wrapper = nullptr;
        }
        if (st->bridge) {   // no FFX context: the game's reference is the last one
            std::lock_guard<std::mutex> lk(g_globalLock);
            if (g_currentSc == st)
                g_currentSc = nullptr;
        }
        ScRelease(st);
        delete this;
    }
    return r;
}

// IDXGIObject
HRESULT STDMETHODCALLTYPE FgSwapChain::SetPrivateData(REFGUID g, UINT n, const void* d) { return sc->SetPrivateData(g, n, d); }
HRESULT STDMETHODCALLTYPE FgSwapChain::SetPrivateDataInterface(REFGUID g, const IUnknown* u) { return sc->SetPrivateDataInterface(g, u); }
HRESULT STDMETHODCALLTYPE FgSwapChain::GetPrivateData(REFGUID g, UINT* n, void* d) { return sc->GetPrivateData(g, n, d); }
HRESULT STDMETHODCALLTYPE FgSwapChain::GetParent(REFIID riid, void** p) { return sc->GetParent(riid, p); }
// IDXGIDeviceSubObject
HRESULT STDMETHODCALLTYPE FgSwapChain::GetDevice(REFIID riid, void** p) { return sc->GetDevice(riid, p); }

// IDXGISwapChain
HRESULT STDMETHODCALLTYPE FgSwapChain::Present(UINT sync, UINT flags)
{
    if (flags & DXGI_PRESENT_TEST)
        return sc->Present(sync, flags);
    return DoPresent(sync, flags, nullptr, false);
}
// the game's Present and Present1
HRESULT FgSwapChain::DoPresent(UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp, bool one)
{
#ifdef FFXFG_DEBUG
    const uint64_t n = st->presents + 1;
    tr::g_seq++;
    tr::g_presRet = 0;
    tr::g_stackTid = GetCurrentThreadId();
    tr::Put(tr::GP_CALL, st->frameId, n);
    if (tr::g_on && (n & 255) == 1)
        tr::HookInputs();
    if (n == 1 || n == 3 || n == 600)
        CheckRealVt("at a present");
#endif
    if (g_hdr10AbSec > 0 && st->xefgEnabled)
        HdrAb();
    // The answers to what the game asks of the swap chain until its next Present (see "Low latency"). While
    // XeSS-FG's presentation thread is inside a Present of the real swap chain this waits for it, as the proxy's
    // Present below would: a frame held up in front of its present, as far as the low latency is concerned.
    const uint64_t first = g_latQueue.load() == st->queue ? g_latFirstSubmit.load() : 0;
    float blockMs = 0;
    if (g_pollCache && st->xefgEnabled && st->lat.abTurn != 0)
        blockMs = PollAnswers();
    else
        PollForget();
    uint32_t id = BeforePresent(st);
    g_ev.mult = std::min<int>(g_mult, (int)st->maxInterpolations + 1);
    TR(tr::GpuMarkSignal(st->gpu1, st->queue, n); tr::Put(tr::PX_CALL, id, n));
    uint64_t span[3] = {};
    t_eclSpan = g_origEcl ? span : nullptr;
    t_pxImages = 0;   // the first real Present on this thread is the frame's image 0 (EvenBefore)
    HRESULT hr = one ? sc->Present1(sync, flags, pp) : sc->Present(sync, flags);
    t_pxImages = -1;
    t_eclSpan = nullptr;
    const uint64_t tEnd = Qpc();
    TR(tr::Put(tr::PX_RET, (uint64_t)hr, n, 0, tEnd); tr::GpuMarkSignal(st->gpu2, st->queue, n));
    LatGpuSignal(st);
    // what the frame waited inside XeSS-FG's Present: between its first and last submission, or, when they did
    // not show, the whole call less what it takes without a wait
    if (span[2] >= 2)
        blockMs += std::max(0.0f, QpcMs(span[0], span[1]) - kLatSpanMs);
    else
        blockMs += std::max(0.0f, QpcMs(st->tStart, tEnd) - kLatPresentMs);
    AfterPresent(st, id, hr, sync, flags, tEnd, blockMs, first);
#ifdef FFXFG_DEBUG
    tr::g_seq++;
    tr::Put(tr::GP_RET, (uint64_t)(QpcMs(tEnd, Qpc()) * 1000.0f), n, (uint16_t)std::min(blockMs * 100.0f, 65535.0f));
    if (st->xefgEnabled)   // the stack sampler looks at frames with interpolation only
        tr::g_presRet = Qpc();
    g_traceNext = n + 1;
#endif
    return hr;
}
// Takes the answers from the swap chain: the frame statistics and the color spaces asked about since the last
// time. Returns how long that took (ms): it waits while a Present of the real swap chain is under way.
float FgSwapChain::PollAnswers()
{
    const uint64_t t0 = Qpc();
    DXGI_FRAME_STATISTICS fs = {};
    const HRESULT fhr = sc->GetFrameStatistics(&fs);
    ScState::ColorSpaceAnswer cs[8];
    int n;
    {
        std::lock_guard<std::mutex> lk(st->pollLock);
        n = st->nColorSpaces;
        memcpy(cs, st->colorSpaces, sizeof(cs));
    }
    for (int i = 0; i < n; i++)
        if (cs[i].asked)
            cs[i].hr = sc->CheckColorSpaceSupport(cs[i].cs, &cs[i].support);
    const float ms = QpcMs(t0, Qpc());
    std::lock_guard<std::mutex> lk(st->pollLock);
    st->stats = fs;
    st->statsHr = fhr;
    // the game may have asked about another color space meanwhile, on another thread: by value, not by place
    int keep = 0;
    for (int k = 0; k < st->nColorSpaces; k++) {
        ScState::ColorSpaceAnswer a = st->colorSpaces[k];
        for (int i = 0; i < n; i++)
            if (cs[i].cs == a.cs) {
                if (cs[i].asked) {
                    a.hr = cs[i].hr;
                    a.support = cs[i].support;
                    a.asked = false;
                    a.idle = 0;
                } else {
                    a.idle++;
                }
            }
        if (a.idle < 600)   // one not asked about any more is forgotten
            st->colorSpaces[keep++] = a;
    }
    st->nColorSpaces = keep;
    st->pollValid = true;
    return ms;
}
// the swap chain itself answers from here on (interpolation off, a resize, a mode or color space change)
void FgSwapChain::PollForget()
{
    std::lock_guard<std::mutex> lk(st->pollLock);
    st->pollValid = false;
    st->nColorSpaces = 0;
}
// A call of the game into the swap chain has returned (started at `t0`). If it waited there while XeSS-FG
// interpolates, for the presentation thread inside a Present of the real swap chain, the game's frame starts
// where it returns rather than where its Present returned, and the log names the call: the low latency then
// finds nothing to wait for (the frame is not held up in front of its next present), as before all this.
void FgSwapChain::Waited(const char* what, int id, uint64_t t0)
{
    const uint64_t t1 = Qpc();
    const float ms = QpcMs(t0, t1);
    TR(tr::Put(tr::SCCALL, id, 0, (uint16_t)std::min(ms * 1000.0f, 65535.0f), t0));
    if (ms < 2.0f || !st->xefgEnabled)
        return;
    st->frameStart = t1;
    if (st->waitedSaid.fetch_add(1) < 8)
        Log("the game's %s waited %.1f ms in the swap chain (for XeSS-FG's presentation thread): its frame starts "
            "there, not where Present returned",
            what, ms);
}
// a call of the game passed on to the proxy, with Waited
#define SC_FORWARD(what, id, call)  \
    {                               \
        const uint64_t t0 = Qpc();  \
        auto r = call;              \
        Waited(what, id, t0);       \
        return r;                   \
    }
HRESULT STDMETHODCALLTYPE FgSwapChain::GetBuffer(UINT i, REFIID riid, void** p) SC_FORWARD("GetBuffer", 1, sc->GetBuffer(i, riid, p))
HRESULT STDMETHODCALLTYPE FgSwapChain::SetFullscreenState(BOOL f, IDXGIOutput* o)
{
    HRESULT hr;
    if (g_noExclusive) {
        st->fakeFullscreen = f;
        hr = S_OK;  // stay windowed (borderless under gamescope looks the same)
    } else {
        hr = sc->SetFullscreenState(f, o);
    }
    Log("SetFullscreenState(%d) -> 0x%08lx%s", f, (unsigned long)hr, g_noExclusive ? " (blocked)" : "");
    PollForget();
    Recheck();
    return hr;
}
HRESULT STDMETHODCALLTYPE FgSwapChain::GetFullscreenState(BOOL* f, IDXGIOutput** o)
{
    const uint64_t t0 = Qpc();
    HRESULT hr;
    if (g_noExclusive) {
        hr = sc->GetFullscreenState(nullptr, o);
        if (f)
            *f = st->fakeFullscreen;
    } else {
        hr = sc->GetFullscreenState(f, o);
    }
    Waited("GetFullscreenState", 8, t0);
    return hr;
}
HRESULT STDMETHODCALLTYPE FgSwapChain::GetDesc(DXGI_SWAP_CHAIN_DESC* d)
{
    const uint64_t t0 = Qpc();
    HRESULT hr = sc->GetDesc(d);
    if (SUCCEEDED(hr) && d && g_noExclusive)
        d->Windowed = !st->fakeFullscreen;
    Waited("GetDesc", 4, t0);
    return hr;
}
HRESULT STDMETHODCALLTYPE FgSwapChain::ResizeBuffers(UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl)
{
    PollForget();
    HRESULT hr = sc->ResizeBuffers(n, w, h, f, fl);
    Log("ResizeBuffers %u %ux%u fmt %d flags 0x%x -> 0x%08lx", n, w, h, (int)f, fl, (unsigned long)hr);
    if (SUCCEEDED(hr)) {
        Resized();
        HdrResized();
    }
    Recheck();
    return hr;
}
// the size the swap chain has now (0x0 in ResizeBuffers means the window's); FFX_XEFG_OFLOW's textures follow it
void FgSwapChain::Resized()
{
    DXGI_SWAP_CHAIN_DESC1 d = {};
    if (FAILED(sc->GetDesc1(&d)))
        return;
    std::lock_guard<std::mutex> lk(st->lock);
    if (d.Width == st->width && d.Height == st->height)
        return;
    st->width = d.Width;
    st->height = d.Height;
    if (g_oflow) {   // also when there was none yet (a swap chain created at 0x0)
        OflowDestroy(st->oflow);
        st->oflow = OflowCreate(st->device, d.Width, d.Height, g_oflowDepth);
        st->oflowLastQpc = 0;   // the next frame resets XeSS-FG's history
        Log("oflow: remade for %ux%u -> %p", d.Width, d.Height, (void*)st->oflow);
    }
}
// After a mode change XeSS-FG must be asked again: it checks fullscreen only when interpolation is enabled.
void FgSwapChain::Recheck()
{
    std::lock_guard<std::mutex> lk(st->lock);
    if (st->xefgEnabled) {
        p_xefgSwapChainSetEnabled(st->xefg, 0);
        st->xefgEnabled = false;  // the next present with frame data enables it again
    }
}
HRESULT STDMETHODCALLTYPE FgSwapChain::ResizeTarget(const DXGI_MODE_DESC* m)
{
    PollForget();
    return sc->ResizeTarget(m);
}
HRESULT STDMETHODCALLTYPE FgSwapChain::GetContainingOutput(IDXGIOutput** o) SC_FORWARD("GetContainingOutput", 7, sc->GetContainingOutput(o))
// As of the last Present call while XeSS-FG interpolates (IGDEXT_XELL_POLLS, see "Low latency"): the swap chain's
// own answer waits for XeSS-FG's presentation thread whenever that is inside a Present.
HRESULT STDMETHODCALLTYPE FgSwapChain::GetFrameStatistics(DXGI_FRAME_STATISTICS* s)
{
    if (s) {
        std::lock_guard<std::mutex> lk(st->pollLock);
        if (st->pollValid) {
            TR(tr::Put(tr::SCCALL, 5, 1));
            *s = st->stats;
            return st->statsHr;
        }
    }
    SC_FORWARD("GetFrameStatistics", 5, sc->GetFrameStatistics(s))
}
HRESULT STDMETHODCALLTYPE FgSwapChain::GetLastPresentCount(UINT* c) SC_FORWARD("GetLastPresentCount", 11, sc->GetLastPresentCount(c))

// IDXGISwapChain1
HRESULT STDMETHODCALLTYPE FgSwapChain::GetDesc1(DXGI_SWAP_CHAIN_DESC1* d) SC_FORWARD("GetDesc1", 9, sc->GetDesc1(d))
HRESULT STDMETHODCALLTYPE FgSwapChain::GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* d) SC_FORWARD("GetFullscreenDesc", 10, sc->GetFullscreenDesc(d))
HRESULT STDMETHODCALLTYPE FgSwapChain::GetHwnd(HWND* h) SC_FORWARD("GetHwnd", 12, sc->GetHwnd(h))
HRESULT STDMETHODCALLTYPE FgSwapChain::GetCoreWindow(REFIID riid, void** p) { return sc->GetCoreWindow(riid, p); }
HRESULT STDMETHODCALLTYPE FgSwapChain::Present1(UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp)
{
    if (flags & DXGI_PRESENT_TEST)
        return sc->Present1(sync, flags, pp);
    return DoPresent(sync, flags, pp, true);
}
BOOL STDMETHODCALLTYPE FgSwapChain::IsTemporaryMonoSupported() { return sc->IsTemporaryMonoSupported(); }
HRESULT STDMETHODCALLTYPE FgSwapChain::GetRestrictToOutput(IDXGIOutput** o) { return sc->GetRestrictToOutput(o); }
HRESULT STDMETHODCALLTYPE FgSwapChain::SetBackgroundColor(const DXGI_RGBA* c) { return sc->SetBackgroundColor(c); }
HRESULT STDMETHODCALLTYPE FgSwapChain::GetBackgroundColor(DXGI_RGBA* c) { return sc->GetBackgroundColor(c); }
HRESULT STDMETHODCALLTYPE FgSwapChain::SetRotation(DXGI_MODE_ROTATION r) { return sc->SetRotation(r); }
HRESULT STDMETHODCALLTYPE FgSwapChain::GetRotation(DXGI_MODE_ROTATION* r) { return sc->GetRotation(r); }

// IDXGISwapChain2
HRESULT STDMETHODCALLTYPE FgSwapChain::SetSourceSize(UINT w, UINT h) { return sc->SetSourceSize(w, h); }
HRESULT STDMETHODCALLTYPE FgSwapChain::GetSourceSize(UINT* w, UINT* h) { return sc->GetSourceSize(w, h); }
HRESULT STDMETHODCALLTYPE FgSwapChain::SetMaximumFrameLatency(UINT l)
{
    HRESULT hr = sc->SetMaximumFrameLatency(l);
    Log("SetMaximumFrameLatency(%u) -> 0x%08lx", l, (unsigned long)hr);
    if (g_ownWait && l >= 1) {
        std::lock_guard<std::mutex> lk(st->lock);
        if (st->latencySem && l > st->maxLatency)
            ReleaseSemaphore(st->latencySem, (LONG)(l - st->maxLatency), nullptr);
        st->maxLatency = l;
        hr = S_OK;
    }
    return hr;
}
HRESULT STDMETHODCALLTYPE FgSwapChain::GetMaximumFrameLatency(UINT* l)
{
    if (g_ownWait && l) {
        *l = st->maxLatency;
        return S_OK;
    }
    return sc->GetMaximumFrameLatency(l);
}
HANDLE STDMETHODCALLTYPE FgSwapChain::GetFrameLatencyWaitableObject()
{
    HANDLE h = nullptr;
    if (g_ownWait) {
        std::lock_guard<std::mutex> lk(st->lock);
        if (!st->latencySem)
            st->latencySem = CreateSemaphoreA(nullptr, (LONG)st->maxLatency, 64, nullptr);
        if (st->latencySem)
            PaceStart(st);
        // the caller closes the handle it gets, as with DXGI
        DuplicateHandle(GetCurrentProcess(), st->latencySem, GetCurrentProcess(), &h, 0, FALSE,
                        DUPLICATE_SAME_ACCESS);
    } else {
        h = sc->GetFrameLatencyWaitableObject();
    }
    Log("GetFrameLatencyWaitableObject -> %p (own %d)", (void*)h, g_ownWait);
    TR(tr::Put(tr::WAITOBJ));
    return h;
}
HRESULT STDMETHODCALLTYPE FgSwapChain::SetMatrixTransform(const DXGI_MATRIX_3X2_F* m) { return sc->SetMatrixTransform(m); }
HRESULT STDMETHODCALLTYPE FgSwapChain::GetMatrixTransform(DXGI_MATRIX_3X2_F* m) { return sc->GetMatrixTransform(m); }

// IDXGISwapChain3
UINT STDMETHODCALLTYPE FgSwapChain::GetCurrentBackBufferIndex() SC_FORWARD("GetCurrentBackBufferIndex", 2, sc->GetCurrentBackBufferIndex())
// As of the last Present call while XeSS-FG interpolates, like GetFrameStatistics: a color space the game asks
// about for the first time goes to the swap chain, and is asked again at each Present from then on.
HRESULT STDMETHODCALLTYPE FgSwapChain::CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE c, UINT* s)
{
    if (!s)
        return sc->CheckColorSpaceSupport(c, s);
    {
        std::lock_guard<std::mutex> lk(st->pollLock);
        for (int i = 0; st->pollValid && i < st->nColorSpaces; i++)
            if (st->colorSpaces[i].cs == c) {
                TR(tr::Put(tr::SCCALL, 6, 1));
                st->colorSpaces[i].asked = true;
                *s = st->colorSpaces[i].support;
                return st->colorSpaces[i].hr;
            }
    }
    const uint64_t t0 = Qpc();
    const HRESULT hr = sc->CheckColorSpaceSupport(c, s);
    Waited("CheckColorSpaceSupport", 6, t0);
    std::lock_guard<std::mutex> lk(st->pollLock);
    if (st->pollValid && st->nColorSpaces < 8) {
        bool have = false;
        for (int i = 0; i < st->nColorSpaces; i++)
            have |= st->colorSpaces[i].cs == c;
        if (!have)
            st->colorSpaces[st->nColorSpaces++] = {c, hr, *s, true, 0};
    }
    return hr;
}
HRESULT STDMETHODCALLTYPE FgSwapChain::SetColorSpace1(DXGI_COLOR_SPACE_TYPE c)
{
    std::lock_guard<std::mutex> lk(st->hdrLock);
    // HDR10 once more while the real swap chain has it and XeSS-FG is not to know: nothing to pass on (a game
    // that sets its colour space every frame would wait for the real swap chain's lock twice each time)
    if (c == kHdr10 && st->hdrSet && !st->hdrTold && !HdrWant())
        return S_OK;
    PollForget();
    const HRESULT hr = sc->SetColorSpace1(c);
    st->hdrTold = c == kHdr10;   // XeSS-FG notes it whatever the real swap chain answered ("HDR10")
    const bool changed = SUCCEEDED(hr) && st->colorSpace.exchange((int)c) != (int)c;
    if (SUCCEEDED(hr))
        st->hdrSet = c == kHdr10;
    HdrSync();
    if (changed && c == kHdr10)
        Log("SetColorSpace1 %d (HDR10): XeSS-FG is %s", (int)c,
            st->hdrTold ? "told" : "not told and interpolates the PQ values as they are (FFX_XEFG_HDR10=1: told)");
    else if (changed)
        Log("SetColorSpace1 %d", (int)c);   // FFX_XEFG_OFLOW reads the back buffer in this encoding
    return hr;
}
// Whether XeSS-FG is to know of an HDR10 back buffer (see "HDR10")
bool FgSwapChain::HdrWant() const { return g_hdr10AbSec > 0 ? st->hdrAbTurn <= 0 : g_hdr10; }
// With st->hdrLock held: XeSS-FG's interpolation takes the back buffer for HDR10 only while the real swap chain
// has HDR10 and XeSS-FG is to know; the real swap chain keeps its colour space. Only the proxy leads to both.
void FgSwapChain::HdrSync()
{
    const int cs = st->colorSpace.load();   // what the real swap chain has, -1: never set
    const bool want = cs == (int)kHdr10 && HdrWant();
    if (want == st->hdrTold)
        return;
    if (want || (cs >= 0 && cs != (int)kHdr10)) {
        // the real swap chain's own colour space once more: XeSS-FG takes it for what it is (it was told HDR10
        // by a call the real swap chain refused, or forgot HDR10, or is told for the first time)
        sc->SetColorSpace1((DXGI_COLOR_SPACE_TYPE)cs);
        st->hdrTold = want;
        return;
    }
    if (st->hdrKept)
        return;
    const HRESULT hr = sc->SetColorSpace1(kRefusedColorSpace);
    st->hdrTold = false;
    if (SUCCEEDED(hr) && cs == (int)kHdr10) {
        // the real swap chain took it: HDR10 again at once, and no further try
        const HRESULT back = sc->SetColorSpace1(kHdr10);
        st->hdrTold = true;
        st->hdrKept = true;
        Log("!! HDR10: the real swap chain took colour space 0x%x (0x%08lx), HDR10 is set again (0x%08lx): XeSS-FG "
            "stays told, as with FFX_XEFG_HDR10=1",
            (unsigned)kRefusedColorSpace, (unsigned long)hr, (unsigned long)back);
    } else if (!st->hdrSaid) {
        st->hdrSaid = true;
        Log("HDR10: XeSS-FG is back at SDR through colour space 0x%x, which the real swap chain %s (0x%08lx)",
            (unsigned)kRefusedColorSpace, FAILED(hr) ? "refused" : "took", (unsigned long)hr);
    }
}
// After the buffers were resized: XeSS-FG forgets HDR10 when it sets its interpolation up again, and the game's
// next SetColorSpace1 goes to the real swap chain whatever it is
void FgSwapChain::HdrResized()
{
    std::lock_guard<std::mutex> lk(st->hdrLock);
    st->hdrSet = false;
    if (st->hdrTold) {
        st->hdrTold = false;
        HdrSync();
    }
}
// FFX_XEFG_HDR10_AB, on the present thread: told and not told in turns while XeSS-FG interpolates
void FgSwapChain::HdrAb()
{
    const uint64_t now = Qpc();
    if (!st->hdrAbStart)
        st->hdrAbStart = now;
    const int turn = (int)(((uint64_t)(QpcMs(st->hdrAbStart, now) / 1000.0f) / (uint64_t)(g_hdr10AbSec > 0 ? g_hdr10AbSec : 1)) % 2);
    if (turn == st->hdrAbTurn)
        return;
    std::lock_guard<std::mutex> lk(st->hdrLock);
    st->hdrAbTurn = turn;
    if (st->colorSpace.load() != (int)kHdr10)
        return;
    HdrSync();
    SYSTEMTIME t;
    GetLocalTime(&t);
    Log("HDR10: XeSS-FG %s at %02d:%02d:%02d.%03d", st->hdrTold ? "told" : "not told", t.wHour, t.wMinute,
        t.wSecond, t.wMilliseconds);
}
HRESULT STDMETHODCALLTYPE FgSwapChain::ResizeBuffers1(UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl, const UINT* mask,
                                                      IUnknown* const* queues)
{
    PollForget();
    HRESULT hr = sc->ResizeBuffers1(n, w, h, f, fl, mask, queues);
    Log("ResizeBuffers1 %u %ux%u fmt %d -> 0x%08lx", n, w, h, (int)f, (unsigned long)hr);
    if (SUCCEEDED(hr)) {
        Resized();
        HdrResized();
    }
    Recheck();
    return hr;
}

// IDXGISwapChain4
HRESULT STDMETHODCALLTYPE FgSwapChain::SetHDRMetaData(DXGI_HDR_METADATA_TYPE t, UINT n, void* d) { return sc->SetHDRMetaData(t, n, d); }
#undef SC_FORWARD

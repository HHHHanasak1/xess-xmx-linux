// ffx_xefg_context.cpp - a swap chain context of amd_fidelityfx_framegeneration_dx12.dll: CreateSwapChainContext makes
// the XeSS-FG and XeLL contexts, the proxy swap chain and the FgSwapChain around it; TearDown frees them when the last
// reference to the context goes (ScRelease in ffx_xefg.h).
#include "ffx_xefg.h"

static void XefgLogCallback(const char* message, xefg_swapchain_logging_level_t level, void*)
{
    Log("xefg[%d] %s", (int)level, message);
}

// The XMX shim's own low latency (AutoPaceWait in shim/src/dll/XellHook.cpp) leaves an XeLL context to its caller when
// told so; a shim without the call never waits for our contexts anyway (it finds no frame in flight at our xellSleep)
typedef void (*PFN_IgdextXellExternalPacing)(void* xellContext, int on);
static void ShimExternalPacing(void* xell, int on)
{
    HMODULE shim = xell ? GetModuleHandleA("igdext64.dll") : nullptr;
    auto fn = shim ? (PFN_IgdextXellExternalPacing)GetProcAddress(shim, "IgdextXellExternalPacing") : nullptr;
    if (fn)
        fn(xell, on);
}

// The XMX shim answers a game's questions to XeSS-FG's swap chain for a game with its own XeSS-FG (ProxyHook in
// shim/src/dll). This DLL wraps that swap chain and answers its game itself (FgSwapChain), so it tells
// the shim to pass this swap chain's calls on unchanged; a shim without the call does not touch them anyway.
typedef void (*PFN_IgdextXefgProxyOwned)(void* swapChain, int on);
static void ShimProxyOwned(void* proxy, int on)
{
    HMODULE shim = proxy ? GetModuleHandleA("igdext64.dll") : nullptr;
    auto fn = shim ? (PFN_IgdextXefgProxyOwned)GetProcAddress(shim, "IgdextXefgProxyOwned") : nullptr;
    if (fn)
        fn(proxy, on);
}

void TearDown(ScState* st)
{
    Log("teardown sc %p (xefg %p, presents %llu)", (void*)st, (void*)st->xefg, (unsigned long long)st->presents);
    {
        // first: once g_timedSc is cleared under this lock, HookEcl is done with st and does not find it again
        std::lock_guard<std::mutex> lk(g_timedLock);
        if (g_timedSc.load() == st) {
            g_timedQueue = nullptr;
            g_timedSc = nullptr;
        }
    }
    if (st->realSc && g_ev.real.load() == st->realSc)
        g_ev.real = nullptr;   // its Presents pass through from now on
    if (!PaceStop(st)) {
        Log("!! the pacing thread did not stop: the state stays allocated");
        return;
    }
    if (!LatGpuStop(st)) {
        Log("!! the low latency's GPU thread did not stop: the state stays allocated");
        return;
    }
#ifdef FFXFG_DEBUG
    if (g_tracePre.load() == st->gpu3)
        g_tracePre = nullptr;
    tr::GpuMarkDestroy(st->gpu1);
    tr::GpuMarkDestroy(st->gpu2);
    tr::GpuMarkDestroy(st->gpu3);
    st->gpu1 = st->gpu2 = st->gpu3 = nullptr;
#endif
    ShimExternalPacing(st->xell, 0);
    ShimProxyOwned(st->proxy, 0);
    if (st->proxy) {
        ULONG r = st->proxy->Release();
        Log("  proxy released, refs left %lu", r);
        st->proxy = nullptr;
    }
    if (st->xefg) {
        xefg_swapchain_result_t r = p_xefgSwapChainDestroy(st->xefg);
        Log("  xefgSwapChainDestroy -> %d", (int)r);
    }
    if (st->xell) {
        xell_result_t r = p_xellDestroyContext(st->xell);
        Log("  xellDestroyContext -> %d", (int)r);
    }
    OflowDestroy(st->oflow);
    st->oflow = nullptr;
    if (st->cmdList)
        st->cmdList->Release();
    if (st->alloc)
        st->alloc->Release();
    if (st->interpTex)
        st->interpTex->Release();
    if (st->latencySem)
        CloseHandle(st->latencySem);
    if (st->ui.resource)
        ((ID3D12Resource*)st->ui.resource)->Release();
    if (st->queue)
        st->queue->Release();
    if (st->device)
        st->device->Release();
    delete st;
}

// ---------------------------------------------------------------------------------------------------------------
// Swap chain context creation

ffxReturnCode_t CreateSwapChainContext(ffxContext* context, ScKind kind, const ffxApiHeader* desc)
{
    IDXGISwapChain4** out = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    IDXGISwapChain4* appSwapChain = nullptr;
    HWND hwnd = nullptr;
    DXGI_SWAP_CHAIN_DESC1 d1 = {};
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fs = {};
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pfs = nullptr;
    IDXGIFactory* factory = nullptr;

    if (kind == ScKind::Wrap) {
        auto* d = (const ffxCreateContextDescFrameGenerationSwapChainWrapDX12*)desc;
        out = d->swapchain;
        queue = d->gameQueue;
        appSwapChain = out ? *out : nullptr;
        if (!appSwapChain)
            return FFX_API_RETURN_ERROR_PARAMETER;
        appSwapChain->GetDesc1(&d1);
    } else if (kind == ScKind::New) {
        auto* d = (const ffxCreateContextDescFrameGenerationSwapChainNewDX12*)desc;
        out = d->swapchain;
        queue = d->gameQueue;
        factory = d->dxgiFactory;
        if (!d->desc)
            return FFX_API_RETURN_ERROR_PARAMETER;
        const DXGI_SWAP_CHAIN_DESC& o = *d->desc;
        hwnd = o.OutputWindow;
        d1.Width = o.BufferDesc.Width;
        d1.Height = o.BufferDesc.Height;
        d1.Format = o.BufferDesc.Format;
        d1.SampleDesc = o.SampleDesc;
        d1.BufferUsage = o.BufferUsage;
        d1.BufferCount = o.BufferCount;
        d1.Scaling = o.BufferDesc.Scaling == DXGI_MODE_SCALING_UNSPECIFIED ? DXGI_SCALING_STRETCH
                     : o.BufferDesc.Scaling == DXGI_MODE_SCALING_CENTERED  ? DXGI_SCALING_NONE
                                                                           : DXGI_SCALING_STRETCH;
        d1.SwapEffect = o.SwapEffect;
        d1.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        d1.Flags = o.Flags;
        fs.RefreshRate = o.BufferDesc.RefreshRate;
        fs.ScanlineOrdering = o.BufferDesc.ScanlineOrdering;
        fs.Scaling = o.BufferDesc.Scaling;
        fs.Windowed = o.Windowed;
        pfs = &fs;
    } else {
        auto* d = (const ffxCreateContextDescFrameGenerationSwapChainForHwndDX12*)desc;
        out = d->swapchain;
        queue = d->gameQueue;
        factory = d->dxgiFactory;
        hwnd = d->hwnd;
        if (!d->desc)
            return FFX_API_RETURN_ERROR_PARAMETER;
        d1 = *d->desc;
        pfs = d->fullscreenDesc;
    }
    if (!out || !queue)
        return FFX_API_RETURN_ERROR_PARAMETER;

    auto* st = new ScState();
    st->width = d1.Width;
    st->height = d1.Height;
    st->format = d1.Format;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&st->device)))) {
        delete st;
        return FFX_API_RETURN_ERROR_RUNTIME_ERROR;
    }
    queue->AddRef();
    st->queue = queue;

    auto fail = [&](const char* what, int code) {
        Log("swap chain context: %s failed (%d)", what, code);
        st->refs = 1;
        if (st->proxy) {
            st->proxy->Release();
            st->proxy = nullptr;
        }
        TearDown(st);
        return FFX_API_RETURN_ERROR_RUNTIME_ERROR;
    };

    xefg_swapchain_d3d12_init_params_t ip = {};
    uint32_t want = 0;
    IntelVendorForXess(st->device);
    xefg_swapchain_result_t r = p_xefgSwapChainD3D12CreateContext(st->device, &st->xefg);
    if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS) {
        st->xefg = nullptr;
        return fail("xefgSwapChainD3D12CreateContext", r);
    }
    if (g_xefgLogLevel >= 0 && g_xefgLogLevel <= 3)
        p_xefgSwapChainSetLoggingCallback(st->xefg, (xefg_swapchain_logging_level_t)g_xefgLogLevel, XefgLogCallback,
                                          nullptr);

    xell_result_t xr = p_xellD3D12CreateContext(st->device, &st->xell);
    if (xr != XELL_RESULT_SUCCESS) {
        st->xell = nullptr;
        return fail("xellD3D12CreateContext", xr);
    }
    xell_sleep_params_t sp = {};
    sp.bLowLatencyMode = 1;
    // FFX_XEFG_CAP: microseconds per real frame, for a steady real frame rate. XeLL's minimum interval counts every
    // frame shown, the generated ones too (27000 gave 54 ms per real frame at 2x), so it gets the interval per frame shown
    int cap = EnvInt("FFX_XEFG_CAP", 0);
    if (cap > 0) {
        sp.minimumIntervalUs = (uint32_t)(cap / (g_mult > 1 ? g_mult : 1));
        Log("XeLL: one real frame per %d us (%.1f fps), %u us per frame shown", cap, 1e6 / cap, sp.minimumIntervalUs);
    }
    xr = p_xellSetSleepMode(st->xell, &sp);
    if (xr != XELL_RESULT_SUCCESS)
        Log("xellSetSleepMode -> %d", (int)xr);
    r = p_xefgSwapChainSetLatencyReduction(st->xefg, st->xell);
    if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS)
        return fail("xefgSwapChainSetLatencyReduction", r);

    // a swap chain made in exclusive fullscreen: XeSS-FG interpolates only windowed, so it starts windowed and the
    // game is told it is fullscreen (as FgSwapChain::SetFullscreenState; under gamescope both look the same)
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fsWin = {};
    if (pfs && !pfs->Windowed && g_noExclusive) {
        fsWin = *pfs;
        fsWin.Windowed = TRUE;
        pfs = &fsWin;
        st->fakeFullscreen = TRUE;
    }
    Log("swap chain %ux%u format %d buffers %u effect %d flags 0x%x scaling %d alpha %d, %s%s", d1.Width, d1.Height,
        (int)d1.Format, d1.BufferCount, (int)d1.SwapEffect, d1.Flags, (int)d1.Scaling, (int)d1.AlphaMode,
        pfs ? (pfs->Windowed ? "windowed" : "exclusive fullscreen") : "no fullscreen desc",
        st->fakeFullscreen ? " (asked for exclusive fullscreen, made windowed)" : "");
    ip.initFlags = XefgInitFlags();
    ip.maxInterpolatedFrames = XEFG_SWAPCHAIN_USE_MAX_SUPPORTED_INTERPOLATED_FRAMES;
    ip.uiMode = XEFG_SWAPCHAIN_UI_MODE_AUTO;
    if ((g_lat == 1 && !g_ffxCap) || g_times) {
        // the low latency measures in XeSS-FG's Present and on the game's queue (HookEcl, LatGpu), and waits for this
        // XeLL context itself
        if (HookEclVt(queue)) {
            g_latFirstSubmit = 0;
            g_latQueue = queue;
            LatGpuStart(st);
        }
        if (g_lat == 1 && !g_ffxCap)
            ShimExternalPacing(st->xell, 1);
    }
    // the real swap chain's Present: the shown frames' figures (and the time line)
    if (g_times TR(|| tr::g_on))
        HookFactory();
    g_captured = nullptr;
    struct Capturing {   // the swap chain XeSS-FG makes now is the real one
        Capturing() { g_capturing++; }
        ~Capturing() { g_capturing--; }
    } capturing;
#ifdef FFXFG_DEBUG
    if (tr::g_on) {
        // the queues' ExecuteCommandLists and Signal, the GPU marks, the input calls
        HookEclVt(queue);
        tr::HookQueueVt(queue);
        g_traceQueue = queue;
        st->gpu1 = tr::GpuMarkCreate(st->device, tr::GPU1);
        st->gpu3 = tr::GpuMarkCreate(st->device, tr::GPU3);
        g_tracePre = st->gpu3;
        st->gpu2 = tr::GpuMarkCreate(st->device, tr::GPU2);
        tr::HookInputs();
    }
#endif

    if (kind == ScKind::Wrap) {
        // XeSS-FG takes the game's reference and recreates the native swap chain, as FFX's wrap does.
        ULONG refsPlusOne = appSwapChain->AddRef();
        appSwapChain->Release();
        Log("wrap: game swap chain %p refs %lu", (void*)appSwapChain, refsPlusOne - 1);
        ip.pApplicationSwapChain = appSwapChain;
        r = p_xefgSwapChainD3D12InitFromSwapChain(st->xefg, queue, &ip);
    } else {
        IDXGIFactory2* f2 = nullptr;
        if (!factory || FAILED(factory->QueryInterface(IID_PPV_ARGS(&f2))))
            return fail("IDXGIFactory2", 0);
        r = p_xefgSwapChainD3D12InitFromSwapChainDesc(st->xefg, hwnd, &d1, pfs, queue, f2, &ip);
        f2->Release();
    }
    Log("xefgSwapChainD3D12InitFromSwapChain(Desc) -> %d", (int)r);
    TR(CheckRealVt("after XeSS-FG's setup"));
    st->realSc = g_captured.exchange(nullptr);
    if (st->realSc)
        g_ev.real = st->realSc;
    if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS)
        return fail("xefgSwapChainD3D12InitFromSwapChain(Desc)", r);
    r = p_xefgSwapChainD3D12GetSwapChainPtr(st->xefg, IID_PPV_ARGS(&st->proxy));
    if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS || !st->proxy)
        return fail("xefgSwapChainD3D12GetSwapChainPtr", r);
    ShimProxyOwned(st->proxy, 1);

    xefg_swapchain_properties_t props = {};
    if (p_xefgSwapChainGetProperties(st->xefg, &props) == XEFG_SWAPCHAIN_RESULT_SUCCESS)
        st->maxInterpolations = props.maxSupportedInterpolations;
    want = (uint32_t)(g_mult - 1);
    if (want > st->maxInterpolations)
        want = st->maxInterpolations;
    if (p_xefgSwapChainSetNumInterpolatedFrames) {
        r = p_xefgSwapChainSetNumInterpolatedFrames(st->xefg, want);
        Log("xefgSwapChainSetNumInterpolatedFrames(%u) -> %d", want, (int)r);
    }

    if (g_oflow && (!st->width || !st->height)) {   // 0x0 in the description: the window's size
        DXGI_SWAP_CHAIN_DESC1 pd = {};
        if (SUCCEEDED(st->proxy->GetDesc1(&pd))) {
            st->width = pd.Width;
            st->height = pd.Height;
        }
    }
    if (g_oflow && !(st->oflow = OflowCreate(st->device, st->width, st->height, g_oflowDepth)))
        Log("oflow: not available, the game's motion vectors are used");
    st->wrapper = new FgSwapChain(st);
    g_ownVt = *(void***)static_cast<IDXGISwapChain4*>(st->wrapper);
    // FFX's context keeps a reference to the swap chain until ffxDestroyContext; Wuthering Waves (FSR 3.1.1) drops
    // its own reference before that and still presents
    st->wrapper->AddRef();
    st->contextRef = st->wrapper;
    *out = st->wrapper;
    {
        std::lock_guard<std::mutex> lk(g_globalLock);
        g_currentSc = st;
    }
    *context = st;
    Log("swap chain context %p: %ux%u fmt %d, init flags 0x%x, max interpolations %u, set %u, proxy %p, wrapper %p",
        (void*)st, st->width, st->height, (int)st->format, ip.initFlags, st->maxInterpolations, want,
        (void*)st->proxy, (void*)st->wrapper);
    return FFX_API_RETURN_OK;
}

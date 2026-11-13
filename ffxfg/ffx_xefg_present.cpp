// ffx_xefg_present.cpp - what amd_fidelityfx_framegeneration_dx12.dll does before and after each Present of the
// XeSS-FG proxy (BeforePresent, AfterPresent): the frame's id, interpolation on or off, the XeLL markers and sleep, the
// optical flow's inputs (FFX_XEFG_OFLOW), the samples for FFX_XEFG_CAP=auto and FFX_XEFG_TIMES. And the hook on the
// D3D12 queues' ExecuteCommandLists, which times the frames for FFX_XEFG_CAP=auto and the low latency.
#include "ffx_xefg.h"

// ---------------------------------------------------------------------------------------------------------------
// Present handling (called by FgSwapChain around the proxy's Present)

static void SetXefgEnabled(ScState* st, bool want)
{
    if (want == st->xefgEnabled)
        return;
#ifdef FFXFG_DEBUG
    if (want && g_orderLeft.load() == -1 && EnvInt("FFX_XEFG_ORDER", 0))
        g_orderLeft = 300;
#endif
    if (want) {
        BOOL fs = FALSE;
        st->proxy->GetFullscreenState(&fs, nullptr);
        Log("enabling interpolation: proxy fullscreen state %d", fs);
    }
    xefg_swapchain_result_t r = p_xefgSwapChainSetEnabled(st->xefg, want ? 1 : 0);
    Log("xefgSwapChainSetEnabled(%d) frame %u -> %d", want, st->frameId, (int)r);
    if (r == XEFG_SWAPCHAIN_RESULT_SUCCESS)
        st->xefgEnabled = want;
}

// --- XeLL markers (all with st->lock held, except XellStartFrame) ---

static const uint8_t XM_SIM_START = 1 << XELL_SIMULATION_START, XM_PRESENT_END = 1 << XELL_PRESENT_END;

// The ring entry of a frame id. An id whose PRESENT_END went out and that comes up again is a new frame (FFX ids
// repeat when the game restarts its counter); a slot with a sleep in flight is never taken over.
static ScState::XellFrame* XellFrameFor(ScState* st, uint32_t id)
{
    for (auto& f : st->xellFrames)
        if (f.used && f.id == id) {
            if ((f.sent & XM_PRESENT_END) && !f.sleeping) {
                f = ScState::XellFrame();
                f.used = true;
                f.id = id;
            }
            return &f;
        }
    for (unsigned i = 0; i < 8; i++) {
        auto& f = st->xellFrames[(st->xellNext + i) % 8];
        if (f.sleeping)
            continue;
        st->xellNext = (st->xellNext + i + 1) % 8;
        f = ScState::XellFrame();
        f.used = true;
        f.id = id;
        return &f;
    }
    return nullptr;   // not reached: at most one sleep is in flight
}

// Sends the wanted markers that have not gone out, in order. A marker before the sleep would break XeLL's order, so
// once one went out the id gets no sleep any more.
static void XellFlush(ScState* st, ScState::XellFrame* f)
{
    if (f->sleeping)
        return;
    for (int m = XELL_SIMULATION_START; m <= XELL_PRESENT_END; m++) {
        uint8_t bit = (uint8_t)(1u << m);
        if ((f->wanted & bit) && !(f->sent & bit)) {
            f->slept = true;
            p_xellAddMarkerData(st->xell, f->id, (xell_latency_marker_type_t)m);
            f->sent |= bit;
        }
    }
}

// Marker `type` for `id`, with the ones before it that are still missing.
void XellMarker(ScState* st, uint32_t id, xell_latency_marker_type_t type)
{
    if (auto* f = XellFrameFor(st, id)) {
        f->wanted |= (uint8_t)((2u << type) - 1);
        XellFlush(st, f);
    }
}

// Starts frame `id` for XeLL: xellSleep(id), then SIMULATION_START(id). The sleep can block, so it runs without the
// lock; markers the other thread asks for the id meanwhile wait until it returns. Nothing if the id already started.
static void XellStartFrame(ScState* st, uint32_t id)
{
    {
        std::lock_guard<std::mutex> lk(st->lock);
        auto* f = XellFrameFor(st, id);
        if (!f)
            return;
        f->wanted |= XM_SIM_START;
        if (f->slept || f->sleeping || !g_sleep) {
            XellFlush(st, f);
            return;
        }
        f->sleeping = true;
    }
    TR(const uint64_t ts0 = Qpc());
    p_xellSleep(st->xell, id);
    TR(tr::Put(tr::XSLEEP, id, (uint64_t)(QpcMs(ts0, Qpc()) * 1000.0f)));
    std::lock_guard<std::mutex> lk(st->lock);
    auto* f = XellFrameFor(st, id);   // the same slot: a sleeping slot is not taken over
    if (!f)
        return;
    f->sleeping = false;
    f->slept = true;
    XellFlush(st, f);
}

// --- FFX_XEFG_OFLOW ---

// Our textures for frame `id`, written right before its present (BeforePresent): depth, motion vectors, an identity
// camera, no jitter. Called with st->lock held.
void OflowTag(ScState* st, uint32_t id, bool reset, float frameTimeMs)
{
    xefg_swapchain_d3d12_resource_data_t od = {};
    od.validity = XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT;
    od.resourceSize = {st->width, st->height};
    od.incomingState = kOflowOutState;
    od.type = XEFG_SWAPCHAIN_RES_DEPTH;
    od.pResource = OflowDepth(st->oflow);
    xefg_swapchain_result_t o1 = p_xefgSwapChainD3D12TagFrameResource(st->xefg, nullptr, id, &od);
    od.type = XEFG_SWAPCHAIN_RES_MOTION_VECTOR;
    od.pResource = OflowMotionVectors(st->oflow);
    xefg_swapchain_result_t o2 = p_xefgSwapChainD3D12TagFrameResource(st->xefg, nullptr, id, &od);
    xefg_swapchain_frame_constant_data_t oc = {};
    for (int i = 0; i < 4; ++i)
        oc.viewMatrix[i * 5] = oc.projectionMatrix[i * 5] = 1.0f;
    oc.motionVectorScaleX = oc.motionVectorScaleY = 1.0f;
    oc.resetHistory = reset ? 1 : 0;
    oc.frameRenderTime = frameTimeMs;
    xefg_swapchain_result_t o3 = p_xefgSwapChainTagFrameConstants(st->xefg, id, &oc);
    st->oflowTags++;
    if (st->oflowTags <= 5 || st->oflowTags % 600 == 0 || o1 || o2 || o3)
        Log("oflow tag #%llu frame %u (%.2f ms) -> depth %d mv %d const %d", (unsigned long long)st->oflowTags, id,
            frameTimeMs, (int)o1, (int)o2, (int)o3);
}

// --- Present ---

// ExecuteCommandLists of the D3D12 queues (vtable slot 10, shared by vkd3d's queues) is patched once, for two things.
// FFX_XEFG_CAP=auto with the optical flow: the GPU time of a frame runs from the game's first submission after a present
// (a time stamp put in front of it here) to our optical flow right before the next present; our own submissions pass
// through. The low latency: when XeSS-FG submitted first and last inside its Present (t_eclSpan, set by the present
// thread around that call): the time between the two is what the frame waited there (LatHold); and when the game
// submitted the first work of a frame (g_latFirstSubmit).
static std::atomic<bool> g_wantFrameStart{false};
static thread_local bool t_ownSubmit = false;

// The state is used under g_timedLock and takes no reference: a reference of the hook could be the last one, and its
// release would run the whole TearDown (XeSS-FG and XeLL destroyed, fence waits) inside the game's submission.
// TearDown clears g_timedSc under the same lock before it frees anything, so it waits at most for the time stamp
// below, which does not block (OflowFrameStart skips a busy slot). The state's lock is taken only if free: the present
// thread may hold it while XeSS-FG submits on this queue, and a missed time stamp costs one sample. (HookQueue takes
// g_timedLock with the state's lock held: no deadlock, as the hook only tries that lock.)
static void STDMETHODCALLTYPE HookEcl(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists)
{
    if (uint64_t* span = t_eclSpan) {
        const uint64_t now = Qpc();
        if (!span[0])
            span[0] = now;
        span[1] = now;
        span[2]++;
    } else if (!t_ownSubmit && !g_latFirstSubmit.load(std::memory_order_relaxed) &&
               q == g_latQueue.load(std::memory_order_relaxed)) {
        uint64_t none = 0;   // the game's first work of its next frame reaches the GPU's queue
        if (g_latFirstSubmit.compare_exchange_strong(none, Qpc())) {
            TR(tr::GpuMarkSignal(g_tracePre.load(), q, g_traceNext.load()));
        }
    }
#ifdef FFXFG_DEBUG
    struct TraceEcl {
        uint64_t t0;
        ID3D12CommandQueue* q;
        UINT n;
        uint64_t sub = 0;   // FFX_XEFG_TRACE_SUBS: the value signalled behind this submission, and its queue's index
        int subQ = 0;
        ~TraceEcl()
        {
            if (t0)
                tr::Put(tr::ECL, n,
                        (q == g_traceQueue.load() ? 1u : 0u) | (t_ownSubmit ? 2u : 0u) | ((uint64_t)subQ << 4) | (sub << 8),
                        (uint16_t)std::min(QpcMs(t0, Qpc()) * 1000.0f, 65535.0f), t0);
        }
    } traceEcl{tr::g_on ? Qpc() : 0, q, n};
#endif
    if (!t_ownSubmit && q == g_timedQueue.load() && g_wantFrameStart.exchange(false)) {
        std::lock_guard<std::mutex> tl(g_timedLock);
        ScState* st = g_timedSc.load();
        if (st) {
            std::unique_lock<std::mutex> lk(st->lock, std::try_to_lock);
            if (lk.owns_lock() && st->oflow) {
                t_ownSubmit = true;
                OflowFrameStart(st->oflow, q);
                t_ownSubmit = false;
            }
        }
    }
    g_origEcl(q, n, lists);
    TR(traceEcl.sub = tr::SubSignal(q, &traceEcl.subQ));
}

bool HookEclVt(ID3D12CommandQueue* queue)
{
    if (!g_origEcl) {
        void** vt = *(void***)queue;
        DWORD old;
        if (!PinSelf() || !VirtualProtect(&vt[10], sizeof(void*), PAGE_READWRITE, &old))   // a vtable is data
            return false;
        g_origEcl = (PFN_ExecuteCommandLists)vt[10];
        vt[10] = (void*)HookEcl;
        VirtualProtect(&vt[10], sizeof(void*), old, &old);
        Log("ExecuteCommandLists of the D3D12 queues hooked (through queue %p)", (void*)queue);
    }
    return true;
}

// FFX_XEFG_CAP=auto: the game's queue is timed
static void HookQueue(ScState* st)
{
    if (!HookEclVt(st->queue))
        return;
    std::lock_guard<std::mutex> lk(g_timedLock);
    g_timedSc = st;
    g_timedQueue = st->queue;
}

static void TimesReport(ScState* st)
{
    auto pct = [](std::vector<float> v, char* out, size_t n) {
        std::sort(v.begin(), v.end());
        size_t c = v.size();
        double sum = 0;
        for (float x : v)
            sum += x;
        snprintf(out, n, "mean %.2f p50 %.2f p95 %.2f p99 %.2f max %.2f", sum / c, v[c / 2], v[c * 95 / 100],
                 v[c * 99 / 100], v[c - 1]);
    };
    char a[128], b[128], c[128];
    pct(st->tFrame, a, sizeof a);
    pct(st->tPresent, b, sizeof b);
    pct(st->tSleep, c, sizeof c);
    Log("times over %zu frames (ms): frame %s | in Present %s | XeLL sleep %s", st->tFrame.size(), a, b, c);
    st->tFrame.clear(), st->tPresent.clear(), st->tSleep.clear();
}

// How the back buffer is encoded, for the optical flow (FFX_BACKBUFFER_TRANSFER_FUNCTION_*): the colour space the game
// set, else DXGI's default for the format (FP16 = linear scRGB, everything else = sRGB).
static int OflowTransfer(ScState* st, ID3D12Resource* bb)
{
    switch (st->colorSpace.load()) {
    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
        return 1;   // PQ (HDR10)
    case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
        return 2;   // scRGB
    case -1:
        break;
    default:
        return 0;
    }
    DXGI_FORMAT f = bb->GetDesc().Format;
    return f == DXGI_FORMAT_R16G16B16A16_FLOAT || f == DXGI_FORMAT_R16G16B16A16_TYPELESS ? 2 : 0;
}

uint32_t BeforePresent(ScState* st)
{
    std::lock_guard<std::mutex> lk(st->lock);
    if (st->bridge && st->oflow) {
        st->pending.clear();
        st->frameId = ++st->nextId;
        st->havePrepare = true;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        bool first = !st->oflowLastQpc;
        float ms = first ? 16.7f
                         : (float)((double)(now.QuadPart - st->oflowLastQpc) * 1000.0 / (double)g_qpf.QuadPart);
        st->oflowLastQpc = now.QuadPart;
        XellMarker(st, st->frameId, XELL_SIMULATION_END);
        XellMarker(st, st->frameId, XELL_RENDERSUBMIT_START);
        OflowTag(st, st->frameId, first || ms > 250.0f, ms);
    } else if (st->bridge) {
        st->havePrepare = !st->pending.empty();
        if (st->havePrepare) {
            st->frameId = st->pending.front();
            st->pending.pop_front();
        } else {
            st->frameId = ++st->nextId;
        }
    } else if (!st->havePrepare) {
        st->frameId++;  // no frame data this time (menus, loading): keep the ids moving, no interpolation
    }
    uint32_t id = st->frameId;
    Order(st->havePrepare ? "present" : "present (no prepare)", id);
    SetXefgEnabled(st, st->gameEnabled && st->havePrepare);

    if (st->oflow && st->havePrepare) {
        UINT bi = st->proxy->GetCurrentBackBufferIndex();
        ID3D12Resource* bb = nullptr;
        if (SUCCEEDED(st->proxy->GetBuffer(bi, IID_PPV_ARGS(&bb)))) {
            t_ownSubmit = true;
            OflowRun(st->oflow, st->queue, bb, D3D12_RESOURCE_STATE_PRESENT, OflowTransfer(st, bb), false);
            t_ownSubmit = false;
            bb->Release();
        }
    }
    if (st->havePrepare && st->ui.resource && !st->oflow) {
        xefg_swapchain_d3d12_resource_data_t rd = {};
        rd.type = XEFG_SWAPCHAIN_RES_UI;
        rd.validity = XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT;
        rd.resourceSize = {st->ui.description.width, st->ui.description.height};
        rd.pResource = (ID3D12Resource*)st->ui.resource;
        rd.incomingState = ToD3D12State(st->ui.state);
        p_xefgSwapChainD3D12TagFrameResource(st->xefg, nullptr, id, &rd);
    }

    // SIMULATION_START .. RENDERSUBMIT_START fill in when no prepare sent them (menus, loading)
    XellMarker(st, id, XELL_RENDERSUBMIT_END);
    XellMarker(st, id, XELL_PRESENT_START);
    p_xefgSwapChainSetPresentId(st->xefg, id);
    st->tStart = Qpc();   // "in Present" (FFX_XEFG_TIMES, the outside limiter test) without our own work above
    return id;
}

// `id` is what BeforePresent returned: a prepare for the next frame may have changed st->frameId during the present.
// `tEnd`: when the proxy's Present returned; `blockMs`: how long the frame was held up in front of it, `first`: when
// its first work was submitted (LatHold).
void AfterPresent(ScState* st, uint32_t id, HRESULT hr, UINT sync, UINT flags, uint64_t tEnd, float blockMs,
                         uint64_t first)
{
    uint32_t next;
    uint64_t presents;
    {
        std::lock_guard<std::mutex> lk(st->lock);
        XellMarker(st, id, XELL_PRESENT_END);
        presents = ++st->presents;
        // only the first presents: a later present sometimes stalled 560-580 ms around this query (Viewfinder with
        // OFLOW_FG at the display's limit while our cap fought that limit; not seen in Forza Horizon 6 at a frame limit),
        // and the status is only for the log
        if (st->presents <= 10) {
            xefg_swapchain_present_status_t ps = {};
            p_xefgSwapChainGetLastPresentStatus(st->xefg, &ps);
            Log("present #%llu frame %u sync %u flags 0x%x hr 0x%08lx: presented %u fgResult %d fgEnabled %u, %zu waiting",
                (unsigned long long)st->presents, id, sync, flags, (unsigned long)hr, ps.framesPresented, (int)ps.frameGenResult,
                ps.isFrameGenEnabled, st->pending.size());
        }
        if (st->frameId == id)   // else a newer prepare arrived during the present and is kept for the next one
            st->havePrepare = false;
        // the id the next prepare is expected to carry
        next = st->bridge ? st->nextId + 1 : id + 1;
    }
    // without the lock: the sleep can block
    // every 4th present: the window's average needs no more, and each read is a file read through Wine (a window
    // without any sample keeps the GPU times as they are, see CapAdjust)
    if (g_capAuto && st->oflow && presents % 4 == 0) {
        int f = GpuFreqMhz();
        if (f > 0) {
            std::lock_guard<std::mutex> lk(st->lock);
            st->capFreqSum += f;
            st->capFreqN++;
        }
    }
    if (g_capAuto && g_timedQueue.load() == st->queue)
        g_wantFrameStart = true;   // the game's next submission starts the next frame on the GPU (HookEcl)
    // The low latency: the game stays here until its next frame should start. Then the frame latency waitable object
    // is released (a game that waits on it starts there) and the frame starts for XeLL: its sleep (XeLL's own
    // interval, if one is set) and SIMULATION_START.
    const float held = LatHold(st, tEnd, blockMs, first, SUCCEEDED(hr));
    FrameDone(st, hr);
    uint64_t t0 = Qpc();
    XellStartFrame(st, next);
    if (g_capAuto) {
        uint64_t t1 = Qpc();
        std::lock_guard<std::mutex> lk(st->lock);
        float sl = QpcMs(t0, t1);
        if (st->oflow) {
            // the GPU time of each frame, measured on the game's queue (HookEcl .. OflowRun)
            if (g_timedQueue.load() != st->queue)
                HookQueue(st);
            float g[64];
            int n = OflowTakeGpuFrames(st->oflow, g, 64);
            st->capWork.insert(st->capWork.end(), g, g + n);
        } else if (st->capLastEnd) {
            st->capWork.push_back(QpcMs(st->capLastEnd, t1) - sl - held);   // no GPU timer: the frame without the sleep
        }
        if (st->capLastEnd) {
            st->capFrame.push_back(QpcMs(st->capLastEnd, t1));
            st->capSleep.push_back(sl);
            st->capPresent.push_back(QpcMs(st->tStart, tEnd));   // in Present: an outside limiter (gamescope) waits here
        }
        st->capLastEnd = t1;
        if (st->capWork.size() >= 30 && st->capFrame.size() >= 30)
            CapAdjust(st);
        else if (st->capFrame.size() >= 300)   // no GPU times arrive (no timer): nothing to adjust, keep it small
            st->capFrame.clear(), st->capSleep.clear(), st->capPresent.clear(), st->capWork.clear();
    }
    if (g_times) {
        uint64_t t1 = Qpc();
        std::lock_guard<std::mutex> lk(st->lock);
        if (st->tLastEnd) {
            st->tFrame.push_back(QpcMs(st->tLastEnd, tEnd));   // from present to present: what reaches the display
            st->tPresent.push_back(QpcMs(st->tStart, tEnd));
            st->tSleep.push_back(QpcMs(t0, t1));
        }
        st->tLastEnd = tEnd;
        if (st->tFrame.size() >= 300)
            TimesReport(st);
    }
    if (g_latQueue.load() == st->queue)
        g_latFirstSubmit = 0;
    st->frameStart = Qpc();   // the game's next frame starts when this returns
}

// ffx_xefg_lowlat.cpp - where the game's next frame starts in amd_fidelityfx_framegeneration_dx12.dll. When the proxy's
// Present has returned, AfterPresent (ffx_xefg_present.cpp) calls LatHold, the low latency (IGDEXT_XELL_CAP): the game
// is kept in Present until its next frame should start. Then it calls FrameDone, which releases our frame latency
// waitable object (with FFX_XEFG_PACE=1: when the GPU has finished the frame).
#include "ffx_xefg.h"

// ---------------------------------------------------------------------------------------------------------------
// Frame latency waitable object, paced by the GPU (FFX_XEFG_PACE=1)

static const DWORD kPaceWatchdogMs = 200;

// Releases the semaphore once per signalled value when the fence reaches it. The thread touches only the fence, the
// events, the semaphore and the three counters; never st->lock, so stopping it from TearDown cannot deadlock, and
// TearDown frees nothing before it has stopped.
static DWORD WINAPI PaceThread(void* p)
{
    auto* st = (ScState*)p;
    while (!st->paceStop.load()) {
        uint64_t target = st->fenceValue.load();
        if (st->paceReleased >= target) {
            WaitForSingleObject(st->paceEvent, INFINITE);   // a new signal or stop
            continue;
        }
        uint64_t want = st->paceReleased + 1;
        if (st->fence->GetCompletedValue() < want) {
            st->fence->SetEventOnCompletion(want, st->paceEvent);
            DWORD r = WaitForSingleObject(st->paceEvent, kPaceWatchdogMs);
            if (st->paceStop.load())
                break;
            if (r == WAIT_TIMEOUT && st->fence->GetCompletedValue() < want) {
                // watchdog: the frame is not done after 200 ms; the game must not wait on us for that
                LONG n = (LONG)(st->fenceValue.load() - st->paceReleased);
                ReleaseSemaphore(st->latencySem, n, nullptr);
                st->paceReleased += n;
                st->watchdogs++;
                if (st->watchdogs <= 5 || st->watchdogs % 100 == 0)
                    Log("pace watchdog #%llu: frame %llu not done after %lu ms, %ld released", (unsigned long long)st->watchdogs,
                        (unsigned long long)want, kPaceWatchdogMs, n);
            }
            continue;   // done, or woken by a newer signal: look again
        }
        ReleaseSemaphore(st->latencySem, 1, nullptr);
        st->paceReleased++;
    }
    return 0;
}

// With the lock held, after the semaphore exists. Without fence or thread the semaphore is released at Present.
void PaceStart(ScState* st)
{
    if (!g_pace || st->fence || !st->queue)
        return;
    ID3D12Fence* fence = nullptr;
    if (FAILED(st->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        Log("pace: no fence, the waitable object is released at Present");
        return;
    }
    st->paceEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    st->paceThread = st->paceEvent ? CreateThread(nullptr, 0, PaceThread, st, CREATE_SUSPENDED, nullptr) : nullptr;
    if (!st->paceThread) {
        Log("pace: no thread (%lu), the waitable object is released at Present", GetLastError());
        if (st->paceEvent)
            CloseHandle(st->paceEvent);
        st->paceEvent = nullptr;
        fence->Release();
        return;
    }
    st->fence = fence;   // last: FrameDone on the present thread reads it without the lock
    SetThreadPriority(st->paceThread, THREAD_PRIORITY_HIGHEST);
    ResumeThread(st->paceThread);
    Log("pace: the waitable object follows the GPU (fence %p, watchdog %lu ms)", (void*)fence, kPaceWatchdogMs);
}

// After a Present that returned: the frame's work, including what XeSS-FG queued on the game's queue inside Present,
// precedes the signal on that queue (a command queue is free-threaded, so the present thread may signal it).
void FrameDone(ScState* st, HRESULT hr)
{
    if (!st->latencySem || FAILED(hr))
        return;
    ID3D12Fence* fence = st->fence;
    if (!fence) {
        ReleaseSemaphore(st->latencySem, 1, nullptr);
        return;
    }
    // one value per present, each released once by the thread; the signal is queued before the value is published,
    // so the thread never waits for a value that will not come
    std::lock_guard<std::mutex> lk(st->paceLock);
    uint64_t v = st->fenceValue.load() + 1;
    if (SUCCEEDED(st->queue->Signal(fence, v))) {
        st->fenceValue.store(v);
        SetEvent(st->paceEvent);
    } else {
        ReleaseSemaphore(st->latencySem, 1, nullptr);
    }
}

// Stops the thread and frees the fence; false if the thread did not stop (then nothing of st may be freed). Called
// from TearDown only, with no lock held and no present in flight (a present holds a reference to st).
bool PaceStop(ScState* st)
{
    if (!st->paceThread) {
        if (st->fence)
            st->fence->Release();
        st->fence = nullptr;
        if (st->paceEvent)
            CloseHandle(st->paceEvent);
        st->paceEvent = nullptr;
        return true;
    }
    st->paceStop.store(true);
    SetEvent(st->paceEvent);
    DWORD r = WaitForSingleObject(st->paceThread, 2000);   // its waits are at most the watchdog long
    if (r != WAIT_OBJECT_0)
        return false;
    CloseHandle(st->paceThread);
    st->paceThread = nullptr;
    // a completion the thread armed may still be pending on the event: let the last value land before closing it
    uint64_t last = st->fenceValue.load();
    if (st->fence->GetCompletedValue() < last) {
        st->fence->SetEventOnCompletion(last, st->paceEvent);
        WaitForSingleObject(st->paceEvent, 500);
    }
    Log("pace: thread stopped, %llu frames released, %llu by the watchdog", (unsigned long long)st->paceReleased,
        (unsigned long long)st->watchdogs);
    st->fence->Release();
    st->fence = nullptr;
    CloseHandle(st->paceEvent);
    st->paceEvent = nullptr;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Low latency (IGDEXT_XELL_CAP, auto by default)
//
// What a frame waits for on this route, from the time lines of CONTROL Resonant, Resident Evil Requiem and Forza
// Horizon 6 (FFX_XEFG_TRACE in a debug build, XeSS-FG 1.3.1 at 3x under vkd3d-proton and DXVK's DXGI, 2026-11-11):
//  - XeSS-FG's proxy Present puts the interpolation on the game's queue, waits until its presentation thread is
//    through with the frame before, presents the first of the frame's images itself and returns; the presentation
//    thread presents the others. Those Presents of the real swap chain block (two may be under way): the last one of
//    a frame returns when the GPU has finished the frame's first image. A game that reaches Present before that
//    waits in it, with its frame, and the input it read for it, already made: the queue the low latency is after.
//  - DXVK's swap chain holds its locks for the whole of a Present, and all three games ask the swap chain something
//    right after their Present that takes one of them: IDXGISwapChain::GetFrameStatistics (CONTROL Resonant, Forza
//    Horizon 6), IDXGISwapChain3::CheckColorSpaceSupport (Resident Evil Requiem). If the presentation thread is
//    inside its last Present by then, the game's next frame starts only when the GPU has finished this one: no
//    queue, but the GPU idles while the game works (48 instead of 31.5 ms a frame in Resident Evil Requiem, 45
//    instead of 25 in Forza Horizon 6). Otherwise it starts at once and waits in its next Present (10 to 14 ms).
//    Which of the two is decided by some 100 us, and the games change between them every few seconds.
// So two things are done:
//  1. Those questions no longer wait for the presentation thread (IGDEXT_XELL_POLLS): the game gets the answers as of
//     its last Present call. They are taken right before the proxy's Present, where waiting for that thread costs
//     nothing: the proxy would wait for it anyway. The game's frame then starts when its Present returns, and
//     nowhere else. (Any other call of the game that waits in the swap chain is named in the log, FgSwapChain::Waited.)
//  2. The game is kept in Present, after the proxy's Present has returned, until its next frame should start: so
//     that the frame reaches its Present a little before XeSS-FG can take it (LatHold). How long a frame was held up
//     in front of its present is measured in that Present: the wait for those answers plus the time between
//     XeSS-FG's first and last submission inside its Present (HookEcl; without a wait they are about 0.2 ms apart).
//     The wait before the next frame grows while frames are held up by more than kLatTargetMs
//     (IGDEXT_XELL_CAP=auto:<ms> adds to that) and shrinks while they are not, so the frame time stays what the GPU
//     or the display allows, and a game that is never held up (its CPU is the limit) is not kept at all. The frame
//     latency waitable object, XeLL's sleep and its SIMULATION_START marker follow the wait: the XMX shim's log
//     (IGDEXT_XELL_LOG) shows the time from there to the end of the frame's present, as for a game with its own
//     XeSS-FG.
//     That alone is right while the GPU is what the frames wait for and the game submits a frame's work as it goes
//     (CONTROL Resonant). Two cases need the GPU's side as well (LatGpu: a fence on the game's queue after each proxy
//     Present tells when the GPU finished a frame, HookEcl when the game submitted the first work of the next one).
//     A game that submits a frame's work at its Present only (Resident Evil Requiem) left the GPU idle for about
//     1 ms a frame when it reached Present 2.5 ms before XeSS-FG could take it: XeSS-FG can take a frame a little
//     after the GPU is through with the one before (1.7 % of the frame rate). And when the display is what frames
//     wait for (a menu at the display's frame rate, Forza Horizon 6 at 120 frames shown per second), XeSS-FG's
//     Present still makes a frame wait, for its turn, but the GPU must have finished the frame by then, so the
//     frame must not start later (CONTROL Resonant's menu lost 1 to 4 % of its frame rate that way). So a frame
//     counts as early only as far as its first work also reached the GPU's queue more than kLatAheadMs before the
//     GPU got free: where the GPU idles before the work arrives, the game is not kept at all.
//  3. The wait is checked (IGDEXT_XELL_CHECK, common/lowlat_check.h: the same check as the XMX shim's for a game's
//     own XeSS-FG). The two times of 2. do not show everything a frame can wait for: a game's own render work may
//     wait for its compute queue in the middle of a frame (Resident Evil Requiem, about 4 ms), XeSS-FG's later
//     images for the display, and a frame that waits in front of Present can still be the one the GPU waits for
//     next. So the wait has to show that it costs no frame rate and lowers the latency: now and then it is left out
//     for 100 frames, between two windows with it, and it stays only if the frame time with it is within 1 % and the
//     latency (frame start -> end of its present) lower by at least half the wait. A wait that costs in two checks
//     in a row is shortened and checked again; one that does not help is off for a while.
//     (Measured 2026-11-11 against the release build, separate runs: in the three games the wait costs 0 to 0.2 ms
//     a frame. What a wait that is too long costs, IGDEXT_XELL_CAP=auto:-20 in Resident Evil Requiem: 1.1 to 1.8 ms.)
// IGDEXT_XELL_CAP=off: no wait of ours and the swap chain's own answers, all as before. IGDEXT_XELL_CAP=<us>
// or FFX_XEFG_CAP: XeLL's interval holds the frame back in its sleep, at the same place, instead of a wait of ours.

// A frame should reach Present this long before XeSS-FG can take it, and its first work the GPU's queue kLatAheadMs
// before the GPU gets free. The time a game needs from its start to its Present varies by 1 to 1.5 ms (standard
// deviation) in the three games, and a frame that comes late leaves the GPU idle for that long.
static const double kLatTargetMs = 2.5;
static const double kLatAheadMs = 1.5;

typedef LONG(NTAPI* PFN_NtDelayExecution)(BOOLEAN alertable, PLARGE_INTEGER interval);   // ntdll, 100 ns, negative = relative

// Waits until `due` (QPC): NtDelayExecution in 100 ns units, stopping short by the late wake-up seen recently (a maximum
// that decays), the rest yielded away
static void WaitUntil(uint64_t due, double& lateMs)
{
    static PFN_NtDelayExecution delay =
        (PFN_NtDelayExecution)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtDelayExecution");
    for (;;) {
        const uint64_t now = Qpc();
        if (now >= due)
            return;
        const double left = QpcMs(now, due);
        const double slack = std::min(2.0, std::max(0.2, lateMs + 0.2));
        if (left > slack) {
            const double ask = left - slack;
            if (delay) {
                LARGE_INTEGER rel;
                rel.QuadPart = -(LONGLONG)(ask * 10000.0);
                delay(FALSE, &rel);
            } else {
                Sleep((DWORD)ask);
            }
            lateMs = std::max((double)QpcMs(now, Qpc()) - ask, lateMs * 0.98);
        } else {
            SwitchToThread();
        }
    }
}

// Notes when the GPU reaches each value signalled on the fence. It touches the fence, the event and the atomics only,
// never st->lock: TearDown stops it before anything is freed.
static DWORD WINAPI LatGpuThread(void* p)
{
    ScState::LatGpu& g = ((ScState*)p)->latGpu;
    TR(tr::t_own = true);
    uint64_t done = 0;
    while (!g.stop.load()) {
        const uint64_t last = g.signalled.load();
        if (done >= last) {
            WaitForSingleObject(g.event, 200);   // a new signal or stop
            continue;
        }
        if (g.fence->GetCompletedValue() <= done) {
            g.fence->SetEventOnCompletion(done + 1, g.event);
            WaitForSingleObject(g.event, 200);
            if (g.stop.load())
                break;
        }
        const uint64_t reached = g.fence->GetCompletedValue();
        if (reached == UINT64_MAX)   // the device is gone
            break;
        const uint64_t now = Qpc();
        for (uint64_t v = done + 1; v <= reached && v <= last; v++)
            g.doneAt[v % 16].store(now);
        done = std::max(done, std::min(reached, last));
    }
    return 0;
}

void LatGpuStart(ScState* st)
{
    ScState::LatGpu& g = st->latGpu;
    if (g.thread || FAILED(st->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence))))
        return;
    for (auto& d : g.doneAt)
        d.store(0);
    g.event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    g.thread = g.event ? CreateThread(nullptr, 0, LatGpuThread, st, 0, nullptr) : nullptr;
    if (!g.thread) {
        Log("low latency: no thread for the GPU's side (%lu): the frames' wait in Present alone decides", GetLastError());
        if (g.event)
            CloseHandle(g.event);
        g.event = nullptr;
        g.fence->Release();
        g.fence = nullptr;
    }
}

// false if the thread did not stop (then nothing of st may be freed); from TearDown only
bool LatGpuStop(ScState* st)
{
    ScState::LatGpu& g = st->latGpu;
    if (g_latQueue.load() == st->queue)
        g_latQueue = nullptr;
    if (!g.thread)
        return true;
    g.stop.store(true);
    SetEvent(g.event);
    if (WaitForSingleObject(g.thread, 2000) != WAIT_OBJECT_0)
        return false;
    CloseHandle(g.thread);
    g.thread = nullptr;
    // a completion the thread asked for may still be pending on the event: let the last value land before closing it
    const uint64_t last = g.signalled.load();
    if (g.fence->GetCompletedValue() < last) {
        g.fence->SetEventOnCompletion(last, g.event);
        WaitForSingleObject(g.event, 500);
    }
    g.fence->Release();
    g.fence = nullptr;
    CloseHandle(g.event);
    g.event = nullptr;
    return true;
}

// right after the proxy's Present returned: the GPU has reached this point when it is through with the frame
void LatGpuSignal(ScState* st)
{
    ScState::LatGpu& g = st->latGpu;
    if (!g.thread)
        return;
    const uint64_t v = g.signalled.load() + 1;
    g.doneAt[v % 16].store(0);
    TR(const bool own = tr::t_own; tr::t_own = true);
    if (SUCCEEDED(st->queue->Signal(g.fence, v))) {
        g.signalled.store(v);
        SetEvent(g.event);
    }
    TR(tr::t_own = own);
}

// On the present thread, after the proxy's Present returned at `tEnd` (`ok`: it succeeded): keeps the game there
// until its next frame should start, and returns that wait in ms. `blockMs`: how long this frame was held up in
// front of its present; `first`: when the game submitted its first work (0 = not seen: its Present call counts).
float LatHold(ScState* st, uint64_t tEnd, float blockMs, uint64_t first, bool ok)
{
    ScState::LatCtl& c = st->lat;
    const float dt = c.lastEnd ? QpcMs(c.lastEnd, tEnd) : 0;
    c.lastEnd = tEnd;
    const bool steady = dt > 0 && dt < 250;   // not the first frame, a loading screen or a pause
    bool on = g_lat == 1 && !g_ffxCap && ok && st->xefgEnabled;
    if (on && g_abSec > 0) {
        if (!c.abStart)
            c.abStart = tEnd;
        const uint64_t turn = (uint64_t)(QpcMs(c.abStart, tEnd) / 1000.0f) / (uint64_t)g_abSec;
        c.abTurn = g_abOld ? (int)(turn % 3) : 1 + (int)(turn % 2);
        on = c.abTurn == 2;
    }
    const int state = on ? 2 : g_pollCache && c.abTurn != 0 ? 1 : 0;
    if (on != c.on || state != c.abSaid) {
        c.on = on;
        c.abSaid = state;
        SYSTEMTIME now;   // the time of day: the XMX shim's log (IGDEXT_XELL_LOG) has it
        GetLocalTime(&now);
        Log("low latency: %s (frame generation %s) at %02d:%02d:%02d.%03d",
            state == 2 ? "frames are held back in Present" : state == 1 ? "no wait" : "no wait, the swap chain answers the game",
            st->xefgEnabled ? "on" : "off", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
        TR(tr::Put(tr::NOTE, 1, state));
    }
    // the figure for the log: the frame's start (when the game got back from Present, or from a call into the swap
    // chain that waited) -> the end of its present
    const uint64_t start = st->frameStart.load();
    if (steady && start && tEnd > start && st->xefgEnabled) {
        c.logLat += QpcMs(start, tEnd);
        c.logBlock += blockMs;
        c.logFrame += dt;
        c.logLatN++;
    }
    // The GPU's side: this frame's value was signalled just now; the frame before is through on the GPU by now, or
    // the GPU is still busy with it (then our work was there long before it gets free).
    ScState::LatGpu& gp = st->latGpu;
    double ahead = 1e9;   // this frame's first work reached the GPU's queue this long before the GPU got free (ms)
    if (gp.thread && steady && st->xefgEnabled) {
        const uint64_t v = gp.signalled.load();
        const uint64_t donePrev = v >= 2 ? gp.doneAt[(v - 1) % 16].load() : 0;
        const uint64_t donePrev2 = v >= 3 ? gp.doneAt[(v - 2) % 16].load() : 0;
        if (!first)
            first = st->tStart;   // the Present call
        ahead = !donePrev ? 30.0 : donePrev >= first ? QpcMs(first, donePrev) : -(double)QpcMs(donePrev, first);
        ahead = std::max(-30.0, std::min(ahead, 30.0));
        c.aheadMs = c.gpuN ? c.aheadMs + 0.15 * (ahead - c.aheadMs) : ahead;
        if (donePrev && c.prevFirst) {
            // the frame before: the GPU's time for it, from its first work or from when the GPU got free for it
            const uint64_t from = std::max(c.prevFirst, donePrev2);
            const float gpu = donePrev > from ? QpcMs(from, donePrev) : 0;
            if (gpu > 0 && gpu < 200)
                c.gpuMs = c.gpuMs > 0 ? c.gpuMs + 0.1 * (gpu - c.gpuMs) : gpu;
            if (c.prevStart && donePrev > c.prevStart && QpcMs(c.prevStart, donePrev) < 500) {
                c.logGpuLat += QpcMs(c.prevStart, donePrev);   // the figure: its start -> the GPU has finished it
                c.logGpuN++;
            }
        }
        if (++c.gpuN < 8)
            ahead = -1e9;   // nothing known yet: no wait
        c.prevFirst = first;
        c.prevStart = start;
    } else {
        c.gpuN = 0;
        c.prevFirst = c.prevStart = 0;
    }
    float held = 0;
    if (!on || !steady) {
        c.hold = 0;
        c.prevEarly = 0;
        c.unheld = 0;
        c.frameMs = steady ? dt : 0;
        // the check: all over when frame generation went off; after a pause or a hitch only one under way
        c.chk.Reset(!on);
        c.chkAllowed = true;
        c.holdKept = 0;
    } else {
        c.frameMs = c.frameMs > 0 ? c.frameMs + (dt - c.frameMs) * 0.1 : dt;
        // How much later this frame could have started: by what it was held up in front of its present over
        // kLatTargetMs, and no more than its first work was at the GPU's queue earlier than kLatAheadMs before the
        // GPU got free (where the GPU is watched). A wait that the check found to cost is shorter by chk.shortMs.
        const double early = std::min(blockMs - std::max(0.3, kLatTargetMs + g_latMarginMs + c.chk.shortMs),
                                      ahead - (kLatAheadMs + g_latMarginMs + c.chk.shortMs));
        // The check: what the two times above say is not all a frame can wait for (see "Low latency"), so the wait
        // has to show that it costs no frame time and lowers the latency. FFX_XEFG_AB and FFX_XEFG_HOLD measure
        // without it.
        bool wait = true;
        if (g_latCheck && !g_abSec && g_holdMs <= 0) {
            const double lat = start && tEnd > start ? QpcMs(start, tEnd) : 0;
            wait = c.chk.Frame((double)tEnd / (double)g_qpf.QuadPart, dt, c.lastHeld, lat, !gp.thread || c.gpuN >= 8, Log);
            if (wait != c.chkAllowed) {
                c.chkAllowed = wait;
                if (!wait) {
                    c.holdKept = c.hold;   // left out for some frames: it comes back as it was
                } else {
                    c.hold = c.holdKept;
                    c.prevEarly = 0;       // the frames without it say nothing about it
                    c.unheld = 0;
                }
            }
        }
        if (!wait) {
            c.hold = 0;
            c.prevEarly = 0;
            c.unheld = 0;
        } else if (early > 0) {
            // the next frame starts later, by a part of what both this frame and the one before were early by (one
            // frame alone may be a hitch), 2 ms a frame at most
            const double both = std::min(early, c.prevEarly);
            if (both > 0)
                c.hold += std::min(0.25 * both, 2.0);
            c.unheld = 0;
        } else {
            // late, or just in time: earlier, the faster the longer it lasts
            c.unheld++;
            c.hold -= std::min(0.15 * -early * (1.0 + 0.5 * (c.unheld - 1)), 2.0);
        }
        c.prevEarly = early;
        if (g_holdMs > 0)   // FFX_XEFG_HOLD
            c.hold = g_holdMs;
        c.hold = std::max(0.0, std::min(c.hold, std::min(0.8 * c.frameMs, 100.0)));
        if (c.hold >= 0.05) {
            WaitUntil(tEnd + (uint64_t)(c.hold * (double)g_qpf.QuadPart / 1000.0), c.lateMs);
            held = QpcMs(tEnd, Qpc());
            c.logWaited++;
            c.logWait += held;
        }
    }
    c.lastHeld = held;
    if (++c.logN >= 300) {
        if (c.logLatN && (g_lat == 1 || g_times)) {
            char gpu[160] = "";
            if (c.logGpuN)
                snprintf(gpu, sizeof gpu, ", -> GPU done %.1f ms; GPU: %.1f ms a frame, the frame's first work %.1f ms %s",
                         c.logGpuLat / c.logGpuN, c.gpuMs, std::abs(c.aheadMs),
                         c.aheadMs >= 0 ? "before it gets free" : "after it got free");
            Log("low latency: waited in %d of %d frames, %.1f ms each; frames held up in front of Present %.1f ms, frame "
                "%.2f ms; start -> present end %.1f ms%s (%d frames)",
                c.logWaited, c.logN, c.logWaited ? c.logWait / c.logWaited : 0.0, c.logBlock / c.logLatN,
                c.logFrame / c.logLatN, c.logLat / c.logLatN, gpu, c.logLatN);
        }
        c.logN = c.logWaited = c.logLatN = c.logGpuN = 0;
        c.logWait = c.logLat = c.logBlock = c.logFrame = c.logGpuLat = 0;
    }
    return held;
}

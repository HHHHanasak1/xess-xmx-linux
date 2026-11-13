// XellHook.cpp - in-process fix for the frame cap Wuthering Waves hands to Intel XeLL (libxell.dll) when frame generation is enabled.
// XeLL's exports are 5-byte "jmp rel32" thunks; xellSetSleepMode / xellSetFgEnabled / xellSleep are redirected to the functions below.
// The game applies a 33.3 ms frame cap (30 fps, counted per displayed frame) right after it enables frame generation and only
// replaces it when the pause menu is opened and closed. A nonzero interval arriving within 1.5 s of xellSetFgEnabled(1) is replaced by
// the interval that was in effect before.
// The fix is on for Wuthering Waves only (process path contains "Wuthering Waves"), a cap set at that moment in any other game
// is left alone.
//   IGDEXT_XELL_FIXCAP=1    apply the fix in any game;  IGDEXT_XELL_KEEPCAP=1   turn it off everywhere
// Low latency, on by default in every game with XeSS frame generation: each frame starts its CPU time before the GPU is free,
// so no frame waits in a queue (AutoPaceWait). It does nothing while frame generation is off. As on the FSR routes
// (ffxfg/ffx_xefg*.cpp, "Low latency"), the wait is checked against the frame time and the latency
// (common/lowlat_check.h), and a game's questions to XeSS-FG's swap chain are answered as of its last Present
// (XefgAnswers.cpp).
//   IGDEXT_XELL_CAP=off     no low latency;  IGDEXT_XELL_CAP=auto:<ms>  start each frame <ms> earlier (later if negative)
//   IGDEXT_XELL_CAP=<us>    instead: at least this interval per displayed frame (XeLL's frame cap, in both of its modes),
//                           up to 1000000; any other value turns both off
//   IGDEXT_XELL_CHECK=0     the low latency's wait is not checked
//   IGDEXT_XELL_POLLS=0     the swap chain answers the game itself
//   IGDEXT_XELL_LOG=1       append the parameters the game passes and xellSleep timing (real frame rate) to C:\igdext_xell.log;
//                           with it, XInputGetState is watched too, for a second latency figure: from where the game
//                           reads its input to the end of the frame's present (InputWatch below)
// The package's own frame generation DLL (ffxfg/ffx_xefg*.cpp, the FSR routes) sets the game's frame start itself, by
// keeping the game in Present ("Low latency" there), and registers its XeLL context with IgdextXellExternalPacing:
// xellSleep of such a context gets no AutoPaceWait.
// The hooks on libxess_fg.dll are in XefgHook.cpp; XellHook.h says what else is where.
#include "Stdafx.h"
#include "Trace.h"
#include "XellHook.h"
#include "../../../common/lowlat_check.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>

#pragma pack(push, 1)
struct XellSleepParams { uint32_t minimumIntervalUs; uint32_t flags; }; // flags bit0 = bLowLatencyMode, bit1 = bLowLatencyBoost
#pragma pack(pop)
typedef uint32_t (*PFN_XellSet)(void*, const XellSleepParams*);
typedef uint32_t (*PFN_XellSleep)(void*, uint32_t);
typedef uint32_t (*PFN_XellFg)(void*, uint32_t, uint32_t);

static PFN_XellSet g_origSet; static PFN_XellSleep g_origSleep; static PFN_XellFg g_origFg;
static uint32_t g_capUs = 0;
static bool g_capBad = false; static double g_autoMarginMs = 0;
static lowlat::Check g_chk;   // the check of the wait (IGDEXT_XELL_CHECK=0: g_chk.on = false)
static uint32_t g_mfg = 0;   // generated frames per rendered frame, 0 = frame generation off
static bool g_fixCap = false; static uint32_t g_effInterval = 0, g_savedInterval = 0; static ULONGLONG g_fgOnTick = 0;
static volatile LONG g_sleepCalls = 0;
typedef uint32_t (*PFN_XellSetLog)(void*, uint32_t, void (*)(const char*, uint32_t));
static PFN_XellSetLog g_setLog;   // xellSetLoggingCallback: XeLL's own messages go to the log with IGDEXT_XELL_LOG
// xellGetFramesReports: per-frame time stamps (ns) XeLL collects; with IGDEXT_XELL_LOG the average time from the start
// of a frame's simulation to the end of its present (the latency XeLL itself works on) is logged every 100 frames
#pragma pack(push, 8)
struct XellFrameReport { uint32_t id; uint64_t simS, simE, rsS, rsE, prS, prE, r1, r2, r3, r4, r5; };
#pragma pack(pop)
typedef uint32_t (*PFN_XellReports)(void*, XellFrameReport*);
typedef LONG (NTAPI* PFN_NtDelayExecution)(BOOLEAN alertable, PLARGE_INTEGER interval);   // ntdll; negative = relative, 100 ns
static PFN_XellReports g_reports;

static void XellLogCb(const char* msg, uint32_t level)
{
    if (msg) XLog("XeLL[%u] %s", level, msg);
}

// IGDEXT_XELL_LOG: where the game reads its input. XInputGetState of Wine's xinput DLLs is hooked (they start with an
// 8-byte hot-patch no-op: a jump to the hook goes there, the original runs from behind it) and the time of each call is
// kept. The latency line then has a second figure next to simulation start -> present end: from the frame's input read
// (the first XInputGetState from 1 ms before its simulation start on) to the end of its present, and how long after the
// simulation start that read came. It is the check that such a read marks the start of a frame, for the routes where a
// game sends XeLL no markers (the FSR routes time their frames from where Present returns to the game, see
// ffxfg/ffx_xefg*.cpp).
namespace
{
struct InputRead { uint64_t ns; uint32_t tid; };
InputRead g_inputReads[1024];
volatile LONG g_inputN = 0;
void* g_inputOrig[6];

uint64_t NowNs()
{
    LARGE_INTEGER now, qf; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&qf);
    return (uint64_t) (now.QuadPart / qf.QuadPart) * 1000000000ull + (uint64_t) (now.QuadPart % qf.QuadPart) * 1000000000ull / (uint64_t) qf.QuadPart;
}

template <int I> DWORD WINAPI Hook_XInputGetState(DWORD index, void* state)
{
    const LONG n = InterlockedIncrement(&g_inputN) - 1;
    InputRead& r = g_inputReads[n & 1023];
    r.tid = GetCurrentThreadId(); r.ns = NowNs();
    return ((DWORD (WINAPI*)(DWORD, void*)) g_inputOrig[I])(index, state);
}

const struct { const wchar_t* dll; const char* name; void* hook; } kInputHooks[6] = {
    { L"xinput1_4.dll", "XInputGetState", (void*) Hook_XInputGetState<0> }, { L"xinput1_3.dll", "XInputGetState", (void*) Hook_XInputGetState<1> },
    { L"xinput9_1_0.dll", "XInputGetState", (void*) Hook_XInputGetState<2> }, { L"xinputuap.dll", "XInputGetState", (void*) Hook_XInputGetState<3> },
    { L"xinput1_4.dll", "XInputGetStateEx", (void*) Hook_XInputGetState<4> }, { L"xinput1_3.dll", "XInputGetStateEx", (void*) Hook_XInputGetState<5> },
};
bool g_inputDone[6];
}

// called now and then from the log path: a game loads its input library late
static void InputWatch()
{
    for (int i = 0; i < 6; ++i)
    {
        if (g_inputDone[i]) continue;
        HMODULE m = GetModuleHandleW(kInputHooks[i].dll);
        if (!m) continue;
        g_inputDone[i] = true;
        uint8_t* p = (uint8_t*) GetProcAddress(m, kInputHooks[i].name);
        static const uint8_t kHot[8] = { 0x48, 0x8d, 0xa4, 0x24, 0, 0, 0, 0 };   // lea rsp,[rsp+0]
        const bool hot = p && memcmp(p, kHot, 8) == 0;
        // somebody's jump of the same kind is there already (the frame generation DLL's trace, ffxfg/ffx_trace.h): ours
        // goes in front of it
        const bool taken = p && p[0] == 0xE9 && p[5] == 0x0F && p[6] == 0x1F && p[7] == 0x00;
        const intptr_t d = p ? (uint8_t*) kInputHooks[i].hook - (p + 5) : 0;
        DWORD old;
        if (!(hot || taken) || d > INT32_MAX || d < INT32_MIN || !SelfModule(GET_MODULE_HANDLE_EX_FLAG_PIN) ||
            !VirtualProtect(p, 8, PAGE_EXECUTE_READWRITE, &old))
        { XLog("input: %ls!%s not watched (not there, or no hot-patch start)", kInputHooks[i].dll, kInputHooks[i].name); continue; }
        g_inputOrig[i] = hot ? p + 8 : p + 5 + *(int32_t*) (p + 1);
        // jmp rel32 and a 3-byte no-op in the place of the 8-byte one, in one write
        InterlockedExchange64((volatile LONG64*) p, (LONG64) (0xE9ull | ((uint64_t) (uint32_t) (int32_t) d << 8) | (0x001F0Full << 40)));
        VirtualProtect(p, 8, old, &old);
        FlushInstructionCache(GetCurrentProcess(), p, 8);
        XLog("input: %ls!%s watched", kInputHooks[i].dll, kInputHooks[i].name);
    }
}

// the input read of the frame whose simulation started at simS and whose present ended at prE: the first one from
// 1 ms before the start on, by the thread `tid` (0 = any)
static uint64_t InputReadOf(uint64_t simS, uint64_t prE, uint32_t tid)
{
    uint64_t best = 0;
    const LONG n = g_inputN;
    for (LONG k = 0; k < 1024 && k < n; ++k)
    {
        const InputRead& r = g_inputReads[k];
        if (r.ns + 1000000ull < simS || r.ns >= prE || (tid && r.tid != tid)) continue;
        if (!best || r.ns < best) best = r.ns;
    }
    return best;
}

static uint32_t Hook_SetFg(void* ctx, uint32_t a, uint32_t b)
{
    static int n = 0;
    if (n++ < 12 || (n % 600) == 0) XLog("xellSetFgEnabled ctx=%p a=%u b=%u", ctx, a, b);
    if (a == 1) { g_savedInterval = g_effInterval; g_fgOnTick = GetTickCount64(); }
    g_mfg = a == 1 ? b : 0;
    g_fgSource = "xellSetFgEnabled"; InterlockedExchange(&g_fgState, g_mfg != 0);
    return g_origFg(ctx, a, b);
}

static uint32_t Hook_SetSleepMode(void* ctx, const XellSleepParams* p)
{
    if (!p) return g_origSet(ctx, p);
    XellSleepParams q = *p;
    XLog("xellSetSleepMode ctx=%p game: minimumIntervalUs=%u lowLatency=%u boost=%u", ctx, p->minimumIntervalUs, p->flags & 1, (p->flags >> 1) & 1);
    if (g_fixCap && q.minimumIntervalUs != 0 && g_fgOnTick && GetTickCount64() - g_fgOnTick < 1500)
    { XLog("   -> frame cap %u us set right after frame generation was enabled: keeping %u us", q.minimumIntervalUs, g_savedInterval); q.minimumIntervalUs = g_savedInterval; }
    g_effInterval = q.minimumIntervalUs;
    if (q.minimumIntervalUs < g_capUs)
    { XLog("   -> IGDEXT_XELL_CAP: %u us instead of %u us", g_capUs, q.minimumIntervalUs); q.minimumIntervalUs = g_capUs; }
    return g_origSet(ctx, &q);
}

// IGDEXT_XELL_CAP=auto: a frame should reach the GPU when the GPU gets free, not wait there. The GPU is free at the end of
// the newest finished frame + the frames in flight x a frame's time; the frame's simulation starts its CPU time before
// that. The CPU time is the time without a queue (a queue adds its wait to it): the lowest simulation -> render submit of
// the 64 frames XeLL reports, after a start with kStartCpuMs that drains the queue. The frame time is the interval of
// those reports, never above the one learned without a wait: under the wait the interval follows the wait, and following
// it upwards would drift. It goes up only when a queue shows that the GPU got slower, and follows a lighter scene down.
// The wait counts from the newest real completion, so a heavier scene moves every start with it.
// The wait is checked (g_chk, common/lowlat_check.h): now and then it is left out for some frames, and it stays only if
// the frame time is no longer with it and the latency (XeLL's simulation start -> present end) is lower. A wait that
// cost is shortened: every frame starts g_chk.shortMs earlier. After frames without the wait, the CPU time learned
// before them is used until the reports are of frames with the wait again.
static void AutoPaceWait(void* ctx, uint32_t frame)
{
    static XellFrameReport r[64];
    static double gHist[3]; static int gN = 0, since = 0; static double uncappedMs = 0;
    static double cpuBase = 1e9, cpuKept = 0; static int queued = 0, waitRun = 0;
    static double sumSleep = 0, sumSpin = 0, sumFrame = 0, sumLat = 0; static int nCalls = 0, slept = 0, nFrame = 0, nLat = 0;
    static double lastLeft = 0, lastG = 0, lastCpu = 0, lastWaitMs = 0; static uint32_t lastQ = 0, latTop = 0;
    static uint64_t lastNs = 0;
    const double kStartCpuMs = 10, kQueueMs = 4, kCpuFloorMs = 0.5; const int kStartFrames = 90, kQueueFrames = 30;
    // XeSS-FG's own switch first, XeLL's otherwise; neither seen = off
    const int fgOn = g_fgState == 1;
    static int fgSaid = -1;
    if (fgOn != fgSaid)
    {
        fgSaid = fgOn;
        XLog("low latency: %s (frame generation %s, %s)", fgOn ? "frames are held back in xellSleep" : "no wait", fgOn ? "on" : "off",
             (const char*) g_fgSource);
    }
    LARGE_INTEGER now, qf; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&qf);
    const uint64_t nowNs = (uint64_t) (now.QuadPart / qf.QuadPart) * 1000000000ull + (uint64_t) (now.QuadPart % qf.QuadPart) * 1000000000ull / (uint64_t) qf.QuadPart;
    const double dtMs = lastNs ? (double) (nowNs - lastNs) / 1e6 : 0;   // this frame: from the xellSleep before to this one
    const double waitedMs = lastWaitMs;                                  // ... and the wait that was in it
    lastNs = nowNs; lastWaitMs = 0;
    const auto reset = [&](bool all) { uncappedMs = 0; gN = 0; since = 0; waitRun = 0; cpuBase = 1e9; queued = 0; if (all) cpuKept = 0; g_chk.Reset(all); };
    if (!fgOn || !g_reports) { reset(true); lastNs = 0; return; }
    memset(r, 0, sizeof(r));
    if (g_reports(ctx, r) != 0) return;
    uint32_t top = 0, low = 0xFFFFFFFFu; uint64_t topE = 0, topS = 0, lowE = 0; double cpuMin = 1e9;
    for (int i = 0; i < 64; ++i)
    {
        if (!r[i].id || !r[i].prE) continue;   // not finished yet: frames in flight
        // a report with the submit right after the simulation start (menus, resizes, skipped frames) would set the CPU
        // time near 0 for 64 frames and make every start wait too long
        if (r[i].simS && r[i].rsS > r[i].simS)
        {
            const double c = (r[i].rsS - r[i].simS) / 1e6;
            if (c >= kCpuFloorMs && c < 1000 && c < cpuMin) cpuMin = c;
        }
        if (r[i].id > top) { top = r[i].id; topE = r[i].prE; topS = r[i].simS; }
        if (r[i].id < low) { low = r[i].id; lowE = r[i].prE; }
    }
    if (!top || top == low || topE <= lowE || frame <= top || frame - top > 16) return;
    // the latency of the newest finished frame (XeLL's simulation start -> present end), each frame once
    double latMs = 0;
    if (top != latTop && topS && topE > topS && topE - topS < 1000000000ull) { latMs = (double) (topE - topS) / 1e6; latTop = top; }
    const double gMs = (topE - lowE) / 1e6 / (top - low);
    if (gMs > 150) { reset(false); return; }   // a loading screen
    if (uncappedMs <= 0)
    {
        // learn the frame time before any wait: three intervals 30 frames apart within 3 %
        if (++since < 30) return;
        since = 0;
        gHist[gN++ % 3] = gMs;
        if (gN < 3) return;
        const double hi = std::max({gHist[0], gHist[1], gHist[2]}), lo = std::min({gHist[0], gHist[1], gHist[2]});
        if (hi < lo * 1.03) { uncappedMs = (gHist[0] + gHist[1] + gHist[2]) / 3; XLog("low latency: frame time %.2f ms without a wait, frame %u", uncappedMs, frame); }
        return;
    }
    const bool started = waitRun >= kStartFrames && cpuMin < 1e8;
    if (started)
    {
        // The wait makes the interval longer but never simulation -> submit; a queue does. So the CPU time over its lowest
        // recent value is the queue. That value rises by 0.2 % of the gap per frame, so a cheap moment (a menu, a cut
        // scene) is forgotten while a queue, which builds within a second or two, still stands out; frames far over the
        // frame time (hitches, menus) do not move it.
        if (gMs < uncappedMs * 1.3) cpuBase = cpuMin < cpuBase ? cpuMin : cpuBase + (cpuMin - cpuBase) * 0.002;
        // the minimum of all 64 for 30 frames: a short window (8 frames) took the game thread's own variation in
        // CPU-limited scenes for a queue and cost frame rate
        if (cpuMin > cpuBase + kQueueMs && gMs > uncappedMs)
        {
            if (++queued >= kQueueFrames) { XLog("low latency: a queue of %.1f ms over %.1f, frame time %.2f -> %.2f ms", cpuMin - cpuBase, cpuBase, uncappedMs, gMs); uncappedMs = gMs; queued = 0; }
        }
        else queued = 0;
        if (gMs < uncappedMs) uncappedMs = std::max(gMs, uncappedMs * 0.999);   // a lighter scene
        cpuKept = cpuMin;
    }
    const double g = gMs < uncappedMs ? gMs : uncappedMs;
    // XeLL's stamps and QPC must be one clock, or the wait would be a constant at its limit
    if ((double) nowNs - (double) topE > 1e9 || (double) topE - (double) nowNs > 1e9)
    { static int said = 0; if (said++ < 3) XLog("low latency: newest finished frame %.0f ms from now (a stall, or another clock), no wait", ((double) nowNs - (double) topE) / 1e6); return; }
    const uint32_t q = frame - top - 1;
    // the reports are of frames with the wait only kStartFrames frames after it came (back): until then the CPU time
    // learned before, or kStartCpuMs the first time
    const double cpuMs = started ? cpuMin : cpuKept > 0 ? cpuKept : kStartCpuMs;
    const double leftMs = ((double) topE - (double) nowNs) / 1e6 + q * g - cpuMs - g_autoMarginMs - g_chk.shortMs;
    lastLeft = leftMs; lastG = g; lastQ = q; lastCpu = cpuMs;
    double sleepMs = leftMs < 0 ? 0 : leftMs > g * 2 ? g * 2 : leftMs;
    // the check: may this frame wait? (not asked during the very first start: the CPU time is a guess there)
    if (g_chk.Frame((double) nowNs / 1e9, dtMs, waitedMs, latMs, started || cpuKept > 0, XLog)) ++waitRun;
    else { sleepMs = 0; waitRun = 0; queued = 0; }
    if (dtMs > 0 && dtMs < 250) { sumFrame += dtMs; ++nFrame; }
    if (latMs > 0) { sumLat += latMs; ++nLat; }
    if (sleepMs > 0)
    {
        // NtDelayExecution in 100 ns units (Sleep takes whole milliseconds, which left 1.5-2.5 ms to spin on a SoC whose
        // CPU and GPU share one power limit); it stops short by the late wake-up seen recently, a maximum that decays,
        // and only that is spun
        static PFN_NtDelayExecution delay = (PFN_NtDelayExecution) GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtDelayExecution");
        static double lateMs = 1.0;
        const LONGLONG until = now.QuadPart + (LONGLONG) (sleepMs * qf.QuadPart / 1000.0);
        const double slack = std::min(2.0, std::max(0.2, lateMs + 0.2));
        LARGE_INTEGER t;
        if (sleepMs > slack)
        {
            const double askMs = sleepMs - slack;
            QueryPerformanceCounter(&t);
            const LONGLONG from = t.QuadPart;
            if (delay) { LARGE_INTEGER rel; rel.QuadPart = -(LONGLONG) (askMs * 10000.0); delay(FALSE, &rel); }
            else Sleep((DWORD) askMs);
            QueryPerformanceCounter(&t);
            lateMs = std::max((t.QuadPart - from) * 1000.0 / qf.QuadPart - askMs, lateMs * 0.98);
        }
        QueryPerformanceCounter(&t);
        const LONGLONG spinFrom = t.QuadPart;
        while (t.QuadPart < until) { SwitchToThread(); QueryPerformanceCounter(&t); }
        sumSpin += (t.QuadPart - spinFrom) * 1000.0 / qf.QuadPart;
        lastWaitMs = (t.QuadPart - now.QuadPart) * 1000.0 / qf.QuadPart;
        sumSleep += lastWaitMs; ++slept;
    }
    if (++nCalls == 300)
    {
        // as the FSR routes' line in ffx_xefg.log: how often and how long the frames waited, the frame time, the latency
        XLog("low latency: waited in %d of %d frames, %.1f ms each; frame %.2f ms; start -> present end %.1f ms (%d frames); last: %.1f ms left, "
             "%u in flight, frame %.2f ms (without a wait %.2f), cpu %.1f ms, %.2f ms spun",
             slept, nCalls, slept ? sumSleep / slept : 0.0, nFrame ? sumFrame / nFrame : 0.0, nLat ? sumLat / nLat : 0.0, nLat, lastLeft, lastQ,
             lastG, uncappedMs, lastCpu, slept ? sumSpin / slept : 0.0);
        nCalls = slept = nFrame = nLat = 0; sumSleep = sumSpin = sumFrame = sumLat = 0;
    }
}

// XeLL contexts whose caller paces the frame start itself (IgdextXellExternalPacing), looked up on every xellSleep
static PtrSlots g_extPaced;

extern "C" void IgdextXellExternalPacing(void* ctx, int on)
{
    if (!ctx) return;
    if (!on)
    {
        if (g_extPaced.Remove(ctx)) XLog("xell context %p: frame start no longer paced by its caller", ctx);
        return;
    }
    const int added = g_extPaced.Add(ctx);
    if (added > 0) XLog("xell context %p: frame start paced by its caller (no AutoPaceWait)", ctx);
    else if (added < 0) XLog("xell context %p: no free slot, AutoPaceWait stays on for it", ctx);
}

static uint32_t Hook_Sleep(void* ctx, uint32_t frame)
{
    LONG n = InterlockedIncrement(&g_sleepCalls);
    const bool pace = g_capAuto && !g_extPaced.Has(ctx);
    if (!g_log) { const uint32_t rr = g_origSleep(ctx, frame); if (pace) AutoPaceWait(ctx, frame); return rr; }
    if (n == 1 && g_setLog) XLog("xellSetLoggingCallback(debug) -> %u", g_setLog(ctx, 0, XellLogCb));
    if (n < 5 || (n % 600) == 0) XLog("xellSleep call #%ld frame=%u", (long) n, frame);
    LARGE_INTEGER qa, qb, qf; QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&qa);
    uint32_t rr = g_origSleep(ctx, frame);
    if (pace) AutoPaceWait(ctx, frame);
    QueryPerformanceCounter(&qb);
    static double sum = 0, mx = 0; static int cnt = 0; static LARGE_INTEGER t0;
    double d = (qb.QuadPart - qa.QuadPart) * 1000.0 / qf.QuadPart;
    if (!t0.QuadPart) t0 = qa;
    sum += d; if (d > mx) mx = d; ++cnt;
    if (cnt == 100)
    {
        double wall = (qb.QuadPart - t0.QuadPart) * 1000.0 / qf.QuadPart;
        XLog("xellSleep stats: 100 calls in %.0f ms (%.1f ms/frame), blocked avg %.2f ms max %.2f ms = %.0f%% of wall", wall, wall / 100, sum / 100, mx, 100.0 * sum / wall);
        if (g_reports)
        {
            static XellFrameReport r[64];
            memset(r, 0, sizeof(r));
            if (g_reports(ctx, r) == 0)
            {
                double lat = 0, sub = 0; int k = 0;
                for (int i = 0; i < 64; ++i)
                {
                    if (!r[i].id || !r[i].simS || r[i].prE <= r[i].simS || r[i].prE - r[i].simS > 1000000000ull) continue;
                    lat += (r[i].prE - r[i].simS) / 1e6; sub += (r[i].rsS > r[i].simS ? (r[i].rsS - r[i].simS) / 1e6 : 0); ++k;
                }
                if (k) XLog("xell latency: simulation start -> present end avg %.1f ms (sim -> render submit %.1f ms) over %d frames", lat / k, sub / k, k);
                // the same from the input read of each frame, by the thread that reads once a frame: of the threads with
                // a read in at least 8 of 10 of these frames, the one with the fewest reads in their time (a game can
                // have another thread that polls the controller more often: Resident Evil Requiem)
                InputWatch();
                uint64_t from = ~0ull, to = 0;
                for (int i = 0; i < 64; ++i)
                {
                    if (!r[i].id || !r[i].simS || r[i].prE <= r[i].simS) continue;
                    from = std::min(from, r[i].simS); to = std::max(to, r[i].prE);
                }
                uint32_t tid = 0; int fewest = 0;
                uint32_t tried[8] = {}; int nTried = 0;
                const LONG nIn = g_inputN;
                for (LONG a = 0; a < 1024 && a < nIn && nTried < 8; a += 13)
                {
                    const uint32_t t = g_inputReads[a].tid;
                    bool seen = false;
                    for (int j = 0; j < nTried; ++j) seen |= tried[j] == t;
                    if (seen) continue;
                    tried[nTried++] = t;
                    int c = 0, frames = 0;
                    for (LONG b = 0; b < 1024 && b < nIn; ++b) c += g_inputReads[b].tid == t && g_inputReads[b].ns >= from && g_inputReads[b].ns < to;
                    for (int i = 0; i < 64; ++i)
                        if (r[i].id && r[i].simS && r[i].prE > r[i].simS && InputReadOf(r[i].simS, r[i].prE, t)) ++frames;
                    if (frames * 10 >= k * 8 && (!tid || c < fewest)) { tid = t; fewest = c; }
                }
                double inLat = 0, inAfter = 0; int ki = 0;
                for (int i = 0; tid && i < 64; ++i)
                {
                    if (!r[i].id || !r[i].simS || r[i].prE <= r[i].simS || r[i].prE - r[i].simS > 1000000000ull) continue;
                    const uint64_t in = InputReadOf(r[i].simS, r[i].prE, tid);
                    if (!in) continue;
                    inLat += (r[i].prE - in) / 1e6; inAfter += ((double) in - (double) r[i].simS) / 1e6; ++ki;
                }
                if (ki) XLog("input latency: input read (XInputGetState, thread %lu) -> present end avg %.1f ms, the read %.1f ms after the simulation start, over %d of %d frames", (unsigned long) tid, inLat / ki, inAfter / ki, ki, k);
            }
        }
        sum = mx = 0; cnt = 0; t0 = qb;
    }
    return rr;
}

static DWORD WINAPI XellThread(LPVOID)
{
    HANDLE ev = WatchStart(WATCH_XELL);
    const ULONGLONG deadline = GetTickCount64() + kWatchTotalMs;
    HMODULE m = nullptr;
    // the check first: the library may have been loaded before the notification was in place; after the last wait it
    // is the final check. A reference is taken (as XefgInstallHooks does): the library must not be unloaded between
    // here and the patching 200 ms later
    while (!(GetModuleHandleExW(0, L"libxell.dll", &m) && m) && WatchWait(ev, deadline, 100)) m = nullptr;
    if (ev) WatchStop();
    if (!m) { XLog("libxell.dll never loaded"); return 0; }
    XLog("libxell.dll found at %p", m);
    Sleep(200);
    auto set = (void*) GetProcAddress(m, "xellSetSleepMode");
    auto slp = (void*) GetProcAddress(m, "xellSleep");
    auto fg = (void*) GetProcAddress(m, "xellSetFgEnabled");
    if (!set || !slp || !fg) { XLog("XeLL exports missing"); FreeLibrary(m); return 0; }
    g_setLog = (PFN_XellSetLog) GetProcAddress(m, "xellSetLoggingCallback");
    g_reports = (PFN_XellReports) GetProcAddress(m, "xellGetFramesReports");
    // the original pointers are stored before each jmp goes live (PatchThunk writes *orig first): xellSleep runs on
    // the render thread every frame and must never find a null original
    PatchThunk(slp, (void*) Hook_Sleep, (void**) &g_origSleep, "xellSleep");
    PatchThunk(set, (void*) Hook_SetSleepMode, (void**) &g_origSet, "xellSetSleepMode");
    PatchThunk(fg, (void*) Hook_SetFg, (void**) &g_origFg, "xellSetFgEnabled");
    FreeLibrary(m);
    return 0;
}

void StartXellHook()
{
    char b[32] = {};
    if (GetEnvironmentVariableA("IGDEXT_XELL_LOG", b, sizeof(b)) > 0) g_log = true;
    char exe[MAX_PATH] = {}; GetModuleFileNameA(nullptr, exe, MAX_PATH);
    g_fixCap = strstr(exe, "Wuthering Waves") != nullptr;
    if (GetEnvironmentVariableA("IGDEXT_XELL_FIXCAP", b, sizeof(b)) > 0 && atoi(b)) g_fixCap = true;
    if (GetEnvironmentVariableA("IGDEXT_XELL_KEEPCAP", b, sizeof(b)) > 0 && atoi(b)) g_fixCap = false;
    const DWORD capLen = GetEnvironmentVariableA("IGDEXT_XELL_CAP", b, sizeof(b));
    if (capLen > 0 && capLen < sizeof(b))
    {
        if (!strncmp(b, "auto", 4) && (b[4] == 0 || b[4] == ':')) { if (b[4] == ':') g_autoMarginMs = atof(b + 5); }
        else if (!_stricmp(b, "off") || !strcmp(b, "0")) g_capAuto = false;
        else
        {
            // a fixed cap replaces the pacing; only a plain number up to 1 s per displayed frame, anything else (a
            // negative number, a typo, ms written as us) would hold the game at seconds per frame and turns it all off
            char* end = nullptr;
            const unsigned long us = b[0] >= '0' && b[0] <= '9' ? strtoul(b, &end, 10) : 0;
            g_capAuto = false;
            if (end && *end == 0 && us <= 1000000) g_capUs = (uint32_t) us;
            else g_capUs = 0, g_capBad = true;
        }
    }
    if (!g_fixCap && !g_log && !g_capUs && !g_capAuto) return;
    if (GetEnvironmentVariableA("IGDEXT_XELL_CHECK", b, sizeof(b)) > 0 && !atoi(b)) g_chk.on = false;
    if (GetEnvironmentVariableA("IGDEXT_XELL_POLLS", b, sizeof(b)) > 0 && !atoi(b)) g_polls = false;
    // the same line as in ffx_xefg.log (the FSR routes)
    XLog("low latency (IGDEXT_XELL_CAP): %s; the game's questions to the swap chain answered %s%s",
         g_capAuto ? "auto, the game's frames are held back in xellSleep" : g_capUs ? "XeLL's fixed cap" : g_capBad ? "off (not off, auto[:ms] or 0..1000000 us)" : "off",
         g_capAuto && g_polls ? "as of the last Present (no wait)" : "by the swap chain", g_capAuto && !g_chk.on ? "; the wait is not checked" : "");
    XLog("XellHook start in %s", exe);
    if (!StartModuleRefThread(XellThread)) XLog("XeLL hook thread not started (%lu)", GetLastError());
}

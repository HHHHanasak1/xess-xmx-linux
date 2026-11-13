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
// (ffxfg/ffx_xefg.cpp, "Low latency"), the wait is checked against the frame time and the latency
// (common/lowlat_check.h), and a game's questions to XeSS-FG's swap chain are answered as of its last Present
// ("A game's questions to XeSS-FG's swap chain" below).
//   IGDEXT_XELL_CAP=off     no low latency;  IGDEXT_XELL_CAP=auto:<ms>  start each frame <ms> earlier (later if negative)
//   IGDEXT_XELL_CAP=<us>    instead: at least this interval per displayed frame (XeLL's frame cap, in both of its modes),
//                           up to 1000000; any other value turns both off
//   IGDEXT_XELL_CHECK=0     the low latency's wait is not checked
//   IGDEXT_XELL_POLLS=0     the swap chain answers the game itself
//   IGDEXT_XELL_LOG=1       append the parameters the game passes and xellSleep timing (real frame rate) to C:\igdext_xell.log;
//                           with it, XInputGetState is watched too, for a second latency figure: from where the game
//                           reads its input to the end of the frame's present (InputWatch below)
// The package's own frame generation DLL (ffxfg/ffx_xefg.cpp, the FSR routes) sets the game's frame start itself, by
// keeping the game in Present ("Low latency" there), and registers its XeLL context with IgdextXellExternalPacing:
// xellSleep of such a context gets no AutoPaceWait.
#include "Stdafx.h"
#include "Trace.h"
#include "LdrNotify.h"
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
static bool g_log = false;
static uint32_t g_capUs = 0;
static bool g_capAuto = true, g_capBad = false; static double g_autoMarginMs = 0;
static bool g_polls = true;   // IGDEXT_XELL_POLLS=0: the swap chain answers the game itself
static lowlat::Check g_chk;   // the check of the wait (IGDEXT_XELL_CHECK=0: g_chk.on = false)
static uint32_t g_mfg = 0;   // generated frames per rendered frame, 0 = frame generation off
// Whether frame generation is on, from two places: XeLL's xellSetFgEnabled (g_mfg; missed when a game enables frame
// generation before XellThread's hooks are in, 200 ms after the library maps) and XeSS-FG's own switch
// xefgSwapChainSetEnabled (Hook_XefgSetEnabled). The newer call of the two decides (Wuthering Waves switches frame
// generation off in its menu through XeLL only, CONTROL Resonant's FSR route through XeSS-FG only). Neither seen = off: a game
// that loads libxess_fg.dll but keeps frame generation off never calls either.
static volatile LONG g_fgState = -1;   // -1 not seen, 0 off, 1 on: the newer of the two calls
static volatile const char* g_fgSource = "not seen";
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

static void XLog(const char* fmt, ...)
{
    if (!g_log) return;
    va_list ap; va_start(ap, fmt); AppendLogLine("C:\\igdext_xell.log", fmt, ap); va_end(ap);
}

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
// ffxfg/ffx_xefg.cpp).
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

static HMODULE SelfModule(DWORD flags);

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

// XeLL contexts whose caller paces the frame start itself (IgdextXellExternalPacing): a few slots, read without a lock
// on every xellSleep
static void* volatile g_extPaced[4];

extern "C" void IgdextXellExternalPacing(void* ctx, int on)
{
    if (!ctx) return;
    for (int i = 0; i < 4; ++i) if (!on && InterlockedCompareExchangePointer(&g_extPaced[i], nullptr, ctx) == ctx)
    { XLog("xell context %p: frame start no longer paced by its caller", ctx); return; }
    if (!on) return;
    for (int i = 0; i < 4; ++i) if (g_extPaced[i] == ctx) return;
    for (int i = 0; i < 4; ++i) if (InterlockedCompareExchangePointer(&g_extPaced[i], ctx, nullptr) == nullptr)
    { XLog("xell context %p: frame start paced by its caller (no AutoPaceWait)", ctx); return; }
    XLog("xell context %p: no free slot, AutoPaceWait stays on for it", ctx);
}

static bool ExternallyPaced(void* ctx)
{
    for (int i = 0; i < 4; ++i) if (g_extPaced[i] == ctx) return true;
    return false;
}

// ---------------------------------------------------------------------------------------------------------------
// A game's questions to XeSS-FG's swap chain (IGDEXT_XELL_POLLS, on with the low latency)
//
// XeSS-FG's proxy swap chain passes IDXGISwapChain::GetFrameStatistics and IDXGISwapChain3::CheckColorSpaceSupport on
// to the real swap chain. DXVK's swap chain holds its lock for the whole of a Present, and XeSS-FG's presentation
// thread is inside one for most of a frame: a game that asks every frame starts its next frame only when that
// Present returns, and the GPU idles meanwhile (ffxfg/ffx_xefg.cpp, "Low latency", has the time lines of three games
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
enum { SC_PRESENT = 8, SC_SETFS = 10, SC_RESIZE = 13, SC_STATS = 16, SC_PRESENT1 = 22, SC_CHECKCS = 37, SC_SETCS = 38, SC_RESIZE1 = 39, SC_SLOTS = 40 };
typedef HRESULT (STDMETHODCALLTYPE* PFN_ScPresent)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE* PFN_ScPresent1)(IDXGISwapChain*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
typedef HRESULT (STDMETHODCALLTYPE* PFN_ScStats)(IDXGISwapChain*, DXGI_FRAME_STATISTICS*);
typedef HRESULT (STDMETHODCALLTYPE* PFN_ScCheckCs)(IDXGISwapChain*, DXGI_COLOR_SPACE_TYPE, UINT*);
typedef HRESULT (STDMETHODCALLTYPE* PFN_ScSetCs)(IDXGISwapChain*, DXGI_COLOR_SPACE_TYPE);
typedef HRESULT (STDMETHODCALLTYPE* PFN_ScSetFs)(IDXGISwapChain*, BOOL, IDXGIOutput*);
typedef HRESULT (STDMETHODCALLTYPE* PFN_ScResize)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef HRESULT (STDMETHODCALLTYPE* PFN_ScResize1)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*, IUnknown* const*);

void** volatile g_scVt;          // the virtual table the hooks are in
void* g_scOrig[SC_SLOTS];        // its own entries
void* volatile g_scOwned[4];     // swap chains whose wrapper answers its game itself (IgdextXefgProxyOwned)

struct ScAnswers
{
    SRWLOCK lock;
    void* sc;                    // the swap chain they are of
    bool statsAsked, statsHave; int statsIdle; DXGI_FRAME_STATISTICS stats; HRESULT statsHr;
    struct Cs { UINT cs; HRESULT hr; UINT support; bool asked, have; int idle; } cs[8]; int nCs;
    LONG saidStats, saidCs, saidWaited;
} g_ans = { SRWLOCK_INIT };

bool ScOwned(void* sc)
{
    for (int i = 0; i < 4; ++i) if (g_scOwned[i] == sc) return true;
    return false;
}

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

HRESULT STDMETHODCALLTYPE Hook_ScPresent(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    const bool outer = !InPresent();
    if (outer) { AnswersTake(sc); g_presentTid = GetCurrentThreadId(); }
    const HRESULT hr = ((PFN_ScPresent) g_scOrig[SC_PRESENT])(sc, sync, flags);
    if (outer) g_presentTid = 0;
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_ScPresent1(IDXGISwapChain* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp)
{
    const bool outer = !InPresent();
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
    AnswersForget();
    return ((PFN_ScSetCs) g_scOrig[SC_SETCS])(sc, c);
}
HRESULT STDMETHODCALLTYPE Hook_ScSetFs(IDXGISwapChain* sc, BOOL f, IDXGIOutput* o)
{
    AnswersForget();
    return ((PFN_ScSetFs) g_scOrig[SC_SETFS])(sc, f, o);
}
HRESULT STDMETHODCALLTYPE Hook_ScResize(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags)
{
    AnswersForget();
    return ((PFN_ScResize) g_scOrig[SC_RESIZE])(sc, n, w, h, f, flags);
}
HRESULT STDMETHODCALLTYPE Hook_ScResize1(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags, const UINT* masks, IUnknown* const* queues)
{
    AnswersForget();
    return ((PFN_ScResize1) g_scOrig[SC_RESIZE1])(sc, n, w, h, f, flags, masks, queues);
}

// the hooks go into the virtual table of XeSS-FG's swap chain, once per table (a libxess_fg that is loaded again
// comes with a fresh one)
void ProxyHook(IUnknown* unk)
{
    if (!unk || !g_capAuto || !g_polls) return;
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
            { SC_RESIZE, (void*) Hook_ScResize }, { SC_RESIZE1, (void*) Hook_ScResize1 },
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
}

// The package's own frame generation DLL (ffxfg/ffx_xefg.cpp) wraps XeSS-FG's swap chain and answers its game itself:
// the hooks above pass every call of such a swap chain on unchanged.
extern "C" void IgdextXefgProxyOwned(void* swapChain, int on)
{
    if (!swapChain) return;
    for (int i = 0; i < 4; ++i) if (!on && InterlockedCompareExchangePointer(&g_scOwned[i], nullptr, swapChain) == swapChain) return;
    if (!on) return;
    for (int i = 0; i < 4; ++i) if (g_scOwned[i] == swapChain) return;
    for (int i = 0; i < 4; ++i) if (InterlockedCompareExchangePointer(&g_scOwned[i], swapChain, nullptr) == nullptr)
    { XLog("swap chain %p: its wrapper answers the game itself", swapChain); return; }
}

static uint32_t Hook_Sleep(void* ctx, uint32_t frame)
{
    LONG n = InterlockedIncrement(&g_sleepCalls);
    const bool pace = g_capAuto && !ExternallyPaced(ctx);
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

// this module; flags as for GetModuleHandleExW (0 takes a reference, GET_MODULE_HANDLE_EX_FLAG_PIN keeps it loaded until
// the process exits, GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT neither)
static HMODULE SelfModule(DWORD flags)
{
    HMODULE self = nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | flags, (LPCWSTR) &SelfModule, &self) ? self : nullptr;
}

static bool PatchThunk(void* thunk, void* hook, void** orig, const char* name)
{
    // a patched export jumps into this module from then on: it must never be unmapped (games load, free and load the
    // shim again at startup)
    static volatile LONG pinned = 0;
    if (!pinned && !SelfModule(GET_MODULE_HANDLE_EX_FLAG_PIN)) { XLog("%s: could not pin igdext64 (%lu), not hooked", name, GetLastError()); return false; }
    pinned = 1;
    uint8_t* p = (uint8_t*) thunk;
    if (p[0] != 0xE9) { XLog("%s: export is not a jmp rel32 thunk (byte %02x), not hooked", name, p[0]); return false; }
    int32_t rel = *(int32_t*) (p + 1);
    *orig = p + 5 + rel;
    intptr_t d = (uint8_t*) hook - (p + 5);
    if (d > INT32_MAX || d < INT32_MIN) { XLog("%s: hook too far from the thunk (%lld), not hooked", name, (long long) d); return false; }
    // only the rel32 (bytes 1-4) changes. When it lies inside one aligned 8-byte word (10 of 16 thunk positions), that
    // word is swapped (compare-exchange: the neighbouring thunks' bytes stay as they are); otherwise the 8 bytes from the
    // thunk on, unaligned as before (atomic on x86-64, a split lock when they cross a cache line)
    const uintptr_t r = (uintptr_t) (p + 1);
    uint8_t* w = (r & 7) <= 4 ? (uint8_t*) (r & ~(uintptr_t) 7) : p;
    uint8_t* lo = w < p ? w : p;
    const SIZE_T span = (SIZE_T) (std::max(w, p) + 8 - lo);
    DWORD old;
    if (!VirtualProtect(lo, span, PAGE_EXECUTE_READWRITE, &old)) { XLog("%s: VirtualProtect failed %lu", name, GetLastError()); return false; }
    if (w != p)
    {
        const unsigned sh = (unsigned) (r & 7) * 8;
        const uint64_t mask = 0xFFFFFFFFull << sh, newRel = (uint64_t) (uint32_t) (int32_t) d << sh;
        uint64_t cur = *(volatile uint64_t*) w;
        for (;;)
        {
            const uint64_t prev = (uint64_t) InterlockedCompareExchange64((volatile LONG64*) w, (LONG64) ((cur & ~mask) | newRel), (LONG64) cur);
            if (prev == cur) break;
            cur = prev;
        }
    }
    else
    {
        uint64_t cur = *(volatile uint64_t*) p;
        uint64_t neu = (cur & ~0xFFFFFFFFFFull) | 0xE9ull | ((uint64_t)(uint32_t)(int32_t) d << 8);
        InterlockedExchange64((volatile LONG64*) p, (LONG64) neu);
    }
    VirtualProtect(lo, span, old, &old);
    FlushInstructionCache(GetCurrentProcess(), lo, span);
    XLog("%s hooked: thunk %p -> %p (orig %p)", name, thunk, hook, *orig);
    return true;
}

// The threads below wait for a library the game loads later. The loader tells them when it is mapped
// (LdrRegisterDllNotification, LdrNotify.h): the callback runs under the loader lock and does nothing but signal the
// thread's event. The install code (PatchThunk pins this module, XefgInstallHooks calls FreeLibrary) runs on the
// thread, never in the callback. One registration serves both threads; the last one done removes it before its module
// reference goes, so the callback is never called into an unmapped module. Without the notification (registration
// refused) the threads poll as they used to.
namespace
{
enum { WATCH_XELL = 0, WATCH_XEFG = 1, WATCH_COUNT = 2 };
const wchar_t* const kWatchedDll[WATCH_COUNT] = { L"libxell.dll", L"libxess_fg.dll" };
const DWORD kWatchTotalMs = 600000;   // 10 minutes, as the polling loops before (6000 x 100 ms, 60000 x 10 ms)
SRWLOCK g_watchLock = SRWLOCK_INIT;
HANDLE g_watchEvent[WATCH_COUNT];     // auto-reset, one per waiter: a load of the other library wakes nobody
PVOID g_watchCookie = nullptr;
int g_watchUsers = 0;
PFN_LdrUnregisterDllNotification g_ldrUnregister = nullptr;

VOID CALLBACK OnWatchedDllLoaded(ULONG reason, const LdrDllNotificationData* data, PVOID)
{
    if (reason != kLdrDllLoaded || !data) return;
    for (int i = 0; i < WATCH_COUNT; ++i)
        if (LdrDllNameIs(data->BaseDllName, kWatchedDll[i])) SetEvent(g_watchEvent[i]);
}

// the event the loader signals when the library is mapped; nullptr when there is no notification (the caller polls).
// The caller checks for a library loaded before this call returns, and ends with WatchStop.
HANDLE WatchStart(int which)
{
    AcquireSRWLockExclusive(&g_watchLock);
    if (g_watchUsers == 0)
    {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        auto reg = ntdll ? (PFN_LdrRegisterDllNotification) GetProcAddress(ntdll, "LdrRegisterDllNotification") : nullptr;
        g_ldrUnregister = ntdll ? (PFN_LdrUnregisterDllNotification) GetProcAddress(ntdll, "LdrUnregisterDllNotification") : nullptr;
        bool ok = reg && g_ldrUnregister;
        for (HANDLE& e : g_watchEvent) ok = (e = CreateEventW(nullptr, FALSE, FALSE, nullptr)) != nullptr && ok;
        if (ok && reg(0, OnWatchedDllLoaded, nullptr, &g_watchCookie) != 0) { g_watchCookie = nullptr; ok = false; }
        if (!ok) for (HANDLE& e : g_watchEvent) { if (e) CloseHandle(e); e = nullptr; }
    }
    HANDLE ev = g_watchCookie ? g_watchEvent[which] : nullptr;
    if (ev) ++g_watchUsers;
    ReleaseSRWLockExclusive(&g_watchLock);
    return ev;
}

void WatchStop()
{
    AcquireSRWLockExclusive(&g_watchLock);
    if (--g_watchUsers == 0)
    {
        g_ldrUnregister(g_watchCookie);   // takes the loader lock: no callback runs after it
        g_watchCookie = nullptr;
        for (HANDLE& e : g_watchEvent) { CloseHandle(e); e = nullptr; }
    }
    ReleaseSRWLockExclusive(&g_watchLock);
}

// one wait of the remaining time: the event, or a poll interval without one; false once the total is used up
bool WatchWait(HANDLE ev, ULONGLONG deadline, DWORD pollMs)
{
    const ULONGLONG now = GetTickCount64();
    if (now >= deadline) return false;
    const ULONGLONG left = deadline - now;
    if (ev) WaitForSingleObject(ev, left > 0xFFFFFFFEull ? 0xFFFFFFFE : (DWORD) left); else Sleep(pollMs);
    return true;
}
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

// A thread that waits for a library holds its own reference to this module, so its code stays mapped while the game
// frees and loads the shim again at startup, and drops it only when it is done.
static DWORD WINAPI ModuleRefThread(LPVOID body)
{
    ((LPTHREAD_START_ROUTINE) body)(nullptr);
    FreeLibraryAndExitThread(SelfModule(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT), 0);
}

static bool StartModuleRefThread(LPTHREAD_START_ROUTINE body)
{
    HMODULE self = SelfModule(0);
    if (!self) return false;
    HANDLE h = CreateThread(nullptr, 0, ModuleRefThread, (LPVOID) body, 0, nullptr);
    if (!h) { FreeLibrary(self); return false; }
    CloseHandle(h);
    return true;
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

// XeFG (libxess_fg.dll) diagnostics, IGDEXT_XEFG_LOG=1 (XESS_LOG=1 sets it): XeFG's own log through its public
// xefgSwapChainSetLoggingCallback, and what the game asks for and gets: the maximum number of interpolated frames after
// context creation (where an Intel Graphics Software override, tools' ControlLib.dll with XESS_FG, takes effect), the
// value the game passes at initialization, and every change of the number. Written to C:\igdext_xefg.log.
typedef int32_t (*PFN_XefgCreate)(void* dev, void** ctx);
typedef int32_t (*PFN_XefgInit)(void* ctx, void* queue, const void* params);
typedef int32_t (*PFN_XefgSetNum)(void* ctx, uint32_t n);
typedef int32_t (*PFN_XefgSetLog)(void* ctx, int32_t level, void (*cb)(const char*, int32_t, void*), void* user);
typedef int32_t (*PFN_XefgGetProps)(void* ctx, void* props);
typedef int32_t (*PFN_XefgGetInit)(void* ctx, void* params);
#pragma pack(push, 8)
struct XefgProps { uint32_t requiredDescriptorCount; uint64_t tempBufferHeapSize, tempTextureHeapSize, constantBufferSize; uint32_t maxSupportedInterpolations; };
struct XefgInitParams { void* pApplicationSwapChain; uint32_t initFlags, maxInterpolatedFrames, creationNodeMask, visibleNodeMask;
                        uint8_t rest[64]; };
#pragma pack(pop)
static PFN_XefgCreate g_fgCreate; static PFN_XefgInit g_fgInit; static PFN_XefgSetNum g_fgSetNum;
// xefgSwapChainD3D12GetSwapChainPtr(ctx, riid, ppSwapChain): the swap chain the game presents to (ProxyHook)
typedef int32_t (*PFN_XefgGetSwapChainPtr)(void* ctx, REFIID riid, void** sc);
static PFN_XefgGetSwapChainPtr g_fgGetSc;
static volatile LONG g_fgScTries;   // from a context, when the game got its swap chain before the hooks were in
static void ProxyFromContext(void* ctx)
{
    if (g_scVt || !g_fgGetSc || !ctx || g_fgScTries >= 8) return;
    InterlockedIncrement(&g_fgScTries);
    IUnknown* sc = nullptr;
    if (g_fgGetSc(ctx, __uuidof(IDXGISwapChain), (void**) &sc) == 0 && sc) { ProxyHook(sc); sc->Release(); }
}
static int32_t Hook_XefgGetSwapChainPtr(void* ctx, REFIID riid, void** sc)
{
    const int32_t r = g_fgGetSc(ctx, riid, sc);
    if (r == 0 && sc && *sc) ProxyHook((IUnknown*) *sc);
    return r;
}
static PFN_XefgSetLog g_fgSetLog; static PFN_XefgGetProps g_fgGetProps; static PFN_XefgGetInit g_fgGetInit;

static bool g_fgLog = false;
static void FLog(const char* fmt, ...)
{
    if (!g_fgLog) return;
    va_list ap; va_start(ap, fmt); AppendLogLine("C:\\igdext_xefg.log", fmt, ap); va_end(ap);
}

static void XefgLogCb(const char* msg, int32_t level, void*)
{
    if (msg) FLog("XeFG[%d] %s", level, msg);
}

static void XefgReportMax(void* ctx, const char* when)
{
    XefgProps p = {};
    int32_t r = g_fgGetProps ? g_fgGetProps(ctx, &p) : -1;
    FLog("  %s: xefgSwapChainGetProperties -> %d, maxSupportedInterpolations %u", when, r, p.maxSupportedInterpolations);
}

// More than 4x (XESS_FG=5..8): XeSS-FG takes its maximum number of interpolated frames from the Intel Graphics Software
// override (ControlLib.dll answers it; Intel's enum and XeSS-FG's parser end at 4x), and keeps that number in its own
// writable data. It is found by observation, without any knowledge of the library's layout: after the game's context
// creation (override 4x: 3 frames) every 32-bit value 3 in libxess_fg's writable sections is a candidate; a probe
// context created through the public API with the override at 3x (XESS_FG_PROBE=2, read by ControlLib.dll) leaves only
// the value that followed it. That one is set to the wanted number, and the public xefgSwapChainGetProperties of the
// game's context must then report it - otherwise the old value goes back and the game keeps 4x.
typedef int32_t (*PFN_XefgDestroy)(void* ctx);
static PFN_XefgDestroy g_fgDestroy;

static void XefgRaise(void* dev, void* gameCtx)
{
    char b[16];
    if (GetEnvironmentVariableA("XESS_FG", b, sizeof(b)) == 0) return;
    int n = atoi(b);
    if (n <= 4) return;
    uint32_t want = (uint32_t) (n > 8 ? 8 : n) - 1;
    HMODULE m = GetModuleHandleW(L"libxess_fg.dll");
    XefgProps p0 = {};
    if (!m || !g_fgCreate || !g_fgDestroy || !g_fgGetProps || g_fgGetProps(gameCtx, &p0) != 0) { FLog("  XESS_FG=%d: no libxess_fg API, 4x", n); return; }
    if (p0.maxSupportedInterpolations != 3) { FLog("  XESS_FG=%d: maximum is %u, not the 4x override (ControlLib.dll missing?)", n, p0.maxSupportedInterpolations); return; }
    // candidates: every aligned 3 in the writable sections
    uint8_t* base = (uint8_t*) m;
    auto* nt = (IMAGE_NT_HEADERS*) (base + ((IMAGE_DOS_HEADER*) base)->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    static uint32_t* cand[4096]; size_t nc = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i)
    {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
        auto* q = (uint32_t*) (base + sec[i].VirtualAddress);
        size_t words = sec[i].Misc.VirtualSize / 4;
        for (size_t k = 0; k < words; ++k)
            if (q[k] == 3) { if (nc < 4096) cand[nc++] = &q[k]; else { FLog("  XESS_FG=%d: too many candidates, 4x", n); return; } }
    }
    // the probe: the same device, override 3x
    SetEnvironmentVariableA("XESS_FG_PROBE", "2");
    void* probe = nullptr;
    int32_t r = g_fgCreate(dev, &probe);
    SetEnvironmentVariableA("XESS_FG_PROBE", nullptr);
    if (r == 0 && probe) g_fgDestroy(probe);
    uint32_t* hit = nullptr; size_t nh = 0;
    for (size_t k = 0; k < nc; ++k) if (*cand[k] == 2) { hit = cand[k]; ++nh; }
    FLog("  XESS_FG=%d: probe context -> %d, %zu candidate(s), %zu followed the override", n, r, nc, nh);
    if (nh != 1)
    {
        for (size_t k = 0; k < nc; ++k) if (*cand[k] == 2) *cand[k] = 3;   // whatever followed the probe goes back
        FLog("  XESS_FG=%d: not unique, 4x", n);
        return;
    }
    *hit = want;
    XefgProps p1 = {};
    if (g_fgGetProps(gameCtx, &p1) == 0 && p1.maxSupportedInterpolations == want)
    {
        FLog("  XESS_FG=%d: libxess_fg+0x%llx %u -> %u, the game's context now reports %u interpolated frames", n,
             (unsigned long long) ((uint8_t*) hit - base), 3u, want, p1.maxSupportedInterpolations);
        return;
    }
    *hit = 3;
    FLog("  XESS_FG=%d: the context reports %u after the change, put back: 4x", n, p1.maxSupportedInterpolations);
}

// The raise happens once per context, at the first call after its creation: in the creation hook, or - when the
// game created the context before the hooks were in place (the shim is loaded by libxess_fg itself during that call,
// as in Wuthering Waves) - at the first of the calls a game makes next to size and set up frame generation.
static void* g_fgDevice;              // the D3D12 device of the last extension request (the probe needs one)
static void* g_fgDone[16]; static LONG g_fgNDone;
static SRWLOCK g_fgLock = SRWLOCK_INIT;
static volatile DWORD g_fgOwner;      // thread inside XefgRaiseOnce: libxess_fg may call its own exports while it
                                      // creates the probe context, and those calls must not wait for themselves
static void XefgRaiseOnce(void* dev, void* ctx)
{
    if (!ctx || g_fgOwner == GetCurrentThreadId()) return;
    AcquireSRWLockExclusive(&g_fgLock);
    g_fgOwner = GetCurrentThreadId();
    bool seen = false;
    // the last 16 contexts (g_fgNDone keeps counting past the ring's size)
    const LONG nDone = std::min<LONG>(g_fgNDone, 16);
    for (LONG i = 0; i < nDone && !seen; ++i) seen = g_fgDone[i] == ctx;
    if (!seen)
    {
        g_fgDone[g_fgNDone % 16] = ctx; ++g_fgNDone;
        if (dev) XefgRaise(dev, ctx); else FLog("  XESS_FG: no D3D12 device seen yet, 4x");
    }
    g_fgOwner = 0;
    ReleaseSRWLockExclusive(&g_fgLock);
}

typedef int32_t (*PFN_XefgD3D12Props)(void* ctx, const void* init, uint32_t w, uint32_t h, uint32_t fmt, void* props);
typedef int32_t (*PFN_XefgBuild)(void* ctx, void* lib, uint8_t blocking, uint32_t flags);
static PFN_XefgD3D12Props g_fgD3D12Props; static PFN_XefgBuild g_fgBuild;

static int32_t Hook_XefgCreate(void* dev, void** ctx)
{
    int32_t r = g_fgCreate(dev, ctx);
    FLog("xefgSwapChainD3D12CreateContext -> %d ctx %p", r, ctx ? *ctx : nullptr);
    if (r == 0 && ctx && *ctx)
    {
        g_fgDevice = dev;
        XefgRaiseOnce(dev, *ctx);
        if (g_fgLog && g_fgSetLog) FLog("  xefgSwapChainSetLoggingCallback(debug) -> %d", g_fgSetLog(*ctx, 0, XefgLogCb, nullptr));
        XefgReportMax(*ctx, "after creation");
    }
    return r;
}
static int32_t Hook_XefgGetProps(void* ctx, void* props) { XefgRaiseOnce(g_fgDevice, ctx); return g_fgGetProps(ctx, props); }
static int32_t Hook_XefgD3D12Props(void* ctx, const void* init, uint32_t w, uint32_t h, uint32_t fmt, void* props)
{
    XefgRaiseOnce(g_fgDevice, ctx);
    return g_fgD3D12Props(ctx, init, w, h, fmt, props);
}
static int32_t Hook_XefgBuild(void* ctx, void* lib, uint8_t blocking, uint32_t flags)
{
    XefgRaiseOnce(g_fgDevice, ctx);
    return g_fgBuild(ctx, lib, blocking, flags);
}

static void XefgInitBefore(const char* name, void* ctx, const void* params)
{
    XefgRaiseOnce(g_fgDevice, ctx);
    uint32_t want = params ? ((const XefgInitParams*) params)->maxInterpolatedFrames : 0;
    FLog("%s: game asks maxInterpolatedFrames %u%s, init flags 0x%x, ui mode %u", name, want,
         want == 0xFFFFFFFFu ? " (maximum supported)" : "", params ? ((const XefgInitParams*) params)->initFlags : 0u,
         params ? *(const uint32_t*) (((const XefgInitParams*) params)->rest + 40) : 0u);
}
static void XefgInitAfter(void* ctx, int32_t r)
{
    if (!g_fgLog) return;
    XefgInitParams got = {};
    int32_t g = g_fgGetInit ? g_fgGetInit(ctx, &got) : -1;
    FLog("  -> %d; initialization parameters (%d): maxInterpolatedFrames %u", r, g, got.maxInterpolatedFrames);
    XefgReportMax(ctx, "after initialization");
}
// xefgSwapChainD3D12InitFromSwapChain(ctx, queue, params)
static int32_t Hook_XefgInit(void* ctx, void* queue, const void* params)
{
    XefgInitBefore("xefgSwapChainD3D12InitFromSwapChain", ctx, params);
    int32_t r = g_fgInit(ctx, queue, params);
    XefgInitAfter(ctx, r);
    return r;
}
// xefgSwapChainD3D12InitFromSwapChainDesc(ctx, hWnd, desc, fullscreenDesc, queue, dxgiFactory, params)
typedef int32_t (*PFN_XefgInitDesc)(void* ctx, HWND wnd, const void* desc, const void* fsDesc, void* queue, void* factory, const void* params);
static PFN_XefgInitDesc g_fgInitDescFn;
static int32_t Hook_XefgInitDesc(void* ctx, HWND wnd, const void* desc, const void* fsDesc, void* queue, void* factory, const void* params)
{
    XefgInitBefore("xefgSwapChainD3D12InitFromSwapChainDesc", ctx, params);
    int32_t r = g_fgInitDescFn(ctx, wnd, desc, fsDesc, queue, factory, params);
    XefgInitAfter(ctx, r);
    return r;
}

// A game with its own multi-frame choice (Wuthering Waves: 2x/3x/4x in its menu) sets the number while it runs; with
// XESS_FG the override decides, as with Intel Graphics Software, so any number but 0 (off) becomes XESS_FG's. If
// XeSS-FG refuses it (above its maximum, e.g. when raising past 4x did not work), the game's own number goes through.
static int32_t Hook_XefgSetNum(void* ctx, uint32_t n)
{
    char b[8];
    int want = GetEnvironmentVariableA("XESS_FG", b, sizeof(b)) > 0 ? atoi(b) : 0;
    if (n > 0 && want >= 2)
    {
        uint32_t m = (uint32_t) (want > 8 ? 8 : want) - 1;
        if (m != n)
        {
            int32_t r = g_fgSetNum(ctx, m);
            FLog("xefgSwapChainSetNumInterpolatedFrames(%u): XESS_FG=%d -> %u -> %d", n, want, m, r);
            if (r == 0) return r;
        }
    }
    int32_t r = g_fgSetNum(ctx, n);
    FLog("xefgSwapChainSetNumInterpolatedFrames(%u) -> %d", n, r);
    return r;
}

// xefgSwapChainTagFrameConstants(ctx, presentId, constants): with XESS_LOG the camera a game hands to XeSS-FG, two
// frames in a row every 300 (what the header's "row-major" means in practice; FSR_FG=xess builds these itself)
struct XefgFrameConstants { float view[16], proj[16], jitterX, jitterY, mvScaleX, mvScaleY; uint32_t reset; float renderTime; };
typedef int32_t (*PFN_XefgTagConstants)(void* ctx, uint32_t id, const XefgFrameConstants* c);
static PFN_XefgTagConstants g_fgTagConstants;
static int32_t Hook_XefgTagConstants(void* ctx, uint32_t id, const XefgFrameConstants* c)
{
    static volatile LONG n;
    LONG k = InterlockedIncrement(&n);
    if (!g_scVt) ProxyFromContext(ctx);
    if (g_fgLog && c && (k % 300 == 0 || k % 300 == 1))
    {
        char b[1024]; int o = 0;
        for (int i = 0; i < 16; ++i) o += snprintf(b + o, sizeof(b) - o, "%s%.5g", i ? (i % 4 ? " " : " | ") : "", c->view[i]);
        o += snprintf(b + o, sizeof(b) - o, "   proj ");
        for (int i = 0; i < 16; ++i) o += snprintf(b + o, sizeof(b) - o, "%s%.5g", i ? (i % 4 ? " " : " | ") : "", c->proj[i]);
        FLog("constants #%ld id %u: view %s; jitter %.3f,%.3f mvScale %.1f,%.1f reset %u renderTime %.6g", k, id, b,
             c->jitterX, c->jitterY, c->mvScaleX, c->mvScaleY, c->reset, c->renderTime);
    }
    return g_fgTagConstants(ctx, id, c);
}

// xefgSwapChainD3D12TagFrameResource / xefgSwapChainSetUiCompositionState: with XESS_LOG which inputs a game hands to
// XeSS-FG (each new type, size, format and validity once)
struct XefgResData { uint32_t type, validity, baseX, baseY, w, h; ID3D12Resource* res; uint32_t state; };
typedef int32_t (*PFN_XefgTagRes)(void* ctx, void* cl, uint32_t id, const XefgResData* d);
static PFN_XefgTagRes g_fgTagRes;
static int32_t Hook_XefgTagRes(void* ctx, void* cl, uint32_t id, const XefgResData* d)
{
    int32_t r = g_fgTagRes(ctx, cl, id, d);
    if (g_fgLog && d)
    {
        static uint64_t seen[32]; static volatile LONG nSeen;
        uint32_t fmt = 0;
        if (d->res) { D3D12_RESOURCE_DESC rd = d->res->GetDesc(); fmt = (uint32_t) rd.Format; }
        uint64_t key = (uint64_t) d->type | (uint64_t) d->validity << 4 | (uint64_t) fmt << 8 | (uint64_t) d->w << 16 | (uint64_t) d->h << 40 | (uint64_t) (cl != nullptr) << 63;
        bool known = false;
        for (LONG i = 0; i < nSeen && i < 32; ++i) known |= seen[i] == key;
        // the slot is taken first: two threads past the check would otherwise both index from one count
        const LONG slot = !known && nSeen < 32 ? InterlockedIncrement(&nSeen) - 1 : 32;
        if (slot < 32)
        {
            seen[slot] = key;
            FLog("tag resource id %u: type %u validity %u base %u,%u size %ux%u format %u state 0x%x cmdlist %d -> %d", id, d->type,
                 d->validity, d->baseX, d->baseY, d->w, d->h, fmt, d->state, cl != nullptr, r);
        }
    }
    return r;
}
typedef int32_t (*PFN_XefgUiState)(void* ctx, uint32_t state);
static PFN_XefgUiState g_fgUiState;
static int32_t Hook_XefgUiState(void* ctx, uint32_t state)
{
    int32_t r = g_fgUiState(ctx, state);
    FLog("xefgSwapChainSetUiCompositionState(%u) -> %d", state, r);
    return r;
}

// xefgSwapChainSetEnabled(ctx, enable): XeSS-FG's on/off switch, the low latency's source for "frame generation on"
typedef int32_t (*PFN_XefgSetEnabled)(void* ctx, uint32_t enable);
static PFN_XefgSetEnabled g_fgSetEnabled;
static int32_t Hook_XefgSetEnabled(void* ctx, uint32_t enable)
{
    const int32_t r = g_fgSetEnabled(ctx, enable);
    if (r == 0) { g_fgSource = "xefgSwapChainSetEnabled"; InterlockedExchange(&g_fgState, enable != 0); }
    if (r == 0 && enable) ProxyFromContext(ctx);
    FLog("xefgSwapChainSetEnabled(%u) -> %d", enable, r);
    return r;
}

static bool g_fgWanted = false;
static volatile LONG g_fgHooked = 0;
static SRWLOCK g_fgHookLock = SRWLOCK_INIT;
// hooks the exports of the libxess_fg that is loaded now, once per load (Wuthering Waves loads it, unloads it and
// loads it again); returns false while there is none or it is not ready (still loading or being unloaded)
static bool XefgInstallHooks()
{
    HMODULE m = nullptr;
    // pinned while it is patched: an unload in between would leave half a hook table
    if (!GetModuleHandleExW(0, L"libxess_fg.dll", &m) || !m) return false;
    AcquireSRWLockExclusive(&g_fgHookLock);
    // hooked = this load's CreateContext jumps to our hook (a reload at the same address comes back unpatched)
    bool done = false;
    if (const uint8_t* c = (const uint8_t*) GetProcAddress(m, "xefgSwapChainD3D12CreateContext"))
        done = c[0] == 0xE9 && c + 5 + *(const int32_t*) (c + 1) == (const uint8_t*) Hook_XefgCreate;
    struct { const char* name; void* hook; void** orig; void* entry; } hooks[] = {
        { "xefgSwapChainD3D12CreateContext", (void*) Hook_XefgCreate, (void**) &g_fgCreate, nullptr },
        { "xefgSwapChainGetProperties", (void*) Hook_XefgGetProps, (void**) &g_fgGetProps, nullptr },
        { "xefgSwapChainD3D12GetProperties", (void*) Hook_XefgD3D12Props, (void**) &g_fgD3D12Props, nullptr },
        { "xefgSwapChainD3D12BuildPipelines", (void*) Hook_XefgBuild, (void**) &g_fgBuild, nullptr },
        { "xefgSwapChainD3D12InitFromSwapChain", (void*) Hook_XefgInit, (void**) &g_fgInit, nullptr },
        { "xefgSwapChainD3D12InitFromSwapChainDesc", (void*) Hook_XefgInitDesc, (void**) &g_fgInitDescFn, nullptr },
        { "xefgSwapChainSetNumInterpolatedFrames", (void*) Hook_XefgSetNum, (void**) &g_fgSetNum, nullptr },
        { "xefgSwapChainTagFrameConstants", (void*) Hook_XefgTagConstants, (void**) &g_fgTagConstants, nullptr },
        { "xefgSwapChainD3D12TagFrameResource", (void*) Hook_XefgTagRes, (void**) &g_fgTagRes, nullptr },
        { "xefgSwapChainSetUiCompositionState", (void*) Hook_XefgUiState, (void**) &g_fgUiState, nullptr },
    };
    bool ready = !done;
    for (auto& h : hooks)
        if (ready && !(h.entry = (void*) GetProcAddress(m, h.name))) ready = false;   // all of them, or none yet
    if (ready)
    {
        char path[MAX_PATH] = {}; GetModuleFileNameA(m, path, MAX_PATH);
        FLog("libxess_fg.dll at %p: %s", m, path);
        g_fgSetLog = (PFN_XefgSetLog) GetProcAddress(m, "xefgSwapChainSetLoggingCallback");
        g_fgGetInit = (PFN_XefgGetInit) GetProcAddress(m, "xefgSwapChainD3D12GetInitializationParameters");
        g_fgDestroy = (PFN_XefgDestroy) GetProcAddress(m, "xefgSwapChainDestroy");
        for (auto& h : hooks)
        {
            // PatchThunk stores the original before the jmp goes live, so the hook never sees a null original
            bool ok = PatchThunk(h.entry, h.hook, h.orig, h.name);
            FLog("%s %s", h.name, ok ? "hooked" : "NOT hooked");
        }
        // apart from the set above: a library without it keeps the other hooks, and low latency then stays off unless
        // XeLL reports frame generation
        if (void* e = (void*) GetProcAddress(m, "xefgSwapChainSetEnabled"))
            FLog("xefgSwapChainSetEnabled %s", PatchThunk(e, (void*) Hook_XefgSetEnabled, (void**) &g_fgSetEnabled, "xefgSwapChainSetEnabled") ? "hooked" : "NOT hooked");
        // and the swap chain's getter, for the game's questions to the swap chain (ProxyHook); a new library has a
        // new virtual table
        g_fgScTries = 0;
        if (void* e = (void*) GetProcAddress(m, "xefgSwapChainD3D12GetSwapChainPtr"))
        {
            const bool ok = PatchThunk(e, (void*) Hook_XefgGetSwapChainPtr, (void**) &g_fgGetSc, "xefgSwapChainD3D12GetSwapChainPtr");
            if (!ok) g_fgGetSc = (PFN_XefgGetSwapChainPtr) e;   // not hooked: still there to ask (ProxyFromContext)
            FLog("xefgSwapChainD3D12GetSwapChainPtr %s", ok ? "hooked" : "NOT hooked");
        }
        g_fgHooked = 1;
        done = true;
    }
    ReleaseSRWLockExclusive(&g_fgHookLock);
    FreeLibrary(m);
    return done;
}

// every D3D12 extension request: libxess_fg asks for the extension while it creates a context, so this is where the
// hooks go in when the library loaded the shim itself
void XefgNoteDevice(const void* dev)
{
    if (!g_fgWanted) return;
    if (dev) g_fgDevice = const_cast<void*>(dev);
    XefgInstallHooks();
}

static DWORD WINAPI XefgThread(LPVOID)
{
    HANDLE ev = WatchStart(WATCH_XEFG);
    const ULONGLONG deadline = GetTickCount64() + kWatchTotalMs;
    // the install first (the library may have loaded before the notification was in place), and once more after the
    // last wait; a wake that finds it not ready (being unloaded) goes back to waiting for the next load
    while (!XefgInstallHooks() && WatchWait(ev, deadline, 10)) {}
    if (ev) WatchStop();
    if (!g_fgHooked) FLog("libxess_fg.dll never loaded");
    return 0;
}

void StartXefgHook()
{
    // the log (IGDEXT_XEFG_LOG=1), the frame generation override (XESS_FG) or the low latency (its on/off source, after
    // StartXellHook set g_capAuto) needs the hooks
    char b[8];
    g_fgLog = GetEnvironmentVariableA("IGDEXT_XEFG_LOG", b, sizeof(b)) > 0 && atoi(b);
    bool raise = GetEnvironmentVariableA("XESS_FG", b, sizeof(b)) > 0 && atoi(b) >= 2;   // also 2..4: the game's own number
    if (!g_fgLog && !raise && !g_capAuto) return;
    g_fgWanted = true;
    if (XefgInstallHooks()) return;   // already loaded (it loaded the shim): hook now, not later
    if (!StartModuleRefThread(XefgThread)) FLog("XeFG hook thread not started (%lu)", GetLastError());
}

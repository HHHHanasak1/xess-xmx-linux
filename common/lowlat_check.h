// lowlat_check.h - the check of the low latency's wait, shared by the three frame generation paths:
//   shim/src/dll/XellHook.cpp   a game's own XeSS-FG (AutoPaceWait: the wait is in xellSleep)
//   ffxfg/ffx_xefg_lowlat.cpp   the FSR routes and OFLOW_FG (LatHold: the wait is in Present)
// Header only; needs <algorithm>, <cmath> and a printf-like log function.
//
// Both pacers hold the game back where a frame starts, by what their measurements say the frame is early. Those
// measurements do not show everything a frame can wait for (a game's render work may wait for its own compute queue
// in the middle of a frame, XeSS-FG's later images wait for the display), so the wait has to show two things itself:
// that it costs no frame rate, and that it lowers the latency.
//
// The wait is on from the start. A check takes three windows of kFrames frames each: with the wait (B1), without it
// (A, after kSettleOff frames), with it again (B2, after kSettleOn frames). The frame time with the wait is the mean of
// B1 and B2, so a scene that gets steadily heavier or lighter during the check cancels out. A check takes the wait
// away only while the frame time is steady: kWindows windows with the wait in a row, and the two halves of each,
// within kSteady x the limit of each other. And it counts only if B1 and B2 agree (within kDrift x the limit): a
// scene that changes says nothing about the wait, and the check is put off (kRetryS seconds, twice as long each
// time, kRetryMaxS at most).
//   - cost = frame time with the wait - frame time without. The limit is kCost of the frame time (kCostMs at least).
//   - gain = latency without the wait - latency with it (the caller's figure: frame start -> end of its present).
// The wait is kept if the cost is within the limit and the gain is at least kGain of the wait (less kGainMs, what
// the figure varies by). One check alone does not take the wait away: the frame time of a game that stands still
// moves by a few tenths of a millisecond within seconds, so a second check, right after it, has to say the same.
// Then a wait that costs is
// shortened (as if its cost grew with its length, by kLessMs at least) and checked again at once; when less than
// kMinMs would be left of it, or when it does not lower the latency, there is no wait for kOffS seconds, twice as
// long each time, kOffMaxS at most. A kept wait is checked again kAgainS seconds after the first time (right after
// a loading screen the frame time still settles), then every kEveryS seconds, and when the wait has become much
// longer or shorter than it was at the last check (kRegime, kRegimeS apart at least). Kept again, a shortened wait
// gets kLongerMs longer.
#pragma once
#include <algorithm>
#include <cmath>

namespace lowlat
{
typedef void (*LogFn)(const char* fmt, ...);

struct Check
{
    // the same on every path
    static constexpr int kFrames = 100, kSettleOff = 20, kSettleOn = 30, kWindows = 3;
    static constexpr double kCost = 0.01, kCostMs = 0.3, kGain = 0.5, kGainMs = 0.3;
    static constexpr double kSteady = 2.0, kDrift = 3.0;
    static constexpr double kFirstS = 5, kAgainS = 30, kEveryS = 600, kPutOffS = 5, kRetryS = 30, kRetryMaxS = 300;
    static constexpr double kOffS = 15, kOffMaxS = 300, kLessMs = 1.0, kMinMs = 1.0, kLongerMs = 0.5;
    static constexpr double kRegime = 0.5, kRegimeMs = 3.0, kRegimeS = 60;
    static constexpr double kNoWaitMs = 0.3;   // a wait below this is none

    enum State { Run, B1, A, B2, Off };
    int state = Run;
    double nextAt = -1;        // Run: the next check is due then (s); Off: the wait comes back then
    double lastAt = 0;         // the last verdict (s)
    int n = 0;                 // frames of the window so far; negative while it settles
    double sum[2] = {0, 0};    // frame time of the window's two halves
    double waitSum = 0, latSum = 0;
    int latN = 0;
    double b1 = 0, b1Wait = 0, b1Lat = 0, a = 0, aLat = 0;   // the windows done so far
    double before = -1;        // the window with the wait right before this one (< 0: none)
    int steadyN = 0;           // windows with the wait in a row that agree
    int b1LatN = 0, aLatN = 0;
    double frameMs = 0;        // the frame time, smoothed: a hitch is a frame over 1.5 x that
    double waitSlow = 0;       // the wait, smoothed
    double waitThen = -1;      // ... as it was at the last verdict
    double shortMs = 0;        // every frame starts this much earlier (what a wait that cost was shortened by)
    double idleSince = -1;     // shortened and not waiting since then (s)
    int fails = 0, kept = 0, putOff = 0, said = 0;
    bool doubt = false;        // the last check said that the wait costs or does not help: the next one has to agree
    bool on = true;            // false: no check, the wait is always allowed (the caller's switch)

    // frame generation went off (all = true), or a pause, a loading screen, a hitch: a check under way is dropped
    void Reset(bool all)
    {
        if (all) {
            const bool keep = on;
            *this = Check();
            on = keep;
            return;
        }
        if (state == B1 || state == A || state == B2) {
            state = Run;
            nextAt = -1;   // due again at once, kFirstS after frames are steady again (Frame sets it)
        }
        frameMs = 0;
    }

    // One frame. t: now (s); dtMs: this frame's time (start to start); waitedMs: the wait that was in it; latMs: the
    // latency of the newest frame that has one (<= 0: none); steady: the pacer's own measurements have settled.
    // Returns whether the pacer may wait now.
    bool Frame(double t, double dtMs, double waitedMs, double latMs, bool steady, LogFn log)
    {
        if (!on)
            return true;
        if (!(dtMs > 0 && dtMs < 250) || !steady) {
            Reset(false);
            return state != Off || t >= nextAt;
        }
        const bool usable = frameMs > 0 && dtMs < 1.5 * frameMs;
        frameMs = frameMs > 0 ? frameMs + (dtMs - frameMs) * 0.1 : dtMs;
        waitSlow += (waitedMs - waitSlow) * 0.05;
        const auto begin = [&](int st, int settle) {
            state = st;
            n = -settle;
            sum[0] = sum[1] = waitSum = latSum = 0;
            latN = 0;
        };
        // true when the window is full
        const auto window = [&]() {
            if (n < 0) {
                n++;
                return false;
            }
            if (usable) {
                sum[n < kFrames / 2 ? 0 : 1] += dtMs;
                waitSum += waitedMs;
                if (latMs > 0) {
                    latSum += latMs;
                    latN++;
                }
                n++;
            }
            return n >= kFrames;
        };
        const auto mean = [&]() { return (sum[0] + sum[1]) / kFrames; };
        switch (state) {
        case Run:
            if (nextAt < 0)
                nextAt = t + kFirstS;
            if (shortMs > 0 && waitSlow < kNoWaitMs) {
                // shortened until nothing is left: a little longer again every kRegimeS seconds
                if (idleSince < 0)
                    idleSince = t;
                else if (t - idleSince > kRegimeS) {
                    shortMs = std::max(0.0, shortMs - kLongerMs);
                    idleSince = t;
                }
            } else {
                idleSince = -1;
            }
            if (waitSlow < kNoWaitMs)
                return true;   // nothing waits: nothing to check
            if (t >= nextAt || (waitThen >= 0 && t - lastAt > kRegimeS &&
                                std::abs(waitSlow - waitThen) > std::max(kRegimeMs, kRegime * waitThen))) {
                before = -1;
                steadyN = 0;
                begin(B1, 0);
            }
            return true;
        case B1:
            if (window()) {
                const double h0 = sum[0] / (kFrames / 2), h1 = sum[1] / (kFrames - kFrames / 2);
                const double limit = std::max(kCostMs, kCost * mean());
                if (std::abs(h1 - h0) > kSteady * limit || waitSum / kFrames < kNoWaitMs) {
                    // the frame time is changing (or the wait has gone): later, without having taken the wait away
                    state = Run;
                    nextAt = t + kPutOffS;
                    return true;
                }
                // steady in itself: it also has to agree with the windows right before it
                steadyN = before >= 0 && std::abs(mean() - before) <= kSteady * limit ? steadyN + 1 : 1;
                before = mean();
                if (steadyN < kWindows) {
                    begin(B1, 0);
                    return true;
                }
                b1 = mean();
                b1Wait = waitSum / kFrames;
                b1Lat = latSum;
                b1LatN = latN;
                begin(A, kSettleOff);
                return false;
            }
            return true;
        case A:
            if (window()) {
                a = mean();
                aLat = latSum;
                aLatN = latN;
                begin(B2, kSettleOn);
                return true;
            }
            return false;
        case B2:
            if (window())
                return Verdict(t, mean(), waitSum / kFrames, log);
            return true;
        default:   // Off
            if (t < nextAt)
                return false;
            state = Run;
            nextAt = t + kFirstS;   // checked as soon as the wait is back and steady
            waitThen = -1;
            return true;
        }
    }

private:
    bool Verdict(double t, double b2, double b2Wait, LogFn log)
    {
        const double with = (b1 + b2) / 2, waited = (b1Wait + b2Wait) / 2;
        const double cost = with - a, limit = std::max(kCostMs, kCost * a);
        const int latWithN = b1LatN + latN;
        const bool haveLat = latWithN >= kFrames && aLatN >= kFrames / 2;
        const double latWith = latWithN ? (b1Lat + latSum) / latWithN : 0, latWithout = aLatN ? aLat / aLatN : 0;
        state = Run;
        lastAt = t;
        if (std::abs(b2 - b1) > kDrift * limit) {
            // the scene changed during the check: no verdict, again later (twice as long each time)
            const double retry = std::min(kRetryS * (double)(1 << std::min(putOff, 8)), kRetryMaxS);
            if (putOff++ < 3 && log)
                log("low latency: the check of the wait is put off for %.0f s, the frame time is changing (%.2f ms, %.2f ms "
                    "without the wait, %.2f ms)",
                    retry, b1, a, b2);
            nextAt = t + retry;
            return true;
        }
        putOff = 0;
        waitThen = waited;
        if (waited < kNoWaitMs) {
            nextAt = t + kAgainS;
            return true;
        }
        const bool costs = cost > limit;
        const bool noGain = !costs && haveLat && latWithout - latWith < kGain * waited - kGainMs;
        if (!costs && !noGain) {
            if (log && (said < 4 || fails || kept == 0))
                log(haveLat ? "low latency: the wait (%.1f ms) is kept: frame %.2f ms with it, %.2f ms without; latency %.1f "
                              "ms with it, %.1f ms without"
                            : "low latency: the wait (%.1f ms) is kept: frame %.2f ms with it, %.2f ms without",
                    waited, with, a, latWith, latWithout);
            said++;
            fails = 0;
            doubt = false;
            if (++kept >= 2)   // kept again at a later check: a shortened wait gets a little longer
                shortMs = std::max(0.0, shortMs - kLongerMs);
            nextAt = t + (kept == 1 ? kAgainS : kEveryS);
            return true;
        }
        if (!doubt) {
            // not on one check's word: once more, as soon as the frame time is steady again
            doubt = true;
            if (log) {
                if (costs)
                    log("low latency: the wait (%.1f ms) may cost frame time: frame %.2f ms with it, %.2f ms without; checked once "
                        "more",
                        waited, with, a);
                else
                    log("low latency: the wait (%.1f ms) may not lower the latency: %.1f ms with it, %.1f ms without; checked "
                        "once more",
                        waited, latWith, latWithout);
            }
            nextAt = t;
            return true;
        }
        doubt = false;
        kept = 0;
        const double less = std::max(kLessMs, waited * (1.0 - limit / std::max(cost, 1e-6)));
        if (costs && waited - less >= kMinMs) {
            shortMs += less;
            if (log)
                log("low latency: the wait (%.1f ms) costs frame time: frame %.2f ms with it, %.2f ms without; it is %.1f ms "
                    "shorter from here on",
                    waited, with, a, shortMs);
            nextAt = t;   // the shorter wait is checked at once
            return true;
        }
        const double off = std::min(kOffS * (double)(1 << std::min(fails, 8)), kOffMaxS);
        if (log && fails < 3) {
            if (costs)
                log("low latency: the wait (%.1f ms) costs frame time: frame %.2f ms with it, %.2f ms without; no wait for "
                    "%.0f s",
                    waited, with, a, off);
            else
                log("low latency: the wait (%.1f ms) does not lower the latency: %.1f ms with it, %.1f ms without (frame "
                    "%.2f ms with it, %.2f ms without); no wait for %.0f s",
                    waited, latWith, latWithout, with, a, off);
        }
        fails++;
        shortMs = 0;
        state = Off;
        nextAt = t + off;
        return false;
    }
};
}   // namespace lowlat

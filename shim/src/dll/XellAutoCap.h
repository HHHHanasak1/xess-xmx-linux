// XellAutoCap.h - IGDEXT_XELL_CAP=auto: keep the game's frame interval just above what the GPU can do. A GPU-limited game
// queues two or three frames, and that wait is most of its latency; a cap a little above the GPU's limit empties the
// queue (Wuthering Waves, 5x: 94 ms to 38 ms for 4.5 % of the frame rate). With a cap in place the frame time only
// shows the cap, so the signal is the time from simulation start to render submit: it drops by frames' worth when a
// queue drains and rises when one forms.
// Probe: a cap well above the frame time for a few windows. The signal drops: there was a queue, so track. It does not:
// that time is the game's own work, so no cap, and try again later (a CPU-limited game keeps running as it was).
// Track: no queue: slowly shorten the interval. A queue: back off at once, then hold before trying again.
#pragma once

class XellAutoCap
{
public:
    // one window of frames: its average frame time and simulation -> render submit time, in ms.
    // Returns the interval per rendered frame in ms; 0 = no cap.
    double Update(double frameMs, double submitMs)
    {
        if (frameMs <= 0 || submitMs < 0)
            return m_capMs;
        if (frameMs > kLoadingMs) {           // a loading screen says nothing about the game
            Reset();
            return m_capMs;
        }
        // a hitch lifts both numbers at once and looks like a queue: it says nothing about the GPU. Many in a row are a
        // new scene.
        if (m_typicalMs > 0 && frameMs > m_typicalMs * kHitch && ++m_hitches < kHitchWindows)
            return m_capMs;
        m_hitches = 0;
        m_typicalMs = m_typicalMs > 0 ? m_typicalMs + (frameMs - m_typicalMs) * kTypicalRise : frameMs;
        switch (m_state) {
        case State::Idle:
            if (--m_left <= 0)
                Probe(frameMs, submitMs);
            break;
        case State::Probe:
            if (submitMs < m_before - QueueMs(frameMs)) {
                m_floorMs = submitMs;
                m_state = State::Track;
                m_hold = kHoldWindows;
                m_clean = 0;
                m_backOffs = 0;
                m_idle = kIdleFirst;
            } else if (--m_left <= 0) {
                m_capMs = 0;
                m_state = State::Idle;
                m_left = m_idle;
                m_idle = m_idle * 2 < kIdleMax ? m_idle * 2 : kIdleMax;
            }
            break;
        case State::Track:
            Track(frameMs, submitMs);
            break;
        }
        return m_capMs;
    }

    void Reset()
    {
        m_state = State::Idle;
        m_left = 1;
        m_idle = kIdleFirst;
        m_capMs = 0;
        m_floorMs = 0;
        m_before = 0;
        m_queueCapMs = 0;
        m_ceilMs = 0;
        m_typicalMs = 0;
        m_hitches = 0;
        m_hold = 0;
        m_clean = 0;
        m_backOffs = 0;
        m_drain = 0;
    }

    double CapMs() const { return m_capMs; }

private:
    enum class State { Idle, Probe, Track };

    static double QueueMs(double frameMs)      // a queue: half a frame or more
    {
        return frameMs * 0.5 > kQueueMinMs ? frameMs * 0.5 : kQueueMinMs;
    }

    void Probe(double frameMs, double submitMs)
    {
        m_before = submitMs;
        m_capMs = Clamp(frameMs * kProbeMargin);
        m_ceilMs = frameMs * kCeiling;        // the frame rate the game has without a cap, whatever happens later
        m_state = State::Probe;
        m_left = kProbeWindows;
    }

    void Track(double frameMs, double submitMs)
    {
        // the empty-queue value: the lowest submit time, which follows a heavier scene slowly
        m_floorMs = submitMs < m_floorMs ? submitMs : m_floorMs + (submitMs - m_floorMs) * kFloorRise;
        if (submitMs > m_floorMs + QueueMs(frameMs)) {
            m_clean = 0;
            if (m_drain > 0) {                 // the last step back needs a few windows to show
                --m_drain;
                return;
            }
            if (m_backOffs >= kMaxBackOffs) {  // backing off changed nothing: ask again from the start
                m_backOffs = 0;
                Probe(frameMs, submitMs);
                return;
            }
            m_queueCapMs = m_capMs;
            m_capMs = Clamp(m_capMs * (frameMs > m_capMs * kNotBinding ? kBackOffFar : kBackOff));
            if (m_ceilMs > 0 && m_capMs > m_ceilMs)
                m_capMs = m_ceilMs;
            ++m_backOffs;
            m_hold = kHoldWindows;
            m_drain = kDrainWindows;
        } else if (m_hold > 0) {
            --m_hold;
            m_backOffs = 0;
            m_drain = 0;
        } else {
            m_backOffs = 0;
            if (frameMs > m_capMs * kNotBinding) {   // the cap is not what limits the game (a hitch, the CPU): stay
                m_clean = 0;
            } else {
                // slowly near where a queue formed last; fast above it, and well below it, where the scene got lighter
                const bool atQueue = m_capMs < m_queueCapMs * kNearAbove && m_capMs > m_queueCapMs * kNearBelow;
                ++m_clean;
                m_capMs = Clamp(m_capMs * (m_clean > kCleanFast && !atQueue ? kStepFast : kStepSlow));
            }
        }
    }

    static double Clamp(double ms) { return ms < kMinMs ? kMinMs : ms > kMaxMs ? kMaxMs : ms; }

    static constexpr double kLoadingMs = 150;      // slower than this is a loading screen
    static constexpr double kProbeMargin = 1.12;
    static constexpr double kBackOff = 1.03;
    static constexpr double kBackOffFar = 1.06;    // the frame time is well above the cap
    static constexpr double kCeiling = 1.25;
    static constexpr double kHitch = 1.5;
    static constexpr double kTypicalRise = 0.1;
    static constexpr double kStepSlow = 0.996;
    static constexpr double kStepFast = 0.98;      // after kCleanFast clean windows: the scene got lighter
    static constexpr double kNotBinding = 1.03;
    static constexpr double kNearAbove = 1.05;
    static constexpr double kNearBelow = 0.93;
    static constexpr double kQueueMinMs = 5;
    static constexpr double kFloorRise = 0.01;
    static constexpr double kMinMs = 2;
    static constexpr double kMaxMs = 100;
    static constexpr int kProbeWindows = 8;
    static constexpr int kHitchWindows = 6;
    static constexpr int kHoldWindows = 12;
    static constexpr int kCleanFast = 4;
    static constexpr int kDrainWindows = 4;
    static constexpr int kMaxBackOffs = 8;
    static constexpr int kIdleFirst = 8;
    static constexpr int kIdleMax = 240;

    State m_state = State::Idle;
    int m_left = 1;
    int m_idle = kIdleFirst;
    double m_capMs = 0;
    double m_floorMs = 0;
    double m_before = 0;
    double m_queueCapMs = 0;
    double m_ceilMs = 0;
    double m_typicalMs = 0;
    int m_hitches = 0;
    int m_hold = 0;
    int m_clean = 0;
    int m_backOffs = 0;
    int m_drain = 0;
};

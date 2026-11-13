// ffx_xefg_cap.cpp - FFX_XEFG_CAP=auto of amd_fidelityfx_framegeneration_dx12.dll: the interval of a real frame
// follows the GPU time of a frame (CapAdjust), from the samples that AfterPresent (ffx_xefg_present.cpp) collects.
#include "ffx_xefg.h"

// The GPU's actual frequency (MHz, 0 while idle) from the xe driver, or from i915 (its per-GT file, else the older
// one on the card), read through Wine's Z: drive. A capped frame lets the GPU idle, its governor lowers the clock and
// the same work takes longer: without this the cap would chase its own tail down (2026-11-06 RE4: 49 -> 35 fps).
int GpuFreqMhz()
{
    static HANDLE h = INVALID_HANDLE_VALUE;
    static bool tried = false;
    if (!tried) {
        tried = true;
        static const char* const kFiles[] = {
            "device\\tile0\\gt0\\freq0\\act_freq",   // xe
            "gt\\gt0\\rps_act_freq_mhz",             // i915
            "gt_act_freq_mhz",                       // i915, also on kernels before the per-GT files
        };
        for (int c = 0; c < 4 && h == INVALID_HANDLE_VALUE; ++c)
            for (int k = 0; k < 3 && h == INVALID_HANDLE_VALUE; ++k) {
                char path[128];
                snprintf(path, sizeof path, "Z:\\sys\\class\\drm\\card%d\\%s", c, kFiles[k]);
                h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                                nullptr);
                if (h != INVALID_HANDLE_VALUE)
                    Log("cap auto: GPU frequency from %s", path);
            }
        if (h == INVALID_HANDLE_VALUE)
            Log("cap auto: no GPU frequency (xe/i915 sysfs not readable), GPU times taken as they are");
    }
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    char b[32] = {};
    DWORD n = 0;
    SetFilePointer(h, 0, nullptr, FILE_BEGIN);
    if (!ReadFile(h, b, sizeof b - 1, &n, nullptr) || !n)
        return -1;
    return atoi(b);
}

#pragma comment(lib, "user32.lib")   // EnumDisplaySettingsA

// The display's refresh rate (FFX_XEFG_REFRESH overrides; Wine reports gamescope's output), checked again every
// 120 calls for a display that changed; 144 when nothing is known.
static double DisplayHz()
{
    static double hz = 0;
    static int calls = 0;
    if (hz > 0 && calls++ % 120)
        return hz;
    double h = 0;
    char e[32];
    if (GetEnvironmentVariableA("FFX_XEFG_REFRESH", e, sizeof e))
        h = atof(e);
    if (h <= 0) {
        DEVMODEA dm = {};
        dm.dmSize = sizeof dm;
        if (EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
            h = dm.dmDisplayFrequency;
    }
    if (h < 24 || h > 600)
        h = 144;
    if (h != hz)
        Log("cap auto: display %.0f Hz", h);
    hz = h;
    return hz;
}

// FFX_XEFG_CAP=auto, every 30 real frames with st->lock held. The real frame interval = the 90th percentile of the GPU
// time of a frame (or, without the optical flow's timer, of the frame without XeLL's sleep) + what a frame needs
// beyond that (the interpolation and the present: FFX_XEFG_CAP_PAD ms, default 1.5, then learned) + 3 %. Frames over
// 3x the median are left out (a paused game). If XeLL hardly slept while frames took longer than the interval, the GPU
// could not keep up: the interval becomes that frame time. Longer at once; shorter after 2 windows in a row, 2 % a
// step at most and not below the last GPU-bound interval, which is forgotten at 0.1 % a window (back up to it + 1 %);
// at least one frame of the display per frame shown.
void CapAdjust(ScState* st)
{
    auto med = [](std::vector<float> v) { std::sort(v.begin(), v.end()); return v[v.size() / 2]; };
    std::vector<float>& w = st->capWork;
    std::sort(w.begin(), w.end());
    float m = w[w.size() / 2];
    std::vector<float> kept;
    for (float x : w)
        if (x <= 3.0f * m)
            kept.push_back(x);
    float p90 = kept[kept.size() * 90 / 100];
    float frameMed = med(st->capFrame), sleepMed = med(st->capSleep), presentMed = med(st->capPresent);
    w.clear(), st->capFrame.clear(), st->capSleep.clear(), st->capPresent.clear();
    int mult = g_mult > 1 ? g_mult : 1;
    static float pad = -1;
    if (pad < 0) {
        char e[32];
        pad = GetEnvironmentVariableA("FFX_XEFG_CAP_PAD", e, sizeof e) ? (float)atof(e) : 1.5f;
    }
    // the GPU time at the clock it reaches when kept busy: p90 x (this window's clock / that clock)
    float fNow = st->capFreqN ? (float)(st->capFreqSum / st->capFreqN) : 0;
    st->capFreqSum = 0, st->capFreqN = 0;
    float scale = 1.0f;
    if (fNow > 0) {
        st->capFreqRef = st->capFreqRef * 0.998f;   // about 6 % a minute at 2 windows a second
        if (fNow > st->capFreqRef)
            st->capFreqRef = fNow;
        scale = fNow / st->capFreqRef;
    }
    float p90n = p90 * scale;
    // what the frame needs beyond the measured GPU time (the interpolation, the present): learned from the windows in
    // which the GPU could not keep up (XeLL hardly slept), starting from FFX_XEFG_CAP_PAD
    if (st->capPad < 0)
        st->capPad = pad;
    // only when the GPU was busy for most of the frame (a menu at the game's own 60 fps has long frames and an idle GPU);
    // with room to spare (XeLL sleeping) it shrinks slowly, so a wrong value cannot stick
    if (st->oflow && st->capUs && sleepMed < 0.3f && frameMed > p90n && p90n > 0.7f * frameMed) {
        float off = frameMed - p90n;
        if (off > 8.0f)
            off = 8.0f;
        st->capPad = 0.7f * st->capPad + 0.3f * off;
    } else if (st->oflow && sleepMed > 2.0f) {
        st->capPad = st->capPad * 0.95f;
        if (st->capPad < 0.5f)
            st->capPad = 0.5f;
    }
    float needMs = st->oflow ? p90n + st->capPad : p90n;
    uint32_t want = (uint32_t)(needMs * 1030.0f);
    // GPU-bound: XeLL hardly slept, frames took longer than the interval and the GPU's own need explains it (in a menu
    // the game's own 60 fps limit gives long frames with an idle GPU)
    if (st->capUs && sleepMed < 0.3f && frameMed * 1000.0f > st->capUs * 1.02f && needMs > 0.85f * frameMed &&
        (uint32_t)(frameMed * 1010.0f) > want) {
        want = (uint32_t)(frameMed * 1010.0f);
        st->capFloor = (float)want;   // where the GPU last fell behind: the interval creeps back down from there only
    }
    // an outside limiter (gamescope's frame limit): the game waits in Present, XeLL hardly sleeps and the GPU has room.
    // XeLL cannot take that wait over (XeSS-FG's second present waits for the limiter anyway) and a cap fighting it made
    // XeSS-FG drop generated frames (2026-11-06 Viewfinder at gamescope 120: dips to 50 every few seconds). So the cap goes
    // off and the limiter paces, until the game stops waiting in Present (it no longer reaches the limit).
    bool outside = st->oflow && sleepMed < 0.3f && presentMed > 0.3f * frameMed && needMs < 0.7f * frameMed;
    if (st->capOff) {
        // still paced from outside while the game waits in Present and the GPU has room; a GPU that needs most of the
        // frame means the game itself is the limit again (XeSS-FG's own Present takes a few ms then, so the wait in
        // Present alone kept the cap off for minutes in RE4 after its menu had looked like an outside limit)
        if (presentMed > 0.15f * frameMed && needMs < 0.8f * frameMed)
            return;
        st->capOff = false;
        st->capUs = 0;
        Log("cap auto: the outside limiter no longer paces (in Present %.2f of %.2f ms, GPU %.2f), cap back on", presentMed,
            frameMed, needMs);
    } else if (outside) {
        st->capOff = true;
        st->capUs = 0;
        st->capFloor = 0;
        xell_sleep_params_t off = {};
        off.bLowLatencyMode = 1;
        xell_result_t ro = st->xell ? p_xellSetSleepMode(st->xell, &off) : XELL_RESULT_SUCCESS;
        Log("cap auto: outside limiter at %.2f ms (in Present %.2f, GPU %.2f), cap off -> %d", frameMed, presentMed,
            needMs, (int)ro);
        return;
    }
    // The floor keeps the interval from swinging straight back into a GPU-bound stretch, and is forgotten slowly
    // (0.999 a window, about 6 % a minute). Once XeLL has slept a quarter of the frame for 4 windows in a row while the
    // GPU needs 10 % less than the floor (a lighter scene, the clock back up), it gives way 10 % a window: in Resident Evil
    // 4 a heavy stretch at 1700 MHz left it at 30.8 ms for minutes while the GPU needed 21 ms (XeLL sleeping 22 ms).
    if (st->capFloor > 0 && sleepMed > 0.25f * frameMed && (float)want < 0.9f * st->capFloor) {
        if (++st->capRoom >= 4)
            st->capFloor *= 0.9f;
    } else {
        st->capRoom = 0;
    }
    st->capFloor *= 0.999f;
    if (want < (uint32_t)st->capFloor)
        want = (uint32_t)st->capFloor;
    uint32_t lo = (uint32_t)(1e6 / DisplayHz() * mult) + 1;
    if (want < lo)
        want = lo;
    if (want > 50000)
        want = 50000;
    uint32_t cap = st->capUs;
    if (!cap || want > cap * 1.04f) {
        cap = want;
        st->capCalm = 0;
    } else if (want < cap * 0.96f) {
        if (++st->capCalm >= 2) {
            cap = want > (uint32_t)(cap * 0.98f) ? want : (uint32_t)(cap * 0.98f);
            st->capCalm = 0;
        }
    } else {
        st->capCalm = 0;
    }
    if (cap == st->capUs)
        return;
    st->capUs = cap;
    xell_sleep_params_t sp = {};
    sp.bLowLatencyMode = 1;
    sp.minimumIntervalUs = cap / mult;   // XeLL counts every frame shown
    xell_result_t r = st->xell ? p_xellSetSleepMode(st->xell, &sp) : XELL_RESULT_SUCCESS;
    static int logged;
    if (logged++ < 300 || logged % 20 == 0)
        Log("cap auto: %s p90 %.2f ms at %.0f MHz (ref %.0f) = %.2f + %.2f, frame %.2f, sleep %.2f -> %u us per real frame "
            "(%.1f fps) -> %d",
            st->oflow ? "GPU" : "CPU", p90, fNow, st->capFreqRef, p90n, st->capPad, frameMed, sleepMed, cap, 1e6 / cap, (int)r);
}

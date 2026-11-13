// ffx_xefg.h - what the source files of amd_fidelityfx_framegeneration_dx12.dll share: the headers they are built
// against, the settings, the XeSS-FG / XeLL entry points, the state of a swap chain context (ScState), the swap chain
// the game gets (FgSwapChain) and the functions that one file calls in another. None of it is exported.
//
//   ffx_xefg.cpp             the FFX API: the DLL's exports, the frame generation context (prepare, configure),
//                            FSR_FG=inject; the environment variables are listed at its top
//   ffx_xefg_init.cpp        once, at the first FFX call: the settings, the XeSS-FG and XeLL libraries; the vendor id
//                            for XeSS-FG and XeLL
//   ffx_xefg_context.cpp     a swap chain context is made (CreateSwapChainContext) and torn down (TearDown)
//   ffx_xefg_swapchain.cpp   FgSwapChain: the game's Present, the answers to what the game asks every frame, "HDR10"
//   ffx_xefg_present.cpp     before and after each Present: the frame's id, the XeLL markers, the optical flow's
//                            inputs; the hook on the D3D12 queues' ExecuteCommandLists
//   ffx_xefg_cap.cpp         FFX_XEFG_CAP=auto
//   ffx_xefg_lowlat.cpp      "Low latency" (IGDEXT_XELL_CAP) and the frame latency waitable object
//   ffx_xefg_shown.cpp       "The shown frames": the real swap chain's Present
//   ffx_trace.h, .cpp        FFX_XEFG_TRACE (a debug build only)
// A comment that names "Low latency", "HDR10" or "The shown frames" means the section with that title in these files.
//
// State that several of these files use is defined here, as C++17 inline variables (one per DLL); what one file alone
// uses is static in that file.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <intrin.h>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <algorithm>
#include <vector>

#pragma warning(disable : 4996)
#include "inc/ffx/api/include/ffx_api.h"
#include "inc/ffx/api/include/ffx_api_types.h"
#include "inc/ffx/api/include/dx12/ffx_api_dx12.h"
#include "inc/ffx/framegeneration/include/ffx_framegeneration.h"
#include "inc/ffx/framegeneration/include/dx12/ffx_api_framegeneration_dx12.h"
#include "xefg_bridge.h"
#include "inc/xess/xess_fg/xefg_swapchain.h"
#include "inc/xess/xess_fg/xefg_swapchain_d3d12.h"
#include "inc/xess/xell/xell.h"
#include "inc/xess/xell/xell_d3d12.h"
#include "ffx_oflow.h"
#include "ffx_common.h"   // Log, EnvInt, InitLogAndEnv, ToD3D12State
#include "../common/lowlat_check.h"

#pragma comment(lib, "dxgi.lib")

// ---------------------------------------------------------------------------------------------------------------
// Small helpers (the log is in ffx_common.h)

static uint64_t Qpc()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)t.QuadPart;
}
static float QpcMs(uint64_t a, uint64_t b) { return (float)((double)(b - a) * 1000.0 / (double)g_qpf.QuadPart); }

// TR(...): only in a debug build (FFXFG_DEBUG): the calls of FFX_XEFG_TRACE, and the reads of the DBG_SWITCH variables.
// DBG_SWITCH: a switch that only a debug build reads from the environment (build.bat with FFXFG_DEBUG=1). In a release
// build it is a constant at its default, and the compiler leaves out the code that the switch would turn on.
#ifdef FFXFG_DEBUG
#include "ffx_trace.h"
#define TR(...) __VA_ARGS__
#define DBG_SWITCH
#else
#define TR(...)
#define DBG_SWITCH const
#endif

// ---------------------------------------------------------------------------------------------------------------
// XeSS-FG / XeLL entry points, loaded at run time

#define XEFG_FUNCS(X)                                \
    X(xefgSwapChainD3D12CreateContext, true)         \
    X(xefgSwapChainD3D12InitFromSwapChain, true)     \
    X(xefgSwapChainD3D12InitFromSwapChainDesc, true) \
    X(xefgSwapChainD3D12GetSwapChainPtr, true)       \
    X(xefgSwapChainD3D12TagFrameResource, true)      \
    X(xefgSwapChainTagFrameConstants, true)          \
    X(xefgSwapChainSetEnabled, true)                 \
    X(xefgSwapChainSetPresentId, true)               \
    X(xefgSwapChainGetProperties, true)              \
    X(xefgSwapChainGetLastPresentStatus, true)       \
    X(xefgSwapChainSetLoggingCallback, true)         \
    X(xefgSwapChainDestroy, true)                    \
    X(xefgSwapChainSetLatencyReduction, true)        \
    X(xefgSwapChainGetVersion, true)                 \
    X(xefgSwapChainSetNumInterpolatedFrames, false)  \
    X(xefgSwapChainSetUiCompositionState, false)

#define XELL_FUNCS(X)              \
    X(xellD3D12CreateContext, true) \
    X(xellDestroyContext, true)     \
    X(xellSetSleepMode, true)       \
    X(xellSleep, true)              \
    X(xellAddMarkerData, true)

#define DECLARE_FN(name, req) inline decltype(&name) p_##name;
XEFG_FUNCS(DECLARE_FN)
XELL_FUNCS(DECLARE_FN)

inline bool g_libsOk;
inline int g_mult = 2;
inline int g_xefgLogLevel = 2;
// Measuring aids and experiments that only a debug build reads (DBG_SWITCH):
//   FFX_XEFG_SLEEP=0     no xellSleep (XeLL stays enabled, as XeSS-FG requires)
//   FFX_XEFG_OWNWAIT=0   hand the game the proxy's frame latency waitable object (a game that waits on it before each
//                        frame can deadlock, for example CONTROL Resonant)
//   FFX_XEFG_PACE=1      release our waitable object when the GPU finished the frame, not when Present returns
//   FFX_XEFG_VALIDITY=0  tag depth and motion vectors UNTIL_NEXT_PRESENT instead of ONLY_NOW (no copy; can flicker)
//   FFX_XEFG_MVNDC=1     motion vectors in NDC (init flag USE_NDC_VELOCITY), as Cyberpunk 2077 passes them
//   FFX_XEFG_AB=s        the low latency's wait is off and on in turns of s seconds (off first); with
//                        FFX_XEFG_AB_OLD=1 three turns: as before (answers from the swap chain, no wait), the answers
//                        as of the last Present, the wait too
//   FFX_XEFG_HOLD=ms     a fixed wait in the place of the one the low latency works out
inline DBG_SWITCH bool g_sleep = true;
inline DBG_SWITCH bool g_ownWait = true;
inline DBG_SWITCH bool g_pace = false;
inline bool g_noExclusive = true;
inline int g_oflow = 0;          // FFX_XEFG_OFLOW=1 (see the top of ffx_xefg.cpp)
inline float g_oflowDepth = 1.0f;
inline bool g_times = false;     // FFX_XEFG_TIMES=1
inline bool g_capAuto = false;   // FFX_XEFG_CAP=auto (the default with FFX_XEFG_OFLOW)
// The low latency (ffx_xefg_lowlat.cpp), from IGDEXT_XELL_CAP as the XMX shim reads it: 1 = auto (the default): the
// game is kept in Present until its next frame should start; 2 = <us>: no wait of ours, XeLL's frame cap (the shim
// sets it) sleeps there; 0 = off / anything else
inline int g_lat = 1;
inline double g_latMarginMs = 0;   // IGDEXT_XELL_CAP=auto:<ms>: each frame starts <ms> earlier (later if negative)
// FFX_XEFG_CAP is set (auto, the default with FFX_XEFG_OFLOW, or <us>): its XeLL interval holds the frames back, no
// wait of ours on top (CapAdjust reads XeLL's sleep to tell whether its interval holds frames back; a wait in front
// of that sleep would hide it)
inline bool g_ffxCap = false;
inline bool g_pollCache = false;    // IGDEXT_XELL_POLLS (see the top of ffx_xefg.cpp)
inline DBG_SWITCH int g_abSec = 0;       // FFX_XEFG_AB
inline DBG_SWITCH bool g_abOld = false;  // FFX_XEFG_AB_OLD
inline DBG_SWITCH float g_holdMs = 0;    // FFX_XEFG_HOLD
inline bool g_latCheck = true;      // IGDEXT_XELL_CHECK=0: the wait is not checked
inline bool g_hdr10 = false;        // FFX_XEFG_HDR10=1: XeSS-FG is told when the back buffer is HDR10 ("HDR10")
inline DBG_SWITCH int g_hdr10AbSec = 0;   // FFX_XEFG_HDR10_AB=s: told and not told in turns of s seconds, told first
inline bool g_hudless = true;   // FFX_XEFG_HUDLESS=0: no HUD-less color, no UI composition
inline int g_camera = 1;        // the game's camera as view and projection matrices (FFXFG_DEBUG: FFX_XEFG_CAMERA)
inline DBG_SWITCH bool g_mvNdc = false;   // FFX_XEFG_MVNDC
#ifdef FFXFG_DEBUG
// debug build (build.bat with FFXFG_DEBUG=1): change or leave out one of the game's inputs at a time, to find the one
// behind an artifact
//   FFX_XEFG_CAMERA=0: identity view and projection matrices, 2: transposed, 3: projection not inverted, 4: both,
//                      5: all zero, 6: the game's camera ignored (a fixed view, the real projection)
//   FFX_XEFG_JITTER=0: no jitter, -1: jitter negated
//   FFX_XEFG_DUMP=<n>: depth and motion vectors of every n-th frame into files (8 at most)
//   FFX_XEFG_RENDERTIME=<ms>: XeSS-FG's frame render time instead of the game's frame time
//   FFX_XEFG_RENDERSCALE=<f>: the game's frame time times this (0.001: seconds)
//   FFX_XEFG_ORDER=1: the order of the game's calls with their frame ids, for the first 300 after interpolation starts
//   FFX_XEFG_TRACE_SUBS=1 (with FFX_XEFG_TRACE): when the GPU finished each submission to a D3D12 queue, too
//   FFX_XEFG_TRACE=1: the time line of every frame as events in ffx_xefg_trace.txt (ffx_trace.h; tools/fsr-trace.py
//                     reads it); FFX_XEFG_TRACE_STACKS=<n>: where the present thread waits after a Present, n samples
inline float g_jitter = 1.0f;
inline int g_dumpEvery = 0;
inline float g_renderTime = -1;
inline float g_renderScale = 1;
#endif
inline DBG_SWITCH bool g_validityNow = true;   // FFX_XEFG_VALIDITY
inline char g_dir[MAX_PATH];    // the DLL's folder (the game's)

// ---------------------------------------------------------------------------------------------------------------
// Contexts

enum : uint32_t { MAGIC_FG = 0x46474658, MAGIC_SC = 0x43534658 };  // "XFGF", "XFSC"

struct FgSwapChain;

struct ScState {
    uint32_t magic = MAGIC_SC;
    std::mutex lock;
    std::atomic<int> refs{1};  // the FFX context; the FgSwapChain adds one

    xefg_swapchain_handle_t xefg = nullptr;
    xell_context_handle_t xell = nullptr;
    IDXGISwapChain4* proxy = nullptr;
    ID3D12Device* device = nullptr;
    FgSwapChain* wrapper = nullptr;
    FgSwapChain* contextRef = nullptr;  // the context's own reference to the wrapper, as FFX's context holds one
    Oflow* oflow = nullptr;             // FFX_XEFG_OFLOW
    uint64_t oflowLastQpc = 0;          // FFX_XEFG_OFLOW with FSR_FG=inject: the last present, for the frame time
    uint64_t oflowTags = 0;
    std::atomic<int> colorSpace{-1};    // the last successful SetColorSpace1, -1 = none (FFX_XEFG_OFLOW's encoding)
    // HDR10 (ffx_xefg_swapchain.cpp), all under hdrLock: what XeSS-FG's interpolation takes the back buffer for
    std::mutex hdrLock;
    bool hdrTold = false;               // for HDR10
    bool hdrSet = false;                // the game set HDR10 since the buffers were last resized
    bool hdrKept = false;               // the real swap chain took the colour space meant to be refused: no more tries
    bool hdrSaid = false;
    uint64_t hdrAbStart = 0;            // FFX_XEFG_HDR10_AB (present thread)
    int hdrAbTurn = -1;                 // 0 told, 1 not told
    // FFX_XEFG_TIMES=1: frame times at the present, reported every 300 frames
    uint64_t tStart = 0, tLastEnd = 0;
    std::vector<float> tFrame, tPresent, tSleep;
    // FFX_XEFG_CAP=auto: the real frame interval follows what the game needs (its frame time without XeLL's sleep)
    std::vector<float> capWork, capFrame, capSleep, capPresent;
    double capFreqSum = 0;    // MHz of the busy samples in this window
    int capFreqN = 0;
    float capFreqRef = 0;     // the clock the GPU reaches when it is kept busy (slowly forgotten)
    float capPad = -1;        // learned extra ms per frame (CapAdjust)
    float capFloor = 0;       // us, the last GPU-bound interval, slowly forgotten
    bool capOff = false;      // an outside limiter paces, no cap of ours
    uint32_t capUs = 0;       // per real frame, 0 = none
    uint64_t capLastEnd = 0;
    int capCalm = 0;          // windows in a row that would allow a shorter interval
    int capRoom = 0;          // windows in a row with the GPU well below capFloor and XeLL sleeping
    uint32_t width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t maxInterpolations = 1;

    // Objects handed to games that drive the interpolation dispatch themselves, made on the first query.
    bool interpMade = false;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* cmdList = nullptr;
    ID3D12Resource* interpTex = nullptr;

    // Per-frame state.
    bool gameEnabled = false;
    bool xefgEnabled = false;
    bool havePrepare = false;     // PrepareV2 seen since the last present
    bool uiComposition = false;
    uint32_t frameId = 0;          // id of the frame being built
    uint64_t presents = 0;
    // XeLL wants, per frame id, xellSleep first and then the six markers once each in order. The game prepares and
    // presents on different threads, some prepare frame N+1 before presenting frame N, and FFX frame ids can repeat
    // or restart, so what went out is kept per id in a small ring (no allocation): a marker asked for out of order
    // fills in the ones before it, a repeated marker is not sent again.
    struct XellFrame {
        uint32_t id = 0;
        bool used = false;
        bool sleeping = false;   // xellSleep(id) in flight outside the lock: markers for the id wait for it
        bool slept = false;      // xellSleep done, or forgone because a marker went out first
        uint8_t wanted = 0, sent = 0;   // bit (1 << xell_latency_marker_type_t)
    };
    XellFrame xellFrames[8];
    unsigned xellNext = 0;   // the slot taken next
    struct FfxApiResource ui = {};
    // Frame latency waitable object of our own, so the game's wait follows our presents instead of XeSS-FG's
    // presentation thread. FFX_XEFG_PACE=1 (default 0, no gain measured in CONTROL Resonant): released when the GPU has finished the presented frame, like
    // DXGI's own object: after each Present the game's queue signals `fence`, and `paceThread` releases the semaphore
    // when the value is reached, or after 200 ms regardless (a stuck wait is the worst case: CONTROL Resonant
    // deadlocked on the proxy's object). FFX_XEFG_PACE=0, the default: released as soon as Present returns.
    HANDLE latencySem = nullptr;
    UINT maxLatency = 1;
    ID3D12CommandQueue* queue = nullptr;   // the game's, given at creation (XeSS-FG presents on it)
    ID3D12Fence* fence = nullptr;          // made with the thread on the first GetFrameLatencyWaitableObject
    HANDLE paceEvent = nullptr;            // a new signal, a fence completion, or stop
    HANDLE paceThread = nullptr;
    std::atomic<bool> paceStop{false};
    std::mutex paceLock;                   // around signal + publish of fenceValue (presents should not overlap, but)
    std::atomic<uint64_t> fenceValue{0};   // last value signalled on the queue
    uint64_t paceReleased = 0;             // values the thread has released the semaphore for (thread only)
    uint64_t watchdogs = 0;                // releases by the 200 ms watchdog (thread only)
    BOOL fakeFullscreen = FALSE;   // what the game asked for while exclusive fullscreen is blocked
    void* realSc = nullptr;   // the real swap chain XeSS-FG made (ffx_xefg_shown.cpp)

    // The low latency (LatHold), on the present thread
    struct LatCtl {
        double hold = 0;             // ms the game is kept in Present after the proxy's Present returned
        double prevEarly = 0;        // how much later the frame before could have started (ms)
        int unheld = 0;              // frames in a row that could not have started later
        uint64_t lastEnd = 0;        // the last return of the proxy's Present (QPC)
        double frameMs = 0;          // the interval of those returns, smoothed
        double lateMs = 1.0;         // late wake-ups of the wait (WaitUntil)
        bool on = false;
        uint64_t abStart = 0;        // FFX_XEFG_AB: 0 = as before, 1 = answers as of the last Present, 2 = our wait too
        int abTurn = -1, abSaid = -1;
        // the GPU's side (LatGpu): how long before the GPU gets free a frame's first work reaches it (ms, smoothed;
        // negative: the GPU idles first), the GPU's time for a frame (ms, smoothed), the frames they are from
        double aheadMs = 0, gpuMs = 0;
        int gpuN = 0;
        uint64_t prevFirst = 0, prevStart = 0;   // of the frame before: its first submission, its start
        // The check of the wait (common/lowlat_check.h): whether the frame may wait now, the wait as it was when the
        // check took it away (it comes back as it was), and the wait that was in the frame just measured
        lowlat::Check chk;
        bool chkAllowed = true;
        double holdKept = 0, lastHeld = 0;
        // the log, every 300 presents
        int logN = 0, logWaited = 0, logLatN = 0, logGpuN = 0;
        double logWait = 0, logLat = 0, logBlock = 0, logFrame = 0, logGpuLat = 0;
    } lat;
    // The GPU's side of the low latency: a fence on the game's queue, signalled after each proxy Present (the frame's
    // interpolation is the last thing on that queue), and a thread that notes when the GPU reaches it (LatGpuThread)
    struct LatGpu {
        ID3D12Fence* fence = nullptr;
        HANDLE event = nullptr, thread = nullptr;
        std::atomic<bool> stop{false};
        std::atomic<uint64_t> signalled{0};
        std::atomic<uint64_t> doneAt[16];   // when the GPU reached value v (QPC), at [v % 16]; 0 = not yet
    } latGpu;
    // when the game's frame began: the last return of Present to the game, or of a call of the game into the swap
    // chain that waited there
    std::atomic<uint64_t> frameStart{0};
    // What the game asks of the swap chain every frame, answered as of its last Present call (IGDEXT_XELL_POLLS, see
    // "Low latency" in ffx_xefg_lowlat.cpp): the frame statistics, and the support of the color spaces it asked about
    struct ColorSpaceAnswer {
        DXGI_COLOR_SPACE_TYPE cs;
        HRESULT hr;
        UINT support;
        bool asked;       // since the last Present: asked again there
        uint32_t idle;    // Presents in a row without a question
    };
    std::mutex pollLock;
    bool pollValid = false;
    DXGI_FRAME_STATISTICS stats = {};
    HRESULT statsHr = E_FAIL;
    ColorSpaceAnswer colorSpaces[8] = {};
    int nColorSpaces = 0;
    std::atomic<int> waitedSaid{0};   // log lines about calls of the game that waited in the swap chain
#ifdef FFXFG_DEBUG
    tr::GpuMark *gpu1 = nullptr, *gpu2 = nullptr, *gpu3 = nullptr;   // FFX_XEFG_TRACE
#endif

    // FSR_FG=inject (xefg_bridge.h): the game knows nothing of frame generation, so the ids are ours. A frame's upscale
    // can be recorded before the previous frame is presented; presents take the upscaled frames in order.
    bool bridge = false;
    uint32_t nextId = 0;
    std::deque<uint32_t> pending;
};

inline std::mutex g_globalLock;
inline ScState* g_currentSc;     // the last created live swap chain context
inline std::atomic<ID3D12CommandQueue*> g_timedQueue{nullptr};   // FFX_XEFG_CAP=auto (HookEcl)
inline std::atomic<ScState*> g_timedSc{nullptr};
inline std::mutex g_timedLock;   // g_timedSc against TearDown: HookEcl uses the state under it

// The shown frames (ffx_xefg_shown.cpp): the real swap chain's Presents on the game's and XeSS-FG's presentation thread
inline struct EvenState {
    std::atomic<void*> real{nullptr};       // the swap chain XeSS-FG presents on
    std::atomic<int> mult{2};               // images per rendered frame
    std::atomic<int> image{0};              // the newest image's index
    // FFX_XEFG_TIMES: the Present calls of the shown frames
    std::mutex lock;
    uint64_t lastCall = 0;
    float gMin = 1e9f, gMax = 0;            // gaps in the rendered frame under way
    int gN = 0;
    std::vector<float> gaps, ratios;
    int frames = 0;
} g_ev;

#ifdef FFXFG_DEBUG
// FFX_XEFG_TRACE: the mark signalled in front of the game's first submission of a frame, and that frame's present number
inline std::atomic<tr::GpuMark*> g_tracePre{nullptr};
inline std::atomic<uint64_t> g_traceNext{0};
#endif
// the low latency's swap chain: its game queue, and the game's first submission on it since Present returned (QPC,
// 0 = none yet; HookEcl)
inline std::atomic<ID3D12CommandQueue*> g_latQueue{nullptr};
inline std::atomic<uint64_t> g_latFirstSubmit{0};

// The hook on the D3D12 queues' ExecuteCommandLists (HookEcl in ffx_xefg_present.cpp): the function it took the place
// of (null: not hooked), and where the present thread has it note XeSS-FG's submissions inside the proxy's Present
typedef void(STDMETHODCALLTYPE* PFN_ExecuteCommandLists)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
inline PFN_ExecuteCommandLists g_origEcl;
inline thread_local uint64_t* t_eclSpan = nullptr;   // [0] first, [1] last submission (QPC), [2] their number
#ifdef FFXFG_DEBUG
inline std::atomic<ID3D12CommandQueue*> g_traceQueue{nullptr};   // FFX_XEFG_TRACE: the game's queue
#endif

// The real swap chain (ffx_xefg_shown.cpp)
inline thread_local int t_pxImages = -1;   // inside the proxy's Present: the images presented there so far; else -1
// while a swap chain context is made (CreateSwapChainContext): the swap chain that XeSS-FG creates then
inline std::atomic<int> g_capturing{0};
inline std::atomic<void*> g_captured{nullptr};   // the swap chain made while capturing
inline void** g_ownVt = nullptr;                 // FgSwapChain's: never taken for the real one

#ifdef FFXFG_DEBUG
// FFX_XEFG_ORDER=1: the order of the game's calls with their frame ids, for the first 300 after interpolation starts
inline std::atomic<int> g_orderLeft{-1};
static void Order(const char* what, uint64_t id)
{
    if (g_orderLeft.load() > 0 && g_orderLeft.fetch_sub(1) > 0)
        Log("order %s %llu", what, (unsigned long long)id);
}
#else
static inline void Order(const char*, uint64_t) {}
#endif

// ---------------------------------------------------------------------------------------------------------------
// What one file calls in another (each is described where it is defined)

// ffx_xefg_init.cpp
bool PinSelf();
void Init();
void IntelVendorForXess(ID3D12Device* dev);
// ffx_xefg.cpp
uint32_t XefgInitFlags();
// ffx_xefg_context.cpp
enum class ScKind { Wrap, New, ForHwnd };
ffxReturnCode_t CreateSwapChainContext(ffxContext* context, ScKind kind, const ffxApiHeader* desc);
void TearDown(ScState* st);
// ffx_xefg_present.cpp
void XellMarker(ScState* st, uint32_t id, xell_latency_marker_type_t type);
void OflowTag(ScState* st, uint32_t id, bool reset, float frameTimeMs);
bool HookEclVt(ID3D12CommandQueue* queue);
uint32_t BeforePresent(ScState* st);
void AfterPresent(ScState* st, uint32_t id, HRESULT hr, UINT sync, UINT flags, uint64_t tEnd, float blockMs,
                  uint64_t first);
// ffx_xefg_cap.cpp
int GpuFreqMhz();
void CapAdjust(ScState* st);
// ffx_xefg_lowlat.cpp
void PaceStart(ScState* st);
void FrameDone(ScState* st, HRESULT hr);
bool PaceStop(ScState* st);
void LatGpuStart(ScState* st);
bool LatGpuStop(ScState* st);
void LatGpuSignal(ScState* st);
float LatHold(ScState* st, uint64_t tEnd, float blockMs, uint64_t first, bool ok);
// ffx_xefg_shown.cpp
void HookFactory();
#ifdef FFXFG_DEBUG
void CheckRealVt(const char* when);
#endif

// a swap chain context's reference goes; the last one tears it down
static void ScRelease(ScState* st)
{
    if (--st->refs == 0)
        TearDown(st);
}

// The swap chain the game gets: forwards everything to the XeSS-FG proxy, adds the XeLL markers and the present id.
// The member functions are in ffx_xefg_swapchain.cpp, except the two that stay inline here.
struct FgSwapChain final : IDXGISwapChain4 {
    std::atomic<ULONG> ref{1};
    ScState* st;
    IDXGISwapChain4* sc;

    FgSwapChain(ScState* s) : st(s), sc(s->proxy) { st->refs++; }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref; }
    ULONG STDMETHODCALLTYPE Release() override;
    // IDXGIObject
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID g, UINT n, const void* d) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID g, const IUnknown* u) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID g, UINT* n, void* d) override;
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** p) override;
    // IDXGIDeviceSubObject
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** p) override;
    // IDXGISwapChain
    HRESULT STDMETHODCALLTYPE Present(UINT sync, UINT flags) override;
    HRESULT DoPresent(UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp, bool one);
    float PollAnswers();
    void PollForget();
    void Waited(const char* what, int id, uint64_t t0);
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT i, REFIID riid, void** p) override;
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL f, IDXGIOutput* o) override;
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* f, IDXGIOutput** o) override;
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* d) override;
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl) override;
    void Resized();
    void Recheck();
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* m) override;
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** o) override;
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* s) override;
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* c) override;
    // IDXGISwapChain1
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* d) override;
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* d) override;
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND* h) override;
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID riid, void** p) override;
    HRESULT STDMETHODCALLTYPE Present1(UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp) override;
    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override;
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** o) override;
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* c) override;
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* c) override;
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION r) override;
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* r) override;
    // IDXGISwapChain2
    HRESULT STDMETHODCALLTYPE SetSourceSize(UINT w, UINT h) override;
    HRESULT STDMETHODCALLTYPE GetSourceSize(UINT* w, UINT* h) override;
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT l) override;
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* l) override;
    HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override;
    HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F* m) override;
    HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F* m) override;
    // IDXGISwapChain3
    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override;
    HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE c, UINT* s) override;
    HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE c) override;
    bool HdrWant() const;
    void HdrSync();
    void HdrResized();
    void HdrAb();
    HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl, const UINT* mask,
                                             IUnknown* const* queues) override;
    // IDXGISwapChain4
    HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE t, UINT n, void* d) override;
};

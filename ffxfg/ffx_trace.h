// ffx_trace.h - FFX_XEFG_TRACE=1 (a debug build, measuring aid): the time line of every frame as events in
// ffx_xefg_trace.txt next to the DLL, to see where a game's frame starts and where it waits. One line per event:
//   <microseconds since the DLL started> <thread id> <type> <n> <a> <b>
// The game's Present call and return, the proxy's Present inside it, every Present of the real swap chain under
// XeSS-FG (the frames as they go to the display, generated ones included), every ExecuteCommandLists and Signal on a
// D3D12 queue, fence waits, the FFX prepare / configure calls, when the GPU reached the frame's Present (a fence of
// ours), and the first call of each message / input API per thread after each Present call and return (where the game
// reads its input). tools/fsr-trace.py reads the file.
// Included by ffx_xefg.h, after Log() and the Qpc helpers: the events' types, Put, and what the DLL's files use of
// the trace. The rest is in ffx_trace.cpp.
#pragma once

namespace tr {

enum Type : uint16_t {
    GP_CALL = 1,   // the game calls Present                      a = frame id, b = present number
    PX_CALL = 2,   // the proxy's Present is called
    PX_RET = 3,    // ... returned                                a = hr
    GP_RET = 4,    // Present returns to the game                 a = wait of ours in us (the low latency), b = its limit
    RP_CALL = 5,   // the real swap chain's Present is called     a = sync interval, b = flags
    RP_RET = 6,    // ... returned                                a = hr
    ECL = 7,       // ExecuteCommandLists                         a = lists, b = 1 the game's queue, n = us inside (max 65535)
    SIG = 8,       // ID3D12CommandQueue::Signal                  a = fence, b = value
    FWAIT = 9,     // ID3D12Fence::SetEventOnCompletion           a = fence, b = value, n = 1 with an event (else us inside)
    PREP = 10,     // FFX prepare dispatch / bridge frame         a = frame id
    CONF = 11,     // FFX configure                               a = frame id, b = frame generation enabled
    GPU1 = 12,     // the GPU reached the game's Present call     a = present number
    GPU2 = 13,     // the GPU reached the proxy Present's return  a = present number
    INPUT = 14,    // first call of a message / input API by this thread since the last GP_CALL / GP_RET:
                   //                                             a = API (kApiNames), b = its calls by this thread since
    XSLEEP = 15,   // xellSleep returned                          a = frame id, b = us inside
    NOTE = 16,     // a = what (1 = low latency wait switched, b = on)
    WAITOBJ = 17,  // GetFrameLatencyWaitableObject
    SCCALL = 18,   // another swap chain method called by the game: a = which (1 GetBuffer, 2 GetCurrentBackBufferIndex,
                   //                                             3 ResizeBuffers, 4 GetDesc*), n = us inside
    GPU3 = 19,     // the queue and the GPU reached the game's first submission of the frame that ends with present a
    SUBDONE = 20,  // FFX_XEFG_TRACE_SUBS: the GPU finished submission a of queue b (ECL: b = flags | queue << 4 | a << 8)
    SUBQ = 21,     // a queue seen by FFX_XEFG_TRACE_SUBS: a = its pointer, b = its type (0 direct, 2 compute, 3 copy), n = index
};

struct Ev {
    uint64_t qpc;
    uint32_t tid;
    std::atomic<uint16_t> type;   // written last: 0 = not filled yet
    uint16_t n;
    uint64_t a, b;
};

inline bool g_on = false;
inline Ev* g_ev = nullptr;
static const uint32_t kCap = 3000000;
inline std::atomic<uint32_t> g_n{0};
inline std::atomic<uint32_t> g_seq{1};   // moves at every GP_CALL and GP_RET
inline thread_local bool t_own = false;   // our own Signal / SetEventOnCompletion calls are not events
// the stack sampler (ffx_trace.cpp): the present thread and its newest event
inline std::atomic<bool> g_stackOn{false};
inline std::atomic<DWORD> g_stackTid{0};
inline std::atomic<uint64_t> g_stackEvent{0};

static inline void Put(uint16_t type, uint64_t a = 0, uint64_t b = 0, uint16_t n = 0, uint64_t qpc = 0)
{
    if (!g_on)
        return;
    uint32_t i = g_n.fetch_add(1, std::memory_order_relaxed);
    if (i >= kCap) {
        g_n.store(kCap, std::memory_order_relaxed);
        return;
    }
    Ev& e = g_ev[i];
    if (!qpc) {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        qpc = (uint64_t)t.QuadPart;
    }
    e.qpc = qpc;
    e.tid = GetCurrentThreadId();
    if (g_stackOn.load(std::memory_order_relaxed) && e.tid == g_stackTid.load(std::memory_order_relaxed))
        g_stackEvent.store(qpc, std::memory_order_relaxed);
    e.n = n;
    e.a = a;
    e.b = b;
    e.type.store(type, std::memory_order_release);
}

// FFX_XEFG_TRACE_SUBS=1: when the GPU finished each submission, too (SubSignal)
inline bool g_subs = false;
inline std::atomic<uint64_t> g_presRet{0};       // QPC of the newest GP_RET, 0 while inside Present

// ffx_trace.cpp
struct GpuMark;   // when the GPU reaches a point of the game's queue
void Start(const char* dir);
void HookInputs();
void HookQueueVt(ID3D12CommandQueue* q);
GpuMark* GpuMarkCreate(ID3D12Device* dev, uint16_t type);
void GpuMarkSignal(GpuMark* g, ID3D12CommandQueue* q, uint64_t value);
void GpuMarkDestroy(GpuMark* g);
uint64_t SubSignal(ID3D12CommandQueue* q, int* index);

}   // namespace tr

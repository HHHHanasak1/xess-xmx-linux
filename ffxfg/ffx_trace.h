// ffx_trace.h - FFX_XEFG_TRACE=1 (ffx_xefg.cpp, measuring aid): the time line of every frame as events in
// ffx_xefg_trace.txt next to the DLL, to see where a game's frame starts and where it waits. One line per event:
//   <microseconds since the DLL started> <thread id> <type> <n> <a> <b>
// The game's Present call and return, the proxy's Present inside it, every Present of the real swap chain under
// XeSS-FG (the frames as they go to the display, generated ones included), every ExecuteCommandLists and Signal on a
// D3D12 queue, fence waits, the FFX prepare / configure calls, when the GPU reached the frame's Present (a fence of
// ours), and the first call of each message / input API per thread after each Present call and return (where the game
// reads its input). tools/fsr-latency-trace.py reads the file.
// Included once, by ffx_xefg.cpp, after its Log() and Qpc helpers.
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

static bool g_on = false;
static Ev* g_ev = nullptr;
static const uint32_t kCap = 3000000;
static std::atomic<uint32_t> g_n{0};
static std::atomic<uint32_t> g_seq{1};   // moves at every GP_CALL and GP_RET
static HANDLE g_writer = nullptr;
static std::atomic<bool> g_stop{false};
static FILE* g_file = nullptr;
static thread_local bool t_own = false;   // our own Signal / SetEventOnCompletion calls are not events
// the stack sampler (below): the present thread and its newest event
static std::atomic<bool> g_stackOn{false};
static std::atomic<DWORD> g_stackTid{0};
static std::atomic<uint64_t> g_stackEvent{0};

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

static DWORD WINAPI Writer(void*)
{
    uint32_t done = 0;
    for (;;) {
        bool stop = g_stop.load();
        uint32_t n = std::min(g_n.load(), kCap);
        while (done < n) {
            Ev& e = g_ev[done];
            uint16_t t = e.type.load(std::memory_order_acquire);
            if (!t)
                break;   // being filled: next round
            fprintf(g_file, "%llu %u %u %u %llu %llu\n",
                    (unsigned long long)((e.qpc - (uint64_t)g_qp0.QuadPart) * 1000000ull / (uint64_t)g_qpf.QuadPart),
                    (unsigned)e.tid, (unsigned)t, (unsigned)e.n, (unsigned long long)e.a, (unsigned long long)e.b);
            done++;
        }
        fflush(g_file);
        if (stop)
            break;
        Sleep(500);
    }
    return 0;
}

// ---- message / input APIs: Wine's builtin DLLs start these with an 8-byte hot-patch no-op (lea rsp,[rsp+0]) -------

static const char* const kApiNames[] = {"PeekMessageW", "PeekMessageA", "GetMessageW", "GetMessageA",
                                        "XInputGetState(1_4)", "XInputGetState(1_3)", "XInputGetState(9_1_0)",
                                        "GetAsyncKeyState", "GetKeyState", "GetKeyboardState", "GetCursorPos",
                                        "GetRawInputData", "GetRawInputBuffer", "XInputGetStateEx(1_4)",
                                        "XInputGetStateEx(1_3)", "XInputGetState(uap)"};
enum { kApis = 16 };
static void* g_orig[kApis];
static thread_local uint32_t t_seq[kApis];
static thread_local uint32_t t_cnt[kApis];

static inline void Input(int api)
{
    uint32_t s = g_seq.load(std::memory_order_relaxed);
    t_cnt[api]++;
    if (t_seq[api] != s) {
        t_seq[api] = s;
        Put(INPUT, (uint64_t)api, t_cnt[api]);
        t_cnt[api] = 0;
    }
}

template <int API> static BOOL WINAPI H_Peek(LPMSG m, HWND h, UINT a, UINT b, UINT c)
{
    Input(API);
    return ((BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT, UINT))g_orig[API])(m, h, a, b, c);
}
template <int API> static BOOL WINAPI H_Get(LPMSG m, HWND h, UINT a, UINT b)
{
    Input(API);
    return ((BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT))g_orig[API])(m, h, a, b);
}
template <int API> static DWORD WINAPI H_XInput(DWORD i, void* state)
{
    Input(API);
    return ((DWORD(WINAPI*)(DWORD, void*))g_orig[API])(i, state);
}
template <int API> static SHORT WINAPI H_Key(int k)
{
    Input(API);
    return ((SHORT(WINAPI*)(int))g_orig[API])(k);
}
template <int API> static BOOL WINAPI H_Ptr(void* p)
{
    Input(API);
    return ((BOOL(WINAPI*)(void*))g_orig[API])(p);
}
static UINT WINAPI H_RawData(HANDLE h, UINT cmd, void* data, UINT* size, UINT hdr)
{
    Input(11);
    return ((UINT(WINAPI*)(HANDLE, UINT, void*, UINT*, UINT))g_orig[11])(h, cmd, data, size, hdr);
}
static UINT WINAPI H_RawBuffer(void* data, UINT* size, UINT hdr)
{
    Input(12);
    return ((UINT(WINAPI*)(void*, UINT*, UINT))g_orig[12])(data, size, hdr);
}

struct InHook {
    const char* dll;
    const char* name;
    void* hook;
    int api;
    bool done;
};
static InHook g_inHooks[] = {
    {"user32.dll", "PeekMessageW", (void*)H_Peek<0>, 0},
    {"user32.dll", "PeekMessageA", (void*)H_Peek<1>, 1},
    {"user32.dll", "GetMessageW", (void*)H_Get<2>, 2},
    {"user32.dll", "GetMessageA", (void*)H_Get<3>, 3},
    {"xinput1_4.dll", "XInputGetState", (void*)H_XInput<4>, 4},
    {"xinput1_3.dll", "XInputGetState", (void*)H_XInput<5>, 5},
    {"xinput9_1_0.dll", "XInputGetState", (void*)H_XInput<6>, 6},
    {"user32.dll", "GetAsyncKeyState", (void*)H_Key<7>, 7},
    {"user32.dll", "GetKeyState", (void*)H_Key<8>, 8},
    {"user32.dll", "GetKeyboardState", (void*)H_Ptr<9>, 9},
    {"user32.dll", "GetCursorPos", (void*)H_Ptr<10>, 10},
    {"user32.dll", "GetRawInputData", (void*)H_RawData, 11},
    {"user32.dll", "GetRawInputBuffer", (void*)H_RawBuffer, 12},
    {"xinput1_4.dll", "XInputGetStateEx", (void*)H_XInput<13>, 13},
    {"xinput1_3.dll", "XInputGetStateEx", (void*)H_XInput<14>, 14},
    {"xinputuap.dll", "XInputGetState", (void*)H_XInput<15>, 15},
};

// 1 hooked, 0 not there yet, -1 cannot be hooked
static int HookInput(InHook& h)
{
    HMODULE m = GetModuleHandleA(h.dll);
    if (!m)
        return 0;
    uint8_t* p = (uint8_t*)GetProcAddress(m, h.name);
    if (!p)
        return -1;
    static const uint8_t kHot[8] = {0x48, 0x8d, 0xa4, 0x24, 0, 0, 0, 0};   // lea rsp,[rsp+0]
    DWORD old;
    // or somebody's jump of the same kind is there already (the XMX shim's input watch): ours goes in front of it
    const bool taken = p[0] == 0xE9 && p[5] == 0x0F && p[6] == 0x1F && p[7] == 0x00;
    if (!memcmp(p, kHot, 8) || taken) {
        intptr_t d = (uint8_t*)h.hook - (p + 5);
        if (d > INT32_MAX || d < INT32_MIN || !VirtualProtect(p, 8, PAGE_EXECUTE_READWRITE, &old))
            return -1;
        g_orig[h.api] = taken ? p + 5 + *(int32_t*)(p + 1) : p + 8;
        // jmp rel32 and a 3-byte no-op in the place of the 8-byte one, in one write
        uint64_t v = 0xE9ull | ((uint64_t)(uint32_t)(int32_t)d << 8) | (0x001F0Full << 40);
        InterlockedExchange64((volatile LONG64*)p, (LONG64)v);
        VirtualProtect(p, 8, old, &old);
        FlushInstructionCache(GetCurrentProcess(), p, 8);
        return 1;
    }
    if (p[0] == 0xFF && p[1] == 0x25) {   // jmp [rip+x]: an import of another DLL, its slot takes the hook
        void** slot = (void**)(p + 6 + *(int32_t*)(p + 2));
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old))
            return -1;
        g_orig[h.api] = *slot;
        InterlockedExchangePointer(slot, h.hook);
        VirtualProtect(slot, sizeof(void*), old, &old);
        return 1;
    }
    return -1;
}

// at the first presents and now and then (a game loads its input library late)
static void HookInputs()
{
    for (auto& h : g_inHooks) {
        if (h.done)
            continue;
        int r = HookInput(h);
        if (r) {
            h.done = true;
            Log("trace: %s!%s %s", h.dll, h.name, r > 0 ? "hooked" : "cannot be hooked");
        }
    }
}

// ---- D3D12 queue and fence vtables (vkd3d-proton: one per class) ----------------------------------------------

static bool PatchVt(void** vt, int slot, void* hook, void** orig)
{
    if (vt[slot] == hook)
        return true;
    DWORD old;
    if (!VirtualProtect(&vt[slot], sizeof(void*), PAGE_READWRITE, &old))
        return false;
    *orig = vt[slot];
    vt[slot] = hook;
    VirtualProtect(&vt[slot], sizeof(void*), old, &old);
    return true;
}

typedef HRESULT(STDMETHODCALLTYPE* PFN_QSignal)(ID3D12CommandQueue*, ID3D12Fence*, UINT64);
typedef HRESULT(STDMETHODCALLTYPE* PFN_FSetEvent)(ID3D12Fence*, UINT64, HANDLE);
static PFN_QSignal g_origSignal;
static PFN_FSetEvent g_origSetEvent;

static HRESULT STDMETHODCALLTYPE H_Signal(ID3D12CommandQueue* q, ID3D12Fence* f, UINT64 v)
{
    if (!t_own)
        Put(SIG, (uint64_t)(uintptr_t)f, v);
    return g_origSignal(q, f, v);
}

static HRESULT STDMETHODCALLTYPE H_SetEvent(ID3D12Fence* f, UINT64 v, HANDLE ev)
{
    if (t_own)
        return g_origSetEvent(f, v, ev);
    uint64_t t0 = Qpc();
    HRESULT hr = g_origSetEvent(f, v, ev);
    float us = QpcMs(t0, Qpc()) * 1000.0f;
    Put(FWAIT, (uint64_t)(uintptr_t)f, v, ev ? 1 : (uint16_t)std::min(us, 65535.0f), t0);
    return hr;
}

// The real swap chain under XeSS-FG (its Present, the DXGI factory it is made through) is hooked by ffx_xefg.cpp
// itself ("The shown frames"), which measures the shown frames there and puts RP_CALL / RP_RET.

// ---- when the GPU reaches a point of the game's queue ----------------------------------------------------------

struct GpuMark {
    ID3D12Fence* fence = nullptr;
    HANDLE ev = nullptr, wake = nullptr, th = nullptr;
    std::atomic<uint64_t> signaled{0};
    std::atomic<bool> stop{false};
    uint16_t type = 0;
};

static DWORD WINAPI GpuThread(void* p)
{
    auto* g = (GpuMark*)p;
    t_own = true;
    uint64_t done = 0;
    while (!g->stop.load()) {
        if (done >= g->signaled.load()) {
            WaitForSingleObject(g->wake, 100);
            continue;
        }
        uint64_t v = done + 1;
        if (g->fence->GetCompletedValue() < v) {
            g->fence->SetEventOnCompletion(v, g->ev);
            WaitForSingleObject(g->ev, 1000);
            if (g->stop.load())
                break;
        }
        uint64_t c = g->fence->GetCompletedValue();
        if (c >= v && c != UINT64_MAX) {
            Put(g->type, c);
            done = c;
        }
    }
    return 0;
}

static GpuMark* GpuMarkCreate(ID3D12Device* dev, uint16_t type)
{
    if (!g_on)
        return nullptr;
    auto* g = new GpuMark();
    g->type = type;
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g->fence)))) {
        delete g;
        return nullptr;
    }
    if (!g_origSetEvent) {
        void** vt = *(void***)g->fence;
        PatchVt(vt, 9, (void*)H_SetEvent, (void**)&g_origSetEvent);
    }
    g->ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    g->wake = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    g->th = CreateThread(nullptr, 0, GpuThread, g, 0, nullptr);
    return g;
}

static void GpuMarkSignal(GpuMark* g, ID3D12CommandQueue* q, uint64_t value)
{
    if (!g || !q)
        return;
    bool was = t_own;
    t_own = true;
    if (SUCCEEDED(q->Signal(g->fence, value))) {
        g->signaled.store(value);
        SetEvent(g->wake);
    }
    t_own = was;
}

static void GpuMarkDestroy(GpuMark* g)
{
    if (!g)
        return;
    g->stop.store(true);
    SetEvent(g->wake);
    SetEvent(g->ev);
    if (g->th) {
        WaitForSingleObject(g->th, 3000);
        CloseHandle(g->th);
    }
    g->fence->Release();
    CloseHandle(g->ev);
    CloseHandle(g->wake);
    delete g;
}

// FFX_XEFG_TRACE_SUBS=1: when the GPU finished each submission. A fence per queue, signalled after every
// ExecuteCommandLists (SubSignal), and a thread per queue that notes when each value is reached. Never torn down.
struct SubQueue {
    ID3D12CommandQueue* q = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE ev = nullptr, wake = nullptr;
    std::atomic<uint64_t> signaled{0};
    std::mutex lock;
    int index = 0;
};
static bool g_subs = false;
static SubQueue g_subQ[8];
static std::atomic<int> g_nSubQ{0};
static std::mutex g_subLock;

static DWORD WINAPI SubThread(void* p)
{
    auto* s = (SubQueue*)p;
    t_own = true;
    uint64_t done = 0;
    for (;;) {
        if (done >= s->signaled.load()) {
            WaitForSingleObject(s->wake, 100);
            continue;
        }
        uint64_t v = done + 1;
        if (s->fence->GetCompletedValue() < v) {
            s->fence->SetEventOnCompletion(v, s->ev);
            WaitForSingleObject(s->ev, 1000);
        }
        uint64_t c = s->fence->GetCompletedValue();
        if (c >= v && c != UINT64_MAX) {
            Put(SUBDONE, c, (uint64_t)s->index);
            done = c;
        }
    }
    return 0;
}

// after an ExecuteCommandLists on q: returns the value signalled behind it (0 = none) and the queue's index
static uint64_t SubSignal(ID3D12CommandQueue* q, int* index)
{
    if (!g_subs || !g_on)
        return 0;
    SubQueue* s = nullptr;
    int n = g_nSubQ.load();
    for (int i = 0; i < n && !s; i++)
        if (g_subQ[i].q == q)
            s = &g_subQ[i];
    if (!s) {
        std::lock_guard<std::mutex> lk(g_subLock);
        n = g_nSubQ.load();
        for (int i = 0; i < n && !s; i++)
            if (g_subQ[i].q == q)
                s = &g_subQ[i];
        if (!s) {
            if (n >= 8)
                return 0;
            SubQueue* c = &g_subQ[n];
            ID3D12Device* dev = nullptr;
            if (FAILED(q->GetDevice(IID_PPV_ARGS(&dev))))
                return 0;
            HRESULT hr = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&c->fence));
            dev->Release();
            if (FAILED(hr))
                return 0;
            if (!g_origSetEvent) {
                void** vt = *(void***)c->fence;
                PatchVt(vt, 9, (void*)H_SetEvent, (void**)&g_origSetEvent);
            }
            c->q = q;
            c->index = n;
            c->ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
            c->wake = CreateEventA(nullptr, FALSE, FALSE, nullptr);
            CloseHandle(CreateThread(nullptr, 0, SubThread, c, 0, nullptr));
            Put(SUBQ, (uint64_t)(uintptr_t)q, (uint64_t)q->GetDesc().Type, (uint16_t)n);
            g_nSubQ = n + 1;
            s = c;
        }
    }
    bool was = t_own;
    t_own = true;
    uint64_t v = 0;
    {
        std::lock_guard<std::mutex> lk(s->lock);   // several threads submit to one queue: the values stay in order
        v = s->signaled.load() + 1;
        if (SUCCEEDED(q->Signal(s->fence, v))) {
            s->signaled.store(v);
            SetEvent(s->wake);
        } else {
            v = 0;
        }
    }
    t_own = was;
    *index = s->index;
    return v;
}

static void HookQueueVt(ID3D12CommandQueue* q)
{
    if (!g_on || g_origSignal)
        return;
    void** vt = *(void***)q;
    PatchVt(vt, 14, (void*)H_Signal, (void**)&g_origSignal);
}

// ---- where the present thread waits after Present returned ------------------------------------------------------
// A sampler (FFX_XEFG_TRACE_STACKS=<n>): when the present thread has shown no event for 3 ms after a Present
// returned, it is suspended for a moment and the code addresses on its stack are logged (module + offset, with the
// nearest export below): which call the game waits in before it starts its next frame. The first n samples only.

static std::atomic<uint64_t> g_presRet{0};       // QPC of the newest GP_RET, 0 while inside Present
static std::atomic<int> g_stackWanted{0};

static void NearestExport(HMODULE m, void* addr, char* out, size_t n)
{
    out[0] = 0;
    auto* base = (uint8_t*)m;
    auto* dos = (IMAGE_DOS_HEADER*)base;
    auto* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress || !dir.Size)
        return;
    auto* ex = (IMAGE_EXPORT_DIRECTORY*)(base + dir.VirtualAddress);
    auto* names = (DWORD*)(base + ex->AddressOfNames);
    auto* ords = (WORD*)(base + ex->AddressOfNameOrdinals);
    auto* funcs = (DWORD*)(base + ex->AddressOfFunctions);
    uintptr_t rva = (uintptr_t)((uint8_t*)addr - base), best = 0;
    const char* bestName = nullptr;
    for (DWORD i = 0; i < ex->NumberOfNames; i++) {
        DWORD f = funcs[ords[i]];
        if (f <= rva && f > best) {
            best = f;
            bestName = (const char*)(base + names[i]);
        }
    }
    if (bestName && rva - best < 0x4000)
        snprintf(out, n, "!%s+0x%llx", bestName, (unsigned long long)(rva - best));
}

static void SampleStack(DWORD tid, float idleMs, const char* what)
{
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!th)
        return;
    static uint64_t words[1024];
    CONTEXT ctx = {};
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    size_t nw = 0;
    if (SuspendThread(th) != (DWORD)-1) {
        if (GetThreadContext(th, &ctx)) {
            MEMORY_BASIC_INFORMATION mbi = {};
            // the stack from rsp up to the end of its region (never past it)
            if (VirtualQuery((void*)ctx.Rsp, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT) {
                uint8_t* end = (uint8_t*)mbi.BaseAddress + mbi.RegionSize;
                for (uint64_t* p = (uint64_t*)(ctx.Rsp & ~7ull); (uint8_t*)(p + 1) <= end && nw < 1024; ++p)
                    words[nw++] = *p;
            }
        }
        ResumeThread(th);
    }
    CloseHandle(th);
    char line[2048];
    int o = 0, hits = 0;
    line[0] = 0;
    auto add = [&](uint64_t a) {
        HMODULE m = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m) || !m)
            return;
        MEMORY_BASIC_INFORMATION mbi = {};
        if (!VirtualQuery((void*)a, &mbi, sizeof mbi) ||
            !(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
            return;
        char path[MAX_PATH] = "", sym[160] = "";
        GetModuleFileNameA(m, path, MAX_PATH);
        const char* base = strrchr(path, 92);   // backslash
        base = base ? base + 1 : path;
        NearestExport(m, (void*)a, sym, sizeof sym);
        if (o < (int)sizeof(line) - 300) {
            o += snprintf(line + o, sizeof(line) - o, "%s %s%s", hits ? " <" : "", base, sym);
            if (!sym[0])
                o += snprintf(line + o, sizeof(line) - o, "+0x%llx", (unsigned long long)(a - (uint64_t)(uintptr_t)m));
        }
        hits++;
    };
    add(ctx.Rip);
    for (size_t i = 0; i < nw && hits < 14; i++)
        add(words[i]);
    Log("trace: thread %lu %.1f ms after %s, stack (code addresses, innermost first):%s", tid, idleMs,
        idleMs < 0 ? "?" : what, line);
}

static DWORD WINAPI StackThread(void*)
{
    uint64_t sampledRet = 0;
    while (!g_stop.load() && g_stackWanted.load() > 0) {
        Sleep(1);
        uint64_t ret = g_presRet.load(), evt = g_stackEvent.load();
        DWORD tid = g_stackTid.load();
        if (!ret || !tid || ret == sampledRet)
            continue;
        float idle = QpcMs(evt > ret ? evt : ret, Qpc());   // since its newest event
        if (idle < 3.0f)
            continue;
        // FFX_XEFG_TRACE_STACK_TID=<thread id>: that thread too (the game's main thread, as the input events name
        // it), FFX_XEFG_TRACE_STACK_AT ms after the Present returned
        static const DWORD tid2 = (DWORD)EnvInt("FFX_XEFG_TRACE_STACK_TID", 0);
        static const float at2 = (float)EnvInt("FFX_XEFG_TRACE_STACK_AT", 8);
        if (tid2 && QpcMs(ret, Qpc()) < at2)
            continue;
        sampledRet = ret;
        g_stackWanted--;
        SampleStack(tid, idle, "its newest event (present thread)");
        if (tid2)
            SampleStack(tid2, QpcMs(ret, Qpc()), "the Present returned");
        Sleep(2000);   // spread over the run
    }
    return 0;
}

static void Start(const char* dir)
{
    char path[MAX_PATH];
    snprintf(path, MAX_PATH, "%sffx_xefg_trace.txt", dir);
    g_file = fopen(path, "w");
    g_ev = g_file ? (Ev*)VirtualAlloc(nullptr, sizeof(Ev) * (size_t)kCap, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) : nullptr;
    if (!g_ev) {
        Log("trace: no file or memory, off");
        return;
    }
    setvbuf(g_file, nullptr, _IOFBF, 1 << 20);
    g_on = true;
    g_writer = CreateThread(nullptr, 0, Writer, nullptr, 0, nullptr);
    SetThreadPriority(g_writer, THREAD_PRIORITY_LOWEST);
    g_stackWanted = EnvInt("FFX_XEFG_TRACE_STACKS", 0);
    if (g_stackWanted > 0) {
        g_stackOn = true;
        CloseHandle(CreateThread(nullptr, 0, StackThread, nullptr, 0, nullptr));
    }
    Log("trace: events to %s", path);
}

}   // namespace tr

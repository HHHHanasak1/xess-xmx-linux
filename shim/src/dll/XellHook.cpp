// XellHook.cpp - optional in-process tweak of Intel XeLL (libxell.dll) so that the frame limiter it enforces can be inspected / lifted.
// XeLL's exports are 5-byte "jmp rel32" thunks; xellSetSleepMode / xellSleep are redirected to the functions below.
// Enabled only when IGDEXT_XELL_LOG or IGDEXT_XELL_MININTERVAL_US is set in the environment (nothing happens otherwise).
//   IGDEXT_XELL_MININTERVAL_US=<n>   force xell_sleep_params_t.minimumIntervalUs (0 = XeLL frame limiter off)
//   IGDEXT_XELL_LOWLATENCY=<0|1>     force bLowLatencyMode
//   IGDEXT_XELL_LOG=1                append the parameters the game passes to C:\igdext_xell.log
#include "Stdafx.h"
#include "Trace.h"
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>

#pragma pack(push, 1)
struct XellSleepParams { uint32_t minimumIntervalUs; uint32_t flags; }; // flags bit0 = bLowLatencyMode, bit1 = bLowLatencyBoost
#pragma pack(pop)
typedef uint32_t (*PFN_XellSet)(void*, const XellSleepParams*);
typedef uint32_t (*PFN_XellGet)(void*, XellSleepParams*);
typedef uint32_t (*PFN_XellSleep)(void*, uint32_t);

static PFN_XellSet g_origSet; static PFN_XellGet g_origGet; static PFN_XellSleep g_origSleep;
static long long g_interval = -1; static int g_lowLatency = -1; static bool g_log = false;
static void* g_ctx = nullptr;
// default fix: the game applies a 33.3 ms frame cap (30 fps, counted per displayed frame) right after it enables frame generation and only
// replaces it when the pause menu is opened and closed. A nonzero interval arriving within 1.5 s of xellSetFgEnabled(1) is replaced by the
// interval that was in effect before (IGDEXT_XELL_KEEPCAP=1 disables this).
static bool g_fixCap = true; static uint32_t g_effInterval = 0, g_savedInterval = 0; static ULONGLONG g_fgOnTick = 0;
typedef uint32_t (*PFN_XellSetLog)(void*, uint32_t, void (*)(const char*, uint32_t));
#pragma pack(push, 8)
struct XellFrameReport { uint32_t id; uint64_t simS, simE, rsS, rsE, prS, prE, r1, r2, r3, r4, r5; };
#pragma pack(pop)
typedef uint32_t (*PFN_XellReports)(void*, XellFrameReport*);
static PFN_XellSetLog g_setLog; static PFN_XellReports g_reports; static bool g_xellDbg = false;
static void XellLogCb(const char* msg, uint32_t level);
static volatile LONG g_sleepCalls = 0;
typedef uint32_t (*PFN_Xell3)(void*, uint32_t, uint32_t);
typedef uint32_t (*PFN_XellDisp)(void*, const void*);
static PFN_Xell3 g_origGen, g_origFg; static PFN_XellDisp g_origDisp; static int g_genOverride = -1; static int g_fgMaxOverride = -1; static bool g_noSpin = false;

static void XLog(const char* fmt, ...)
{
    if (!g_log) return;
    FILE* f = nullptr;
    if (fopen_s(&f, "C:\\igdext_xell.log", "ab") != 0 || !f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "%02d:%02d:%02d.%03d [%lu] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, GetCurrentThreadId());
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}

static void XellLogCb(const char* msg, uint32_t level)
{
    if (msg) XLog("XeLL[%u] %s", level, msg);
}
static void DumpReports(void* ctx)
{
    if (!g_reports) return;
    static XellFrameReport r[64];
    memset(r, 0, sizeof(r));
    if (g_reports(ctx, r) != 0) { XLog("xellGetFramesReports failed"); return; }
    {
        const uint64_t* q = (const uint64_t*) r; char b[1200]; int o = 0;
        for (int i = 0; i < 36; ++i) o += snprintf(b + o, sizeof(b) - o, "%llu ", (unsigned long long) q[i]);
        XLog("raw report words: %s", b);
    }
    {
        static int fullDumps = 0;
        if (g_sleepCalls > 300 && fullDumps < 200)
        {
            ++fullDumps;
            for (int i = 0; i < 64; ++i) if (r[i].id && r[i].simS)
                XLog("ABS #%u simS %.2f simE %.2f rsS %.2f rsE %.2f prS %.2f prE %.2f", r[i].id, fmod(r[i].simS / 1e6, 1e6), fmod(r[i].simE / 1e6, 1e6), fmod(r[i].rsS / 1e6, 1e6), fmod(r[i].rsE / 1e6, 1e6), fmod(r[i].prS / 1e6, 1e6), fmod(r[i].prE / 1e6, 1e6));
        }
    }
    uint32_t best = 0; for (int i = 0; i < 64; ++i) if (r[i].id > best) best = r[i].id;
    XLog("frame reports (newest id %u), ms: sim / submit / present / gap-to-next-sim", best);
    int shown = 0;
    for (int i = 0; i < 64 && shown < 8; ++i)
    {
        // find frame best-shown-... print the last 8 frames in id order
        uint32_t want = best - 8 + (uint32_t) i;
        for (int j = 0; j < 64; ++j) if (r[j].id == want && r[j].simS)
        {
            const XellFrameReport& a = r[j]; const XellFrameReport* n = nullptr;
            for (int k = 0; k < 64; ++k) if (r[k].id == want + 1) n = &r[k];
            XLog("  #%u  simS 0  simE %.2f  rsS %.2f  rsE %.2f  prS %.2f  prE %.2f  gapToNextSimStart %.2f ms", a.id, (a.simE - a.simS) / 1e6, ((int64_t) a.rsS - (int64_t) a.simS) / 1e6, ((int64_t) a.rsE - (int64_t) a.simS) / 1e6, ((int64_t) a.prS - (int64_t) a.simS) / 1e6, ((int64_t) a.prE - (int64_t) a.simS) / 1e6, n ? (n->simS - a.simS) / 1e6 : 0.0);
            ++shown;
        }
    }
}
static uint32_t Hook_SetGen(void* ctx, uint32_t a, uint32_t b)
{
    static int n = 0;
    if (n++ < 12 || (n % 600) == 0) XLog("xellSetGeneratedFramesCount ctx=%p a=%u count=%u", ctx, a, b);
    if (g_genOverride >= 0) b = (uint32_t) g_genOverride;
    return g_origGen(ctx, a, b);
}
static uint32_t Hook_SetFg(void* ctx, uint32_t a, uint32_t b)
{
    static int n = 0;
    if (n++ < 12 || (n % 600) == 0) XLog("xellSetFgEnabled ctx=%p a=%u b=%u", ctx, a, b);
    if (a == 1) { g_savedInterval = g_effInterval; g_fgOnTick = GetTickCount64(); }
    if (g_fgMaxOverride >= 0) b = (uint32_t) g_fgMaxOverride;
    return g_origFg(ctx, a, b);
}
static uint32_t Hook_SetDisp(void* ctx, const void* p)
{
    static int n = 0;
    if (p && (n++ < 6 || (n % 600) == 0))
    {
        const uint32_t* d = (const uint32_t*) p; char b[300]; int o = 0;
        for (int i = 0; i < 12; ++i) o += snprintf(b + o, sizeof(b) - o, "%08x ", d[i]);
        XLog("xellSetDisplayInfo ctx=%p data: %s", ctx, b);
        const float* f = (const float*) p; o = 0;
        for (int i = 0; i < 12; ++i) o += snprintf(b + o, sizeof(b) - o, "%g ", f[i]);
        XLog("   as float: %s", b);
    }
    return g_origDisp(ctx, p);
}
typedef uint32_t (*PFN_XellMark)(void*, uint32_t, uint32_t);
static PFN_XellMark g_origMark; static volatile LONG g_markCnt[8];
static uint32_t Hook_Mark(void* ctx, uint32_t frame, uint32_t marker)
{
    static LARGE_INTEGER f, t0; static LONG total = 0;
    if (!f.QuadPart) { QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0); }
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    LONG n = InterlockedIncrement(&total);
    if (marker < 8) InterlockedIncrement(&g_markCnt[marker]);
    static LONG gameLogged = 0;
    if (marker < 8 && gameLogged < 220 && n > 2000) { ++gameLogged; XLog("game marker frame=%u type=%u t=%.2f ms", frame, marker, (c.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart); }
    if ((n % 1000) == 0) XLog("marker counts sim_s=%ld sim_e=%ld rs_s=%ld rs_e=%ld pr_s=%ld pr_e=%ld input=%ld", (long) g_markCnt[0], (long) g_markCnt[1], (long) g_markCnt[2], (long) g_markCnt[3], (long) g_markCnt[4], (long) g_markCnt[5], (long) g_markCnt[6]);
    return g_origMark(ctx, frame, marker);
}
typedef uint64_t (*PFN_Xell4)(uint64_t, uint64_t, uint64_t, uint64_t);
static PFN_Xell4 g_origSetP, g_origGetP;
static void DumpPtr(const char* tag, uint64_t p)
{
    if (p < 0x10000) return;
    __try { const uint32_t* d = (const uint32_t*) p; XLog("   %s memory: %08x %08x %08x %08x  (as float %g %g)", tag, d[0], d[1], d[2], d[3], *(const float*) &d[0], *(const float*) &d[1]); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}
static uint64_t Hook_SetP(uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    static int n = 0;
    if (n++ < 40) { XLog("xellSetContextParameterP ctx=%llx id=%llu p3=%llx p4=%llx", (unsigned long long) a, (unsigned long long) b, (unsigned long long) c, (unsigned long long) d); DumpPtr("p3", c); }
    return g_origSetP(a, b, c, d);
}
static uint64_t Hook_GetP(uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    static int n = 0;
    uint64_t r = g_origGetP(a, b, c, d);
    if (n++ < 20) { XLog("xellGetContextParameterP ctx=%llx id=%llu -> %llu", (unsigned long long) a, (unsigned long long) b, (unsigned long long) r); DumpPtr("p3", c); }
    return r;
}
static void Adjust(XellSleepParams& p)
{
    if (g_interval >= 0) p.minimumIntervalUs = (uint32_t) g_interval;
    if (g_lowLatency >= 0) p.flags = (p.flags & ~1u) | (g_lowLatency ? 1u : 0u);
}

static uint32_t Hook_SetSleepMode(void* ctx, const XellSleepParams* p)
{
    if (!p) return g_origSet(ctx, p);
    XellSleepParams q = *p;
    XLog("xellSetSleepMode ctx=%p game: minimumIntervalUs=%u lowLatency=%u boost=%u", ctx, p->minimumIntervalUs, p->flags & 1, (p->flags >> 1) & 1);
    Adjust(q);
    if (g_fixCap && g_interval < 0 && q.minimumIntervalUs != 0 && g_fgOnTick && GetTickCount64() - g_fgOnTick < 1500)
    { XLog("   -> frame cap %u us set right after frame generation was enabled: keeping %u us", q.minimumIntervalUs, g_savedInterval); q.minimumIntervalUs = g_savedInterval; }
    g_effInterval = q.minimumIntervalUs;
    if (q.minimumIntervalUs != p->minimumIntervalUs || q.flags != p->flags)
        XLog("   -> forced: minimumIntervalUs=%u lowLatency=%u", q.minimumIntervalUs, q.flags & 1);
    return g_origSet(ctx, &q);
}

volatile unsigned long g_gameThreadId = 0;
static uint32_t Hook_Sleep(void* ctx, uint32_t frame)
{
    g_gameThreadId = GetCurrentThreadId();
    LONG n = InterlockedIncrement(&g_sleepCalls);
    g_ctx = ctx;
    if (g_xellDbg && n == 1 && g_setLog) { uint32_t rr = g_setLog(ctx, 0, XellLogCb); XLog("xellSetLoggingCallback(debug) -> %u", rr); }
    if (g_xellDbg && (n % 90) == 45) DumpReports(ctx);
    if ((g_interval >= 0 || g_lowLatency >= 0) && (n % 30) == 1 && g_origGet && g_origSet)
    {
        XellSleepParams cur = {};
        if (g_origGet(ctx, &cur) == 0)
        {
            XellSleepParams want = cur; Adjust(want);
            if (want.minimumIntervalUs != cur.minimumIntervalUs || want.flags != cur.flags)
            {
                XLog("xellSleep: mode was minimumIntervalUs=%u lowLatency=%u -> forcing %u/%u", cur.minimumIntervalUs, cur.flags & 1, want.minimumIntervalUs, want.flags & 1);
                g_origSet(ctx, &want);
            }
        }
    }
    if (g_log && (n < 5 || (n % 600) == 0)) XLog("xellSleep call #%ld frame=%u", (long) n, frame);
    LARGE_INTEGER qa, qb, qf; QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&qa);
    uint32_t rr = g_origSleep(ctx, frame);
    QueryPerformanceCounter(&qb);
    if (g_log)
    {
        static double sum = 0, mx = 0; static int cnt = 0; static LARGE_INTEGER t0;
        double d = (qb.QuadPart - qa.QuadPart) * 1000.0 / qf.QuadPart;
        if (!t0.QuadPart) t0 = qa;
        sum += d; if (d > mx) mx = d; ++cnt;
        if (cnt == 100)
        {
            double wall = (qb.QuadPart - t0.QuadPart) * 1000.0 / qf.QuadPart;
            XLog("xellSleep stats: 100 calls in %.0f ms (%.1f ms/frame), blocked avg %.2f ms max %.2f ms = %.0f%% of wall", wall, wall / 100, sum / 100, mx, 100.0 * sum / wall);
            sum = mx = 0; cnt = 0; t0 = qb;
        }
    }
    return rr;
}

static bool PatchThunk(void* thunk, void* hook, void** orig, const char* name)
{
    uint8_t* p = (uint8_t*) thunk;
    if (p[0] != 0xE9) { XLog("%s: export is not a jmp rel32 thunk (byte %02x), not hooked", name, p[0]); return false; }
    int32_t rel = *(int32_t*) (p + 1);
    *orig = p + 5 + rel;
    intptr_t d = (uint8_t*) hook - (p + 5);
    if (d > INT32_MAX || d < INT32_MIN) { XLog("%s: hook too far from the thunk (%lld), not hooked", name, (long long) d); return false; }
    DWORD old;
    if (!VirtualProtect(p, 8, PAGE_EXECUTE_READWRITE, &old)) { XLog("%s: VirtualProtect failed %lu", name, GetLastError()); return false; }
    uint64_t cur = *(volatile uint64_t*) p;
    uint64_t neu = (cur & ~0xFFFFFFFFFFull) | 0xE9ull | ((uint64_t)(uint32_t)(int32_t) d << 8);
    InterlockedExchange64((volatile LONG64*) p, (LONG64) neu);
    VirtualProtect(p, 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 8);
    XLog("%s hooked: thunk %p -> %p (orig %p)", name, thunk, hook, *orig);
    return true;
}

// XeLL 1.3.0 (Wuthering Waves build) waits inside a busy loop at RVA 0x23000: `if (remaining <= 0) return now; else spin until elapsed`.
// Turning the `jg` at 0x2301c into two NOPs makes the function return immediately (no wait), everything else in XeLL keeps running.
typedef uint64_t* (*PFN_Spin)(void*, uint64_t*, uint64_t, int64_t);
static PFN_Spin g_origSpin; static bool g_spinLog = false;
static uint64_t* Hook_Spin(void* a, uint64_t* out, uint64_t lastNs, int64_t remaining)
{
    static int n = 0; static LARGE_INTEGER f, t0; static double sumReq = 0, sumWait = 0, maxReq = 0;
    if (!f.QuadPart) { QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0); }
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    uint64_t* r = g_origSpin(a, out, lastNs, remaining);
    LARGE_INTEGER d; QueryPerformanceCounter(&d);
    double req = remaining > 0 ? remaining / 1e4 : 0.0, w = (d.QuadPart - c.QuadPart) * 1000.0 / f.QuadPart;
    sumReq += req; sumWait += w; if (req > maxReq) maxReq = req;
    if (++n % 120 == 0)
    {
        double wall = (d.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
        XLog("spin stats: %d calls in %.0f ms, avg requested %.2f ms, max %.2f ms, time spent waiting %.1f%% of wall", 120, wall, sumReq / 120, maxReq, 100.0 * sumWait / wall);
        sumReq = sumWait = maxReq = 0; t0 = d;
    }
    return r;
}
static void HookSpin(HMODULE m)
{
    uint8_t* base = (uint8_t*) m;
    static const uint8_t prologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x20 };
    if (memcmp(base + 0x23000, prologue, sizeof(prologue)) != 0) { XLog("SPINLOG: unexpected libxell build"); return; }
    uint8_t* tr = (uint8_t*) VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) { XLog("SPINLOG: trampoline alloc failed"); return; }
    memcpy(tr, prologue, sizeof(prologue));
    tr[16] = 0xFF; tr[17] = 0x25; *(uint32_t*) (tr + 18) = 0; *(uint64_t*) (tr + 22) = (uint64_t) (base + 0x23010);
    g_origSpin = (PFN_Spin) tr;
    intptr_t d = (uint8_t*) Hook_Spin - (base + 0x23000 + 5);
    if (d > INT32_MAX || d < INT32_MIN) { XLog("SPINLOG: hook out of rel32 range"); return; }
    DWORD old; VirtualProtect(base + 0x23000, 16, PAGE_EXECUTE_READWRITE, &old);
    // 16 bytes = 4 whole instructions; jmp rel32 + 11 bytes of nops keeps the region valid
    uint8_t patch[16]; memset(patch, 0x90, sizeof(patch)); patch[0] = 0xE9; *(int32_t*) (patch + 1) = (int32_t) d;
    memcpy(base + 0x23000, patch, 16);
    VirtualProtect(base + 0x23000, 16, old, &old);
    FlushInstructionCache(GetCurrentProcess(), base + 0x23000, 16);
    XLog("SPINLOG: spin function hooked");
}
static void PatchNoSpin(HMODULE m)
{
    uint8_t* base = (uint8_t*) m;
    static const uint8_t prologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x20 };
    if (memcmp(base + 0x23000, prologue, sizeof(prologue)) != 0 || base[0x2301c] != 0x7F || base[0x2301d] != 0x42)
    { XLog("NOSPIN: libxell.dll is not the expected build, spin loop not patched"); return; }
    DWORD old;
    if (!VirtualProtect(base + 0x2301c, 2, PAGE_EXECUTE_READWRITE, &old)) { XLog("NOSPIN: VirtualProtect failed"); return; }
    base[0x2301c] = 0x90; base[0x2301d] = 0x90;
    VirtualProtect(base + 0x2301c, 2, old, &old);
    FlushInstructionCache(GetCurrentProcess(), base + 0x2301c, 2);
    XLog("NOSPIN: XeLL busy-wait disabled");
}

static DWORD WINAPI XellThread(LPVOID)
{
    HMODULE m = nullptr;
    for (int i = 0; i < 6000 && !(m = GetModuleHandleW(L"libxell.dll")); ++i) Sleep(100);
    if (!m) { XLog("libxell.dll never loaded"); return 0; }
    XLog("libxell.dll found at %p", m);
    Sleep(200);
    if (g_noSpin) PatchNoSpin(m);
    if (g_spinLog) HookSpin(m);
    auto set = (void*) GetProcAddress(m, "xellSetSleepMode");
    auto get = (void*) GetProcAddress(m, "xellGetSleepMode");
    auto slp = (void*) GetProcAddress(m, "xellSleep");
    if (!set || !get || !slp) { XLog("XeLL exports missing"); return 0; }
    g_setLog = (PFN_XellSetLog) GetProcAddress(m, "xellSetLoggingCallback"); g_reports = (PFN_XellReports) GetProcAddress(m, "xellGetFramesReports");
    void* dummy = nullptr;
    // the getter is only called through its original target; do not patch it
    { uint8_t* p = (uint8_t*) get; g_origGet = (PFN_XellGet) ((p[0] == 0xE9) ? (void*) (p + 5 + *(int32_t*) (p + 1)) : get); }
    PatchThunk(slp, (void*) Hook_Sleep, &dummy, "xellSleep"); g_origSleep = (PFN_XellSleep) dummy;
    PatchThunk(set, (void*) Hook_SetSleepMode, &dummy, "xellSetSleepMode"); g_origSet = (PFN_XellSet) dummy;
    { auto fg0 = (void*) GetProcAddress(m, "xellSetFgEnabled"); if (fg0) { PatchThunk(fg0, (void*) Hook_SetFg, &dummy, "xellSetFgEnabled"); g_origFg = (PFN_Xell3) dummy; } }
    if (g_log)
    {
        auto gen = (void*) GetProcAddress(m, "xellSetGeneratedFramesCount"); auto di = (void*) GetProcAddress(m, "xellSetDisplayInfo");
        if (gen) { PatchThunk(gen, (void*) Hook_SetGen, &dummy, "xellSetGeneratedFramesCount"); g_origGen = (PFN_Xell3) dummy; }
        auto mk = (void*) GetProcAddress(m, "xellAddMarkerData"); if (mk) { PatchThunk(mk, (void*) Hook_Mark, &dummy, "xellAddMarkerData"); g_origMark = (PFN_XellMark) dummy; }
        auto sp = (void*) GetProcAddress(m, "xellSetContextParameterP"); auto gp = (void*) GetProcAddress(m, "xellGetContextParameterP");
        if (sp) { PatchThunk(sp, (void*) Hook_SetP, &dummy, "xellSetContextParameterP"); g_origSetP = (PFN_Xell4) dummy; }
        if (gp) { PatchThunk(gp, (void*) Hook_GetP, &dummy, "xellGetContextParameterP"); g_origGetP = (PFN_Xell4) dummy; }
        if (di) { PatchThunk(di, (void*) Hook_SetDisp, &dummy, "xellSetDisplayInfo"); g_origDisp = (PFN_XellDisp) dummy; }
    }
    return 0;
}

void StartXellHook()
{
    char b[32];
    if (GetEnvironmentVariableA("IGDEXT_XELL_LOG", b, sizeof(b)) > 0) g_log = true;
    if (GetEnvironmentVariableA("IGDEXT_XELL_DEBUG", b, sizeof(b)) > 0) { g_log = true; g_xellDbg = true; }
    if (GetEnvironmentVariableA("IGDEXT_XELL_MININTERVAL_US", b, sizeof(b)) > 0) g_interval = atoll(b);
    if (GetEnvironmentVariableA("IGDEXT_XELL_LOWLATENCY", b, sizeof(b)) > 0) g_lowLatency = atoi(b) ? 1 : 0;
    if (GetEnvironmentVariableA("IGDEXT_XELL_SPINLOG", b, sizeof(b)) > 0) { g_log = true; g_spinLog = true; }
    if (GetEnvironmentVariableA("IGDEXT_XELL_NOSPIN", b, sizeof(b)) > 0) g_noSpin = atoi(b) != 0;
    if (GetEnvironmentVariableA("IGDEXT_XELL_FGMAX", b, sizeof(b)) > 0) g_fgMaxOverride = atoi(b);
    if (GetEnvironmentVariableA("IGDEXT_XELL_GENCOUNT", b, sizeof(b)) > 0) g_genOverride = atoi(b);
    if (GetEnvironmentVariableA("IGDEXT_XELL_KEEPCAP", b, sizeof(b)) > 0 && atoi(b)) g_fixCap = false;
    if (!g_fixCap && !g_log && g_interval < 0 && g_lowLatency < 0 && !g_noSpin) return;
    if (g_xellDbg) XLog("XeLL debug logging requested");
    char exe[MAX_PATH] = {}; GetModuleFileNameA(nullptr, exe, MAX_PATH);
    XLog("XellHook start in %s: log=%d interval=%lld lowLatency=%d", exe, (int) g_log, g_interval, g_lowLatency);
    HANDLE h = CreateThread(nullptr, 0, XellThread, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
}

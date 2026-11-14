// ffx_xefg.cpp - amd_fidelityfx_framegeneration_dx12.dll that implements AMD's FFX frame generation API on top of
// Intel XeSS frame generation (libxess_fg.dll, proxy swap chain) and XeLL (libxell.dll).
//
// The game keeps its FSR 3.1+/4 integration; the FFX loader loads this DLL in place of AMD's frame generation
// provider. Frame generation swap chain contexts (WRAP / NEW / FOR_HWND) create an XeSS-FG proxy swap chain,
// wrapped in FgSwapChain for the XeLL present markers; FRAMEGENERATION contexts tag depth, motion vectors,
// HUD-less color and the camera constants for XeSS-FG. A game that gives a HUD-less color gets XeSS-FG's UI composition
// from it, as AMD's frame generation separates the UI that way. The game's own interpolation dispatch is a no-op:
// XeSS-FG interpolates inside the proxy swap chain.
//
// The same DLL, as ffx_xefg_bridge.dll, serves a game without FSR frame generation (FSR_FG=inject, xefg_bridge.h),
// and with FFX_XEFG_OFLOW=1 (OFLOW_FG=1) any DX12 game: the motion vectors come from FidelityFX optical flow on the
// final image (ffx_oflow.cpp), the bridge makes a frame on every present itself.
//
// This file has the FFX API: the DLL's exports and the frame generation context. The rest of the DLL is in
// ffx_xefg_*.cpp; ffx_xefg.h says what is where.
//
// Environment:
//   FFX_XEFG_MULT=n   frames shown per rendered frame (2 = one interpolated frame; default: XESS_FG, else 2)
//   FFX_XEFG_LOG=path log file (default ffx_xefg.log next to the DLL); FFX_XEFG_LOG=0 turns it off
//   FFX_XEFG_XEFGLOG=0..3  XeSS-FG library log level forwarded to the log (default 2 = warnings)
//   (FFX_XEFG_SLEEP, OWNWAIT, PACE, VALIDITY, MVNDC, AB, AB_OLD, HOLD and HDR10_AB, measuring aids, are read by a
//   debug build only: see DBG_SWITCH in ffx_xefg.h)
//   FFX_XEFG_EXCLUSIVE=1   let the game enter exclusive fullscreen (XeSS-FG then refuses to interpolate)
//   FFX_XEFG_HUDLESS=0     the game's HUD-less color is not used: no UI composition, XeSS-FG interpolates the
//                          whole image, the UI included
//   FFX_XEFG_VENDOR=0      XeSS-FG and XeLL are not given 0x8086 for an Intel GPU that DXVK reports as another
//                          vendor's (FSR_VENDOR=amd; ffx_xefg_init.cpp)
//   FFX_XEFG_OFLOW=1       motion vectors from FidelityFX optical flow on the final image instead of the game's,
//                          constant depth (FFX_XEFG_OFLOW_DEPTH, default 1.0), no camera, no HUD-less color or UI: what
//                          a driver-level frame generation has (ffx_oflow.cpp; tools/proton-hook.py sets it for OFLOW_FG=1)
//   FFX_XEFG_OFLOW_DIV=n   the optical flow sees the image at 1/n size (1..4, default 1)
//   FFX_XEFG_CAP=us|auto|0 the interval of a real frame in microseconds through XeLL, for even frames; auto (the default
//                          with FFX_XEFG_OFLOW) follows the GPU time of a frame; 0 = none. FFX_XEFG_CAP_PAD=ms is auto's
//                          first guess of what a frame needs beyond that (default 1.5, then learned), FFX_XEFG_REFRESH=Hz
//                          the display's refresh rate when Windows reports a wrong one
//   FFX_XEFG_TIMES=1       frame time, time in Present and XeLL's sleep (percentiles) in the log every 300 frames, and
//                          the gaps between the shown frames' Presents (ffx_xefg_shown.cpp)
//   IGDEXT_XELL_CAP        the low latency, as for a game's own XeSS-FG (read by the XMX shim too): unset or auto[:ms]
//                          = the game stays in Present until its next frame should start, so that the frame reaches
//                          its Present when XeSS-FG can take it instead of waiting there (ffx_xefg_lowlat.cpp);
//                          off = no such wait; <us> = XeLL's fixed cap holds the frame back there instead
//   IGDEXT_XELL_POLLS=0|1  1 (the default with the low latency, with FFX_XEFG_OFLOW too): what a game asks of the
//                          swap chain every frame (GetFrameStatistics, CheckColorSpaceSupport) is answered as of
//                          its last Present and never waits for XeSS-FG's presentation thread; 0: asked from the
//                          swap chain each time (the XMX shim reads it too, for a game's own XeSS-FG)
//   IGDEXT_XELL_CHECK=0    the low latency's wait is not checked (by default it stays only where the frame time with
//                          it is within 1 % of the frame time without it and the latency is lower; the XMX shim
//                          reads it too)
//   FFX_XEFG_HDR10=1       XeSS-FG is told when the game's back buffer is HDR10 (PQ); by default it is not, and
//                          interpolates the PQ values as they are (ffx_xefg_swapchain.cpp)
// The same KEY=VALUE lines can go into ffx_xefg.env next to the DLL (the environment wins). XESS_FG itself belongs in
// the launch options: the XMX shim reads it when it starts, before this DLL is loaded.
#include "ffx_xefg.h"

// The upscalers the game's FidelityFX loader offers on this GPU, through the public version query; the first one is what
// a game that does not choose (FSR 3.1.0 to 3.1.3 cannot) gets
static void LogUpscalers(ID3D12Device* dev)
{
    static bool done;
    if (done || !dev)
        return;
    done = true;
    HMODULE m = GetModuleHandleA("amd_fidelityfx_loader_dx12.dll");
    if (!m || m == g_self)
        m = GetModuleHandleA("amd_fidelityfx_dx12.dll");
    auto query = m && m != g_self ? (PfnFfxQuery)GetProcAddress(m, "ffxQuery") : nullptr;
    if (!query)
        return;
    uint64_t n = 8, ids[8] = {};
    const char* names[8] = {};
    ffxQueryDescGetVersions q = {};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType = FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_UPSCALE, 0);
    q.device = dev;
    q.outputCount = &n;
    q.versionIds = ids;
    q.versionNames = names;
    ffxReturnCode_t r = query(nullptr, &q.header);
    char list[512] = "";
    for (uint64_t i = 0; r == FFX_API_RETURN_OK && i < n && i < 8; i++)
        snprintf(list + strlen(list), sizeof list - strlen(list), "%s%s", i ? ", " : "", names[i] ? names[i] : "?");
    Log("upscalers on this GPU (%d): %s", (int)r, list);
}

// ---------------------------------------------------------------------------------------------------------------
// Helpers

// Row-vector (DirectX) view matrix from camera position and basis.
static void BuildView(const float* pos, const float* right, const float* up, const float* fwd, float* m)
{
    auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    const float r[16] = {right[0], up[0], fwd[0], 0, right[1], up[1], fwd[1], 0, right[2], up[2], fwd[2], 0,
                         -dot(pos, right), -dot(pos, up), -dot(pos, fwd), 1};
    memcpy(m, r, sizeof(r));
}

// Left-handed perspective projection, depth 0..1 (XMMatrixPerspectiveFovLH layout), with the FFX depth flags.
static void BuildProjection(float fovY, float aspect, float n, float f, bool inverted, bool infinite, float* m)
{
    memset(m, 0, 16 * sizeof(float));
    // the planes in either order (with inverted depth UE passes near FLT_MAX, far = the near plane), FLT_MAX = infinity
    float lo = std::min(n, f), hi = std::max(n, f);
    if (infinite || hi >= 1e30f)
        hi = INFINITY;
    n = lo;
    f = hi;
    if (!(fovY > 0.0001f) || !(aspect > 0.0001f) || !(n > 0) || n == f) {
        m[0] = m[5] = m[10] = m[15] = 1;
        return;
    }
    if (inverted) {
        float t = n;
        n = f;
        f = t;
    }
    float ys = 1.0f / tanf(fovY * 0.5f);
    m[0] = ys / aspect;
    m[5] = ys;
    m[11] = 1;
    if (std::isinf(f)) {  // far at infinity
        m[10] = 1;
        m[14] = -n;
    } else if (std::isinf(n)) {  // inverted infinite: near plane value at infinity
        m[10] = 0;
        m[14] = f;
    } else {
        m[10] = f / (f - n);
        m[14] = -n * f / (f - n);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Contexts (a swap chain context is an ScState, ffx_xefg.h)

static const uint64_t kProviderVersionId = 0xf600000000c01006ull;  // what AMD's 4.0.1 DLL reports
static const char* kProviderVersionName = "3.1.6";

struct FgCtx {
    uint32_t magic = MAGIC_FG;
    uint32_t flags = 0;
    uint32_t displayW = 0, displayH = 0, maxRenderW = 0, maxRenderH = 0;
    ID3D12Device* device = nullptr;
    uint64_t prepares = 0;
};

static uint32_t g_lastFgFlags;   // creation flags of the last FG context, used for the XeSS-FG init flags
static bool g_haveFgFlags;

// the current swap chain with a reference, so that a context destroyed on another thread stays valid while in use
static ScState* ScAcquire()
{
    std::lock_guard<std::mutex> lk(g_globalLock);
    if (g_currentSc)
        g_currentSc->refs++;
    return g_currentSc;
}

struct ScRef {
    ScState* st;
    ~ScRef() { if (st) ScRelease(st); }
};

// whether a swap chain context is alive (there is one swap chain at a time)
static bool HaveSwapChain()
{
    std::lock_guard<std::mutex> lk(g_globalLock);
    return g_currentSc != nullptr;
}

// XeSS-FG takes the depth direction and the kind of motion vectors when the swap chain is created, and games create
// the swap chain before the frame generation context that says what they are. The flags a game gave last time are
// kept per game (C:\users\<user>\AppData\Local\ffx_xefg\<exe>.flags in the prefix); without them inverted depth is
// assumed, as in Unreal Engine, Northlight (CONTROL Resonant) and RE Engine. With the wrong depth direction XeSS-FG takes the
// foreground for the background: moving characters flicker, the more the lower the frame rate.
static const uint32_t kXefgFfxFlags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED |
                                      FFX_FRAMEGENERATION_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION |
                                      FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS;
static uint32_t g_initFfxFlags;   // the FFX flags the XeSS-FG swap chain was set up for

static bool FlagsFile(char* path)
{
    char exe[MAX_PATH], base[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", base, MAX_PATH);
    if (!n || n >= MAX_PATH || !GetModuleFileNameA(nullptr, exe, MAX_PATH))
        return false;
    const char* name = strrchr(exe, '\\');
    name = name ? name + 1 : exe;
    snprintf(path, MAX_PATH, "%s\\ffx_xefg", base);
    CreateDirectoryA(path, nullptr);
    snprintf(path, MAX_PATH, "%s\\ffx_xefg\\%s.flags", base, name);
    return true;
}

static bool LoadFlags(uint32_t* flags)
{
    char path[MAX_PATH];
    if (!FlagsFile(path))
        return false;
    FILE* f = fopen(path, "r");
    if (!f)
        return false;
    bool ok = fscanf(f, "%x", flags) == 1;
    fclose(f);
    return ok;
}

static void SaveFlags(uint32_t flags)
{
    char path[MAX_PATH];
    if (!FlagsFile(path))
        return;
    if (FILE* f = fopen(path, "w")) {
        fprintf(f, "%x\n", flags);
        fclose(f);
    }
}

uint32_t XefgInitFlags()
{
    uint32_t f = XEFG_SWAPCHAIN_INIT_FLAG_UITEXTURE_NOT_PREMUL_ALPHA;
    if (g_oflow) {   // our display-size motion vectors in pixels, depth not inverted
        Log("XeSS-FG set up for the optical flow inputs (display-size motion vectors)");
        return f | XEFG_SWAPCHAIN_INIT_FLAG_HIGH_RES_MV;
    }
    uint32_t ffx;
    bool have;
    {
        std::lock_guard<std::mutex> lk(g_globalLock);
        have = g_haveFgFlags;
        ffx = g_lastFgFlags;
    }
    if (have)
        Log("XeSS-FG set up with the game's flags 0x%x", ffx);
    else if (LoadFlags(&ffx))
        Log("XeSS-FG set up with the flags the game gave last time (0x%x)", ffx);
    else {
        ffx = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED;
        Log("XeSS-FG set up for inverted depth (the game's flags come later)");
    }
    g_initFfxFlags = ffx & kXefgFfxFlags;
    if (ffx & FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED)
        f |= XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH;
    if (ffx & FFX_FRAMEGENERATION_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION)
        f |= XEFG_SWAPCHAIN_INIT_FLAG_JITTERED_MV;
    if (ffx & FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS)
        f |= XEFG_SWAPCHAIN_INIT_FLAG_HIGH_RES_MV;
    if (g_mvNdc)
        f |= XEFG_SWAPCHAIN_INIT_FLAG_USE_NDC_VELOCITY;
    return f;
}

// ---------------------------------------------------------------------------------------------------------------
// Frame generation context

#ifdef FFXFG_DEBUG
// FFX_XEFG_DUMP: copies of the game's depth and motion vectors, written some frames later (when the game's command
// list has run) as ffx_xefg_<frame>_<what>.bin: header width, height, row pitch, DXGI format (4 x uint32), rows
struct Dump {
    ID3D12Resource* buf = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    uint32_t frame = 0, writeAt = 0;
    const char* what = "";
    float camera[13] = {};  // fov, position, right, up, forward
};
static Dump g_dumps[16];
static int g_nDumps;

static void DumpCopy(ID3D12Device* dev, ID3D12GraphicsCommandList* cl, ID3D12Resource* res, uint32_t state,
                     uint32_t frame, uint32_t writeAt, const char* what, const float* camera)
{
    if (!dev || !cl || !res || g_nDumps >= 16)
        return;
    D3D12_RESOURCE_DESC rd = res->GetDesc();
    Dump& d = g_dumps[g_nDumps];
    UINT64 total = 0;
    dev->GetCopyableFootprints(&rd, 0, 1, 0, &d.fp, nullptr, nullptr, &total);
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            IID_PPV_ARGS(&d.buf))))
        return;
    D3D12_RESOURCE_STATES s = ToD3D12State(state);
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = s;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (s != D3D12_RESOURCE_STATE_COPY_SOURCE)
        cl->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst = {d.buf, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT}, src = {res};
    dst.PlacedFootprint = d.fp;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    if (s != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        cl->ResourceBarrier(1, &b);
    }
    d.frame = frame;
    d.writeAt = writeAt;
    d.what = what;
    memcpy(d.camera, camera, sizeof(d.camera));
    g_nDumps++;
}

static void DumpWrite(uint32_t now)
{
    for (int i = 0; i < g_nDumps; i++) {
        Dump& d = g_dumps[i];
        if (!d.buf || now < d.writeAt)
            continue;
        void* p = nullptr;
        D3D12_RANGE r = {0, (SIZE_T)d.fp.Footprint.RowPitch * d.fp.Footprint.Height};
        char path[MAX_PATH];
        snprintf(path, MAX_PATH, "%sffx_xefg_%u_%s.bin", g_dir, d.frame, d.what);
        if (SUCCEEDED(d.buf->Map(0, &r, &p))) {
            if (FILE* f = fopen(path, "wb")) {
                uint32_t h[4] = {d.fp.Footprint.Width, d.fp.Footprint.Height, d.fp.Footprint.RowPitch,
                                 (uint32_t)d.fp.Footprint.Format};
                fwrite(h, sizeof(h), 1, f);
                fwrite(d.camera, sizeof(d.camera), 1, f);
                fwrite((const uint8_t*)p + d.fp.Offset, 1, r.End, f);
                fclose(f);
                Log("dump %s: %ux%u format %u", path, h[0], h[1], h[3]);
            }
            d.buf->Unmap(0, nullptr);
        }
        d.buf->Release();
        d.buf = nullptr;
    }
}
#endif

static void TagPrepare(FgCtx* fg, uint64_t frameID, void* cmdList, FfxApiDimensions2D renderSize,
                       FfxApiFloatCoords2D jitter, FfxApiFloatCoords2D mvScale, float frameTimeDelta, bool reset,
                       float cameraNear, float cameraFar, float fovY, const struct FfxApiResource& depth,
                       const struct FfxApiResource& mv, const float* camPos, const float* camUp, const float* camRight,
                       const float* camFwd)
{
    ScState* st = ScAcquire();
    if (!st)
        return;
    ScRef ref{st};   // released after the lock below
    std::lock_guard<std::mutex> lk(st->lock);
    if (st->bridge && st->oflow)   // BeforePresent makes the frames
        return;
    uint32_t id = (uint32_t)frameID;
    if (st->bridge) {
        id = ++st->nextId;
        st->pending.push_back(id);
        if (st->pending.size() > 8)   // upscaled but never presented
            st->pending.pop_front();
    }
    Order("prepare", id);
    TR(tr::Put(tr::PREP, id));

    // SIMULATION_START went out with the sleep after the last present when the id was the expected one, else now
    XellMarker(st, id, XELL_SIMULATION_END);
    XellMarker(st, id, XELL_RENDERSUBMIT_START);

    if (st->oflow) {
        OflowTag(st, id, reset, frameTimeDelta);
        st->frameId = id;
        st->havePrepare = true;
        fg->prepares++;
        return;
    }

    xefg_swapchain_d3d12_resource_data_t rd = {};
    // ONLY_NOW: XeSS-FG records a copy on the game's command list; UNTIL_NEXT_PRESENT (FFX_XEFG_VALIDITY=0, for
    // measuring the copy's cost) reads the game's texture at present time, which the game may have written since
    rd.validity = g_validityNow ? XEFG_SWAPCHAIN_RV_ONLY_NOW : XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT;
    rd.resourceSize = {renderSize.width, renderSize.height};
    rd.type = XEFG_SWAPCHAIN_RES_DEPTH;
    rd.pResource = (ID3D12Resource*)depth.resource;
    rd.incomingState = ToD3D12State(depth.state);
    xefg_swapchain_result_t r1 =
        p_xefgSwapChainD3D12TagFrameResource(st->xefg, (ID3D12CommandList*)cmdList, id, &rd);
    rd.type = XEFG_SWAPCHAIN_RES_MOTION_VECTOR;
    rd.pResource = (ID3D12Resource*)mv.resource;
    rd.incomingState = ToD3D12State(mv.state);
    if (fg->flags & FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS)
        rd.resourceSize = {mv.description.width, mv.description.height};
    xefg_swapchain_result_t r2 =
        p_xefgSwapChainD3D12TagFrameResource(st->xefg, (ID3D12CommandList*)cmdList, id, &rd);

    xefg_swapchain_frame_constant_data_t c = {};
    static const float kPos[3] = {0, 0, 0}, kUp[3] = {0, 1, 0}, kRight[3] = {1, 0, 0}, kFwd[3] = {0, 0, 1};
    bool projInverted = (fg->flags & FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED) != 0;
#ifdef FFXFG_DEBUG
    if (g_camera == 6)   // as if the game gave no camera (a fixed view, the real projection)
        camPos = camRight = camUp = camFwd = nullptr;
    if (g_camera == 3 || g_camera == 4)   // projection not inverted (only the init flag says so)
        projInverted = false;
#endif
    if (g_camera == 5) {
        // debug: all zero, no camera at all
    } else if (g_camera) {
        BuildView(camPos ? camPos : kPos, camRight ? camRight : kRight, camUp ? camUp : kUp, camFwd ? camFwd : kFwd,
                  c.viewMatrix);
        float aspect = renderSize.height ? (float)renderSize.width / (float)renderSize.height : 1.0f;
        BuildProjection(fovY, aspect, cameraNear, cameraFar, projInverted,
                        (fg->flags & FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE) != 0, c.projectionMatrix);
#ifdef FFXFG_DEBUG
        if (g_camera == 2 || g_camera == 4)   // transposed
            for (int r = 0; r < 4; ++r)
                for (int k = r + 1; k < 4; ++k) {
                    std::swap(c.viewMatrix[r * 4 + k], c.viewMatrix[k * 4 + r]);
                    std::swap(c.projectionMatrix[r * 4 + k], c.projectionMatrix[k * 4 + r]);
                }
#endif
    } else {
        for (int i = 0; i < 4; ++i)
            c.viewMatrix[i * 5] = c.projectionMatrix[i * 5] = 1.0f;
    }
    c.jitterOffsetX = jitter.x;
    c.jitterOffsetY = jitter.y;
    // FFX: motion vectors times the scale are pixels of the render size; in NDC that is 2 / size, y up
    c.motionVectorScaleX = g_mvNdc && renderSize.width ? mvScale.x * 2.0f / renderSize.width : mvScale.x;
    c.motionVectorScaleY = g_mvNdc && renderSize.height ? -mvScale.y * 2.0f / renderSize.height : mvScale.y;
    c.resetHistory = reset ? 1 : 0;
    c.frameRenderTime = frameTimeDelta;
#ifdef FFXFG_DEBUG
    c.jitterOffsetX *= g_jitter;
    c.jitterOffsetY *= g_jitter;
    c.frameRenderTime = g_renderTime >= 0 ? g_renderTime : frameTimeDelta * g_renderScale;
#endif
    xefg_swapchain_result_t r3 = p_xefgSwapChainTagFrameConstants(st->xefg, id, &c);

    if (!st->bridge) {
        st->frameId = id;
        st->havePrepare = true;
    }
    fg->prepares++;
#ifdef FFXFG_DEBUG
    static uint32_t dumpFrames;   // frames with interpolation on (menus and loading screens have no depth)
    if (g_dumpEvery > 0) {
        if (st->xefgEnabled)
            dumpFrames++;
        DumpWrite((uint32_t)fg->prepares);
        // two frames in a row, to compare what moved
        if (st->xefgEnabled && dumpFrames > 1 && dumpFrames % g_dumpEvery <= 1 && g_nDumps < 16) {
            float cam[13] = {fovY};
            for (int i = 0; i < 3; i++) {
                cam[1 + i] = camPos ? camPos[i] : 0;
                cam[4 + i] = camRight ? camRight[i] : 0;
                cam[7 + i] = camUp ? camUp[i] : 0;
                cam[10 + i] = camFwd ? camFwd[i] : 0;
            }
            uint32_t at = (uint32_t)fg->prepares + 10;
            auto* cl = (ID3D12GraphicsCommandList*)cmdList;
            DumpCopy(st->device, cl, (ID3D12Resource*)depth.resource, depth.state, id, at, "depth", cam);
            DumpCopy(st->device, cl, (ID3D12Resource*)mv.resource, mv.state, id, at, "mv", cam);
        }
    }
#endif
    if (fg->prepares <= 5 || fg->prepares % 600 == 0 || r1 || r2 || r3)
        Log("prepare #%llu frame %u render %ux%u jitter %.3f,%.3f mvScale %.1f,%.1f dt %.2f reset %d fov %.3f "
            "near %.3f far %.1f -> tag depth %d mv %d const %d",
            (unsigned long long)fg->prepares, id, renderSize.width, renderSize.height, jitter.x, jitter.y, mvScale.x,
            mvScale.y, frameTimeDelta, reset, fovY, cameraNear, cameraFar, (int)r1, (int)r2, (int)r3);
    if (fg->prepares <= 3 || fg->prepares % 600 == 0)
        Log("  camera %s pos %.2f,%.2f,%.2f right %.3f,%.3f,%.3f up %.3f,%.3f,%.3f fwd %.3f,%.3f,%.3f, flags 0x%x, "
            "depth %ux%u fmt %u, mv %ux%u fmt %u",
            camPos ? "given" : "not given", camPos ? camPos[0] : 0, camPos ? camPos[1] : 0, camPos ? camPos[2] : 0,
            camRight ? camRight[0] : 0, camRight ? camRight[1] : 0, camRight ? camRight[2] : 0, camUp ? camUp[0] : 0,
            camUp ? camUp[1] : 0, camUp ? camUp[2] : 0, camFwd ? camFwd[0] : 0, camFwd ? camFwd[1] : 0,
            camFwd ? camFwd[2] : 0, fg->flags, depth.description.width, depth.description.height,
            depth.description.format, mv.description.width, mv.description.height, mv.description.format);
}

static ffxReturnCode_t FgConfigure(FgCtx* fg, const ffxApiHeader* desc)
{
    for (const ffxApiHeader* h = desc; h; h = h->pNext) {
        switch (h->type) {
        case FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION: {
            auto* d = (const ffxConfigureDescFrameGeneration*)h;
            ScState* st = ScAcquire();
            if (!st)
                break;
            ScRef ref{st};   // released after the lock below
            std::lock_guard<std::mutex> lk(st->lock);
            if (st->gameEnabled != d->frameGenerationEnabled)
                Log("game frame generation %s (frame %llu, callbacks present %p fg %p)",
                    d->frameGenerationEnabled ? "on" : "off", (unsigned long long)d->frameID,
                    (void*)d->presentCallback, (void*)d->frameGenerationCallback);
            st->gameEnabled = d->frameGenerationEnabled;
            Order("configure", d->frameID);
            TR(tr::Put(tr::CONF, d->frameID, d->frameGenerationEnabled));
            // only while frame generation is on: RE Requiem configures with frame 0 every frame before that, and
            // XeSS-FG rejects a second tag for the same frame
            if (d->HUDLessColor.resource && d->frameGenerationEnabled && g_hudless) {
                xefg_swapchain_d3d12_resource_data_t rd = {};
                rd.type = XEFG_SWAPCHAIN_RES_HUDLESS_COLOR;
                rd.validity = XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT;
                rd.resourceSize = {d->HUDLessColor.description.width, d->HUDLessColor.description.height};
                rd.pResource = (ID3D12Resource*)d->HUDLessColor.resource;
                rd.incomingState = ToD3D12State(d->HUDLessColor.state);
                p_xefgSwapChainD3D12TagFrameResource(st->xefg, nullptr, (uint32_t)d->frameID, &rd);
                if (!st->uiComposition && p_xefgSwapChainSetUiCompositionState) {
                    xefg_swapchain_result_t r =
                        p_xefgSwapChainSetUiCompositionState(st->xefg, XEFG_SWAPCHAIN_UI_COMPOSITION_STATE_ENABLED);
                    Log("UI composition on (HUD-less color) -> %d", (int)r);
                    st->uiComposition = true;
                }
            }
            break;
        }
        case FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION_KEYVALUE:
        case FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION_REGISTERDISTORTIONRESOURCE:  // XeSS-FG has no distortion input
        case FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG:
            break;
        default:
            if (h == desc)
                return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
        }
    }
    return FFX_API_RETURN_OK;
}

static ffxReturnCode_t FgDispatch(FgCtx* fg, const ffxApiHeader* desc)
{
    switch (desc->type) {
    case FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2: {
        auto* d = (const ffxDispatchDescFrameGenerationPrepareV2*)desc;
        TagPrepare(fg, d->frameID, d->commandList, d->renderSize, d->jitterOffset, d->motionVectorScale,
                   d->frameTimeDelta, d->reset, d->cameraNear, d->cameraFar, d->cameraFovAngleVertical, d->depth,
                   d->motionVectors, d->cameraPosition, d->cameraUp, d->cameraRight, d->cameraForward);
        return FFX_API_RETURN_OK;
    }
    case FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE: {
        auto* d = (const ffxDispatchDescFrameGenerationPrepare*)desc;
        const ffxDispatchDescFrameGenerationPrepareCameraInfo* cam = nullptr;
        for (const ffxApiHeader* h = desc->pNext; h; h = h->pNext)
            if (h->type == FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_CAMERAINFO)
                cam = (const ffxDispatchDescFrameGenerationPrepareCameraInfo*)h;
        TagPrepare(fg, d->frameID, d->commandList, d->renderSize, d->jitterOffset, d->motionVectorScale,
                   d->frameTimeDelta, false, d->cameraNear, d->cameraFar, d->cameraFovAngleVertical, d->depth,
                   d->motionVectors, cam ? cam->cameraPosition : nullptr, cam ? cam->cameraUp : nullptr,
                   cam ? cam->cameraRight : nullptr, cam ? cam->cameraForward : nullptr);
        return FFX_API_RETURN_OK;
    }
    case FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION:
        // The game dispatches the interpolation itself; XeSS-FG does it inside the proxy swap chain instead.
        return FFX_API_RETURN_OK;
    default:
        return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// FSR_FG=inject: a game with FSR upscaling and no FSR frame generation (xefg_bridge.h)

extern "C" __declspec(dllexport) HRESULT XefgBridgeCreateSwapChain(IDXGIFactory* factory, ID3D12CommandQueue* queue, HWND hwnd,
                                                                   const DXGI_SWAP_CHAIN_DESC1* desc,
                                                                   const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen,
                                                                   IDXGISwapChain1** out)
{
    Init();
    if (!g_libsOk)
        return E_FAIL;
    DXGI_SWAP_CHAIN_DESC1 d1 = *desc;
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fs = {};
    if (fullscreen)
        fs = *fullscreen;
    IDXGISwapChain4* sc = nullptr;
    ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 d = {};
    d.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
    d.swapchain = &sc;
    d.hwnd = hwnd;
    d.desc = &d1;
    d.fullscreenDesc = fullscreen ? &fs : nullptr;
    d.dxgiFactory = factory;
    d.gameQueue = queue;
    ffxContext ctx = nullptr;
    if (CreateSwapChainContext(&ctx, ScKind::ForHwnd, &d.header) != FFX_API_RETURN_OK)
        return E_FAIL;
    auto* st = (ScState*)ctx;
    st->bridge = true;
    st->gameEnabled = true;
    // no FFX context here: only the game's reference stays
    st->contextRef = nullptr;
    st->wrapper->Release();
    ScRelease(st);
    *out = sc;
    Log("injected swap chain %p for a game without FSR frame generation", (void*)sc);
    return S_OK;
}

extern "C" __declspec(dllexport) void XefgBridgeFrame(const XefgBridgeFrameDesc* f)
{
    Init();
    ScState* st;
    {
        std::lock_guard<std::mutex> lk(g_globalLock);
        st = g_currentSc;
        if (!st || !st->bridge)
            return;
        st->refs++;
    }
    uint32_t flags = (f->depthInverted ? FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED : 0) |
                     (f->depthInfinite ? FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE : 0) |
                     (f->jitteredMotionVectors ? FFX_FRAMEGENERATION_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION : 0) |
                     (f->displayResolutionMotionVectors ? FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS : 0);
    static FgCtx fg;
    if (!fg.prepares || fg.flags != flags) {
        // the swap chain was made before the first frame: the next start uses these
        uint32_t saved;
        if (!LoadFlags(&saved) || (saved & kXefgFfxFlags) != (flags & kXefgFfxFlags))
            SaveFlags(flags & kXefgFfxFlags);
        if (g_initFfxFlags != (flags & kXefgFfxFlags))
            Log("!! XeSS-FG was set up for flags 0x%x, the game uses 0x%x: wrong until the game starts again", g_initFfxFlags,
                flags & kXefgFfxFlags);
    }
    fg.flags = flags;
    fg.device = st->device;
    TagPrepare(&fg, 0, f->commandList, f->renderSize, f->jitter, f->motionVectorScale, f->frameTimeDelta, f->reset,
               f->cameraNear, f->cameraFar, f->cameraFovY, f->depth, f->motionVectors, nullptr, nullptr, nullptr, nullptr);
    ScRelease(st);
}

// ---------------------------------------------------------------------------------------------------------------
// Exports

// Command list and texture for a game that queries them to dispatch the interpolation itself, made on the first
// query (with the lock held): most games never ask, and FSR_FG=inject never does.
static void InterpObjects(ScState* st)
{
    if (st->interpMade)
        return;
    st->interpMade = true;
    if (SUCCEEDED(st->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&st->alloc))) &&
        SUCCEEDED(st->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, st->alloc, nullptr,
                                                IID_PPV_ARGS(&st->cmdList))))
        st->cmdList->Close();
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = st->width;
    rd.Height = st->height;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = st->format;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(st->device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                   IID_PPV_ARGS(&st->interpTex))))
        st->interpTex = nullptr;
    Log("interpolation command list %p and texture %p for the game's own dispatch", (void*)st->cmdList, (void*)st->interpTex);
}

static bool IsOurType(uint64_t t)
{
    return t == FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION ||
           t == FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12 ||
           t == FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_NEW_DX12 ||
           t == FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
}

extern "C" __declspec(dllexport) ffxReturnCode_t ffxCreateContext(ffxContext* context, ffxCreateContextDescHeader* desc,
                                                                  const ffxAllocationCallbacks*)
{
    Init();
    if (!context || !desc)
        return FFX_API_RETURN_ERROR_PARAMETER;
    if (!IsOurType(desc->type))
        return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
    if (!g_libsOk) {
        Log("create %llx: XeSS-FG/XeLL not available", (unsigned long long)desc->type);
        return FFX_API_RETURN_ERROR_RUNTIME_ERROR;
    }

    if (desc->type == FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION) {
        auto* d = (const ffxCreateContextDescFrameGeneration*)desc;
        auto* fg = new FgCtx();
        fg->flags = d->flags;
        fg->displayW = d->displaySize.width;
        fg->displayH = d->displaySize.height;
        fg->maxRenderW = d->maxRenderSize.width;
        fg->maxRenderH = d->maxRenderSize.height;
        for (const ffxApiHeader* h = desc->pNext; h; h = h->pNext)
            if (h->type == FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12)
                fg->device = ((const ffxCreateBackendDX12Desc*)h)->device;
        {
            std::lock_guard<std::mutex> lk(g_globalLock);
            g_lastFgFlags = d->flags;
            g_haveFgFlags = true;
        }
        *context = fg;
        Log("fg context %p: flags 0x%x display %ux%u maxRender %ux%u", (void*)fg, fg->flags, fg->displayW, fg->displayH,
            fg->maxRenderW, fg->maxRenderH);
        uint32_t saved;
        if (!LoadFlags(&saved) || (saved & kXefgFfxFlags) != (d->flags & kXefgFfxFlags))
            SaveFlags(d->flags & kXefgFfxFlags);
        if (HaveSwapChain() && g_initFfxFlags != (d->flags & kXefgFfxFlags))
            Log("!! XeSS-FG was set up for flags 0x%x, the game uses 0x%x (depth direction, motion vectors): wrong until "
                "the game starts again, which then uses the game's",
                g_initFfxFlags, d->flags & kXefgFfxFlags);
        LogUpscalers(fg->device);
        return FFX_API_RETURN_OK;
    }
    ScKind kind = desc->type == FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12  ? ScKind::Wrap
                  : desc->type == FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_NEW_DX12 ? ScKind::New
                                                                                                     : ScKind::ForHwnd;
    return CreateSwapChainContext(context, kind, desc);
}

extern "C" __declspec(dllexport) ffxReturnCode_t ffxDestroyContext(ffxContext* context, const ffxAllocationCallbacks*)
{
    Init();
    if (!context || !*context)
        return FFX_API_RETURN_ERROR_PARAMETER;
    uint32_t magic = *(uint32_t*)*context;
    if (magic == MAGIC_FG) {
        auto* fg = (FgCtx*)*context;
        Log("destroy fg context %p (prepares %llu)", (void*)fg, (unsigned long long)fg->prepares);
        delete fg;
    } else if (magic == MAGIC_SC) {
        auto* st = (ScState*)*context;
        {
            std::lock_guard<std::mutex> lk(g_globalLock);
            if (g_currentSc == st)
                g_currentSc = nullptr;
        }
        FgSwapChain* w = st->contextRef;
        st->contextRef = nullptr;
        ULONG left = w ? w->Release() : 0;
        Log("destroy swap chain context %p (game still holds the swap chain: %s)", (void*)st, left ? "yes" : "no");
        ScRelease(st);
    } else {
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    *context = nullptr;
    return FFX_API_RETURN_OK;
}

extern "C" __declspec(dllexport) ffxReturnCode_t ffxConfigure(ffxContext* context, const ffxConfigureDescHeader* desc)
{
    Init();
    if (!desc)
        return FFX_API_RETURN_ERROR_PARAMETER;
    if (!context || !*context)
        return desc->type == FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG ? FFX_API_RETURN_OK
                                                                     : FFX_API_RETURN_ERROR_PARAMETER;
    uint32_t magic = *(uint32_t*)*context;
    if (magic == MAGIC_FG)
        return FgConfigure((FgCtx*)*context, desc);
    if (magic != MAGIC_SC)
        return FFX_API_RETURN_ERROR_PARAMETER;

    auto* st = (ScState*)*context;
    switch (desc->type) {
    case FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12: {
        auto* d = (const ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12*)desc;
        std::lock_guard<std::mutex> lk(st->lock);
        if (st->ui.resource)
            ((ID3D12Resource*)st->ui.resource)->Release();
        st->ui = d->uiResource;
        if (st->ui.resource) {
            ((ID3D12Resource*)st->ui.resource)->AddRef();
            if (!st->uiComposition && p_xefgSwapChainSetUiCompositionState) {
                p_xefgSwapChainSetUiCompositionState(st->xefg, XEFG_SWAPCHAIN_UI_COMPOSITION_STATE_ENABLED);
                st->uiComposition = true;
            }
        }
        Log("UI resource %p flags 0x%x", st->ui.resource, d->flags);
        return FFX_API_RETURN_OK;
    }
    case FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_KEYVALUE_DX12:
    case FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG:
        return FFX_API_RETURN_OK;
    default:
        return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
    }
}

extern "C" __declspec(dllexport) ffxReturnCode_t ffxQuery(ffxContext* context, ffxQueryDescHeader* desc)
{
    Init();
    if (!desc)
        return FFX_API_RETURN_ERROR_PARAMETER;

    if (desc->type == FFX_API_QUERY_DESC_TYPE_GET_VERSIONS) {
        auto* d = (ffxQueryDescGetVersions*)desc;
        if (!IsOurType(d->createDescType) || !g_libsOk) {
            if (d->outputCount)
                *d->outputCount = 0;
            return FFX_API_RETURN_OK;
        }
        if (d->outputCount) {
            uint64_t cap = *d->outputCount;
            if (cap > 0) {
                if (d->versionIds)
                    d->versionIds[0] = kProviderVersionId;
                if (d->versionNames)
                    d->versionNames[0] = kProviderVersionName;
            }
            *d->outputCount = 1;
        }
        return FFX_API_RETURN_OK;
    }
    if (desc->type == FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION) {
        auto* d = (ffxQueryGetProviderVersion*)desc;
        d->versionId = kProviderVersionId;
        d->versionName = kProviderVersionName;
        return FFX_API_RETURN_OK;
    }
    if (desc->type == FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE) {
        auto* d = (ffxQueryDescFrameGenerationGetGPUMemoryUsage*)desc;
        if (d->gpuMemoryUsageFrameGeneration)
            *d->gpuMemoryUsageFrameGeneration = {};
        return FFX_API_RETURN_OK;
    }
    if (desc->type == FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE_V2) {
        auto* d = (ffxQueryDescFrameGenerationGetGPUMemoryUsageV2*)desc;
        if (d->gpuMemoryUsageFrameGeneration)
            *d->gpuMemoryUsageFrameGeneration = {};
        return FFX_API_RETURN_OK;
    }
    if (desc->type == FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_GPU_MEMORY_USAGE_DX12) {
        auto* d = (ffxQueryFrameGenerationSwapChainGetGPUMemoryUsageDX12*)desc;
        if (d->gpuMemoryUsageFrameGenerationSwapchain)
            *d->gpuMemoryUsageFrameGenerationSwapchain = {};
        return FFX_API_RETURN_OK;
    }
    if (desc->type == FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_GPU_MEMORY_USAGE_DX12_V2) {
        auto* d = (ffxQueryFrameGenerationSwapChainGetGPUMemoryUsageDX12V2*)desc;
        if (d->gpuMemoryUsageFrameGenerationSwapchain)
            *d->gpuMemoryUsageFrameGenerationSwapchain = {};
        return FFX_API_RETURN_OK;
    }

    if (!context || !*context || *(uint32_t*)*context != MAGIC_SC)
        return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
    auto* st = (ScState*)*context;
    if (desc->type == FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_INTERPOLATIONCOMMANDLIST_DX12) {
        auto* d = (ffxQueryDescFrameGenerationSwapChainInterpolationCommandListDX12*)desc;
        std::lock_guard<std::mutex> lk(st->lock);
        InterpObjects(st);
        if (d->pOutCommandList)
            *d->pOutCommandList = st->cmdList;
        return FFX_API_RETURN_OK;
    }
    if (desc->type == FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_INTERPOLATIONTEXTURE_DX12) {
        auto* d = (ffxQueryDescFrameGenerationSwapChainInterpolationTextureDX12*)desc;
        std::lock_guard<std::mutex> lk(st->lock);
        InterpObjects(st);
        if (d->pOutTexture) {
            *d->pOutTexture = {};
            d->pOutTexture->resource = st->interpTex;
            d->pOutTexture->description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
            d->pOutTexture->description.format = FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM;
            if (st->format == DXGI_FORMAT_R8G8B8A8_UNORM)
                d->pOutTexture->description.format = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
            else if (st->format == DXGI_FORMAT_B8G8R8A8_UNORM)
                d->pOutTexture->description.format = FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM;
            else if (st->format == DXGI_FORMAT_R16G16B16A16_FLOAT)
                d->pOutTexture->description.format = FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
            d->pOutTexture->description.width = st->width;
            d->pOutTexture->description.height = st->height;
            d->pOutTexture->description.depth = 1;
            d->pOutTexture->description.mipCount = 1;
            d->pOutTexture->description.usage = FFX_API_RESOURCE_USAGE_UAV;
            d->pOutTexture->state = FFX_API_RESOURCE_STATE_UNORDERED_ACCESS;
        }
        return FFX_API_RETURN_OK;
    }
    return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
}

extern "C" __declspec(dllexport) ffxReturnCode_t ffxDispatch(ffxContext* context, const ffxDispatchDescHeader* desc)
{
    Init();
    if (!desc || !context || !*context)
        return FFX_API_RETURN_ERROR_PARAMETER;
    uint32_t magic = *(uint32_t*)*context;
    if (magic == MAGIC_FG)
        return FgDispatch((FgCtx*)*context, desc);
    if (magic == MAGIC_SC && desc->type == FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WAIT_FOR_PRESENTS_DX12)
        return FFX_API_RETURN_OK;
    return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
    }
    return TRUE;
}

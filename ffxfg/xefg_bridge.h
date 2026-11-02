// xefg_bridge.h - frame generation for a game that has FSR upscaling but no FSR frame generation (FSR_FG=inject).
//
// dxgi_xefg.cpp (dxgi.dll in the game's folder) gives the game an XeSS-FG swap chain through
// XefgBridgeCreateSwapChain; ffx_xess.cpp hands over each frame's inputs through XefgBridgeFrame. Both are exports of
// ffx_xefg.cpp, loaded as ffx_xefg_bridge.dll so that the game's FidelityFX loader does not take it for a provider.
#pragma once
#include <stdint.h>

#define XEFG_BRIDGE_DLL "ffx_xefg_bridge.dll"

struct XefgBridgeFrameDesc {
    void* commandList;
    FfxApiDimensions2D renderSize;
    FfxApiFloatCoords2D jitter;
    FfxApiFloatCoords2D motionVectorScale;
    float frameTimeDelta;   // ms
    bool reset;
    float cameraNear, cameraFar, cameraFovY;
    struct FfxApiResource depth, motionVectors;
    bool depthInverted, depthInfinite, jitteredMotionVectors, displayResolutionMotionVectors;
};

typedef void (*PFN_XefgBridgeFrame)(const XefgBridgeFrameDesc*);

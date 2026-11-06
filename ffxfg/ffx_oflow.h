// ffx_oflow.h - see ffx_oflow.cpp (FFX_XEFG_OFLOW=1)
#pragma once
#include <d3d12.h>
#include <stdint.h>

struct Oflow;
extern const D3D12_RESOURCE_STATES kOflowOutState;
extern void (*g_oflowLog)(const char* fmt, ...);   // set by the user of this file
Oflow* OflowCreate(ID3D12Device* dev, uint32_t width, uint32_t height, float depthValue);
void OflowDestroy(Oflow* o);
// GPU frame time: OflowFrameStart puts a time stamp on the queue when the game's next frame begins (after the present
// and XeLL's sleep); the next OflowRun closes it. OflowTakeGpuFrames hands over the finished frames (ms, oldest first).
void OflowFrameStart(Oflow*, ID3D12CommandQueue*);
int OflowTakeGpuFrames(Oflow*, float* ms, int max);
ID3D12Resource* OflowMotionVectors(Oflow* o);   // display size, R16G16_FLOAT, pixels, current -> previous
ID3D12Resource* OflowDepth(Oflow* o);           // display size, R32_FLOAT, constant
// records and submits on `q`: optical flow of `color` against the previous run, then the expansion
bool OflowRun(Oflow* o, ID3D12CommandQueue* q, ID3D12Resource* color, D3D12_RESOURCE_STATES colorState, bool hdr,
              bool reset);

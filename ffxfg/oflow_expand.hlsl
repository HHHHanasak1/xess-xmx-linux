// oflow_expand.hlsl - FidelityFX optical flow block vectors (one per 8x8 block, pixels) -> per-pixel motion vectors,
// bilinear between the block centres; constant depth (experiment FFX_XEFG_OFLOW=1, ffx_oflow.cpp)
Texture2D<int2> g_blocks : register(t0);
RWTexture2D<float2> g_mv : register(u0);
RWTexture2D<float> g_depth : register(u1);
cbuffer C : register(b0) { uint2 g_size; float g_depthValue; uint g_block; float g_scale; uint g_writeDepth; };  // g_block in display pixels; g_scale: vectors to display pixels

float2 Block(int2 p, int2 n) { return float2(g_blocks[clamp(p, int2(0, 0), n - 1)]); }

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= g_size))
        return;
    uint bw, bh;
    g_blocks.GetDimensions(bw, bh);
    int2 n = int2(bw, bh);
    float2 f = (float2(id.xy) + 0.5) / g_block - 0.5;
    int2 b = int2(floor(f));
    float2 t = f - b;
    float2 v = lerp(lerp(Block(b, n), Block(b + int2(1, 0), n), t.x),
                    lerp(Block(b + int2(0, 1), n), Block(b + int2(1, 1), n), t.x), t.y);
    g_mv[id.xy] = v * g_scale;
    if (g_writeDepth)   // constant: written by the first run only
        g_depth[id.xy] = g_depthValue;
}

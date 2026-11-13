// oflow_down.hlsl - the final image at 1/g_div size (box filter) for the optical flow, FFX_XEFG_OFLOW_DIV (ffx_oflow.cpp).
// The values stay in the swap chain's encoding (sRGB, PQ or linear scRGB), which is what the optical flow expects;
// the target is a float texture so scRGB values above 1 survive.
Texture2D<float4> g_color : register(t0);
RWTexture2D<float4> g_small : register(u0);
cbuffer C : register(b0) { uint2 g_size; uint g_div; uint g_pad; uint2 g_src; };

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= g_size))
        return;
    float4 s = 0;
    for (uint y = 0; y < g_div; ++y)
        for (uint x = 0; x < g_div; ++x)
            s += g_color[min(id.xy * g_div + uint2(x, y), g_src - 1)];
    g_small[id.xy] = s / float(g_div * g_div);
}

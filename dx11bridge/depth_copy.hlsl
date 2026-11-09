// depth_copy.hlsl - D3D11 compute shader of libxess_dx11.dll: copies the game's depth (a depth format a shared texture
// cannot carry) into the R32_FLOAT texture the D3D12 side reads. Compiled with fxc /T cs_5_0 by build.bat.
Texture2D<float> src : register(t0);
RWTexture2D<float> dst : register(u0);

cbuffer region : register(b0)
{
    uint2 base;   // first texel of the region (the depth base of the execute parameters)
    uint2 size;   // its size
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= size))
        return;
    uint2 p = base + id.xy;
    dst[p] = src.Load(int3(p, 0));
}

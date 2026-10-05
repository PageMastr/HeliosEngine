// rendertest: fullscreen blits of bindless textures into the output (Load, no filtering), and a
// mip-level viewer (one column per mip, nearest sampling through a bindless sampler at explicit LODs).
import core.bindless;

struct Push {
    uint source;       // bindless sampled-image slot
    uint samplerIndex; // for psMips
    uint mipCount;     // for psMips
    uint extent;       // for psMips: output width | height << 16
};
[[vk::push_constant]] ConstantBuffer<Push> gPush;

struct VSOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VSOut vsFullscreen(uint vertexId : SV_VertexID) {
    const float2 p = float2(float((vertexId << 1u) & 2u), float(vertexId & 2u));
    VSOut o;
    o.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    o.uv = float2(p.x, 1.0 - p.y);
    return o;
}

[shader("fragment")]
float4 psBlit(VSOut input) : SV_Target0 {
    return float4(texture2D(gPush.source).Load(int3(int2(input.position.xy), 0)).rgb, 1.0);
}

[shader("fragment")]
float4 psMips(VSOut input) : SV_Target0 {
    // Integer pixel -> texel mapping, then a sample at that texel's centre: which texel a pixel shows
    // must not depend on a driver's interpolation or texel-coordinate precision. Sampling the
    // interpolated coordinate put some pixel centres exactly on texel edges, where NVIDIA and lavapipe
    // pick different texels (ꟻLIP up to 0.54 on the first win-gpu run; a 1e-5 shift on lavapipe
    // reproduced it).
    const uint2 target = uint2(gPush.extent & 0xFFFFu, gPush.extent >> 16u);
    const uint2 pixel = uint2(input.position.xy);
    const uint columnWidth = target.x / gPush.mipCount;
    const uint mip = min(pixel.x / columnWidth, gPush.mipCount - 1u);
    uint width, height, levels;
    texture2D(gPush.source).GetDimensions(mip, width, height, levels);
    // Pixel centre (p + 0.5) scaled by size / extent, in integers: floor((2p + 1) * size / (2 * extent)).
    const uint2 local = uint2(pixel.x - mip * columnWidth, pixel.y);
    const uint2 texel = ((local * 2u + 1u) * uint2(width, height)) / (uint2(columnWidth, target.y) * 2u);
    const float2 uv = (float2(texel) + 0.5) / float2(width, height);
    const float3 c = texture2D(gPush.source).SampleLevel(samplerState(gPush.samplerIndex), uv, float(mip)).rgb;
    return float4(c, 1.0);
}

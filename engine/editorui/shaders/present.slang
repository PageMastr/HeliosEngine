// Copies the editor frame (RGBA8, sRGB-encoded values) to the swapchain image of the same size.
// An sRGB swapchain encodes on write, so the shader decodes first and the bytes stay unchanged.
import core.bindless;

struct Push {
    uint source;
    uint decodeSrgb;
};
[[vk::push_constant]] ConstantBuffer<Push> gPush;

struct VSOut {
    float4 position : SV_Position;
};

[shader("vertex")]
VSOut vsFullscreen(uint vertexId : SV_VertexID) {
    const float2 p = float2(float((vertexId << 1u) & 2u), float(vertexId & 2u));
    VSOut o;
    o.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    return o;
}

float3 srgbToLinear(float3 c) {
    const float3 lo = c / 12.92;
    const float3 hi = pow((c + 0.055) / 1.055, float3(2.4));
    return select(c <= 0.04045, lo, hi);
}

[shader("fragment")]
float4 psPresent(VSOut input) : SV_Target0 {
    const int2 pixel = int2(input.position.xy);
    float4 c = texture2D(gPush.source).Load(int3(pixel, 0));
    if (gPush.decodeSrgb != 0u) c.rgb = srgbToLinear(c.rgb);
    return float4(c.rgb, 1.0);
}

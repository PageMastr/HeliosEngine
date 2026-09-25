// Tonemap / blit (fullscreen triangle): HDR scene color * exposure -> ACES fit (Narkowicz 2015)
// -> sRGB. UNORM outputs are encoded here; sRGB outputs let the hardware encode.
import core.bindless;

struct Push {
    float* exposure;
    uint hdr;         // bindless sampled-image slot
    uint encodeSrgb;  // 1: output is UNORM, apply the sRGB transfer function in the shader
};
[[vk::push_constant]] ConstantBuffer<Push> gPush;

struct VSOut {
    float4 position : SV_Position;
};

[shader("vertex")]
VSOut vsFullscreen(uint vertexId : SV_VertexID) {
    // (0,0), (2,0), (0,2) -> clip (-1,-1), (3,-1), (-1,3): one triangle covering the target.
    const float2 p = float2(float((vertexId << 1u) & 2u), float(vertexId & 2u));
    VSOut o;
    o.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    return o;
}

float3 acesFit(float3 x) {
    return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14));
}

float3 linearToSrgb(float3 c) {
    const float3 lo = c * 12.92;
    const float3 hi = 1.055 * pow(c, float3(1.0 / 2.4)) - 0.055;
    return select(c <= 0.0031308, lo, hi);
}

[shader("fragment")]
float4 psTonemap(VSOut input) : SV_Target0 {
    const int2 pixel = int2(input.position.xy);
    const float3 hdr = texture2D(gPush.hdr).Load(int3(pixel, 0)).rgb;
    float3 c = acesFit(hdr * gPush.exposure[0]);
    if (gPush.encodeSrgb != 0u) c = linearToSrgb(c);
    return float4(c, 1.0);
}

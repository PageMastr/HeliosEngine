// rendertest "bindless": eight instanced quads, each sampling its own bindless texture with its own
// bindless sampler (indices vary per instance -> NonUniformResourceIndex).
//
// Driver-independent by construction: every quad is 64 x 64 pixels at an integer pixel origin of the
// 320 x 180 output, and the fragment shader derives its texture coordinate from the pixel centre
// (SV_Position, exact) instead of an interpolated attribute. A 16-texel texture spans 32 pixels, so
// every pixel centre samples a quarter of the way into a texel, never on a texel edge. The first
// layout sampled some nearest-filtered pixels exactly on texel edges, where the texel a driver picks
// depends on its interpolation and texel-coordinate precision (implementation-defined): NVIDIA's
// picks differed from lavapipe's by up to ꟻLIP 0.81, and shifting the coordinate by 1e-5 on lavapipe
// gave the same 0.81 (first win-gpu run, 2026-10-04).
import core.bindless;

struct Push {
    uint textures[8];
    uint samplers[8];
};
[[vk::push_constant]] ConstantBuffer<Push> gPush;

static const float2 kTarget = float2(320.0, 180.0);  // the scene's output size (scenes.cpp)
static const float kQuad = 64.0;                     // pixels per quad side

/// Top-left pixel of quad `q` in a 4 x 2 grid: integers, so quad edges fall between pixel centres.
float2 quadOrigin(uint q) {
    return float2(12.0 + 77.0 * float(q % 4u), 17.0 + 82.0 * float(q / 4u));
}

struct VSOut {
    float4 position : SV_Position;
    nointerpolation uint quad : QUAD;
};

[shader("vertex")]
VSOut vsMain(uint vertexId : SV_VertexID, uint instance : SV_InstanceID) {
    // Counter-clockwise in clip space (pixel rows grow downwards).
    const float2 corners[6] = { float2(0, 0), float2(0, 1), float2(1, 1), float2(0, 0), float2(1, 1), float2(1, 0) };
    const float2 pixel = quadOrigin(instance) + corners[vertexId] * kQuad;
    VSOut o;
    // Pixel (0, 0) is the top-left corner; clip space is +Y up (the RHI flips the viewport).
    o.position = float4(pixel.x / kTarget.x * 2.0 - 1.0, 1.0 - pixel.y / kTarget.y * 2.0, 0.0, 1.0);
    o.quad = instance;
    return o;
}

[shader("fragment")]
float4 psMain(VSOut input) : SV_Target0 {
    // -0.25 .. 1.75 across the quad, so the repeat / clamp / mirror behaviour of the samplers shows.
    const float2 uv = (input.position.xy - quadOrigin(input.quad)) / (kQuad * 0.5) - 0.25;
    return float4(sampleTexture2DNonUniform(gPush.textures[input.quad], gPush.samplers[input.quad], uv).rgb, 1.0);
}

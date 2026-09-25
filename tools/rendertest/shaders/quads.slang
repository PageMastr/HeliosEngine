// rendertest "bindless": eight instanced quads, each sampling its own bindless texture with its own
// bindless sampler (indices vary per instance -> NonUniformResourceIndex).
import core.bindless;

struct Push {
    uint textures[8];
    uint samplers[8];
};
[[vk::push_constant]] ConstantBuffer<Push> gPush;

struct VSOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation uint quad : QUAD;
};

[shader("vertex")]
VSOut vsMain(uint vertexId : SV_VertexID, uint instance : SV_InstanceID) {
    const float2 corners[6] = { float2(0, 0), float2(1, 0), float2(1, 1), float2(0, 0), float2(1, 1), float2(0, 1) };
    const float2 c = corners[vertexId];
    const float2 cell = float2(float(instance % 4u), float(instance / 4u));
    // 4 x 2 grid of quads with margins, +Y up in clip space.
    const float2 origin = float2(-0.95 + cell.x * 0.48, 0.02 - cell.y * 0.97);
    VSOut o;
    o.position = float4(origin + c * float2(0.42, 0.9), 0.0, 1.0);
    o.uv = float2(c.x, 1.0 - c.y) * 2.5 - 0.25;  // repeat/clamp behaviour of the samplers shows
    o.quad = instance;
    return o;
}

[shader("fragment")]
float4 psMain(VSOut input) : SV_Target0 {
    return float4(sampleTexture2DNonUniform(gPush.textures[input.quad], gPush.samplers[input.quad], input.uv).rgb, 1.0);
}

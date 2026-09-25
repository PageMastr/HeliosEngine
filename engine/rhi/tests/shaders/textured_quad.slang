// Quad sampling a bindless texture with a bindless sampler; indices come from push constants.
import core.bindless;

struct Push {
    uint texture;
    uint samplerIndex;
    float2 scale;  // quad half-extent in clip space
};
[[vk::push_constant]] ConstantBuffer<Push> gPush;

struct VSOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VSOut vsMain(uint vertexId : SV_VertexID) {
    // Two counter-clockwise triangles; uv (0,0) is the top-left texel of the texture.
    const float2 corners[6] = { float2(-1, -1), float2(1, -1), float2(1, 1), float2(-1, -1), float2(1, 1), float2(-1, 1) };
    const float2 c = corners[vertexId];
    VSOut o;
    o.position = float4(c * gPush.scale, 0.0, 1.0);
    o.uv = float2(c.x * 0.5 + 0.5, 0.5 - c.y * 0.5);
    return o;
}

[shader("fragment")]
float4 psMain(VSOut input) : SV_Target0 {
    return sampleTexture2D(gPush.texture, gPush.samplerIndex, input.uv);
}

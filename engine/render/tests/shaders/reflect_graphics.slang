// Reflection probe (render_tests): two stages; the bindless texture and sampler arrays are used by
// the fragment stage only, the push constants by both.
import core.bindless;

struct Push {
    uint texture;
    uint samplerIndex;
    float2 scale;
};
[[vk::push_constant]] ConstantBuffer<Push> gPush;

struct VSOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VSOut vsMain(uint vertexId : SV_VertexID) {
    VSOut o;
    const float2 c = float2(float(vertexId & 1u), float(vertexId >> 1u));
    o.position = float4(c * gPush.scale, 0.0, 1.0);
    o.uv = c;
    return o;
}

[shader("fragment")]
float4 psMain(VSOut input) : SV_Target0 {
    return sampleTexture2D(gPush.texture, gPush.samplerIndex, input.uv);
}

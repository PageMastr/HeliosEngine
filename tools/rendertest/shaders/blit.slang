// rendertest: fullscreen blits of bindless textures into the output (Load, no filtering), and a
// mip-level viewer (four columns showing mips 0..3 with nearest sampling).
import core.bindless;

struct Push {
    uint source;       // bindless sampled-image slot
    uint samplerIndex; // for psMips
    uint mipCount;     // for psMips
    uint pad;
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
    const float columns = float(gPush.mipCount);
    const float column = min(floor(input.uv.x * columns), columns - 1.0);
    const float2 uv = float2(frac(input.uv.x * columns), input.uv.y);
    const float3 c = texture2D(gPush.source).SampleLevel(samplerState(gPush.samplerIndex), uv, column).rgb;
    return float4(c, 1.0);
}

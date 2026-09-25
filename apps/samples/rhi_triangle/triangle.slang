// Spinning triangle for the rhi_triangle sample. Positions are generated from SV_VertexID and
// rotated by an angle from push constants; the aspect ratio keeps it undistorted on resize.
struct Push {
    float angle;
    float aspect;  // width / height
    float2 pad;
};
[[vk::push_constant]] ConstantBuffer<Push> gPush;

struct VSOut {
    float4 position : SV_Position;
    float3 color : COLOR0;
};

[shader("vertex")]
VSOut vsMain(uint vertexId : SV_VertexID) {
    const float2 positions[3] = { float2(-0.6, -0.45), float2(0.6, -0.45), float2(0.0, 0.7) };
    const float3 colors[3] = { float3(1.0, 0.3, 0.1), float3(0.1, 0.9, 0.4), float3(0.2, 0.4, 1.0) };
    const float s = sin(gPush.angle);
    const float c = cos(gPush.angle);
    const float2 p = positions[vertexId];
    const float2 r = float2(p.x * c - p.y * s, p.x * s + p.y * c);
    VSOut o;
    o.position = float4(r.x / max(gPush.aspect, 1e-3), r.y, 0.0, 1.0);
    o.color = colors[vertexId];
    return o;
}

[shader("fragment")]
float4 psMain(VSOut input) : SV_Target0 {
    return float4(input.color, 1.0);
}

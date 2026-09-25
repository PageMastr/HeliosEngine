// rendertest "triangle": one counter-clockwise triangle with per-vertex colors (RC-1).

struct VSOut {
    float4 position : SV_Position;
    float3 color : COLOR;
};

[shader("vertex")]
VSOut vsMain(uint vertexId : SV_VertexID) {
    const float2 positions[3] = { float2(0.0, 0.75), float2(-0.7, -0.6), float2(0.7, -0.6) };
    const float3 colors[3] = { float3(1.0, 0.2, 0.2), float3(0.2, 1.0, 0.25), float3(0.2, 0.35, 1.0) };
    VSOut o;
    o.position = float4(positions[vertexId], 0.0, 1.0);
    o.color = colors[vertexId];
    return o;
}

[shader("fragment")]
float4 psMain(VSOut input) : SV_Target0 {
    return float4(input.color, 1.0);
}

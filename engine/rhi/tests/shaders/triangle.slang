// Vertex-pulled triangle: positions/colors are generated from SV_VertexID, no buffers needed.
struct VSOut {
    float4 position : SV_Position;
    float3 color : COLOR0;
};

[shader("vertex")]
VSOut vsMain(uint vertexId : SV_VertexID) {
    // Counter-clockwise in +Y-up clip space (math-module convention).
    const float2 positions[3] = { float2(-0.8, -0.7), float2(0.8, -0.7), float2(0.0, 0.8) };
    const float3 colors[3] = { float3(1, 0, 0), float3(0, 1, 0), float3(0, 0, 1) };
    VSOut o;
    o.position = float4(positions[vertexId], 0.0, 1.0);
    o.color = colors[vertexId];
    return o;
}

[shader("fragment")]
float4 psMain(VSOut input) : SV_Target0 {
    return float4(input.color, 1.0);
}

// Phase 0 viewport placeholder: a background with a pixel grid, drawn by a render-graph pass into
// a texture the shell shows as an ImGui image (the real viewport of 07 §1.5 replaces it in Phase 1).
struct Push {
    float4 background;
    float4 lineColor;
    float spacing;
    float pad0;
    float pad1;
    float pad2;
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

[shader("fragment")]
float4 psGrid(VSOut input) : SV_Target0 {
    const float2 cell = fmod(floor(input.position.xy), gPush.spacing);
    const bool onLine = cell.x == 0.0 || cell.y == 0.0;
    return onLine ? gPush.lineColor : gPush.background;
}

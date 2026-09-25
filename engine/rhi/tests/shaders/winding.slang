// Draws one triangle from push-constant corners in a solid color (winding / culling test).
struct Push {
    float2 p0;
    float2 p1;
    float2 p2;
    float2 pad;
    float4 color;
};
[[vk::push_constant]] ConstantBuffer<Push> gPush;

[shader("vertex")]
float4 vsMain(uint vertexId : SV_VertexID) : SV_Position {
    const float2 p = vertexId == 0 ? gPush.p0 : (vertexId == 1 ? gPush.p1 : gPush.p2);
    return float4(p, 0.0, 1.0);
}

[shader("fragment")]
float4 psMain() : SV_Target0 {
    return gPush.color;
}

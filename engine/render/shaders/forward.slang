// Forward geometry pass (engine/render forward pipeline v0). Vertices are pulled through buffer
// device addresses; transforms are camera-relative (03 §2.7) and the projection is infinite
// reverse-Z, so depth is cleared to 0 and tested with GreaterOrEqual. Layouts mirror
// helios/render/forward.h (scalar block layout).

struct Vertex {
    float3 position;
    float3 normal;
};

struct InstanceData {
    float4x4 localToCameraRelative;
    float4 color;
};

struct ViewConstants {
    float4x4 viewProj;
    float4 lightDirection;
    float4 lightColor;
    float4 ambient;
};

struct Push {
    ViewConstants* view;
    InstanceData* instances;
    Vertex* vertices;
    uint instance;
    uint pad;
};
[[vk::push_constant]] ConstantBuffer<Push> gPush;

struct VSOut {
    float4 position : SV_Position;
    float3 normal : NORMAL;
    float4 color : COLOR;
};

[shader("vertex")]
VSOut vsMain(uint vertexId : SV_VertexID) {
    const Vertex v = gPush.vertices[vertexId];
    const InstanceData inst = gPush.instances[gPush.instance];
    const float4 cameraRelative = mul(inst.localToCameraRelative, float4(v.position, 1.0));
    VSOut o;
    o.position = mul(gPush.view[0].viewProj, cameraRelative);
    o.normal = mul(inst.localToCameraRelative, float4(v.normal, 0.0)).xyz;  // rigid + uniform scale
    o.color = inst.color;
    return o;
}

[shader("fragment")]
float4 psMain(VSOut input) : SV_Target0 {
    const ViewConstants view = gPush.view[0];
    const float3 n = normalize(input.normal);
    const float ndl = saturate(dot(n, view.lightDirection.xyz));
    // Hemispheric ambient: brighter from above.
    const float3 ambient = view.ambient.rgb * (0.75 + 0.25 * n.y);
    return float4(input.color.rgb * (ambient + view.lightColor.rgb * ndl), 1.0);
}

/// Debug view: world-space normals as color.
[shader("fragment")]
float4 psNormals(VSOut input) : SV_Target0 {
    return float4(normalize(input.normal) * 0.5 + 0.5, 1.0);
}

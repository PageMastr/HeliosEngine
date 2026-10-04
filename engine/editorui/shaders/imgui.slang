// Dear ImGui draw lists through the Helios RHI (07 §1.3): vertices are pulled from an upload
// buffer through a buffer device address, textures come from the bindless heap. Positions are in
// framebuffer pixels (y down); scale/translate map them to +Y-up clip space.
import core.bindless;

struct ImVert {
    float2 pos;
    float2 uv;
    uint col;  // RGBA8, R in the low byte (IM_COL32)
};

struct Push {
    ImVert* vertices;
    float2 scale;
    float2 translate;
    uint vertexOffset;
    uint texture;
    uint samplerIndex;
    uint pad;
};
[[vk::push_constant]] ConstantBuffer<Push> gPush;

struct VSOut {
    float4 position : SV_Position;
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VSOut vsMain(uint vertexId : SV_VertexID) {
    const ImVert v = gPush.vertices[gPush.vertexOffset + vertexId];
    VSOut o;
    o.position = float4(v.pos * gPush.scale + gPush.translate, 0.0, 1.0);
    o.color = float4(float(v.col & 0xFFu), float((v.col >> 8u) & 0xFFu), float((v.col >> 16u) & 0xFFu),
                     float(v.col >> 24u)) * (1.0 / 255.0);
    o.uv = v.uv;
    return o;
}

[shader("fragment")]
float4 psMain(VSOut input) : SV_Target0 {
    return input.color * sampleTexture2D(gPush.texture, gPush.samplerIndex, input.uv);
}

// rendertest "compute" / "postchain" / "mips": a procedural pattern written to a storage image.
import core.bindless;

struct Push {
    uint target;  // bindless storage-image slot
    uint width;
    uint height;
    uint variant;
};
[[vk::push_constant]] ConstantBuffer<Push> gPush;

[shader("compute")]
[numthreads(8, 8, 1)]
void csPattern(uint3 id : SV_DispatchThreadID) {
    if (id.x >= gPush.width || id.y >= gPush.height) return;
    const float2 uv = (float2(id.xy) + 0.5) / float2(gPush.width, gPush.height);
    const float2 c = (uv - 0.5) * float2(float(gPush.width) / float(gPush.height), 1.0);
    const float r = length(c);
    const float rings = 0.5 + 0.5 * cos(r * 48.0 + float(gPush.variant));
    const uint checker = ((id.x >> 4u) ^ (id.y >> 4u)) & 1u;
    float3 color = float3(uv.x, uv.y * 0.8, rings);
    if (checker != 0u) color *= 0.55;
    if (abs(c.x) < 0.01 || abs(c.y) < 0.01) color = float3(1.0, 1.0, 0.9);
    rwTexture2D(gPush.target)[id.xy] = float4(color, 1.0);
}

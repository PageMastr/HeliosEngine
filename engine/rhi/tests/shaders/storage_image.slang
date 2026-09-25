// Writes a gradient into a bindless storage image.
import core.bindless;

struct Push {
    uint image;
    uint width;
    uint height;
    uint pad;
};
[[vk::push_constant]] ConstantBuffer<Push> gPush;

[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= gPush.width || id.y >= gPush.height) return;
    const float4 color = float4(float(id.x) / float(gPush.width - 1), float(id.y) / float(gPush.height - 1), 0.25, 1.0);
    rwTexture2D(gPush.image)[id.xy] = color;
}

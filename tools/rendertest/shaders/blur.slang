// rendertest "postchain": separable 9-tap binomial blur between storage images, and the 2x2 box
// downsample of the "mips" scene (reads mip N-1 through a single-mip view, writes mip N).
import core.bindless;

struct Push {
    uint source;  // bindless sampled-image slot
    uint target;  // bindless storage-image slot
    uint width;   // target size
    uint height;
    int2 direction;
};
[[vk::push_constant]] ConstantBuffer<Push> gPush;

[shader("compute")]
[numthreads(8, 8, 1)]
void csBlur(uint3 id : SV_DispatchThreadID) {
    if (id.x >= gPush.width || id.y >= gPush.height) return;
    const float weights[5] = { 70.0 / 256.0, 56.0 / 256.0, 28.0 / 256.0, 8.0 / 256.0, 1.0 / 256.0 };
    const int2 maxCoord = int2(int(gPush.width) - 1, int(gPush.height) - 1);
    float3 sum = texture2D(gPush.source).Load(int3(int2(id.xy), 0)).rgb * weights[0];
    for (int k = 1; k < 5; ++k) {
        const int2 a = clamp(int2(id.xy) + gPush.direction * k, int2(0, 0), maxCoord);
        const int2 b = clamp(int2(id.xy) - gPush.direction * k, int2(0, 0), maxCoord);
        sum += (texture2D(gPush.source).Load(int3(a, 0)).rgb + texture2D(gPush.source).Load(int3(b, 0)).rgb) * weights[k];
    }
    rwTexture2D(gPush.target)[id.xy] = float4(sum, 1.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void csDownsample(uint3 id : SV_DispatchThreadID) {
    if (id.x >= gPush.width || id.y >= gPush.height) return;
    const int2 s = int2(id.xy) * 2;
    const float3 c = texture2D(gPush.source).Load(int3(s, 0)).rgb + texture2D(gPush.source).Load(int3(s + int2(1, 0), 0)).rgb +
                     texture2D(gPush.source).Load(int3(s + int2(0, 1), 0)).rgb + texture2D(gPush.source).Load(int3(s + int2(1, 1), 0)).rgb;
    rwTexture2D(gPush.target)[id.xy] = float4(c * 0.25, 1.0);
}

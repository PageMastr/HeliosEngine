// Automatic exposure (async compute): log-average luminance of a 16x16 grid of HDR samples,
// reduced in one workgroup, written as an exposure multiplier (key 0.18) to a buffer.
import core.bindless;

struct Push {
    float* output;
    uint hdr;      // bindless sampled-image slot of the HDR scene color
    uint width;
    uint height;
    float bias;    // EV
};
[[vk::push_constant]] ConstantBuffer<Push> gPush;

groupshared float gLogLum[256];

[shader("compute")]
[numthreads(16, 16, 1)]
void csExposure(uint3 tid : SV_GroupThreadID, uint index : SV_GroupIndex) {
    const uint x = min((tid.x * 2u + 1u) * gPush.width / 32u, gPush.width - 1u);
    const uint y = min((tid.y * 2u + 1u) * gPush.height / 32u, gPush.height - 1u);
    const float3 c = texture2D(gPush.hdr).Load(int3(int(x), int(y), 0)).rgb;
    const float lum = dot(c, float3(0.2126, 0.7152, 0.0722));
    gLogLum[index] = log2(max(lum, 1.0e-4));
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = 128u; stride > 0u; stride >>= 1u) {
        if (index < stride) gLogLum[index] += gLogLum[index + stride];
        GroupMemoryBarrierWithGroupSync();
    }
    if (index == 0u) {
        const float average = exp2(gLogLum[0] / 256.0);
        gPush.output[0] = clamp(0.18 / average, 0.05, 20.0) * exp2(gPush.bias);
    }
}

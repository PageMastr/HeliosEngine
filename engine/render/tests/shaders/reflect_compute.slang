// Reflection probe (render_tests): push constants of every member kind, specialization constants
// of three types, a bindless storage buffer and a compute workgroup size.
import core.bindless;

struct Push {
    float4x4 viewProj;
    uint* output;
    uint count;
    float scale;
    float4 tint[2];
};
[[vk::push_constant]] ConstantBuffer<Push> gPush;

[vk::constant_id(3)] const int kQuality = 2;
[vk::constant_id(7)] const bool kFancy = true;
[vk::constant_id(9)] const float kGain = 1.5;

[shader("compute")]
[numthreads(8, 4, 1)]
void csMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= gPush.count) return;
    float v = float(kQuality) * gPush.scale * kGain + gPush.tint[1].x + gPush.viewProj[0][0];
    if (kFancy) v += 1.0;
    gPush.output[id.x] = uint(v) + storageBuffer(1).Load(id.x * 4);
}

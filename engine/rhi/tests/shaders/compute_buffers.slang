// Compute writes through a buffer device address and through the bindless storage-buffer array.
import core.bindless;

struct Push {
    uint* output;        // buffer device address
    uint count;
    uint bindlessBuffer; // slot in the bindless storage-buffer array
    uint salt;
};
[[vk::push_constant]] ConstantBuffer<Push> gPush;

[shader("compute")]
[numthreads(64, 1, 1)]
void csWriteAddress(uint3 id : SV_DispatchThreadID) {
    const uint i = id.x;
    if (i >= gPush.count) return;
    gPush.output[i] = i * i + gPush.salt;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void csWriteBindless(uint3 id : SV_DispatchThreadID) {
    const uint i = id.x;
    if (i >= gPush.count) return;
    storageBuffer(gPush.bindlessBuffer).Store(i * 4, (i ^ 0xABCDu) + gPush.salt);
}

// Adds 1000 to every element (used to chain work across queues).
[shader("compute")]
[numthreads(64, 1, 1)]
void csIncrement(uint3 id : SV_DispatchThreadID) {
    const uint i = id.x;
    if (i >= gPush.count) return;
    gPush.output[i] = gPush.output[i] + 1000u;
}

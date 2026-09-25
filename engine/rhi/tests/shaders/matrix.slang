// Pins the C++ <-> Slang matrix convention: a helios::math::Mat4 pushed unchanged must satisfy
// mul(M, v) == M * v (column-major storage, column vectors).
struct Push {
    float4x4 m;
    float4 v;
    float4* output;
};
[[vk::push_constant]] ConstantBuffer<Push> gPush;

[shader("compute")]
[numthreads(1, 1, 1)]
void csMain() {
    gPush.output[0] = mul(gPush.m, gPush.v);
    gPush.output[1] = gPush.m[0];  // first *row* in Slang indexing
}

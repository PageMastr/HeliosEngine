// Helios global bindless heap (docs/plan/03-rendering.md §1.1). `import core.bindless;`
//
// One descriptor set (set 0) shared by every pipeline, update-after-bind and partially bound.
// The layout must match engine/rhi (VulkanDevice::createBindlessHeap in src/vulkan/vk_device.cpp,
// BindlessBinding in helios/rhi/types.h):
//
//   binding 0  sampled images   Texture2D / Texture2DArray / TextureCube / Texture3D aliases
//   binding 1  storage images   RWTexture2D<float4|uint> / RWTexture2DArray / RWTexture3D aliases
//   binding 2  storage buffers  RWByteAddressBuffer (most buffer access uses device addresses)
//   binding 3  samplers         deduplicated SamplerStates
//
// Index 0 of every array holds a default resource (1x1 magenta texture, 1x1 storage image, 256-byte
// buffer, linear/repeat sampler), so an uninitialized index shows up visibly instead of faulting.
// Everything else a pass needs travels in push constants (<= 128 bytes, scalar layout) carrying
// these indices and buffer device addresses.
//
// Uniform indices (the same for every invocation, e.g. from push constants) may use the plain
// accessors; indices that vary per invocation (per-instance material data) must use the
// *NonUniform variants, which wrap NonUniformResourceIndex.
module bindless;

public static const uint kInvalidBindlessIndex = 0xFFFFFFFF;

// The arrays alias on purpose (descriptor indexing allows different image types on one binding);
// the resulting E39001 warning is disabled in cmake/HeliosShaders.cmake.
[[vk::binding(0, 0)]] public Texture2D gTexture2D[];
[[vk::binding(0, 0)]] public Texture2D<uint4> gTexture2DUint[];
[[vk::binding(0, 0)]] public Texture2DArray gTexture2DArray[];
[[vk::binding(0, 0)]] public TextureCube gTextureCube[];
[[vk::binding(0, 0)]] public Texture3D gTexture3D[];

[[vk::binding(1, 0)]] public RWTexture2D<float4> gRWTexture2D[];
[[vk::binding(1, 0)]] public RWTexture2D<uint4> gRWTexture2DUint[];
[[vk::binding(1, 0)]] public RWTexture2DArray<float4> gRWTexture2DArray[];
[[vk::binding(1, 0)]] public RWTexture3D<float4> gRWTexture3D[];

[[vk::binding(2, 0)]] public RWByteAddressBuffer gBuffers[];

[[vk::binding(3, 0)]] public SamplerState gSamplers[];

public Texture2D texture2D(uint index) { return gTexture2D[index]; }
public Texture2D texture2DNonUniform(uint index) { return gTexture2D[NonUniformResourceIndex(index)]; }
public Texture2DArray texture2DArray(uint index) { return gTexture2DArray[index]; }
public TextureCube textureCube(uint index) { return gTextureCube[index]; }
public Texture3D texture3D(uint index) { return gTexture3D[index]; }
public RWTexture2D<float4> rwTexture2D(uint index) { return gRWTexture2D[index]; }
public RWByteAddressBuffer storageBuffer(uint index) { return gBuffers[index]; }
public RWByteAddressBuffer storageBufferNonUniform(uint index) { return gBuffers[NonUniformResourceIndex(index)]; }
public SamplerState samplerState(uint index) { return gSamplers[index]; }

/// Samples a bindless 2D texture with a bindless sampler (both indices uniform).
public float4 sampleTexture2D(uint texture, uint samplerIndex, float2 uv) {
    return gTexture2D[texture].Sample(gSamplers[samplerIndex], uv);
}

/// Same as sampleTexture2D for indices that vary per invocation.
public float4 sampleTexture2DNonUniform(uint texture, uint samplerIndex, float2 uv) {
    return gTexture2D[NonUniformResourceIndex(texture)].Sample(gSamplers[NonUniformResourceIndex(samplerIndex)], uv);
}

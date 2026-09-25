#pragma once
// Helios shader reflection (.hsr, docs/plan/03-rendering.md §1.7): what a SPIR-V module exposes to
// the engine — entry points and workgroup sizes, the push-constant layout, specialization
// constants, descriptor bindings with their access and the entry points that use them, and a
// content hash for the derived-data cache.
//
// helios-shaderc (tools/shaderc) writes it next to every SPIR-V module in the binary format below
// (plus an optional human-readable JSONC rendering); reflectSpirv() computes the same data from any
// SPIR-V at run time, so pipelines can check push-constant sizes and entry points without a sidecar.
//
// Binary format, version 1 (all integers little-endian, records packed without padding):
//
//   Header (48 bytes)
//     0  u32  magic 'HSR1' (bytes 48 53 52 31)
//     4  u16  version (1)
//     6  u16  header size (48)
//     8  u32  total size in bytes
//    12  u32  SPIR-V version word (0x00010600 = 1.6)
//    16  u64  content hash low  (XXH3-128 of the SPIR-V bytes)
//    24  u64  content hash high
//    32  u32  entry-point count            E
//    36  u32  push-constant block size in bytes
//    40  u16  push-constant member count   M
//    42  u16  specialization-constant count S
//    44  u32  binding count                B
//   Entry points (E x 20 bytes): u32 name, u8 stage (ShaderStage), u8 flags (bit 0: uses push
//     constants), u16 reserved (0), u32 workgroup x, y, z (0 for non-compute stages)
//   Push-constant block (4 bytes): u32 block type name
//   Push-constant members (M x 20 bytes): u32 name, u32 offset, u32 size, u8 kind (ShaderValueKind),
//     u8 component count (vector size, rows*columns of a matrix, else 1), u16 reserved, u32 array count
//     (0 = not an array)
//   Specialization constants (S x 16 bytes): u32 constant id, u32 name, u8 kind, u8[3] reserved,
//     u32 default value bits (bool 0/1, int/uint two's complement, float IEEE-754)
//   Bindings (B x 24 bytes): u32 set, u32 binding, u32 descriptor count (0 = runtime array),
//     u8 kind (ShaderBindingKind), u8 access (ShaderAccess), u16 reserved, u32 name,
//     u32 entry-point mask (bit i = entry point i uses the binding; entry points >= 32 are not tracked)
//   String table: u32 byte size, then NUL-terminated UTF-8 strings. Every "name" field above is a
//     byte offset into the table's string bytes.
//
// Readers reject a wrong magic, version 0 or a newer version, truncated data, out-of-range string
// offsets, unknown enum values, and blobs whose string references would decode to more than
// 1 MiB + 16x the blob size (Corrupt / VersionMismatch). The input is treated as untrusted: memory
// and time stay linear in its size. reflectSpirv() likewise bounds its work on malformed modules
// (cyclic or exponentially shared type graphs, huge member indices) and rejects modules whose
// push-constant block or specialization constants exceed the u16 counts of the format.
// Threading: plain values and pure functions.

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "helios/core/hash.h"
#include "helios/core/result.h"
#include "helios/core/types.h"

namespace helios::render {

enum class ShaderStage : u8 {
    Vertex, TessControl, TessEval, Geometry, Fragment, Compute, Task, Mesh,
    RayGen, Intersection, AnyHit, ClosestHit, Miss, Callable, Unknown,
};
std::string_view shaderStageName(ShaderStage stage) noexcept;

enum class ShaderValueKind : u8 {
    Unknown, Bool, Int, UInt, Half, Float, Double, Int64, UInt64, Vector, Matrix, Struct, Pointer,
};
std::string_view shaderValueKindName(ShaderValueKind kind) noexcept;

enum class ShaderBindingKind : u8 {
    SampledImage, StorageImage, Sampler, CombinedImageSampler, UniformBuffer, StorageBuffer,
    UniformTexelBuffer, StorageTexelBuffer, AccelerationStructure, Unknown,
};
std::string_view shaderBindingKindName(ShaderBindingKind kind) noexcept;

enum class ShaderAccess : u8 { None = 0, Read = 1, Write = 2, ReadWrite = 3 };
std::string_view shaderAccessName(ShaderAccess access) noexcept;

struct ShaderEntryPoint {
    std::string name;
    ShaderStage stage = ShaderStage::Unknown;
    std::array<u32, 3> workgroupSize{0, 0, 0};
    bool usesPushConstants = false;
    friend bool operator==(const ShaderEntryPoint&, const ShaderEntryPoint&) = default;
};

/// A member of the push-constant block (scalar block layout: offsets as declared, no padding).
struct ShaderMember {
    std::string name;
    u32 offset = 0;
    u32 size = 0;
    ShaderValueKind kind = ShaderValueKind::Unknown;  ///< Element kind for arrays.
    u32 components = 1;
    u32 arrayCount = 0;  ///< 0 = not an array.
    friend bool operator==(const ShaderMember&, const ShaderMember&) = default;
};

struct ShaderPushConstants {
    std::string blockName;  ///< Type name of the block ("" when the module has none).
    u32 size = 0;           ///< Bytes (end of the last member); must be <= 128 (03 §1.1).
    std::vector<ShaderMember> members;
    friend bool operator==(const ShaderPushConstants&, const ShaderPushConstants&) = default;
};

struct ShaderSpecConstant {
    u32 id = 0;
    std::string name;
    ShaderValueKind kind = ShaderValueKind::Unknown;  ///< Bool, Int, UInt or Float.
    u32 defaultBits = 0;
    friend bool operator==(const ShaderSpecConstant&, const ShaderSpecConstant&) = default;
};

struct ShaderBinding {
    u32 set = 0;
    u32 binding = 0;
    ShaderBindingKind kind = ShaderBindingKind::Unknown;
    u32 count = 1;  ///< Array size; 0 = runtime-sized (bindless).
    std::string name;
    ShaderAccess access = ShaderAccess::Read;
    u32 entryPointMask = 0;
    friend bool operator==(const ShaderBinding&, const ShaderBinding&) = default;
};

struct ShaderReflection {
    u32 spirvVersion = 0;
    Hash128 contentHash{};
    std::vector<ShaderEntryPoint> entryPoints;
    ShaderPushConstants pushConstants;
    std::vector<ShaderSpecConstant> specConstants;  ///< Sorted by id.
    std::vector<ShaderBinding> bindings;            ///< Sorted by (set, binding, name).

    const ShaderEntryPoint* findEntryPoint(std::string_view name) const noexcept;
    friend bool operator==(const ShaderReflection&, const ShaderReflection&) = default;
};

inline constexpr u32 kHsrMagic = 0x31525348u;  // "HSR1"
inline constexpr u16 kHsrVersion = 1;

/// Reflects a SPIR-V module (any version; entry-point binding use needs SPIR-V >= 1.4 interfaces).
/// Fails with Corrupt on malformed SPIR-V.
Result<ShaderReflection> reflectSpirv(std::span<const u32> words);
/// Same from bytes (must be a multiple of 4).
Result<ShaderReflection> reflectSpirvBytes(std::span<const u8> bytes);

/// Binary .hsr (see the header comment); deterministic for equal input.
std::vector<u8> serializeReflection(const ShaderReflection& reflection);
Result<ShaderReflection> parseReflection(std::span<const u8> bytes);
/// Human-readable JSONC rendering of the same data (for diffs and review; not read back).
std::string reflectionToJsonc(const ShaderReflection& reflection);

} // namespace helios::render

// helios/pcg/program.h — terrain-graph bytecode (02 §5.8 "Bytecode", 03 §5.5a).
//
// A TerrainProgram is register bytecode compiled for one (graph, level) pair: the level decides
// which fine octaves are skipped (03 §5.5a "Level-adaptive octaves"), so CPU and GPU skip exactly
// the same octaves. Every instruction is sixteen little-endian u32 words; the same words are
// uploaded to the GPU and interpreted by shaders/pcg/hnoise.slang, so this layout is part of the
// twin contract:
//
//   w0  op | dst << 8 | a << 16 | b << 24
//   w1  c | octaves << 8 | (u8)wavelengthExp << 16 | flags << 24
//   w2  seed
//   w3  amplitude (Q16.16 metres, i32)
//   w4  gain (Q16, i32)
//   w5..w7  reserved (0)
//   w8..w15 four Q32.32 constants k0..k3 as (lo, hi) word pairs
//
// Registers: height registers h0..h15 hold Q32.32 per sample; position registers p0..p3 hold three
// Q32.32 coordinates per sample (p0 is the tile's base position).
//
//   Const   h[dst] = k0
//   Noise   h[dst] = amp * noise(p[a], seed, e)                        (octaves == 1)
//   Fbm     h[dst] = sum_{i<octaves} amp_i * noise(p[a], seed_i, e - i)
//   Ridged  h[dst] = sum_{i<octaves} amp_i * sq(max(0, 1 - |noise_i|))
//   Warp    p[dst] = p[a] + amp * (noise(p[a], seed, e), noise(.., seed + s, e), noise(.., seed + 2s, e))
//   Add/Sub/Mul/Min/Max   h[dst] = h[a] op h[b]
//   Clamp   h[dst] = min(max(h[a], k0), k1)
//   Remap   h[dst] = k2 + (h[a] - k0) * k1   [clamped to [min(k2,k3), max(k2,k3)] if flags & 1]
//   Select  h[dst] = h[a] >= k0 ? h[b] : h[c]
//
// Threading: programs are immutable values; share them freely.
#pragma once

#include <array>
#include <span>
#include <vector>

#include "helios/core/result.h"
#include "helios/core/types.h"
#include "helios/pcg/terrain_graph.h"

namespace helios::pcg {

enum class Opcode : u8 {
    Const = 0,
    Noise,
    Fbm,
    Ridged,
    Warp,
    Add,
    Sub,
    Mul,
    Min,
    Max,
    Clamp,
    Remap,
    Select,
    Count
};

inline constexpr u32 kMaxHeightRegisters = 16;
inline constexpr u32 kMaxPositionRegisters = 4;
inline constexpr u32 kMaxOctaves = 24;
inline constexpr u32 kInstrWords = 16;
inline constexpr u8 kRemapClampFlag = 1;

/// One instruction (see the header comment for the word layout).
struct Instr {
    std::array<u32, kInstrWords> w{};

    Opcode op() const noexcept { return static_cast<Opcode>(w[0] & 0xFF); }
    u8 dst() const noexcept { return static_cast<u8>(w[0] >> 8); }
    u8 a() const noexcept { return static_cast<u8>(w[0] >> 16); }
    u8 b() const noexcept { return static_cast<u8>(w[0] >> 24); }
    u8 c() const noexcept { return static_cast<u8>(w[1] & 0xFF); }
    u8 octaves() const noexcept { return static_cast<u8>(w[1] >> 8); }
    i32 wavelengthExp() const noexcept { return static_cast<i8>(static_cast<u8>(w[1] >> 16)); }
    u8 flags() const noexcept { return static_cast<u8>(w[1] >> 24); }
    u32 seed() const noexcept { return w[2]; }
    i32 amplitude() const noexcept { return static_cast<i32>(w[3]); }
    i32 gain() const noexcept { return static_cast<i32>(w[4]); }
    i64 k(u32 i) const noexcept {
        return static_cast<i64>((static_cast<u64>(w[9 + 2 * i]) << 32) | w[8 + 2 * i]);
    }

    static Instr make(Opcode op, u8 dst, u8 a = 0, u8 b = 0, u8 c = 0) noexcept;
    void setFractal(u8 octaves, i32 wavelengthExp, u32 seed, i32 amplitude, i32 gain) noexcept;
    void setFlags(u8 flags) noexcept;
    void setK(u32 i, i64 value) noexcept;

    friend bool operator==(const Instr&, const Instr&) = default;
};
static_assert(sizeof(Instr) == kInstrWords * 4);

/// Level-adaptive octave rule inputs (03 §5.5a).
struct CompileOptions {
    /// Sample spacing of the tiles this program evaluates, metres. 0 = full detail.
    f64 sampleSpacingMetres = 0.0;
    /// Collision levels always evaluate every octave (02 §5.8 contract).
    bool collisionLevel = false;
    /// Octaves are only skipped when the spacing exceeds this (coarser than the collision levels).
    f64 minSkipSpacingMetres = 2.0;
    /// Octaves whose wavelength is below this multiple of the spacing are skip candidates.
    f64 skipWavelengthFactor = 2.0;
    /// The summed effect of every skipped octave on the graph OUTPUT (its amplitude times the noise
    /// bound times the generator's static output sensitivity through remaps, muls and the rest)
    /// stays below this multiple of the spacing: 0.5 px at the level's minimum view distance, which
    /// CDLOD makes proportional to the spacing (range ratio 2). 0.06 gives 03 §5.5a's 12 cm where the
    /// first such level starts (spacing ~2 m, ~500 m away). Generators that feed a select condition
    /// are never skipped.
    f64 skippedAmplitudePerSpacing = 0.06;

    /// The amplitude limit for this spacing, metres.
    f64 maxSkippedAmplitudeMetres() const noexcept { return skippedAmplitudePerSpacing * sampleSpacingMetres; }
};

/// Compiled bytecode for one (graph, level).
struct TerrainProgram {
    std::vector<Instr> code;
    u8 heightRegisters = 0;   ///< registers used (<= kMaxHeightRegisters)
    u8 positionRegisters = 1; ///< including p0 (<= kMaxPositionRegisters)
    u8 outputRegister = 0;
    u32 evaluatedOctaves = 0; ///< noise evaluations per sample (warps count 3)
    u32 skippedOctaves = 0;   ///< octaves removed by the level-adaptive rule
    /// Static bound on how far the skipped octaves can move the output height, metres
    /// (<= CompileOptions::maxSkippedAmplitudeMetres()).
    f64 skippedAmplitudeMetres = 0.0;

    /// Program words for GPU upload (code.size() * 16 words).
    std::span<const u32> words() const noexcept {
        return {code.empty() ? nullptr : code.front().w.data(), code.size() * kInstrWords};
    }
    /// Stable hash of the bytecode and register counts.
    u64 hash() const noexcept;
};

/// Compiles a validated graph. Registers are reused after their last use (linear scan in node
/// order), so a 40-node graph typically needs 4-8 height registers. Fails on an invalid graph or on
/// non-finite or negative CompileOptions.
Result<TerrainProgram> compileTerrainGraph(const TerrainGraph& graph, const CompileOptions& options = {});

/// Checks a program before either twin runs it: register counts and indices, opcodes, octave ranges,
/// and that every register is written before it is read and the output register is written (the
/// CPU and GPU register files start differently, so a read-before-write would make them diverge).
/// TileEvaluator::evaluate() calls it; a GPU uploader must call it too (Corrupt / InvalidArgument).
Result<void> validateTerrainProgram(const TerrainProgram& program);

/// Amplitude (metres) of octave `octave` of a generator: the Q16 recurrence amp_{i+1} = amp_i * gain,
/// exactly as the kernels compute it.
Q16 octaveAmplitude(Q16 amplitude, Q16 gain, u32 octave) noexcept;

} // namespace helios::pcg

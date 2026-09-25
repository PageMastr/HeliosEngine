// Tile domains, the SoA tile evaluator and the per-sample reference evaluator (helios/pcg/tile.h).
#include "helios/pcg/tile.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <numbers>

#include "helios/core/assert.h"
#include "helios/core/hash.h"
#include "helios/core/memory.h"
#include "kernels/vm_kernels.h"

namespace helios::pcg {

static_assert(vm::kStride == kTileStride && vm::kSamples == kTileSamples);
static_assert(vm::kHeightRegs == kMaxHeightRegisters && vm::kPosRegs == kMaxPositionRegisters);
static_assert(vm::kOpCount == static_cast<u32>(Opcode::Count) && vm::kInstrWords == kInstrWords);
static_assert(sizeof(vm::Domain::w) / sizeof(u32) == TileDomain::kWords);

namespace {

MemoryTag pcgTag() {
    static const MemoryTag tag = registerMemoryTag("pcg.tiles");
    return tag;
}

struct SampleTables {
    alignas(64) u32 i[kTileStride];
    alignas(64) u32 j[kTileStride];
    SampleTables() {
        for (u32 s = 0; s < kTileStride; ++s) {
            const u32 idx = s < kTileSamples ? s : kTileSamples - 1; // padding repeats the last sample
            i[s] = idx % kTileEdge;
            j[s] = idx / kTileEdge;
        }
    }
};

const SampleTables& sampleTables() {
    static const SampleTables tables;
    return tables;
}

const vm::Kernels* kernelsFor(KernelKind kind) noexcept {
    switch (kind) {
        case KernelKind::Avx2: return vm::kAvx2Kernels;
        case KernelKind::Sse42: return vm::kSse42Kernels;
        default: return vm::kScalarKernels;
    }
}

i64 packI64(u32 lo, u32 hi) noexcept { return static_cast<i64>((static_cast<u64>(hi) << 32) | lo); }

} // namespace

// ---------------------------------------------------------------------------------------------
// TileDomain
// ---------------------------------------------------------------------------------------------

TileDomain TileDomain::cubeSphere(const CubeTile& tile, f64 radiusMetres) noexcept {
    TileDomain d;
    d.kind = DomainKind::CubeSphere;
    d.tile = tile;
    d.radiusQ8 = hnoise::radiusToQ8(radiusMetres);
    return d;
}

TileDomain TileDomain::planar(const FixedPos& origin, i64 spacingRaw, u32 tileX, u32 tileY) noexcept {
    TileDomain d;
    d.kind = DomainKind::Planar;
    d.tile.x = tileX;
    d.tile.y = tileY;
    d.origin = origin;
    d.spacingRaw = spacingRaw;
    return d;
}

f64 TileDomain::sampleSpacingMetres() const noexcept {
    if (kind == DomainKind::Planar) return std::abs(Q32::fromRaw(spacingRaw).toDouble());
    const f64 radius = static_cast<f64>(radiusQ8) / 256.0;
    return (std::numbers::pi / 2.0) * radius / (64.0 * std::ldexp(1.0, tile.level));
}

std::array<u32, TileDomain::kWords> TileDomain::pack() const noexcept {
    std::array<u32, kWords> w{};
    w[0] = static_cast<u32>(kind);
    w[1] = static_cast<u32>(tile.face);
    w[2] = tile.level;
    w[3] = tile.x;
    w[4] = tile.y;
    w[5] = radiusQ8;
    const i64 o[3] = {origin.x, origin.y, origin.z};
    for (u32 a = 0; a < 3; ++a) {
        w[6 + 2 * a] = static_cast<u32>(static_cast<u64>(o[a]));
        w[7 + 2 * a] = static_cast<u32>(static_cast<u64>(o[a]) >> 32);
    }
    w[12] = static_cast<u32>(static_cast<u64>(spacingRaw));
    w[13] = static_cast<u32>(static_cast<u64>(spacingRaw) >> 32);
    return w;
}

Result<void> validateTileDomain(const TileDomain& d) noexcept {
    if (d.kind == DomainKind::Planar) return {}; // every planar tile is well defined (coordinates wrap)
    if (d.kind != DomainKind::CubeSphere) return Error{ErrorCode::InvalidArgument, "tile domain: unknown kind"};
    // Checked rather than clamped: the kernels, the reference and the Slang twin would otherwise
    // each evaluate some other tile (level 26 as level 25, face 6 as +X, x >= 2^level wrapped).
    if (static_cast<u32>(d.tile.face) >= 6 || d.tile.level > hnoise::kMaxCubeLevel || d.tile.x >= (1u << d.tile.level) ||
        d.tile.y >= (1u << d.tile.level)) {
        return Error{ErrorCode::InvalidArgument, "tile domain: cube tile outside face 0..5, level 0..25, x, y < 2^level"};
    }
    return {};
}

FixedPos samplePosition(const TileDomain& d, u32 index) noexcept {
    const u32 idx = index < kTileSamples ? index : kTileSamples - 1;
    const u32 i = idx % kTileEdge, j = idx / kTileEdge;
    if (d.kind == DomainKind::CubeSphere) {
        const u32 level = std::min<u32>(d.tile.level, hnoise::kMaxCubeLevel);
        const i32 s = hnoise::faceCoordQ30(d.tile.x, i, level);
        const i32 t = hnoise::faceCoordQ30(d.tile.y, j, level);
        const CubeFace face = static_cast<u32>(d.tile.face) < 6 ? d.tile.face : CubeFace::PosX;
        return hnoise::cubeSpherePosition(face, s, t, d.radiusQ8);
    }
    const u64 mx = static_cast<u32>(d.tile.x * 64u + i), my = static_cast<u32>(d.tile.y * 64u + j);
    const u64 spacing = static_cast<u64>(d.spacingRaw);
    return {static_cast<i64>(static_cast<u64>(d.origin.x) + mx * spacing),
            static_cast<i64>(static_cast<u64>(d.origin.y) + my * spacing), d.origin.z};
}

u64 hashHeights(std::span<const i64> heights) noexcept {
    // XXH3 of the little-endian bytes (every supported target is little-endian).
    return hash64(heights.data(), heights.size_bytes(), 0x68656967687473ull);
}

// ---------------------------------------------------------------------------------------------
// TileEvaluator
// ---------------------------------------------------------------------------------------------

struct TileEvaluator::Scratch {
    void* block = nullptr;
    vm::Registers regs{};

    Scratch() {
        constexpr usize kPlane64 = kTileStride * sizeof(i64);
        constexpr usize kPlane32 = (kTileStride * sizeof(u32) + 63) & ~usize(63);
        constexpr usize kBytes = kPlane64 * kMaxHeightRegisters + kPlane32 * 2 * 3 * kMaxPositionRegisters;
        block = alignedAlloc(kBytes, 64, pcgTag());
        HELIOS_VERIFY(block != nullptr, "pcg: out of memory for tile scratch");
        std::memset(block, 0, kBytes);
        auto* p = static_cast<u8*>(block);
        for (u32 r = 0; r < kMaxHeightRegisters; ++r, p += kPlane64) regs.h[r] = reinterpret_cast<i64*>(p);
        for (u32 r = 0; r < kMaxPositionRegisters; ++r) {
            for (u32 a = 0; a < 3; ++a) {
                regs.posHi[r][a] = reinterpret_cast<i32*>(p);
                p += kPlane32;
                regs.posLo[r][a] = reinterpret_cast<u32*>(p);
                p += kPlane32;
            }
        }
        regs.sampleI = sampleTables().i;
        regs.sampleJ = sampleTables().j;
    }
    ~Scratch() { alignedFree(block); }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
};

TileEvaluator::TileEvaluator(std::optional<KernelKind> kernel)
    : m_kernel(kernel && kernelSupported(*kernel) ? *kernel : activeKernel()), m_scratch(std::make_unique<Scratch>()) {}

TileEvaluator::~TileEvaluator() = default;

Result<void> validateTerrainProgram(const TerrainProgram& program) {
    if (program.heightRegisters > kMaxHeightRegisters || program.positionRegisters > kMaxPositionRegisters ||
        program.positionRegisters == 0) {
        return Error{ErrorCode::InvalidArgument, "terrain program uses too many registers"};
    }
    if (program.outputRegister >= std::max<u32>(program.heightRegisters, 1)) {
        return Error{ErrorCode::InvalidArgument, "terrain program output register out of range"};
    }
    // Registers written so far. Reading one before it is written would make the CPU result depend on
    // the previous tile (the scratch registers persist) and differ from the Slang twin (which starts
    // every sample with zero heights and p1..p3 = p0), so such a program is corrupt.
    std::array<bool, kMaxHeightRegisters> heightWritten{};
    std::array<bool, kMaxPositionRegisters> posWritten{};
    posWritten[0] = true;
    for (usize n = 0; n < program.code.size(); ++n) {
        const Instr& in = program.code[n];
        const Opcode op = in.op();
        if (op >= Opcode::Count) return makeError(ErrorCode::Corrupt, "instruction {}: invalid opcode", n);
        const bool posDst = op == Opcode::Warp;
        const bool posA = op == Opcode::Noise || op == Opcode::Fbm || op == Opcode::Ridged || op == Opcode::Warp;
        const u32 dstLimit = posDst ? program.positionRegisters : program.heightRegisters;
        const u32 aLimit = posA ? program.positionRegisters : program.heightRegisters;
        if (in.dst() >= dstLimit || in.a() >= aLimit || in.b() >= std::max<u32>(program.heightRegisters, 1) ||
            in.c() >= std::max<u32>(program.heightRegisters, 1) || (posDst && in.dst() == 0)) {
            return makeError(ErrorCode::Corrupt, "instruction {}: register out of range", n);
        }
        if (posA) {
            const i32 e = in.wavelengthExp();
            const i32 last = e - (op == Opcode::Warp ? 0 : std::max<i32>(in.octaves(), 1) - 1);
            if (in.octaves() > kMaxOctaves || e > hnoise::kMaxWavelengthExp || last < hnoise::kMinWavelengthExp) {
                return makeError(ErrorCode::Corrupt, "instruction {}: octave range out of bounds", n);
            }
        }
        u32 heightReads = 0; // how many of a, b, c the op reads as height registers
        switch (op) {
            case Opcode::Const: break;
            case Opcode::Noise:
            case Opcode::Fbm:
            case Opcode::Ridged:
            case Opcode::Warp:
                if (!posWritten[in.a()]) {
                    return makeError(ErrorCode::Corrupt, "instruction {}: reads p{} before it is written", n, in.a());
                }
                break;
            case Opcode::Clamp:
            case Opcode::Remap: heightReads = 1; break;
            case Opcode::Select: heightReads = 3; break;
            default: heightReads = 2; break;
        }
        const u8 reads[3] = {in.a(), in.b(), in.c()};
        for (u32 r = 0; r < heightReads; ++r) {
            if (!heightWritten[reads[r]]) {
                return makeError(ErrorCode::Corrupt, "instruction {}: reads h{} before it is written", n, reads[r]);
            }
        }
        (posDst ? posWritten[in.dst()] : heightWritten[in.dst()]) = true;
    }
    if (!heightWritten[program.outputRegister]) {
        return makeError(ErrorCode::Corrupt, "terrain program never writes its output register h{}", program.outputRegister);
    }
    return {};
}

Result<void> TileEvaluator::evaluate(const TerrainProgram& program, const TileDomain& domain, std::span<i64> heights) {
    if (heights.size() < kTileSamples) return Error{ErrorCode::InvalidArgument, "height span smaller than a tile"};
    HELIOS_TRY(validateTileDomain(domain));
    HELIOS_TRY(validateTerrainProgram(program));
    const vm::Kernels* k = kernelsFor(m_kernel);
    HELIOS_ASSERT(k != nullptr);
    vm::Domain packed{};
    const auto words = domain.pack();
    std::memcpy(packed.w, words.data(), sizeof(packed.w));
    k->domain(packed, m_scratch->regs);
    for (const Instr& in : program.code) k->ops[static_cast<u32>(in.op())](in.w.data(), m_scratch->regs);
    std::memcpy(heights.data(), m_scratch->regs.h[program.outputRegister], kTileSamples * sizeof(i64));
    return {};
}

FixedPos TileEvaluator::position(u32 index) const noexcept {
    const vm::Registers& r = m_scratch->regs;
    const u32 s = std::min(index, kTileSamples - 1);
    return {packI64(r.posLo[0][0][s], static_cast<u32>(r.posHi[0][0][s])),
            packI64(r.posLo[0][1][s], static_cast<u32>(r.posHi[0][1][s])),
            packI64(r.posLo[0][2][s], static_cast<u32>(r.posHi[0][2][s]))};
}

// ---------------------------------------------------------------------------------------------
// Per-sample reference (hnoise.h + helios::Q16/Q32; shares no code with the kernels)
// ---------------------------------------------------------------------------------------------

i64 evaluateSampleReference(const TerrainProgram& program, const TileDomain& domain, u32 index) {
    if (!validateTerrainProgram(program).ok() || !validateTileDomain(domain).ok()) return 0;
    std::array<Q32, kMaxHeightRegisters> h{};
    std::array<FixedPos, kMaxPositionRegisters> p{};
    p[0] = samplePosition(domain, index);
    for (const Instr& in : program.code) {
        const auto k = [&](u32 i) { return Q32::fromRaw(in.k(i)); };
        switch (in.op()) {
            case Opcode::Const: h[in.dst()] = k(0); break;
            case Opcode::Noise:
            case Opcode::Fbm:
            case Opcode::Ridged: {
                Q16 amp = Q16::fromRaw(in.amplitude());
                const Q16 gain = Q16::fromRaw(in.gain());
                i64 sum = 0;
                for (u32 o = 0; o < in.octaves(); ++o) {
                    const LatticeCoord c = hnoise::latticeCoord(p[in.a()], in.wavelengthExp() - static_cast<i32>(o));
                    i32 n = hnoise::noise3(hnoise::octaveSeed(in.seed(), o), c);
                    if (in.op() == Opcode::Ridged) {
                        const i32 t = std::max(65536 - (n < 0 ? -n : n), 0);
                        n = (Q16::fromRaw(t) * Q16::fromRaw(t)).raw();
                    }
                    sum = static_cast<i64>(static_cast<u64>(sum) + static_cast<u64>(static_cast<i64>(amp.raw()) * n));
                    amp = amp * gain;
                }
                h[in.dst()] = Q32::fromRaw(sum);
                break;
            }
            case Opcode::Warp: {
                const FixedPos src = p[in.a()];
                const LatticeCoord c = hnoise::latticeCoord(src, in.wavelengthExp());
                i64 out[3] = {src.x, src.y, src.z};
                for (u32 a = 0; a < 3; ++a) {
                    const i32 n = hnoise::noise3(in.seed() + a * hnoise::kWarpAxisSeedStep, c);
                    out[a] = static_cast<i64>(static_cast<u64>(out[a]) + static_cast<u64>(static_cast<i64>(in.amplitude()) * n));
                }
                p[in.dst()] = {out[0], out[1], out[2]};
                break;
            }
            case Opcode::Add: h[in.dst()] = h[in.a()] + h[in.b()]; break;
            case Opcode::Sub: h[in.dst()] = h[in.a()] - h[in.b()]; break;
            case Opcode::Mul: h[in.dst()] = h[in.a()] * h[in.b()]; break;
            case Opcode::Min: h[in.dst()] = std::min(h[in.a()], h[in.b()]); break;
            case Opcode::Max: h[in.dst()] = std::max(h[in.a()], h[in.b()]); break;
            case Opcode::Clamp: h[in.dst()] = std::min(std::max(h[in.a()], k(0)), k(1)); break;
            case Opcode::Remap: {
                Q32 v = k(2) + (h[in.a()] - k(0)) * k(1);
                if (in.flags() & kRemapClampFlag) v = std::min(std::max(v, std::min(k(2), k(3))), std::max(k(2), k(3)));
                h[in.dst()] = v;
                break;
            }
            case Opcode::Select: h[in.dst()] = h[in.a()] >= k(0) ? h[in.b()] : h[in.c()]; break;
            default: break;
        }
    }
    return h[program.outputRegister].raw();
}

} // namespace helios::pcg

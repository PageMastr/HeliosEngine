// SPIR-V reflection and the .hsr format (see shader_reflection.h).

#include "helios/render/shader_reflection.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <unordered_map>

namespace helios::render {

std::string_view shaderStageName(ShaderStage stage) noexcept {
    switch (stage) {
    case ShaderStage::Vertex: return "vertex";
    case ShaderStage::TessControl: return "hull";
    case ShaderStage::TessEval: return "domain";
    case ShaderStage::Geometry: return "geometry";
    case ShaderStage::Fragment: return "fragment";
    case ShaderStage::Compute: return "compute";
    case ShaderStage::Task: return "amplification";
    case ShaderStage::Mesh: return "mesh";
    case ShaderStage::RayGen: return "raygeneration";
    case ShaderStage::Intersection: return "intersection";
    case ShaderStage::AnyHit: return "anyhit";
    case ShaderStage::ClosestHit: return "closesthit";
    case ShaderStage::Miss: return "miss";
    case ShaderStage::Callable: return "callable";
    case ShaderStage::Unknown: break;
    }
    return "unknown";
}

std::string_view shaderValueKindName(ShaderValueKind kind) noexcept {
    switch (kind) {
    case ShaderValueKind::Bool: return "bool";
    case ShaderValueKind::Int: return "int";
    case ShaderValueKind::UInt: return "uint";
    case ShaderValueKind::Half: return "half";
    case ShaderValueKind::Float: return "float";
    case ShaderValueKind::Double: return "double";
    case ShaderValueKind::Int64: return "int64";
    case ShaderValueKind::UInt64: return "uint64";
    case ShaderValueKind::Vector: return "vector";
    case ShaderValueKind::Matrix: return "matrix";
    case ShaderValueKind::Struct: return "struct";
    case ShaderValueKind::Pointer: return "pointer";
    case ShaderValueKind::Unknown: break;
    }
    return "unknown";
}

std::string_view shaderBindingKindName(ShaderBindingKind kind) noexcept {
    switch (kind) {
    case ShaderBindingKind::SampledImage: return "sampledImage";
    case ShaderBindingKind::StorageImage: return "storageImage";
    case ShaderBindingKind::Sampler: return "sampler";
    case ShaderBindingKind::CombinedImageSampler: return "combinedImageSampler";
    case ShaderBindingKind::UniformBuffer: return "uniformBuffer";
    case ShaderBindingKind::StorageBuffer: return "storageBuffer";
    case ShaderBindingKind::UniformTexelBuffer: return "uniformTexelBuffer";
    case ShaderBindingKind::StorageTexelBuffer: return "storageTexelBuffer";
    case ShaderBindingKind::AccelerationStructure: return "accelerationStructure";
    case ShaderBindingKind::Unknown: break;
    }
    return "unknown";
}

std::string_view shaderAccessName(ShaderAccess access) noexcept {
    switch (access) {
    case ShaderAccess::None: return "none";
    case ShaderAccess::Read: return "read";
    case ShaderAccess::Write: return "write";
    case ShaderAccess::ReadWrite: return "readWrite";
    }
    return "none";
}

const ShaderEntryPoint* ShaderReflection::findEntryPoint(std::string_view name) const noexcept {
    for (const ShaderEntryPoint& e : entryPoints) {
        if (e.name == name) return &e;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------------------------
// SPIR-V parsing
// ---------------------------------------------------------------------------------------------
namespace {

namespace spv {
constexpr u32 kMagic = 0x07230203u;
enum Op : u32 {
    OpName = 5, OpMemberName = 6, OpEntryPoint = 15, OpExecutionMode = 16, OpTypeVoid = 19, OpTypeBool = 20,
    OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23, OpTypeMatrix = 24, OpTypeImage = 25, OpTypeSampler = 26,
    OpTypeSampledImage = 27, OpTypeArray = 28, OpTypeRuntimeArray = 29, OpTypeStruct = 30, OpTypePointer = 32,
    OpTypeForwardPointer = 39, OpConstantTrue = 41, OpConstantFalse = 42, OpConstant = 43, OpSpecConstantTrue = 48,
    OpSpecConstantFalse = 49, OpSpecConstant = 50, OpFunction = 54, OpVariable = 59, OpDecorate = 71,
    OpMemberDecorate = 72, OpExecutionModeId = 331, OpTypeAccelerationStructureKHR = 5341,
};
enum Decoration : u32 {
    SpecId = 1, Block = 2, RowMajor = 4, ColMajor = 5, BufferBlock = 3, ArrayStride = 6, MatrixStride = 7,
    NonWritable = 24, NonReadable = 25, Binding = 33, DescriptorSet = 34, Offset = 35,
};
enum StorageClass : u32 {
    UniformConstant = 0, Uniform = 2, PushConstant = 9, StorageBuffer = 12, PhysicalStorageBuffer = 5349,
};
constexpr u32 kExecutionModeLocalSize = 17;
constexpr u32 kExecutionModeLocalSizeId = 38;
constexpr u32 kDimBuffer = 5;
} // namespace spv

ShaderStage stageFromModel(u32 model) {
    switch (model) {
    case 0: return ShaderStage::Vertex;
    case 1: return ShaderStage::TessControl;
    case 2: return ShaderStage::TessEval;
    case 3: return ShaderStage::Geometry;
    case 4: return ShaderStage::Fragment;
    case 5: return ShaderStage::Compute;
    case 5267: case 5364: return ShaderStage::Task;
    case 5268: case 5365: return ShaderStage::Mesh;
    case 5313: return ShaderStage::RayGen;
    case 5314: return ShaderStage::Intersection;
    case 5315: return ShaderStage::AnyHit;
    case 5316: return ShaderStage::ClosestHit;
    case 5317: return ShaderStage::Miss;
    case 5318: return ShaderStage::Callable;
    default: return ShaderStage::Unknown;
    }
}

struct TypeInfo {
    u32 op = 0;
    std::vector<u32> operands;  // words after the result id
};

struct MemberDecor {
    u32 offset = 0;
    bool hasOffset = false;
    u32 matrixStride = 0;
    bool rowMajor = false;
    bool nonWritable = false;
    bool nonReadable = false;
};

struct Decor {
    bool hasSpecId = false;
    u32 specId = 0;
    bool block = false;
    bool bufferBlock = false;
    u32 arrayStride = 0;
    bool nonWritable = false;
    bool nonReadable = false;
    bool hasBinding = false;
    u32 binding = 0;
    bool hasSet = false;
    u32 set = 0;
};

/// Key of a (struct id, member index) pair in the sparse member tables. Member decorations and
/// names precede the type declarations in a module, so the member count is not known when they
/// are read; sparse tables keep memory linear in the input whatever indices it claims.
constexpr u64 memberKey(u32 id, u32 member) noexcept { return (u64{id} << 32) | member; }

/// Upper bound on the type-graph nodes visited while sizing push-constant members: legitimate
/// blocks visit a few dozen; cyclic or exponentially shared type graphs (malformed input) hit it.
constexpr u64 kMaxTypeSteps = u64{1} << 20;

class SpirvReader {
public:
    explicit SpirvReader(std::span<const u32> words) : m_words(words) {}

    Result<ShaderReflection> run() {
        if (m_words.size() < 5 || m_words[0] != spv::kMagic) {
            return Error{ErrorCode::Corrupt, "not a SPIR-V module (bad magic or too short)"};
        }
        ShaderReflection out;
        out.spirvVersion = m_words[1];
        m_bound = m_words[3];
        out.contentHash = hash128(m_words.data(), m_words.size_bytes());
        usize pos = 5;
        while (pos < m_words.size()) {
            const u32 word = m_words[pos];
            const u32 count = word >> 16;
            const u32 op = word & 0xFFFFu;
            if (count == 0 || pos + count > m_words.size()) {
                return Error{ErrorCode::Corrupt, std::format("malformed SPIR-V instruction at word {}", pos)};
            }
            const std::span<const u32> ops = m_words.subspan(pos + 1, count - 1);
            HELIOS_TRY(instruction(op, ops));
            pos += count;
        }
        return build(std::move(out));
    }

private:
    Result<std::string> literalString(std::span<const u32> ops, usize& index) const {
        std::string s;
        while (index < ops.size()) {
            const u32 w = ops[index++];
            for (u32 b = 0; b < 4; ++b) {
                const char c = static_cast<char>((w >> (8 * b)) & 0xFFu);
                if (c == '\0') return s;
                s.push_back(c);
            }
        }
        return Error{ErrorCode::Corrupt, "unterminated SPIR-V literal string"};
    }

    Result<void> checkId(u32 id) const {
        if (id == 0 || id >= m_bound) return Error{ErrorCode::Corrupt, std::format("SPIR-V id {} out of bound {}", id, m_bound)};
        return {};
    }

    Decor& decor(u32 id) { return m_decor[id]; }

    Result<void> instruction(u32 op, std::span<const u32> ops) {
        auto need = [&](usize n) -> Result<void> {
            if (ops.size() < n) return Error{ErrorCode::Corrupt, std::format("SPIR-V op {} is too short", op)};
            return {};
        };
        switch (op) {
        case spv::OpName: {
            HELIOS_TRY(need(1));
            usize i = 1;
            HELIOS_TRY_ASSIGN(std::string name, literalString(ops, i));
            m_names[ops[0]] = std::move(name);
            break;
        }
        case spv::OpMemberName: {
            HELIOS_TRY(need(2));
            HELIOS_TRY(checkId(ops[0]));
            usize i = 2;
            HELIOS_TRY_ASSIGN(std::string name, literalString(ops, i));
            m_memberNames[memberKey(ops[0], ops[1])] = std::move(name);
            break;
        }
        case spv::OpEntryPoint: {
            HELIOS_TRY(need(3));
            EntryRecord e;
            e.model = ops[0];
            e.function = ops[1];
            usize i = 2;
            HELIOS_TRY_ASSIGN(e.name, literalString(ops, i));
            for (; i < ops.size(); ++i) e.interface.push_back(ops[i]);
            m_entries.push_back(std::move(e));
            break;
        }
        case spv::OpExecutionMode:
        case spv::OpExecutionModeId: {
            HELIOS_TRY(need(2));
            const u32 mode = ops[1];
            if ((op == spv::OpExecutionMode && mode == spv::kExecutionModeLocalSize) ||
                (op == spv::OpExecutionModeId && mode == spv::kExecutionModeLocalSizeId)) {
                HELIOS_TRY(need(5));
                m_localSize[ops[0]] = {{ops[2], ops[3], ops[4]}, op == spv::OpExecutionModeId};
            }
            break;
        }
        case spv::OpDecorate: {
            HELIOS_TRY(need(2));
            HELIOS_TRY(checkId(ops[0]));
            Decor& d = decor(ops[0]);
            const u32 dec = ops[1];
            const u32 lit = ops.size() > 2 ? ops[2] : 0;
            switch (dec) {
            case spv::SpecId: d.hasSpecId = true; d.specId = lit; break;
            case spv::Block: d.block = true; break;
            case spv::BufferBlock: d.bufferBlock = true; break;
            case spv::ArrayStride: d.arrayStride = lit; break;
            case spv::NonWritable: d.nonWritable = true; break;
            case spv::NonReadable: d.nonReadable = true; break;
            case spv::Binding: d.hasBinding = true; d.binding = lit; break;
            case spv::DescriptorSet: d.hasSet = true; d.set = lit; break;
            default: break;
            }
            break;
        }
        case spv::OpMemberDecorate: {
            HELIOS_TRY(need(3));
            HELIOS_TRY(checkId(ops[0]));
            MemberDecor& m = m_memberDecor[memberKey(ops[0], ops[1])];
            const u32 lit = ops.size() > 3 ? ops[3] : 0;
            switch (ops[2]) {
            case spv::Offset: m.offset = lit; m.hasOffset = true; break;
            case spv::MatrixStride: m.matrixStride = lit; break;
            case spv::RowMajor: m.rowMajor = true; break;
            case spv::ColMajor: m.rowMajor = false; break;
            case spv::NonWritable: m.nonWritable = true; break;
            case spv::NonReadable: m.nonReadable = true; break;
            default: break;
            }
            break;
        }
        case spv::OpTypeVoid: case spv::OpTypeBool: case spv::OpTypeInt: case spv::OpTypeFloat:
        case spv::OpTypeVector: case spv::OpTypeMatrix: case spv::OpTypeImage: case spv::OpTypeSampler:
        case spv::OpTypeSampledImage: case spv::OpTypeArray: case spv::OpTypeRuntimeArray: case spv::OpTypeStruct:
        case spv::OpTypePointer: case spv::OpTypeAccelerationStructureKHR: {
            HELIOS_TRY(need(1));
            HELIOS_TRY(checkId(ops[0]));
            m_types[ops[0]] = TypeInfo{op, std::vector<u32>(ops.begin() + 1, ops.end())};
            break;
        }
        case spv::OpConstant: {
            HELIOS_TRY(need(3));
            HELIOS_TRY(checkId(ops[1]));
            m_constants[ops[1]] = ops[2];
            break;
        }
        case spv::OpConstantTrue: case spv::OpConstantFalse: {
            HELIOS_TRY(need(2));
            m_constants[ops[1]] = op == spv::OpConstantTrue ? 1u : 0u;
            break;
        }
        case spv::OpSpecConstant: case spv::OpSpecConstantTrue: case spv::OpSpecConstantFalse: {
            HELIOS_TRY(need(2));
            HELIOS_TRY(checkId(ops[1]));
            SpecRecord s;
            s.type = ops[0];
            s.id = ops[1];
            s.bits = op == spv::OpSpecConstant ? (ops.size() > 2 ? ops[2] : 0u) : (op == spv::OpSpecConstantTrue ? 1u : 0u);
            m_specs.push_back(s);
            m_constants[ops[1]] = s.bits;
            break;
        }
        case spv::OpVariable: {
            HELIOS_TRY(need(3));
            HELIOS_TRY(checkId(ops[1]));
            m_variables.push_back({ops[0], ops[1], ops[2]});
            break;
        }
        default: break;
        }
        return {};
    }

    const TypeInfo* type(u32 id) const {
        auto it = m_types.find(id);
        return it == m_types.end() ? nullptr : &it->second;
    }

    ShaderValueKind scalarKind(const TypeInfo& t) const {
        if (t.op == spv::OpTypeBool) return ShaderValueKind::Bool;
        if (t.op == spv::OpTypeInt && t.operands.size() >= 2) {
            const bool sign = t.operands[1] != 0;
            if (t.operands[0] == 64) return sign ? ShaderValueKind::Int64 : ShaderValueKind::UInt64;
            return sign ? ShaderValueKind::Int : ShaderValueKind::UInt;
        }
        if (t.op == spv::OpTypeFloat && !t.operands.empty()) {
            if (t.operands[0] == 16) return ShaderValueKind::Half;
            if (t.operands[0] == 64) return ShaderValueKind::Double;
            return ShaderValueKind::Float;
        }
        return ShaderValueKind::Unknown;
    }

    /// Size in bytes of a type laid out with its decorations (scalar layout); `depth` bounds recursion.
    u32 sizeOf(u32 id, const MemberDecor* md, u32 depth = 0) const {
        if (++m_typeSteps > kMaxTypeSteps) {
            m_typeGraphTooComplex = true;
            return 0;
        }
        const TypeInfo* t = type(id);
        if (!t || depth > 32) return 0;
        switch (t->op) {
        case spv::OpTypeBool: return 4;
        case spv::OpTypeInt:
        case spv::OpTypeFloat: return t->operands.empty() ? 0 : t->operands[0] / 8;
        case spv::OpTypeVector: return t->operands.size() < 2 ? 0 : sizeOf(t->operands[0], nullptr, depth + 1) * t->operands[1];
        case spv::OpTypeMatrix: {
            if (t->operands.size() < 2) return 0;
            const u32 columns = t->operands[1];
            const TypeInfo* column = type(t->operands[0]);
            const u32 rows = column && column->operands.size() >= 2 ? column->operands[1] : 0;
            const u32 scalar = column ? sizeOf(column->operands[0], nullptr, depth + 1) : 0;
            const u32 stride = md && md->matrixStride ? md->matrixStride : rows * scalar;
            // RowMajor: each of the `rows` rows is stride-separated; ColMajor: each column.
            const u32 vectors = md && md->rowMajor ? rows : columns;
            const u32 vectorSize = md && md->rowMajor ? columns * scalar : rows * scalar;
            return vectors == 0 ? 0 : stride * (vectors - 1) + vectorSize;
        }
        case spv::OpTypeArray: {
            if (t->operands.size() < 2) return 0;
            auto len = m_constants.find(t->operands[1]);
            const u32 n = len == m_constants.end() ? 0 : len->second;
            auto d = m_decor.find(id);
            const u32 elem = sizeOf(t->operands[0], md, depth + 1);
            const u32 stride = d != m_decor.end() && d->second.arrayStride ? d->second.arrayStride : elem;
            return n == 0 ? 0 : stride * (n - 1) + elem;
        }
        case spv::OpTypeStruct: {
            u32 size = 0;
            u32 offset = 0;
            for (u32 m = 0; m < t->operands.size() && !m_typeGraphTooComplex; ++m) {
                const MemberDecor* mdec = memberDecor(id, m);
                if (mdec && mdec->hasOffset) offset = mdec->offset;
                const u32 msize = sizeOf(t->operands[m], mdec, depth + 1);
                size = std::max(size, offset + msize);
                offset += msize;
            }
            return size;
        }
        case spv::OpTypePointer: return 8;  // PhysicalStorageBuffer pointers (BDA)
        default: return 0;
        }
    }

    ShaderMember describeMember(u32 typeId, const MemberDecor* md) const {
        ShaderMember m;
        m.size = sizeOf(typeId, md);
        u32 id = typeId;
        const TypeInfo* t = type(id);
        for (u32 guard = 0; t && guard < 8; ++guard) {
            ++m_typeSteps;
            // Slang wraps arrays inside structs in a one-member "_Array_..." struct; report the array.
            if (t->op == spv::OpTypeStruct && t->operands.size() == 1 && nameOf(id).starts_with("_Array")) {
                id = t->operands[0];
                t = type(id);
                continue;
            }
            if (t->op != spv::OpTypeArray || t->operands.size() < 2) break;
            auto len = m_constants.find(t->operands[1]);
            const u32 n = len == m_constants.end() ? 0 : len->second;
            m.arrayCount = m.arrayCount ? m.arrayCount * n : n;
            id = t->operands[0];
            t = type(id);
        }
        if (!t) return m;
        switch (t->op) {
        case spv::OpTypeVector:
            m.kind = ShaderValueKind::Vector;
            m.components = t->operands.size() >= 2 ? t->operands[1] : 1;
            break;
        case spv::OpTypeMatrix: {
            m.kind = ShaderValueKind::Matrix;
            const TypeInfo* column = type(t->operands[0]);
            const u32 rows = column && column->operands.size() >= 2 ? column->operands[1] : 0;
            m.components = rows * (t->operands.size() >= 2 ? t->operands[1] : 0);
            break;
        }
        case spv::OpTypeStruct: m.kind = ShaderValueKind::Struct; break;
        case spv::OpTypePointer: m.kind = ShaderValueKind::Pointer; break;
        default: m.kind = scalarKind(*t); break;
        }
        return m;
    }

    Result<ShaderReflection> build(ShaderReflection out) {
        std::unordered_map<u32, u32> entryIndexOfFunction;
        for (const EntryRecord& e : m_entries) {
            ShaderEntryPoint ep;
            ep.name = e.name;
            ep.stage = stageFromModel(e.model);
            auto ls = m_localSize.find(e.function);
            if (ls != m_localSize.end()) {
                for (u32 k = 0; k < 3; ++k) {
                    u32 v = ls->second.values[k];
                    if (ls->second.isId) {
                        auto c = m_constants.find(v);
                        v = c == m_constants.end() ? 0 : c->second;
                    }
                    ep.workgroupSize[k] = v;
                }
            }
            out.entryPoints.push_back(std::move(ep));
        }
        // Which entry points list each id in their interface (one pass over every interface word,
        // so the cost stays linear in the module size).
        struct EntryUse {
            u32 mask = 0;
            bool any = false;
        };
        std::unordered_map<u32, EntryUse> entryUse;
        for (u32 i = 0; i < m_entries.size(); ++i) {
            for (u32 id : m_entries[i].interface) {
                EntryUse& use = entryUse[id];
                if (i < 32) use.mask |= 1u << i;
                use.any = true;
            }
        }
        // Variables: push constants and descriptor bindings.
        for (const VariableRecord& v : m_variables) {
            const TypeInfo* ptr = type(v.pointerType);
            if (!ptr || ptr->op != spv::OpTypePointer || ptr->operands.size() < 2) continue;
            const u32 pointee = ptr->operands[1];
            const auto use = entryUse.find(v.id);
            const u32 entryMask = use == entryUse.end() ? 0u : use->second.mask;
            const bool anyEntry = use != entryUse.end() && use->second.any;
            if (v.storageClass == spv::PushConstant) {
                const TypeInfo* block = type(pointee);
                if (!block || block->op != spv::OpTypeStruct) continue;
                if (block->operands.size() > 0xFFFFu) {
                    return Error{ErrorCode::Corrupt, "SPIR-V push-constant block has more than 65535 members"};
                }
                out.pushConstants.blockName = nameOf(pointee);
                out.pushConstants.size = sizeOf(pointee, nullptr);
                out.pushConstants.members.clear();
                for (u32 m = 0; m < block->operands.size() && !m_typeGraphTooComplex; ++m) {
                    const MemberDecor* md = memberDecor(pointee, m);
                    ShaderMember member = describeMember(block->operands[m], md);
                    if (member.components > 255) {
                        // Valid SPIR-V has at most 16 vector components and 4x4 matrices; the .hsr
                        // record stores the count in a byte.
                        return Error{ErrorCode::Corrupt, "SPIR-V push-constant member with more than 255 components"};
                    }
                    member.offset = md && md->hasOffset ? md->offset : 0;
                    if (auto name = m_memberNames.find(memberKey(pointee, m)); name != m_memberNames.end()) {
                        member.name = name->second;
                    }
                    out.pushConstants.members.push_back(std::move(member));
                }
                if (m_typeGraphTooComplex) {
                    return Error{ErrorCode::Corrupt, "SPIR-V push-constant types are cyclic or too complex"};
                }
                for (u32 i = 0; i < m_entries.size() && i < 32; ++i) {
                    if (entryMask & (1u << i)) out.entryPoints[i].usesPushConstants = true;
                }
                continue;
            }
            if (v.storageClass != spv::UniformConstant && v.storageClass != spv::Uniform &&
                v.storageClass != spv::StorageBuffer) {
                continue;
            }
            auto vd = m_decor.find(v.id);
            if (vd == m_decor.end() || !vd->second.hasBinding) continue;
            ShaderBinding b;
            b.set = vd->second.hasSet ? vd->second.set : 0;
            b.binding = vd->second.binding;
            b.name = nameOf(v.id);
            b.entryPointMask = anyEntry ? entryMask : 0;
            // Unwrap arrays.
            u32 inner = pointee;
            const TypeInfo* t = type(inner);
            if (t && t->op == spv::OpTypeRuntimeArray) {
                b.count = 0;
                inner = t->operands.empty() ? 0 : t->operands[0];
            } else if (t && t->op == spv::OpTypeArray && t->operands.size() >= 2) {
                auto len = m_constants.find(t->operands[1]);
                b.count = len == m_constants.end() ? 1 : len->second;
                inner = t->operands[0];
            }
            t = type(inner);
            bool nonWritable = vd->second.nonWritable;
            bool nonReadable = vd->second.nonReadable;
            if (t) {
                auto innerDecor = m_decor.find(inner);
                if (t->op == spv::OpTypeImage && t->operands.size() >= 6) {
                    const bool storage = t->operands[5] == 2;
                    const bool buffer = t->operands[1] == spv::kDimBuffer;
                    b.kind = storage ? (buffer ? ShaderBindingKind::StorageTexelBuffer : ShaderBindingKind::StorageImage)
                                     : (buffer ? ShaderBindingKind::UniformTexelBuffer : ShaderBindingKind::SampledImage);
                    if (!storage) nonWritable = true;
                } else if (t->op == spv::OpTypeSampler) {
                    b.kind = ShaderBindingKind::Sampler;
                    nonWritable = true;
                } else if (t->op == spv::OpTypeSampledImage) {
                    b.kind = ShaderBindingKind::CombinedImageSampler;
                    nonWritable = true;
                } else if (t->op == spv::OpTypeAccelerationStructureKHR) {
                    b.kind = ShaderBindingKind::AccelerationStructure;
                    nonWritable = true;
                } else if (t->op == spv::OpTypeStruct) {
                    const bool bufferBlock = innerDecor != m_decor.end() && innerDecor->second.bufferBlock;
                    if (v.storageClass == spv::StorageBuffer || bufferBlock) {
                        b.kind = ShaderBindingKind::StorageBuffer;
                        // Read-only / write-only when every member says so.
                        if (!t->operands.empty()) {
                            bool allNw = true;
                            bool allNr = true;
                            for (u32 m = 0; m < t->operands.size() && (allNw || allNr); ++m) {
                                const MemberDecor* md = memberDecor(inner, m);
                                allNw = allNw && md && md->nonWritable;
                                allNr = allNr && md && md->nonReadable;
                            }
                            nonWritable = nonWritable || allNw;
                            nonReadable = nonReadable || allNr;
                        }
                    } else {
                        b.kind = ShaderBindingKind::UniformBuffer;
                        nonWritable = true;
                    }
                }
            }
            b.access = static_cast<ShaderAccess>((nonReadable ? 0 : 1) | (nonWritable ? 0 : 2));
            out.bindings.push_back(std::move(b));
        }
        std::sort(out.bindings.begin(), out.bindings.end(), [](const ShaderBinding& a, const ShaderBinding& b) {
            if (a.set != b.set) return a.set < b.set;
            if (a.binding != b.binding) return a.binding < b.binding;
            return a.name < b.name;
        });
        for (const SpecRecord& s : m_specs) {
            auto d = m_decor.find(s.id);
            if (d == m_decor.end() || !d->second.hasSpecId) continue;
            ShaderSpecConstant c;
            c.id = d->second.specId;
            c.name = nameOf(s.id);
            const TypeInfo* t = type(s.type);
            c.kind = t ? scalarKind(*t) : ShaderValueKind::Unknown;
            c.defaultBits = s.bits;
            out.specConstants.push_back(std::move(c));
        }
        std::sort(out.specConstants.begin(), out.specConstants.end(),
                  [](const ShaderSpecConstant& a, const ShaderSpecConstant& b) { return a.id < b.id; });
        if (out.specConstants.size() > 0xFFFFu) {
            return Error{ErrorCode::Corrupt, "SPIR-V module has more than 65535 specialization constants"};
        }
        return out;
    }

    const MemberDecor* memberDecor(u32 id, u32 member) const {
        auto it = m_memberDecor.find(memberKey(id, member));
        return it == m_memberDecor.end() ? nullptr : &it->second;
    }

    std::string nameOf(u32 id) const {
        auto it = m_names.find(id);
        return it == m_names.end() ? std::string() : it->second;
    }

    struct EntryRecord {
        u32 model = 0;
        u32 function = 0;
        std::string name;
        std::vector<u32> interface;
    };
    struct LocalSize {
        std::array<u32, 3> values{};
        bool isId = false;
    };
    struct SpecRecord {
        u32 type = 0;
        u32 id = 0;
        u32 bits = 0;
    };
    struct VariableRecord {
        u32 pointerType = 0;
        u32 id = 0;
        u32 storageClass = 0;
    };

    std::span<const u32> m_words;
    u32 m_bound = 0;
    std::unordered_map<u32, std::string> m_names;
    std::unordered_map<u64, std::string> m_memberNames;    // memberKey(struct, member)
    std::unordered_map<u64, MemberDecor> m_memberDecor;    // memberKey(struct, member)
    std::unordered_map<u32, Decor> m_decor;
    std::unordered_map<u32, TypeInfo> m_types;
    std::unordered_map<u32, u32> m_constants;
    std::unordered_map<u32, LocalSize> m_localSize;
    std::vector<EntryRecord> m_entries;
    std::vector<SpecRecord> m_specs;
    std::vector<VariableRecord> m_variables;
    mutable u64 m_typeSteps = 0;
    mutable bool m_typeGraphTooComplex = false;
};

// ---------------------------------------------------------------------------------------------
// Binary .hsr
// ---------------------------------------------------------------------------------------------
constexpr u32 kHeaderSize = 48;
/// Decoded string bytes a blob may always expand to (plus 16x its own size; see parseReflection).
constexpr u64 kHsrMaxDecodedStringBytes = u64{1} << 20;
constexpr u32 kEntrySize = 20;
constexpr u32 kMemberSize = 20;
constexpr u32 kSpecSize = 16;
constexpr u32 kBindingSize = 24;

class Writer {
public:
    void u8v(u8 v) { m_bytes.push_back(v); }
    void u16v(u16 v) {
        u8v(static_cast<u8>(v));
        u8v(static_cast<u8>(v >> 8));
    }
    void u32v(u32 v) {
        for (u32 i = 0; i < 4; ++i) u8v(static_cast<u8>(v >> (8 * i)));
    }
    void u64v(u64 v) {
        for (u32 i = 0; i < 8; ++i) u8v(static_cast<u8>(v >> (8 * i)));
    }
    std::vector<u8>& bytes() { return m_bytes; }

private:
    std::vector<u8> m_bytes;
};

class StringTable {
public:
    u32 add(std::string_view s) {
        for (const auto& [text, offset] : m_index) {
            if (text == s) return offset;
        }
        const u32 offset = static_cast<u32>(m_bytes.size());
        m_bytes.insert(m_bytes.end(), s.begin(), s.end());
        m_bytes.push_back(0);
        m_index.emplace_back(std::string(s), offset);
        return offset;
    }
    const std::vector<u8>& bytes() const { return m_bytes; }

private:
    std::vector<u8> m_bytes;
    std::vector<std::pair<std::string, u32>> m_index;
};

class Reader {
public:
    explicit Reader(std::span<const u8> bytes) : m_bytes(bytes) {}
    bool has(usize n) const { return m_pos + n <= m_bytes.size(); }
    u8 u8v() { return m_bytes[m_pos++]; }
    u16 u16v() {
        const u16 v = static_cast<u16>(m_bytes[m_pos] | (m_bytes[m_pos + 1] << 8));
        m_pos += 2;
        return v;
    }
    u32 u32v() {
        u32 v = 0;
        for (u32 i = 0; i < 4; ++i) v |= static_cast<u32>(m_bytes[m_pos + i]) << (8 * i);
        m_pos += 4;
        return v;
    }
    u64 u64v() {
        u64 v = 0;
        for (u32 i = 0; i < 8; ++i) v |= static_cast<u64>(m_bytes[m_pos + i]) << (8 * i);
        m_pos += 8;
        return v;
    }
    usize pos() const { return m_pos; }

private:
    std::span<const u8> m_bytes;
    usize m_pos = 0;
};

} // namespace

Result<ShaderReflection> reflectSpirv(std::span<const u32> words) { return SpirvReader(words).run(); }

Result<ShaderReflection> reflectSpirvBytes(std::span<const u8> bytes) {
    if (bytes.size() % 4 != 0) return Error{ErrorCode::Corrupt, "SPIR-V size is not a multiple of 4"};
    std::vector<u32> words(bytes.size() / 4);
    if (!bytes.empty()) std::memcpy(words.data(), bytes.data(), bytes.size());  // memcpy(null, null, 0) is UB
    return reflectSpirv(words);
}

std::vector<u8> serializeReflection(const ShaderReflection& r) {
    StringTable strings;
    Writer body;
    for (const ShaderEntryPoint& e : r.entryPoints) {
        body.u32v(strings.add(e.name));
        body.u8v(static_cast<u8>(e.stage));
        body.u8v(e.usesPushConstants ? 1 : 0);
        body.u16v(0);
        for (u32 v : e.workgroupSize) body.u32v(v);
    }
    body.u32v(strings.add(r.pushConstants.blockName));
    for (const ShaderMember& m : r.pushConstants.members) {
        body.u32v(strings.add(m.name));
        body.u32v(m.offset);
        body.u32v(m.size);
        body.u8v(static_cast<u8>(m.kind));
        body.u8v(static_cast<u8>(std::min<u32>(m.components, 255)));
        body.u16v(0);
        body.u32v(m.arrayCount);
    }
    for (const ShaderSpecConstant& s : r.specConstants) {
        body.u32v(s.id);
        body.u32v(strings.add(s.name));
        body.u8v(static_cast<u8>(s.kind));
        body.u8v(0);
        body.u8v(0);
        body.u8v(0);
        body.u32v(s.defaultBits);
    }
    for (const ShaderBinding& b : r.bindings) {
        body.u32v(b.set);
        body.u32v(b.binding);
        body.u32v(b.count);
        body.u8v(static_cast<u8>(b.kind));
        body.u8v(static_cast<u8>(b.access));
        body.u16v(0);
        body.u32v(strings.add(b.name));
        body.u32v(b.entryPointMask);
    }
    const std::vector<u8>& table = strings.bytes();
    Writer out;
    const u32 total = kHeaderSize + static_cast<u32>(body.bytes().size()) + 4 + static_cast<u32>(table.size());
    out.u32v(kHsrMagic);
    out.u16v(kHsrVersion);
    out.u16v(static_cast<u16>(kHeaderSize));
    out.u32v(total);
    out.u32v(r.spirvVersion);
    out.u64v(r.contentHash.low);
    out.u64v(r.contentHash.high);
    out.u32v(static_cast<u32>(r.entryPoints.size()));
    out.u32v(r.pushConstants.size);
    out.u16v(static_cast<u16>(r.pushConstants.members.size()));
    out.u16v(static_cast<u16>(r.specConstants.size()));
    out.u32v(static_cast<u32>(r.bindings.size()));
    std::vector<u8> bytes = std::move(out.bytes());
    bytes.insert(bytes.end(), body.bytes().begin(), body.bytes().end());
    Writer tableSize;
    tableSize.u32v(static_cast<u32>(table.size()));
    bytes.insert(bytes.end(), tableSize.bytes().begin(), tableSize.bytes().end());
    bytes.insert(bytes.end(), table.begin(), table.end());
    return bytes;
}

Result<ShaderReflection> parseReflection(std::span<const u8> bytes) {
    Reader in(bytes);
    if (!in.has(kHeaderSize)) return Error{ErrorCode::Corrupt, ".hsr: truncated header"};
    if (in.u32v() != kHsrMagic) return Error{ErrorCode::Corrupt, ".hsr: bad magic"};
    const u16 version = in.u16v();
    if (version == 0) return Error{ErrorCode::Corrupt, ".hsr: version 0"};
    if (version > kHsrVersion) {
        return Error{ErrorCode::VersionMismatch, std::format(".hsr: version {} is newer than {}", version, kHsrVersion)};
    }
    const u16 headerSize = in.u16v();
    const u32 total = in.u32v();
    if (headerSize != kHeaderSize || total != bytes.size()) return Error{ErrorCode::Corrupt, ".hsr: size mismatch"};
    ShaderReflection r;
    r.spirvVersion = in.u32v();
    r.contentHash.low = in.u64v();
    r.contentHash.high = in.u64v();
    const u32 entries = in.u32v();
    r.pushConstants.size = in.u32v();
    const u32 members = in.u16v();
    const u32 specs = in.u16v();
    const u32 bindings = in.u32v();
    const u64 recordBytes = u64{entries} * kEntrySize + 4 + u64{members} * kMemberSize + u64{specs} * kSpecSize +
                            u64{bindings} * kBindingSize;
    if (kHeaderSize + recordBytes + 4 > bytes.size()) return Error{ErrorCode::Corrupt, ".hsr: truncated records"};
    // String table (after the records).
    const usize tableAt = kHeaderSize + static_cast<usize>(recordBytes);
    Reader tableReader(bytes.subspan(tableAt));
    const u32 tableSize = tableReader.u32v();
    if (tableAt + 4 + u64{tableSize} != bytes.size()) return Error{ErrorCode::Corrupt, ".hsr: bad string table size"};
    const std::span<const u8> table = bytes.subspan(tableAt + 4, tableSize);
    // Every record may name the same long string; bound the decoded text so a small blob cannot
    // expand into gigabytes (the writer deduplicates strings, so legitimate blobs stay far below).
    const u64 decodeBudget = kHsrMaxDecodedStringBytes + 16 * u64{bytes.size()};
    u64 decoded = 0;
    bool badString = false;
    bool tooLarge = false;
    auto str = [&](u32 offset) -> std::string {
        if (badString || tooLarge) return {};
        if (offset >= table.size()) {
            badString = true;
            return {};
        }
        const auto end = std::find(table.begin() + offset, table.end(), u8{0});
        if (end == table.end()) {
            badString = true;
            return {};
        }
        decoded += static_cast<u64>(end - (table.begin() + offset));
        if (decoded > decodeBudget) {
            tooLarge = true;
            return {};
        }
        return std::string(table.begin() + offset, end);
    };
    bool badEnum = false;
    for (u32 i = 0; i < entries; ++i) {
        ShaderEntryPoint e;
        e.name = str(in.u32v());
        const u8 stage = in.u8v();
        badEnum |= stage > static_cast<u8>(ShaderStage::Unknown);
        e.stage = static_cast<ShaderStage>(stage);
        e.usesPushConstants = (in.u8v() & 1) != 0;
        (void)in.u16v();
        for (u32& v : e.workgroupSize) v = in.u32v();
        r.entryPoints.push_back(std::move(e));
    }
    r.pushConstants.blockName = str(in.u32v());
    for (u32 i = 0; i < members; ++i) {
        ShaderMember m;
        m.name = str(in.u32v());
        m.offset = in.u32v();
        m.size = in.u32v();
        const u8 kind = in.u8v();
        badEnum |= kind > static_cast<u8>(ShaderValueKind::Pointer);
        m.kind = static_cast<ShaderValueKind>(kind);
        m.components = in.u8v();
        (void)in.u16v();
        m.arrayCount = in.u32v();
        r.pushConstants.members.push_back(std::move(m));
    }
    for (u32 i = 0; i < specs; ++i) {
        ShaderSpecConstant s;
        s.id = in.u32v();
        s.name = str(in.u32v());
        const u8 kind = in.u8v();
        badEnum |= kind > static_cast<u8>(ShaderValueKind::Pointer);
        s.kind = static_cast<ShaderValueKind>(kind);
        (void)in.u8v();
        (void)in.u8v();
        (void)in.u8v();
        s.defaultBits = in.u32v();
        r.specConstants.push_back(std::move(s));
    }
    for (u32 i = 0; i < bindings; ++i) {
        ShaderBinding b;
        b.set = in.u32v();
        b.binding = in.u32v();
        b.count = in.u32v();
        const u8 kind = in.u8v();
        const u8 access = in.u8v();
        badEnum |= kind > static_cast<u8>(ShaderBindingKind::Unknown) || access > 3;
        b.kind = static_cast<ShaderBindingKind>(kind);
        b.access = static_cast<ShaderAccess>(access);
        (void)in.u16v();
        b.name = str(in.u32v());
        b.entryPointMask = in.u32v();
        r.bindings.push_back(std::move(b));
    }
    if (tooLarge) return Error{ErrorCode::Corrupt, ".hsr: string references expand beyond the decode limit"};
    if (badString) return Error{ErrorCode::Corrupt, ".hsr: string offset out of range"};
    if (badEnum) return Error{ErrorCode::Corrupt, ".hsr: unknown enum value"};
    return r;
}

std::string reflectionToJsonc(const ShaderReflection& r) {
    auto quote = [](std::string_view s) {
        // Names come from the SPIR-V (any bytes): escape what JSON requires.
        std::string out = "\"";
        for (char c : s) {
            if (c == '"' || c == '\\') {
                out += '\\';
                out += c;
            } else if (static_cast<unsigned char>(c) < 0x20) {
                out += std::format("\\u{:04x}", static_cast<unsigned>(static_cast<unsigned char>(c)));
            } else {
                out += c;
            }
        }
        return out + "\"";
    };
    auto specValue = [](const ShaderSpecConstant& s) -> std::string {
        switch (s.kind) {
        case ShaderValueKind::Bool: return s.defaultBits ? "true" : "false";
        case ShaderValueKind::Int: return std::format("{}", static_cast<i32>(s.defaultBits));
        case ShaderValueKind::Float: {
            f32 f;
            std::memcpy(&f, &s.defaultBits, 4);
            // JSON has no NaN/Infinity literals: render those as strings.
            return std::isfinite(f) ? std::format("{}", f) : std::format("\"{}\"", f);
        }
        default: return std::format("{}", s.defaultBits);
        }
    };
    std::string out = "// Helios shader reflection (.hsr v1, docs/plan/03-rendering.md §1.7). Generated; do not edit.\n{\n";
    out += std::format("  \"spirvVersion\": \"{}.{}\",\n", (r.spirvVersion >> 16) & 0xFF, (r.spirvVersion >> 8) & 0xFF);
    out += std::format("  \"contentHash\": \"{}\",  // XXH3-128 of the SPIR-V\n", r.contentHash.toHex());
    out += "  \"entryPoints\": [";
    for (usize i = 0; i < r.entryPoints.size(); ++i) {
        const ShaderEntryPoint& e = r.entryPoints[i];
        out += std::format("{}\n    {{ \"name\": {}, \"stage\": \"{}\"", i ? "," : "", quote(e.name), shaderStageName(e.stage));
        if (e.stage == ShaderStage::Compute || e.stage == ShaderStage::Mesh || e.stage == ShaderStage::Task) {
            out += std::format(", \"workgroupSize\": [{}, {}, {}]", e.workgroupSize[0], e.workgroupSize[1], e.workgroupSize[2]);
        }
        out += std::format(", \"usesPushConstants\": {} }}", e.usesPushConstants ? "true" : "false");
    }
    out += r.entryPoints.empty() ? "],\n" : "\n  ],\n";
    out += std::format("  \"pushConstants\": {{ \"block\": {}, \"size\": {}, \"members\": [", quote(r.pushConstants.blockName),
                       r.pushConstants.size);
    for (usize i = 0; i < r.pushConstants.members.size(); ++i) {
        const ShaderMember& m = r.pushConstants.members[i];
        out += std::format("{}\n    {{ \"name\": {}, \"offset\": {}, \"size\": {}, \"type\": \"{}\"", i ? "," : "",
                           quote(m.name), m.offset, m.size, shaderValueKindName(m.kind));
        if (m.components > 1) out += std::format(", \"components\": {}", m.components);
        if (m.arrayCount) out += std::format(", \"arrayCount\": {}", m.arrayCount);
        out += " }";
    }
    out += r.pushConstants.members.empty() ? "] },\n" : "\n  ] },\n";
    out += "  \"specializationConstants\": [";
    for (usize i = 0; i < r.specConstants.size(); ++i) {
        const ShaderSpecConstant& s = r.specConstants[i];
        out += std::format("{}\n    {{ \"id\": {}, \"name\": {}, \"type\": \"{}\", \"default\": {} }}", i ? "," : "", s.id,
                           quote(s.name), shaderValueKindName(s.kind), specValue(s));
    }
    out += r.specConstants.empty() ? "],\n" : "\n  ],\n";
    out += "  \"bindings\": [";
    for (usize i = 0; i < r.bindings.size(); ++i) {
        const ShaderBinding& b = r.bindings[i];
        out += std::format("{}\n    {{ \"set\": {}, \"binding\": {}, \"kind\": \"{}\", \"count\": {}, \"name\": {}, "
                           "\"access\": \"{}\", \"entryPoints\": [",
                           i ? "," : "", b.set, b.binding, shaderBindingKindName(b.kind), b.count, quote(b.name),
                           shaderAccessName(b.access));
        bool first = true;
        for (u32 e = 0; e < r.entryPoints.size() && e < 32; ++e) {
            if (!(b.entryPointMask & (1u << e))) continue;
            out += std::format("{}{}", first ? "" : ", ", quote(r.entryPoints[e].name));
            first = false;
        }
        out += "] }";
    }
    out += r.bindings.empty() ? "]\n" : "\n  ]\n";
    out += "}\n";
    return out;
}

} // namespace helios::render

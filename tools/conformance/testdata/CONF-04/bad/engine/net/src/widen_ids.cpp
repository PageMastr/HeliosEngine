// Operands widened by brace initialization, as this codebase does before a shift (engine/server/src/keys.cpp).
using u64 = unsigned long long;
u64 widenedId(unsigned prefix, unsigned shard, u64 off) {
    return (u64{prefix} << 22) | (u64{shard} << 17) | off;
}

// The Snowflake layout by multiplication with a brace-initialized one: ms * 2^22 is ms << 22.
using u64 = unsigned long long;
u64 scaledId(u64 ms, u64 shard, u64 seq) {
    return ms * (u64{1} << 22) + shard * (u64{1} << 12) + seq;
}

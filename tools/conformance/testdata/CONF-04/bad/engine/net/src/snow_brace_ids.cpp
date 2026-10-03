// The Snowflake layout with qualified brace-initialized operands.
#include <cstdint>
std::uint64_t snowBraced(std::uint32_t ms, std::uint32_t shard, std::uint64_t seq) {
    return (std::uint64_t{ms} << 22) | (std::uint64_t{shard} << 12) | seq;
}

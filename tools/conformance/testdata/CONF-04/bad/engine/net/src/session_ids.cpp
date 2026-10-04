// A Snowflake built by multiplication: ms * 2^22 is ms << 22.
#include <cstdint>
std::uint64_t sessionId(std::uint64_t ms, std::uint64_t node, std::uint64_t seq) {
    return ms * (1ull << 22) + (1ull << 12) * node + seq;
}

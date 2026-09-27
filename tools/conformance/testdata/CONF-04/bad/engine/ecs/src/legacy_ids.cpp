// The retired 41/5/8/9 layout.
#include <cstdint>
constexpr int kSeqBits = 9;
std::uint64_t legacyId(std::uint64_t ms, std::uint64_t dc, std::uint64_t machineId, std::uint64_t seq) {
    return (ms << 22) | (dc << 17) | (machineId << kSeqBits) | seq;
}

#pragma once
#include <cstdint>
struct BlockIdLayout {
    static constexpr unsigned kOffsetBits = 17;
    static constexpr unsigned kShardBits = 5;
    static constexpr unsigned kPrefixShift = kOffsetBits + kShardBits;
};
constexpr std::uint64_t composeBlockId(std::uint64_t prefix, std::uint64_t shard, std::uint64_t offset) {
    return (prefix << BlockIdLayout::kPrefixShift) | (shard << BlockIdLayout::kOffsetBits) | offset;
}

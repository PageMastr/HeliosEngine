// Not ID code: sizes and packing that are not the ID layout.
constexpr unsigned kPage = 1u << 12;
constexpr unsigned long long kBig = 1ull << 22;
unsigned long long pack(unsigned long long hi, unsigned lo) { return (hi << 32) | lo; }
// nodeId in a comment, and a "node_id" string outside ID code
const char* kLabel = "node_id";
// A constant under #if 0 is never compiled, so its 22 is not a shift amount.
#if 0
constexpr unsigned kOldShift = 22;
#else
constexpr unsigned kOldShift = 20;
#endif
unsigned long long scaled(unsigned long long v) { return v << kOldShift; }
// A brace-initialized literal is a size too.
constexpr unsigned long long kWideBig = std::uint64_t{1} << 22;
constexpr unsigned long long kWideBigU = std::uint64_t{ 1ull } << 22;

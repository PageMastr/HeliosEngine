// Not ID code: sizes and packing that are not the ID layout.
constexpr unsigned kPage = 1u << 12;
constexpr unsigned long long kBig = 1ull << 22;
unsigned long long pack(unsigned long long hi, unsigned lo) { return (hi << 32) | lo; }
// nodeId in a comment, and a "node_id" string outside ID code
const char* kLabel = "node_id";

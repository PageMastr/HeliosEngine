// Fuzz target: Address::parse / toString. Property: anything that parses formats to a string
// that parses back to the same address.

#include <cstdlib>
#include <string_view>

#include "fuzz_common.h"
#include "helios/net/address.h"

using namespace helios::net;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    const auto a = Address::parse(text);
    if (!a) return 0;
    const std::string s = a->toString();
    const auto b = Address::parse(s);
    if (!b || *b != *a) std::abort();
    if (a->unmapped().toV4Mapped().unmapped() != a->unmapped()) std::abort();
    (void)a->hash();
    return 0;
}

void heliosFuzzSeeds(std::vector<std::vector<uint8_t>>& out) {
    for (const char* s : {"127.0.0.1:40000", "[::1]:40001", "10.0.0.5", "[2001:db8:85a3::8a2e:370:7334]:65535",
                          "::ffff:1.2.3.4", "[::]:0", "255.255.255.255:65535", "1:2:3:4:5:6:7:8", "[fe80::1]:9",
                          "1::", "::", "0.0.0.0:7777"}) {
        const std::string_view v(s);
        out.emplace_back(v.begin(), v.end());
    }
}

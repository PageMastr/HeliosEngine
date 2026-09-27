// Seeded parse failure: an #endif without its #if, then an #if without its #endif.
#include <doctest/doctest.h>

#endif

#if defined(HELIOS_DEMO)
namespace {
TEST_CASE("demo: plain") {
    CHECK(true);
}
} // namespace

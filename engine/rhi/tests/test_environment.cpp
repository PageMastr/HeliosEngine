// Environment overrides of a DeviceDesc (rhi::detail::applyEnvironment, src/rhi_internal.h), read
// through a fake environment so the test neither depends on nor changes the process environment.

#include <doctest/doctest.h>

#include <optional>
#include <string>
#include <string_view>

#include "rhi_internal.h"

namespace {

using namespace helios;
using rhi::DeviceDesc;

std::optional<std::string> validationOn(const char* name) {
    if (std::string_view(name) == "HELIOS_RHI_VALIDATION") return std::string("1");
    return std::nullopt;
}

std::optional<std::string> validationOff(const char* name) {
    if (std::string_view(name) == "HELIOS_RHI_VALIDATION") return std::string("0");
    return std::nullopt;
}

std::optional<std::string> noVariables(const char*) { return std::nullopt; }

TEST_CASE("rhi environment: HELIOS_RHI_VALIDATION overrides validation unless validationFromEnvironment is false") {
    DeviceDesc desc;
    REQUIRE(desc.validationFromEnvironment);
    desc.validation = false;
    CHECK(rhi::detail::applyEnvironment(desc, validationOn).validation);
    desc.validation = true;
    CHECK_FALSE(rhi::detail::applyEnvironment(desc, validationOff).validation);
    CHECK(rhi::detail::applyEnvironment(desc, noVariables).validation);

    // helios-rendertest's no-validation probe (review mutant RE1 dropped this condition).
    desc.validationFromEnvironment = false;
    desc.validation = false;
    CHECK_FALSE(rhi::detail::applyEnvironment(desc, validationOn).validation);
    desc.validation = true;
    CHECK(rhi::detail::applyEnvironment(desc, validationOff).validation);
}

} // namespace

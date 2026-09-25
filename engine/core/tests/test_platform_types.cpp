#include <doctest/doctest.h>

#include <string_view>

#include "helios/core/platform.h"
#include "helios/core/types.h"

using namespace helios;
using namespace helios::literals;

namespace {
enum class TestFlags : u32 { None = 0, A = 1, B = 2, C = 4 };
HELIOS_ENUM_FLAGS(TestFlags)
} // namespace

TEST_CASE("platform: exactly one OS/compiler/arch is detected") {
    int platforms = 0;
#if defined(HELIOS_PLATFORM_WINDOWS)
    ++platforms;
    CHECK(platform::kIsWindows);
#endif
#if defined(HELIOS_PLATFORM_LINUX)
    ++platforms;
    CHECK(platform::kIsLinux);
    CHECK(platform::kIsPosix);
#endif
#if defined(HELIOS_PLATFORM_MACOS)
    ++platforms;
#endif
    CHECK(platforms == 1);
    CHECK(platform::kIsWindows != platform::kIsPosix);

    int compilers = 0;
#if defined(HELIOS_COMPILER_MSVC)
    ++compilers;
#endif
#if defined(HELIOS_COMPILER_CLANG)
    ++compilers;
#endif
#if defined(HELIOS_COMPILER_GCC)
    ++compilers;
#endif
    CHECK(compilers == 1);
    CHECK(static_cast<int>(platform::kIsMsvc) + static_cast<int>(platform::kIsClang) + static_cast<int>(platform::kIsGcc) == 1);

#if defined(HELIOS_ARCH_X64)
    CHECK(std::string_view(platform::kArchName) == "x64");
#endif
    CHECK(std::string_view(platform::kPlatformName).size() > 0);
    CHECK(std::string_view(platform::kCompilerName).size() > 0);
}

TEST_CASE("platform: attribute and utility macros") {
    int x = 3;
    if (HELIOS_LIKELY(x == 3)) x = 4;
    if (HELIOS_UNLIKELY(x == 3)) x = 0;
    CHECK(x == 4);
    HELIOS_ASSUME(x == 4);
    CHECK(std::string_view(HELIOS_STRINGIFY(HELIOS_CACHE_LINE_SIZE)) == "64");
    const int HELIOS_CONCAT(joined, Name) = 7;
    CHECK(joinedName == 7);
    CHECK(std::string_view(HELIOS_FUNCTION_NAME).size() > 0);
}

TEST_CASE("types: sizes and literals") {
    static_assert(sizeof(u8) == 1 && sizeof(u16) == 2 && sizeof(u32) == 4 && sizeof(u64) == 8);
    static_assert(sizeof(i8) == 1 && sizeof(i16) == 2 && sizeof(i32) == 4 && sizeof(i64) == 8);
    static_assert(sizeof(f32) == 4 && sizeof(f64) == 8 && sizeof(usize) == 8 && sizeof(isize) == 8);
    static_assert(4_KiB == 4096 && 2_MiB == 2ull * 1024 * 1024 && 1_GiB == kGiB);
}

TEST_CASE("types: alignment and power-of-two helpers") {
    static_assert(isPowerOfTwo(1u) && isPowerOfTwo(64u) && !isPowerOfTwo(0u) && !isPowerOfTwo(12u));
    static_assert(nextPowerOfTwo(5u) == 8u && nextPowerOfTwo(8u) == 8u && nextPowerOfTwo(0u) == 1u);
    static_assert(alignUp<u64>(13, 8) == 16 && alignUp<u64>(16, 8) == 16 && alignDown<u64>(13, 8) == 8);
    static_assert(isAligned<u64>(64, 16) && !isAligned<u64>(65, 16));
    alignas(64) char buffer[128];
    CHECK(isAligned(buffer, 64));
    CHECK(alignPointer(buffer + 1, 16) == buffer + 16);
}

TEST_CASE("types: byte order helpers") {
    static_assert(byteSwap16(0x1234) == 0x3412);
    static_assert(byteSwap32(0x11223344u) == 0x44332211u);
    static_assert(byteSwap64(0x0102030405060708ull) == 0x0807060504030201ull);
    u8 bytes[8];
    storeLE<u32>(bytes, 0xAABBCCDDu);
    CHECK(bytes[0] == 0xDD);
    CHECK(bytes[3] == 0xAA);
    CHECK(loadLE<u32>(bytes) == 0xAABBCCDDu);
    storeBE<u64>(bytes, 0x0102030405060708ull);
    CHECK(bytes[0] == 0x01);
    CHECK(bytes[7] == 0x08);
    CHECK(loadBE<u64>(bytes) == 0x0102030405060708ull);
    const u32 value = 7;
    CHECK(objectBytes(value).size() == 4);
}

TEST_CASE("types: enum flag operators") {
    TestFlags f = TestFlags::A | TestFlags::C;
    CHECK(hasFlag(f, TestFlags::A));
    CHECK(!hasFlag(f, TestFlags::B));
    CHECK(hasAnyFlag(f, TestFlags::B | TestFlags::C));
    f &= ~TestFlags::A;
    CHECK(f == TestFlags::C);
    f ^= TestFlags::B;
    CHECK(toUnderlying(f) == 6u);
}

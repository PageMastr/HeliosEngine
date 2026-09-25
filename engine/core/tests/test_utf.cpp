#include <doctest/doctest.h>

#include <string>

#include "helios/core/utf.h"

using namespace helios;

namespace {
// "aé€😀": 1-, 2-, 3- and 4-byte sequences.
const std::string kMixed = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80";
const std::string kFffd = "\xEF\xBF\xBD";
} // namespace

TEST_CASE("utf: UTF-8 <-> UTF-16 round trip including surrogate pairs") {
    const std::u16string u16 = utf8ToUtf16(kMixed);
    CHECK(u16 == std::u16string{u'a', 0x00E9, 0x20AC, 0xD83D, 0xDE00});
    CHECK(utf16ToUtf8(u16) == kMixed);
    CHECK(utf8Length(kMixed) == 4);
    CHECK(utf8ToUtf32(kMixed) == std::u32string{U'a', 0xE9, 0x20AC, 0x1F600});
    CHECK(utf32ToUtf8(U"aé€\U0001F600") == kMixed);
    CHECK(wideToUtf8(utf8ToWide(kMixed)) == kMixed);
    if constexpr (sizeof(wchar_t) == 2) {
        CHECK(utf8ToWide(kMixed).size() == 5);
    } else {
        CHECK(utf8ToWide(kMixed).size() == 4);
    }
    CHECK(utf8ToUtf16("").empty());
}

TEST_CASE("utf: validation") {
    CHECK(isValidUtf8(kMixed));
    CHECK(isValidUtf8(""));
    CHECK(isValidUtf8(kFffd)); // a genuine U+FFFD is valid
    CHECK(!isValidUtf8("\xC0\xAF"));         // overlong '/'
    CHECK(!isValidUtf8("\xE0\x80\x80"));     // overlong NUL
    CHECK(!isValidUtf8("\xED\xA0\x80"));     // UTF-16 surrogate
    CHECK(!isValidUtf8("\xF4\x90\x80\x80")); // > U+10FFFF
    CHECK(!isValidUtf8("\xE2\x82"));         // truncated
    CHECK(!isValidUtf8("\x80"));             // stray continuation
    CHECK(!isValidUtf8("\xFF"));
}

TEST_CASE("utf: invalid input decodes to U+FFFD per maximal subpart") {
    CHECK(sanitizeUtf8("\xC0\xAF") == kFffd + kFffd);
    CHECK(sanitizeUtf8("\xE0\x80\x80") == kFffd + kFffd + kFffd);
    CHECK(sanitizeUtf8("a\xE2\x82z") == "a" + kFffd + "z"); // truncated sequence: one U+FFFD, 'z' kept
    CHECK(sanitizeUtf8("\xF0\x9F\x98") == kFffd);
    CHECK(sanitizeUtf8(kMixed) == kMixed);
    usize pos = 0;
    CHECK(decodeUtf8("\xE2\x82\xAC!", pos) == 0x20AC);
    CHECK(pos == 3);
    CHECK(decodeUtf8("\xE2\x82\xAC!", pos) == U'!');
    CHECK(decodeUtf8("x", pos) == kReplacementChar); // past the end
}

TEST_CASE("utf: unpaired surrogates and invalid code points are replaced") {
    CHECK(utf16ToUtf8(std::u16string{0xD800}) == kFffd);
    CHECK(utf16ToUtf8(std::u16string{0xDC00, u'x'}) == kFffd + "x");
    CHECK(utf16ToUtf8(std::u16string{0xD83D, u'x'}) == kFffd + "x");
    std::string out;
    appendUtf8(out, 0x110000);
    appendUtf8(out, 0xD800);
    CHECK(out == kFffd + kFffd);
    CHECK(utf32ToUtf8(std::u32string{0x7F, 0x80, 0x7FF, 0x800, 0xFFFF, 0x10000, 0x10FFFF}) ==
          "\x7F\xC2\x80\xDF\xBF\xE0\xA0\x80\xEF\xBF\xBF\xF0\x90\x80\x80\xF4\x8F\xBF\xBF");
}

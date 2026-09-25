#include <doctest/doctest.h>

#include <format>
#include <memory>
#include <string>

#include "helios/core/result.h"

using namespace helios;

namespace {

Result<int> parsePositive(int v) {
    if (v <= 0) return makeError(ErrorCode::InvalidArgument, "{} is not positive", v);
    return v;
}

Result<int> doubled(int v) {
    HELIOS_TRY_ASSIGN(const int x, parsePositive(v));
    return x * 2;
}

Result<void> check(int v) {
    HELIOS_TRY(parsePositive(v));
    return {};
}

Result<std::string> chain(int v) {
    HELIOS_TRY(check(v));
    HELIOS_TRY_ASSIGN(auto d, doubled(v));
    return std::to_string(d);
}

Result<std::unique_ptr<int>> makeOwned(int v) {
    if (v < 0) return Error{ErrorCode::OutOfRange, "negative"};
    return std::make_unique<int>(v);
}

} // namespace

TEST_CASE("result: value and error states") {
    Result<int> ok = 5;
    CHECK(ok.hasValue());
    CHECK(ok.ok());
    CHECK(static_cast<bool>(ok));
    CHECK(ok.value() == 5);
    CHECK(*ok == 5);
    CHECK(ok.errorCode() == ErrorCode::Ok);

    Result<int> bad = Error{ErrorCode::NotFound, "missing thing"};
    CHECK(!bad);
    CHECK(bad.error().code == ErrorCode::NotFound);
    CHECK(bad.error().message == "missing thing");
    CHECK(bad.errorCode() == ErrorCode::NotFound);
    CHECK(bad.valueOr(9) == 9);
    CHECK(ok.valueOr(9) == 5);
}

TEST_CASE("result: HELIOS_TRY and HELIOS_TRY_ASSIGN propagate errors") {
    CHECK(doubled(4).value() == 8);
    CHECK(doubled(-1).error().code == ErrorCode::InvalidArgument);
    CHECK(doubled(-1).error().message == "-1 is not positive");
    CHECK(check(3).ok());
    CHECK(!check(0).ok());
    CHECK(chain(21).value() == "42");
    CHECK(chain(-5).error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("result: move-only values") {
    auto r = makeOwned(3);
    REQUIRE(r);
    std::unique_ptr<int> owned = std::move(r).value();
    CHECK(*owned == 3);
    CHECK(makeOwned(-1).error().code == ErrorCode::OutOfRange);
    Result<std::unique_ptr<int>> inPlace(std::in_place, new int(11));
    CHECK(**inPlace == 11);
}

TEST_CASE("result: map and andThen") {
    Result<int> r = 10;
    auto s = r.map([](int v) { return std::to_string(v + 1); });
    CHECK(s.value() == "11");
    Result<int> e = Error{ErrorCode::Timeout};
    CHECK(e.map([](int v) { return v; }).error().code == ErrorCode::Timeout);
    CHECK(r.andThen(parsePositive).value() == 10);
    CHECK(Result<int>(-3).andThen(parsePositive).error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("result: void results") {
    Result<void> ok;
    CHECK(ok.ok());
    ok.value(); // no-op on success
    Result<void> bad = Error{ErrorCode::Busy, "try later"};
    CHECK(!bad);
    CHECK(bad.errorCode() == ErrorCode::Busy);
}

TEST_CASE("result: error codes and formatting") {
    CHECK(errorCodeName(ErrorCode::PermissionDenied) == "PermissionDenied");
    CHECK(errorCodeName(ErrorCode::LimitExceeded) == "LimitExceeded");
    const Error e{ErrorCode::ParseError, "line 3"};
    CHECK(e.toString() == "ParseError: line 3");
    CHECK(Error{ErrorCode::Cancelled}.toString() == "Cancelled");
    CHECK(std::format("{}", e) == "ParseError: line 3");
    CHECK(std::format("{}", ErrorCode::IoError) == "IoError");
    CHECK(e == Error{ErrorCode::ParseError, "line 3"});
}

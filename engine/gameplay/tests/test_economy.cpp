// Reason-code registry (06 §4, 05 §1.6).
#include <doctest/doctest.h>

#include <vector>

#include "helios/gameplay/economy.h"

using namespace helios;
using namespace helios::gameplay;

namespace {
ReasonCodeDef code(const char* c, ReasonKind k) {
    ReasonCodeDef d;
    d.code = Name(c);
    d.kind = k;
    return d;
}
} // namespace

TEST_CASE("economy: reason codes are validated, indexed and hashed deterministically") {
    std::vector<ReasonCodeDef> defs = {code("Sink.Tax.Market.Broker", ReasonKind::Sink), code("Faucet.Bounty.NPC", ReasonKind::Faucet),
                                       code("Transfer.Trade", ReasonKind::Transfer)};
    defs[1].faucet = FaucetCap{5000000};
    defs[1].maxTxPerHour = 100000;
    defs[1].killSwitch = Name("econ.bounty");
    std::vector<ReasonCodeRegistry::Record> recs = {{11, &defs[0]}, {12, &defs[1]}, {13, &defs[2]}};
    auto reg = ReasonCodeRegistry::build(recs);
    REQUIRE_MESSAGE(reg.ok(), (reg.ok() ? std::string() : reg.error().message));
    const auto& r = **reg;
    CHECK(r.size() == 3);
    CHECK(r.codes()[0].code == "Faucet.Bounty.NPC"); // sorted
    const ReasonCodeInfo* bounty = r.find("Faucet.Bounty.NPC");
    REQUIRE(bounty);
    CHECK(bounty->dailyCap == 5000000);
    CHECK(bounty->killSwitch == "econ.bounty");
    CHECK(r.findByRecord(12) == bounty);
    CHECK(r.find("Faucet.Unknown") == nullptr);
    CHECK(ReasonCodeRegistry::systemAccount(*bounty) == "mint:Faucet.Bounty.NPC");
    const ReasonCodeInfo* tax = r.find("Sink.Tax.Market.Broker");
    CHECK(ReasonCodeRegistry::systemAccount(*tax) == "burn:Sink.Tax.Market.Broker");
    const ReasonCodeInfo* trade = r.find("Transfer.Trade");
    CHECK(ReasonCodeRegistry::systemAccount(*trade).empty());
    // Faucets touch only their own mint account, sinks their own burn account, transfers neither.
    CHECK(ReasonCodeRegistry::mayTouch(*bounty, "char:42"));
    CHECK(ReasonCodeRegistry::mayTouch(*bounty, "mint:Faucet.Bounty.NPC"));
    CHECK_FALSE(ReasonCodeRegistry::mayTouch(*bounty, "mint:Faucet.Quest"));
    CHECK_FALSE(ReasonCodeRegistry::mayTouch(*bounty, "burn:Faucet.Bounty.NPC"));
    CHECK(ReasonCodeRegistry::mayTouch(*tax, "burn:Sink.Tax.Market.Broker"));
    CHECK_FALSE(ReasonCodeRegistry::mayTouch(*tax, "mint:Sink.Tax.Market.Broker"));
    CHECK_FALSE(ReasonCodeRegistry::mayTouch(*trade, "mint:Transfer.Trade"));
    CHECK(ReasonCodeRegistry::mayTouch(*trade, "escrow:7"));
    // The hash does not depend on record order.
    std::vector<ReasonCodeRegistry::Record> reversed(recs.rbegin(), recs.rend());
    CHECK((*ReasonCodeRegistry::build(reversed))->hash() == r.hash());
    defs[1].maxTxPerHour = 1;
    CHECK((*ReasonCodeRegistry::build(recs))->hash() != r.hash());
}

TEST_CASE("economy: invalid reason codes are rejected") {
    auto one = [](ReasonCodeDef d) {
        std::vector<ReasonCodeRegistry::Record> recs = {{1, &d}};
        return ReasonCodeRegistry::build(recs).errorCode();
    };
    CHECK(one(code("Sink.Bounty", ReasonKind::Faucet)) == ErrorCode::InvalidArgument);   // class prefix
    CHECK(one(code("Faucet", ReasonKind::Faucet)) == ErrorCode::InvalidArgument);        // no sub-code
    CHECK(one(code("Faucet..X", ReasonKind::Faucet)) == ErrorCode::InvalidArgument);     // syntax
    auto capped = code("Sink.Fee", ReasonKind::Sink);
    capped.faucet = FaucetCap{10};
    CHECK(one(capped) == ErrorCode::InvalidArgument); // only faucets have caps
    auto negative = code("Faucet.X", ReasonKind::Faucet);
    negative.maxAmountPerTx = -1;
    CHECK(one(negative) == ErrorCode::InvalidArgument);
    auto a = code("Transfer.Trade", ReasonKind::Transfer);
    std::vector<ReasonCodeRegistry::Record> dup = {{1, &a}, {2, &a}};
    CHECK(ReasonCodeRegistry::build(dup).errorCode() == ErrorCode::AlreadyExists);
}

TEST_CASE("economy: two reason codes with one record id are rejected (review regression)") {
    std::vector<ReasonCodeDef> defs = {code("Faucet.Bounty.NPC", ReasonKind::Faucet), code("Transfer.Trade", ReasonKind::Transfer)};
    std::vector<ReasonCodeRegistry::Record> recs = {{7, &defs[0]}, {7, &defs[1]}};
    CHECK(ReasonCodeRegistry::build(recs).errorCode() == ErrorCode::AlreadyExists);
    recs[1].rid = 0; // unknown ids never collide
    std::vector<ReasonCodeRegistry::Record> unknown = {{0, &defs[0]}, {0, &defs[1]}};
    CHECK(ReasonCodeRegistry::build(recs).ok());
    CHECK(ReasonCodeRegistry::build(unknown).ok());
}

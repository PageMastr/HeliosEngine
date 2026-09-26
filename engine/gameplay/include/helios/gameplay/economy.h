#pragma once
// Reason-code registry (06 §4, 05 §1.6): every ledger journal entry names a ReasonCodeDef whose class
// (faucet, sink or transfer) restricts which system accounts it may touch, so faucet and sink
// telemetry is correct by construction. Cells use it to reject unknown codes before a ledger call
// and to build the system account names; the ledger enforces the same rules in Go.
//
// Rules checked when the registry is built:
//   - codes are dotted names whose first segment names the class ("Faucet.Bounty.NPC" must be a
//     Faucet, "Sink.Tax.Market.Broker" a Sink, "Transfer.Trade" a Transfer) and are unique;
//   - only faucets may carry a world-script daily cap (05 §1.23); caps and rate limits are >= 0.
//
// Threading: immutable after build(); safe to share.

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "gameplay/economy.gen.h"
#include "helios/core/result.h"
#include "helios/core/types.h"
#include "helios/reflect/types.h"

namespace helios::gameplay {

/// A validated reason code.
struct ReasonCodeInfo {
    refl::RecordId rid = 0;
    std::string code;
    ReasonKind kind = ReasonKind::Transfer;
    i64 dailyCap = -1; ///< faucet cap per (project, reason, day); -1 = none
    i64 maxTxPerHour = 0;
    i64 maxAmountPerTx = 0;
    std::string killSwitch;
};

/// "Faucet" / "Sink" / "Transfer".
std::string_view reasonKindName(ReasonKind kind) noexcept;

class ReasonCodeRegistry {
public:
    /// One record with its RecordId ($rid of the .hrec file; 0 if unknown).
    struct Record {
        refl::RecordId rid = 0;
        const ReasonCodeDef* def = nullptr;
    };

    /// Validates and indexes the records (sorted by code).
    static Result<std::shared_ptr<const ReasonCodeRegistry>> build(std::span<const Record> records);

    usize size() const noexcept { return m_codes.size(); }
    /// nullptr for unknown codes (the ledger rejects them, 05 §1.6).
    const ReasonCodeInfo* find(std::string_view code) const noexcept;
    const ReasonCodeInfo* findByRecord(refl::RecordId rid) const noexcept;
    std::span<const ReasonCodeInfo> codes() const noexcept { return m_codes; }

    /// The system account a posting under `code` may touch: "mint:<code>" for faucets,
    /// "burn:<code>" for sinks, empty for transfers (05 §1.6).
    static std::string systemAccount(const ReasonCodeInfo& code);
    /// True if a posting under `code` may touch `account`: player/org/escrow accounts always (no
    /// "mint:"/"burn:" prefix), a system account only if it is the code's own.
    static bool mayTouch(const ReasonCodeInfo& code, std::string_view account) noexcept;

    /// FNV-1a 64 over the sorted codes and their rules (content version).
    u64 hash() const noexcept { return m_hash; }

private:
    std::vector<ReasonCodeInfo> m_codes; // sorted by code
    u64 m_hash = 0;
};

} // namespace helios::gameplay

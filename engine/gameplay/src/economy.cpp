// Reason-code registry (06 §4, 05 §1.6).
#include "helios/gameplay/economy.h"

#include <algorithm>
#include <format>

#include "helios/core/hash.h"
#include "helios/gameplay/tags.h"

namespace helios::gameplay {

std::string_view reasonKindName(ReasonKind kind) noexcept {
    switch (kind) {
    case ReasonKind::Faucet: return "Faucet";
    case ReasonKind::Sink: return "Sink";
    case ReasonKind::Transfer: return "Transfer";
    }
    return "?";
}

Result<std::shared_ptr<const ReasonCodeRegistry>> ReasonCodeRegistry::build(std::span<const Record> records) {
    auto reg = std::make_shared<ReasonCodeRegistry>();
    reg->m_codes.reserve(records.size());
    for (const Record& r : records) {
        if (!r.def) return Error{ErrorCode::InvalidArgument, "reason code record without a definition"};
        const ReasonCodeDef& d = *r.def;
        const std::string_view code = d.code.view();
        if (!isValidTagName(code)) return makeError(ErrorCode::InvalidArgument, "invalid reason code '{}'", code);
        const std::string_view first = code.substr(0, code.find('.'));
        if (first != reasonKindName(d.kind) || first.size() == code.size()) {
            return makeError(ErrorCode::InvalidArgument, "reason code '{}' must start with '{}.' (its class)", code,
                             reasonKindName(d.kind));
        }
        if (d.faucet && d.kind != ReasonKind::Faucet) {
            return makeError(ErrorCode::InvalidArgument, "reason code '{}': only faucets have a daily cap", code);
        }
        if ((d.faucet && d.faucet->dailyCap < 0) || d.maxTxPerHour < 0 || d.maxAmountPerTx < 0) {
            return makeError(ErrorCode::InvalidArgument, "reason code '{}': caps and limits must be >= 0", code);
        }
        ReasonCodeInfo info;
        info.rid = r.rid;
        info.code = std::string(code);
        info.kind = d.kind;
        info.dailyCap = d.faucet ? d.faucet->dailyCap : -1;
        info.maxTxPerHour = d.maxTxPerHour;
        info.maxAmountPerTx = d.maxAmountPerTx;
        info.killSwitch = std::string(d.killSwitch.view());
        reg->m_codes.push_back(std::move(info));
    }
    std::sort(reg->m_codes.begin(), reg->m_codes.end(),
              [](const ReasonCodeInfo& a, const ReasonCodeInfo& b) { return a.code < b.code; });
    for (usize i = 1; i < reg->m_codes.size(); ++i) {
        if (reg->m_codes[i].code == reg->m_codes[i - 1].code) {
            return makeError(ErrorCode::AlreadyExists, "duplicate reason code '{}'", reg->m_codes[i].code);
        }
    }
    // findByRecord() must be unambiguous (sorted, not pairwise: content can hold many codes).
    std::vector<std::pair<refl::RecordId, usize>> rids;
    rids.reserve(reg->m_codes.size());
    for (usize i = 0; i < reg->m_codes.size(); ++i) {
        if (reg->m_codes[i].rid != 0) rids.emplace_back(reg->m_codes[i].rid, i);
    }
    std::sort(rids.begin(), rids.end());
    for (usize k = 1; k < rids.size(); ++k) {
        if (rids[k].first == rids[k - 1].first) {
            return makeError(ErrorCode::AlreadyExists, "reason codes '{}' and '{}' share record id {}",
                             reg->m_codes[rids[k - 1].second].code, reg->m_codes[rids[k].second].code, rids[k].first);
        }
    }
    u64 h = kFnv1a64Offset;
    auto mix = [&](u64 v, int bytes) {
        for (int i = 0; i < bytes; ++i) {
            h ^= (v >> (8 * i)) & 0xff;
            h *= kFnv1a64Prime;
        }
    };
    for (const ReasonCodeInfo& c : reg->m_codes) {
        for (char ch : c.code) mix(static_cast<u8>(ch), 1);
        mix(0, 1);
        mix(static_cast<u64>(c.kind), 1);
        mix(static_cast<u64>(c.dailyCap), 8);
        mix(static_cast<u64>(c.maxTxPerHour), 8);
        mix(static_cast<u64>(c.maxAmountPerTx), 8);
        for (char ch : c.killSwitch) mix(static_cast<u8>(ch), 1);
        mix(0, 1);
    }
    reg->m_hash = h;
    return std::shared_ptr<const ReasonCodeRegistry>(std::move(reg));
}

const ReasonCodeInfo* ReasonCodeRegistry::find(std::string_view code) const noexcept {
    auto it = std::lower_bound(m_codes.begin(), m_codes.end(), code,
                               [](const ReasonCodeInfo& c, std::string_view v) { return std::string_view(c.code) < v; });
    return it != m_codes.end() && it->code == code ? &*it : nullptr;
}

const ReasonCodeInfo* ReasonCodeRegistry::findByRecord(refl::RecordId rid) const noexcept {
    if (rid == 0) return nullptr;
    for (const ReasonCodeInfo& c : m_codes) {
        if (c.rid == rid) return &c;
    }
    return nullptr;
}

std::string ReasonCodeRegistry::systemAccount(const ReasonCodeInfo& code) {
    switch (code.kind) {
    case ReasonKind::Faucet: return "mint:" + code.code;
    case ReasonKind::Sink: return "burn:" + code.code;
    case ReasonKind::Transfer: return {};
    }
    return {};
}

bool ReasonCodeRegistry::mayTouch(const ReasonCodeInfo& code, std::string_view account) noexcept {
    const bool system = account.starts_with("mint:") || account.starts_with("burn:");
    if (!system) return true;
    if (code.kind == ReasonKind::Transfer) return false;
    const std::string_view prefix = code.kind == ReasonKind::Faucet ? "mint:" : "burn:";
    return account.starts_with(prefix) && account.substr(prefix.size()) == code.code;
}

} // namespace helios::gameplay

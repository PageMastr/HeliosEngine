#pragma once
// ToolsFramework vocabulary (07 §1.2): document ids, transaction ids and command origins.
//
// Threading: plain value types; the free functions are pure.

#include <compare>
#include <optional>
#include <string>
#include <string_view>

#include "helios/core/guid.h"
#include "helios/core/log.h"
#include "helios/core/types.h"

namespace helios::tf {

HELIOS_LOG_CHANNEL(LogTools, "Tools");

/// A document's stable identity (07 §1.2). Records use a GUID minted when the document is first
/// opened or created in a session; the record's own `$rid` stays the content identity on disk.
using DocId = Guid;

/// Input path that produced a transaction (07 §1.2 `origin`). ToolsFramework stamps it from the
/// command invoker the input arrived through; callers never pass it with the command arguments.
enum class Origin : u8 {
    Ui,         ///< A human's input in the editor (or a web tool).
    UiScripted, ///< Injected input from helios-uitest (`ui.*` over the remote-control socket).
    Luau,       ///< Editor automation (Luau editor VM).
    Rpc,        ///< Remote control (JSON-RPC 2.0: DCC plug-ins, test harnesses, helios:// links).
    Cli,        ///< helios-tool.
    Import,     ///< Asset import, live links, external file edits.
    Collab,     ///< Rebase and merge of collaborative sessions (Phase 3).
};
inline constexpr u32 kOriginCount = 7;

/// "ui", "ui-scripted", "luau", "rpc", "cli", "import", "collab" (the journal and report spelling).
std::string_view originName(Origin origin) noexcept;
std::optional<Origin> parseOrigin(std::string_view text) noexcept;

/// Transaction id `(user, lamport)` (07 §1.2). Lamport values increase with every commit in a
/// session and continue from the highest journaled value after recovery.
struct TxId {
    std::string user;
    u64 lamport = 0;

    bool isNull() const noexcept { return lamport == 0; }
    /// "user:lamport".
    std::string toString() const;
    friend bool operator==(const TxId&, const TxId&) = default;
    friend auto operator<=>(const TxId&, const TxId&) = default;
};

/// 16 lowercase hex digits (content hashes in journals, RPC results and file headers).
std::string hashHex(u64 hash);
std::optional<u64> parseHashHex(std::string_view text) noexcept;

} // namespace helios::tf

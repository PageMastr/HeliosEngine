#pragma once
// ToolsFramework vocabulary (07 §1.2): document ids, transaction ids and command origins.
//
// Threading: plain value types; the free functions are pure.

#include <algorithm>
#include <compare>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "helios/core/guid.h"
#include "helios/core/log.h"
#include "helios/core/types.h"
#include "helios/editor_api.h"

namespace helios::tf {

/// Log channel of the ToolsFramework (defined in types.cpp: one per process).
HELIOS_LOG_CHANNEL_EXTERN(HELIOS_EDITOR_API, LogTools);

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

/// True when `ids` contains `id`. Use this rather than std::find over document ids: MSVC's STL
/// (14.51) vectorizes std::find for trivially equality-comparable element types and, under
/// clang-cl, static_asserts on a 16-byte Guid ("unexpected size").
inline bool containsDoc(std::span<const DocId> ids, const DocId& id) noexcept {
    return std::any_of(ids.begin(), ids.end(), [&](const DocId& d) { return d == id; });
}

/// 16 lowercase hex digits (content hashes in journals, RPC results and file headers).
std::string hashHex(u64 hash);
std::optional<u64> parseHashHex(std::string_view text) noexcept;

/// True for a character of Unicode general category Cf (format: the bidi embeddings, overrides and
/// isolates U+202A-U+202E and U+2066-U+2069, the marks U+200E/U+200F, zero-width characters, the
/// BOM, tag characters), Zl (U+2028) or Zp (U+2029), per Unicode 15.1. Such a character is
/// invisible, and a bidi control reorders how the rest of a line displays. Threading: pure.
bool isFormatOrSeparator(char32_t cp) noexcept;

/// `text` for a terminal, a log line or an error message: control characters escaped (C0 and DEL
/// as `\xNN`, C1 U+0080-U+009F as `\u00NN`, and each byte that is not well-formed UTF-8 as `\xNN`,
/// so a lone 0x9B is not the 8-bit CSI either), so a string from a journal (untrusted input) cannot
/// carry terminal escape sequences; format characters and line/paragraph separators
/// (isFormatOrSeparator) escaped as `\uNNNN` (`\UNNNNNNNN` above U+FFFF), so a bidi override cannot
/// make a path or label display as something else; cut after `maxBytes` bytes with "...". The
/// result is well-formed UTF-8, and printable(printable(x)) == printable(x). A backslash is kept as
/// it is (escaping it would break that idempotence, which lets an escaped message be escaped again
/// by its printer, and would double every Windows path separator), so the result is for reading,
/// not for decoding: `\x1b` in it may be those four characters of the original. Threading: pure.
std::string printable(std::string_view text, usize maxBytes = ~usize{0});

} // namespace helios::tf

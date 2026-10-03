#pragma once
// Command bus (07 §1.2): every action is registered once as
// `{id: "doc.setProperty", args, canExecute, execute -> Transaction}` and the one registry drives
// menus, toolbars, shortcuts, the command palette, Luau, the remote-control socket, helios-tool
// and tests.
//
// Commands run through a CommandInvoker. The Framework owns one invoker per input path (Origin)
// and hands each path its own (the editor UI gets Ui, the helios-uitest injection path UiScripted,
// the Luau VM Luau, the RPC server Rpc, helios-tool Cli), so a transaction's origin is stamped
// from the input path, never from the command's arguments.
//
// Threading: the bus and the invokers belong to the Framework's owner thread.

#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "helios/core/result.h"
#include "helios/reflect/json.h"
#include "helios/toolsfw/types.h"

namespace helios::tf {

class Framework;
class TxBuilder;

enum class ArgType : u8 { Any, String, Number, Integer, Bool, Object, Array };

std::string_view argTypeName(ArgType type) noexcept;

/// One named argument of a command (arguments are a JSON object).
struct ArgDesc {
    std::string name;
    ArgType type = ArgType::Any;
    bool required = true;
    std::string doc;
};

class CommandContext;

struct CommandDesc {
    std::string id;       ///< "doc.setProperty": lowercase dotted namespace + camelCase verb.
    std::string label;    ///< Menu / palette text ("Set Property").
    std::string category; ///< Palette grouping ("Edit").
    std::string doc;      ///< One-paragraph help (tooltips, `helios-tool commands`, docs coverage).
    std::vector<ArgDesc> args;
    /// Default shortcut in edui's notation ("Ctrl+Shift+P"); empty for none.
    std::string shortcut;
    /// Intentionally reachable from the command palette only (the layout lint of 07 §4.4 accepts it
    /// without a menu, toolbar or context-menu entry).
    bool paletteOnly = false;
    /// Runs without EditorUI (helios-tool, the RPC server of a headless process). UI-only commands
    /// (themes, panels) set false.
    bool headless = true;
    /// Optional predicate (menus grey out, invoke() refuses). Arguments may be absent here.
    std::function<bool(const CommandContext&)> canExecute;
    /// Runs the command. Edits go through ctx.tx(); on success they commit as one transaction
    /// (joined to an open group), on failure they are rolled back.
    std::function<Result<void>(CommandContext&)> execute;
};

/// What a command sees while it runs.
class CommandContext {
public:
    Framework& framework() const noexcept { return *m_framework; }
    Origin origin() const noexcept { return m_origin; }
    const CommandDesc& desc() const noexcept { return *m_desc; }

    /// The arguments object (invalid JsonValue when the command was invoked without arguments).
    refl::JsonValue args() const noexcept { return m_args; }
    bool has(std::string_view name) const noexcept;
    Result<std::string> stringArg(std::string_view name) const;
    Result<f64> numberArg(std::string_view name) const;
    Result<i64> integerArg(std::string_view name) const;
    Result<bool> boolArg(std::string_view name) const;
    /// The argument's value as compact JSON text (for property values).
    Result<std::string> jsonArg(std::string_view name) const;
    std::optional<std::string> optionalString(std::string_view name) const;

    /// The transaction the command's edits go into. Only valid inside execute().
    TxBuilder& tx() const;
    void setLabel(std::string label);
    void setMergeKey(std::string key);
    /// Result value returned to RPC and Luau callers (compact JSON).
    void setResult(std::string json) { m_result = std::move(json); }
    const std::string& result() const noexcept { return m_result; }

private:
    friend class CommandInvoker;
    CommandContext(Framework& fw, Origin origin, const CommandDesc& desc, refl::JsonValue args, TxBuilder* tx) noexcept
        : m_framework(&fw), m_origin(origin), m_desc(&desc), m_args(args), m_tx(tx) {}
    Framework* m_framework;
    Origin m_origin;
    const CommandDesc* m_desc;
    refl::JsonValue m_args;
    TxBuilder* m_tx;
    std::string m_result;
};

/// The registry of commands.
class CommandBus {
public:
    CommandBus() = default;
    CommandBus(const CommandBus&) = delete;
    CommandBus& operator=(const CommandBus&) = delete;

    /// Registers a command. Fails with AlreadyExists for a duplicate id, InvalidArgument for an
    /// empty id or missing execute.
    Result<void> add(CommandDesc desc);
    /// Removes a command (gem unload). False when unknown.
    bool remove(std::string_view id);
    const CommandDesc* find(std::string_view id) const noexcept;
    /// All commands sorted by id.
    std::vector<const CommandDesc*> commands() const;
    usize size() const noexcept { return m_commands.size(); }

    /// Records that a menu, toolbar or context menu exposes `id` (layout lint, 07 §4.4). `where` is
    /// a readable location ("MainMenu/Edit").
    void markExposed(std::string_view id, std::string_view where);
    /// Menu/toolbar locations that expose `id` (empty when none).
    std::vector<std::string> exposures(std::string_view id) const;
    /// Commands that no menu, toolbar or context menu exposes and that are not paletteOnly.
    std::vector<std::string> unexposedCommands(bool headlessOnlyToo = false) const;

private:
    std::map<std::string, CommandDesc, std::less<>> m_commands;
    std::map<std::string, std::vector<std::string>, std::less<>> m_exposed;
};

/// Result of a successful invoke().
struct InvokeResult {
    std::string result;  ///< Compact JSON set by the command (empty = none).
    TxId tx;             ///< The committed transaction (null if the command made no edit or joined a group).
};

/// Runs commands for one input path; see the header comment.
class CommandInvoker {
public:
    CommandInvoker(Framework& framework, Origin origin) noexcept : m_framework(&framework), m_origin(origin) {}

    Origin origin() const noexcept { return m_origin; }
    /// Validates the arguments (a JSON object text, or empty) against the command's ArgDescs, runs
    /// it and commits its edits. NotFound for an unknown id, InvalidArgument for bad arguments,
    /// InvalidState when canExecute refuses; command errors are returned unchanged.
    Result<InvokeResult> invoke(std::string_view id, std::string_view argsJson = {});
    /// canExecute() with the given arguments (false for unknown commands).
    bool canExecute(std::string_view id, std::string_view argsJson = {}) const;

private:
    Framework* m_framework;
    Origin m_origin;
};

} // namespace helios::tf

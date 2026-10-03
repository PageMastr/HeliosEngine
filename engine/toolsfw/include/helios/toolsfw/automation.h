#pragma once
// Luau editor automation (07 §1.2 "Automation API"): a sandboxed editor VM (engine/script,
// HostProfile::Editor), separate from game VMs, whose only way to change documents is the command
// bus and transactions (origin `luau`; no side door, 07 §0 rule 3). Files are reachable only
// through the document API.
//
//   Editor.cmd(id: string, args: string?) -> string?     run a command, result JSON (or nil)
//   Editor.transaction(label: string, fn: () -> ())       group every edit of fn into one undo step
//   Editor.undo() / Editor.redo() -> boolean
//   Editor.find(query: string) -> {string}                document names containing query
//   Editor.log(message: string)
//   Record.get(doc: string, path: string) -> string       compact JSON of a value
//   Record.set(doc: string, path: string, json: string)   one-op transaction (doc.setProperty)
//   Validate.run(doc: string?) -> number                  error count (issues are logged)
//
// Threading: the Automation object and its VM belong to the Framework's owner thread.

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "helios/core/result.h"

namespace helios::script {
class ScriptVm;
}

namespace helios::tf {

class Framework;

struct AutomationResult {
    std::vector<std::string> log;  ///< print() and Editor.log() lines.
};

class Automation {
public:
    static Result<std::unique_ptr<Automation>> create(Framework& framework);
    ~Automation();
    Automation(const Automation&) = delete;
    Automation& operator=(const Automation&) = delete;

    /// Runs a script chunk to completion (it may not wait()). Errors come back with the Luau
    /// message; a group left open by a failing Editor.transaction is rolled back.
    Result<AutomationResult> run(std::string_view chunkName, std::string_view source);

private:
    explicit Automation(Framework& framework) noexcept;
    Framework* m_framework;
    std::unique_ptr<script::ScriptVm> m_vm;
    std::vector<std::string> m_log;
    u64 m_runs = 0;
    friend struct AutomationBindings;
};

} // namespace helios::tf

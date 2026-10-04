#include "helios/toolsfw/command.h"

#include <algorithm>
#include <format>

#include "helios/toolsfw/framework.h"
#include "helios/toolsfw/json_util.h"

#include "framework_internal.h"

namespace helios::tf {

std::string_view argTypeName(ArgType type) noexcept {
    switch (type) {
    case ArgType::Any: return "any";
    case ArgType::String: return "string";
    case ArgType::Number: return "number";
    case ArgType::Integer: return "integer";
    case ArgType::Bool: return "bool";
    case ArgType::Object: return "object";
    case ArgType::Array: return "array";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------------------------
// CommandContext
// ---------------------------------------------------------------------------------------------
bool CommandContext::has(std::string_view name) const noexcept {
    return m_args.isObject() && m_args.get(name).isValid();
}

Result<std::string> CommandContext::stringArg(std::string_view name) const {
    if (auto v = json::getString(m_args, name)) return std::string(*v);
    return Error{ErrorCode::InvalidArgument, std::format("{}: missing string argument '{}'", m_desc->id, name)};
}

std::optional<std::string> CommandContext::optionalString(std::string_view name) const {
    if (auto v = json::getString(m_args, name)) return std::string(*v);
    return std::nullopt;
}

Result<f64> CommandContext::numberArg(std::string_view name) const {
    if (auto v = json::getNumber(m_args, name)) return *v;
    return Error{ErrorCode::InvalidArgument, std::format("{}: missing number argument '{}'", m_desc->id, name)};
}

Result<i64> CommandContext::integerArg(std::string_view name) const {
    if (auto v = json::getInteger(m_args, name)) return *v;
    return Error{ErrorCode::InvalidArgument, std::format("{}: missing integer argument '{}'", m_desc->id, name)};
}

Result<bool> CommandContext::boolArg(std::string_view name) const {
    if (auto v = json::getBool(m_args, name)) return *v;
    return Error{ErrorCode::InvalidArgument, std::format("{}: missing bool argument '{}'", m_desc->id, name)};
}

Result<std::string> CommandContext::jsonArg(std::string_view name) const {
    if (!has(name)) return Error{ErrorCode::InvalidArgument, std::format("{}: missing argument '{}'", m_desc->id, name)};
    return json::compact(m_args.get(name));
}

TxBuilder& CommandContext::tx() const {
    HELIOS_ASSERT(m_tx != nullptr, "CommandContext::tx() outside execute()");
    return *m_tx;
}

void CommandContext::setLabel(std::string label) {
    if (m_tx && !m_framework->inGroup()) m_tx->setLabel(std::move(label));
}

void CommandContext::setMergeKey(std::string key) {
    if (m_tx && !m_framework->inGroup()) m_tx->setMergeKey(std::move(key));
}

// ---------------------------------------------------------------------------------------------
// CommandBus
// ---------------------------------------------------------------------------------------------
Result<void> CommandBus::add(CommandDesc desc) {
    if (desc.id.empty()) return Error{ErrorCode::InvalidArgument, "command id is empty"};
    if (!desc.execute) return Error{ErrorCode::InvalidArgument, std::format("command {} has no execute", desc.id)};
    if (m_commands.contains(desc.id)) return Error{ErrorCode::AlreadyExists, std::format("command {} already registered", desc.id)};
    if (desc.label.empty()) desc.label = desc.id;
    std::string id = desc.id;
    m_commands.emplace(std::move(id), std::move(desc));
    return {};
}

bool CommandBus::remove(std::string_view id) {
    const auto it = m_commands.find(id);
    if (it == m_commands.end()) return false;
    m_commands.erase(it);
    return true;
}

const CommandDesc* CommandBus::find(std::string_view id) const noexcept {
    const auto it = m_commands.find(id);
    return it == m_commands.end() ? nullptr : &it->second;
}

std::vector<const CommandDesc*> CommandBus::commands() const {
    std::vector<const CommandDesc*> out;
    out.reserve(m_commands.size());
    for (const auto& [id, desc] : m_commands) out.push_back(&desc);
    return out;
}

void CommandBus::markExposed(std::string_view id, std::string_view where) {
    auto& list = m_exposed[std::string(id)];
    if (std::find(list.begin(), list.end(), where) == list.end()) list.emplace_back(where);
}

std::vector<std::string> CommandBus::exposures(std::string_view id) const {
    const auto it = m_exposed.find(id);
    return it == m_exposed.end() ? std::vector<std::string>{} : it->second;
}

std::vector<std::string> CommandBus::unexposedCommands(bool headlessOnlyToo) const {
    std::vector<std::string> out;
    for (const auto& [id, desc] : m_commands) {
        if (desc.paletteOnly) continue;
        if (!headlessOnlyToo && desc.headless) continue;
        if (!m_exposed.contains(id)) out.push_back(id);
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// CommandInvoker
// ---------------------------------------------------------------------------------------------
namespace {

bool matches(ArgType type, refl::JsonValue v) {
    i64 i = 0;
    switch (type) {
    case ArgType::Any: return true;
    case ArgType::String: return v.isString();
    case ArgType::Number: return v.isNumber();
    case ArgType::Integer: return v.isNumber() && v.getI64(i);
    case ArgType::Bool: return v.isBool();
    case ArgType::Object: return v.isObject();
    case ArgType::Array: return v.isArray();
    }
    return false;
}

Result<void> validateArgs(const CommandDesc& desc, refl::JsonValue args) {
    for (const ArgDesc& a : desc.args) {
        const refl::JsonValue v = args.get(a.name);
        if (!v.isValid()) {
            if (a.required) return Error{ErrorCode::InvalidArgument, std::format("{}: missing argument '{}'", desc.id, a.name)};
            continue;
        }
        if (!matches(a.type, v)) {
            return Error{ErrorCode::InvalidArgument,
                         std::format("{}: argument '{}' must be {}, got {}", desc.id, a.name, argTypeName(a.type), v.typeName())};
        }
    }
    for (const auto& m : args.members()) {
        const bool known = std::any_of(desc.args.begin(), desc.args.end(), [&](const ArgDesc& a) { return a.name == m.key; });
        if (!known) return Error{ErrorCode::InvalidArgument, std::format("{}: unknown argument '{}'", desc.id, m.key)};
    }
    return {};
}

} // namespace

bool CommandInvoker::canExecute(std::string_view id, std::string_view argsJson) const {
    const CommandDesc* desc = m_framework->commands().find(id);
    if (!desc) return false;
    if (!desc->canExecute) return true;
    auto doc = json::parseObject(argsJson, id);
    if (!doc) return false;
    CommandContext ctx(*m_framework, m_origin, *desc, doc->root(), nullptr);
    return desc->canExecute(ctx);
}

Result<InvokeResult> CommandInvoker::invoke(std::string_view id, std::string_view argsJson) {
    const CommandDesc* desc = m_framework->commands().find(id);
    if (!desc) return Error{ErrorCode::NotFound, std::format("unknown command '{}'", id)};
    HELIOS_TRY_ASSIGN(const refl::JsonDocument doc, json::parseObject(argsJson, id));
    HELIOS_TRY(validateArgs(*desc, doc.root()));
    TxBuilder* group = detail::FwAccess::group(*m_framework);
    if (group && group->origin() != m_origin) {
        // The group commits with its own origin, so this input path's edits would be stamped
        // with another path's origin (07 §1.2). The caller retries once the group has closed.
        return Error{ErrorCode::InvalidState, std::format("{}: a {} transaction group is open; {} commands wait until it closes", id,
                                                          originName(group->origin()), originName(m_origin))};
    }
    std::unique_ptr<TxBuilder> local;
    TxBuilder* tx = group;
    if (!tx) {
        local = m_framework->begin(m_origin, desc->label);
        tx = local.get();
    }
    CommandContext ctx(*m_framework, m_origin, *desc, doc.root(), tx);
    if (desc->canExecute && !desc->canExecute(ctx)) {
        return Error{ErrorCode::InvalidState, std::format("{} cannot run now", desc->id)};
    }
    const usize mark = detail::FwAccess::opCount(*tx);
    const u64 lastBefore = m_framework->log().empty() ? 0 : m_framework->log().back().id.lamport;
    if (auto r = desc->execute(ctx); !r) {
        if (local) {
            local->abort();
        } else {
            detail::FwAccess::truncate(*tx, mark);
        }
        return std::move(r).error();
    }
    InvokeResult out;
    out.result = ctx.result();
    if (local) {
        HELIOS_TRY_ASSIGN(out.tx, local->commit());
        // Commands such as edit.undo or doc.revert commit their own transaction.
        if (out.tx.isNull() && !m_framework->log().empty() && m_framework->log().back().id.lamport > lastBefore) {
            out.tx = m_framework->log().back().id;
        }
    }
    return out;
}

} // namespace helios::tf

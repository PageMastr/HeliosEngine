#include "helios/toolsfw/automation.h"

#include <cctype>
#include <format>
#include <string>

#include "helios/reflect/path.h"
#include "helios/script/binding.h"
#include "helios/script/vm.h"
#include "helios/toolsfw/framework.h"
#include "helios/toolsfw/json_util.h"
#include "helios/toolsfw/validate.h"

namespace helios::tf {

using script::ScriptErrorCode;

struct AutomationBindings {
    static Automation& self(lua_State* L) { return *static_cast<Automation*>(script::bindingUserdata(L)); }
    static Framework& fw(lua_State* L) { return *self(L).m_framework; }

    static std::string checkString(lua_State* L, int idx) {
        size_t n = 0;
        const char* s = luaL_checklstring(L, idx, &n);
        return std::string(s, n);
    }

    [[noreturn]] static void fail(lua_State* L, const Error& e) {
        script::raiseError(L, ScriptErrorCode::HostError, e.toString());
    }

    static int cmd(lua_State* L) {
        const std::string id = checkString(L, 1);
        const std::string args = lua_isnoneornil(L, 2) ? std::string() : checkString(L, 2);
        auto r = fw(L).invoker(Origin::Luau).invoke(id, args);
        if (!r) fail(L, r.error());
        if (r->result.empty()) {
            lua_pushnil(L);
        } else {
            lua_pushlstring(L, r->result.data(), r->result.size());
        }
        return 1;
    }

    static int transaction(lua_State* L) {
        const std::string label = checkString(L, 1);
        luaL_checktype(L, 2, LUA_TFUNCTION);
        Framework& f = fw(L);
        f.beginGroup(Origin::Luau, label);
        lua_pushvalue(L, 2);
        const int status = script::callLuau(L, 0, 0);
        if (status != LUA_OK) {
            f.cancelGroup();
            lua_error(L);  // re-raise the callee's error object
        }
        auto tx = f.endGroup();
        if (!tx) fail(L, tx.error());
        return 0;
    }

    static int undoRedo(lua_State* L, bool redo) {
        auto r = fw(L).invoker(Origin::Luau).invoke(redo ? "edit.redo" : "edit.undo");
        lua_pushboolean(L, r.ok());
        return 1;
    }
    static int undo(lua_State* L) { return undoRedo(L, false); }
    static int redo(lua_State* L) { return undoRedo(L, true); }

    static int find(lua_State* L) {
        const std::string query = checkString(L, 1);
        lua_newtable(L);
        int i = 1;
        for (const Document* d : fw(L).documents().documents()) {
            if (d->destroyed() || d->name().find(query) == std::string::npos) continue;
            lua_pushlstring(L, d->name().data(), d->name().size());
            lua_rawseti(L, -2, i++);
        }
        return 1;
    }

    static int log(lua_State* L) {
        const std::string message = checkString(L, 1);
        HELIOS_LOG_INFO(LogTools, "[luau] {}", message);
        self(L).m_log.push_back(message);
        return 0;
    }

    static int recordGet(lua_State* L) {
        const std::string doc = checkString(L, 1);
        const std::string path = checkString(L, 2);
        const Document* d = fw(L).documents().find(doc);
        if (!d) fail(L, Error{ErrorCode::NotFound, std::format("no open document '{}'", doc)});
        auto v = refl::getJson(d->type(), d->object(), path);
        if (!v) fail(L, v.error());
        lua_pushlstring(L, v->data(), v->size());
        return 1;
    }

    static int recordSet(lua_State* L) {
        const std::string doc = checkString(L, 1);
        const std::string path = checkString(L, 2);
        const std::string value = checkString(L, 3);
        refl::JsonWriter w(refl::JsonStyle::Compact);
        w.beginObject();
        w.key("doc");
        w.string(doc);
        w.key("path");
        w.string(path);
        w.key("value");
        auto normalized = json::normalize(value);
        if (!normalized) fail(L, normalized.error());
        w.raw(*normalized);
        w.endObject();
        auto r = fw(L).invoker(Origin::Luau).invoke("doc.setProperty", w.take());
        if (!r) fail(L, r.error());
        return 0;
    }

    static int validateRun(lua_State* L) {
        std::string args;
        if (!lua_isnoneornil(L, 1)) args = "{\"doc\":" + json::quote(checkString(L, 1)) + "}";
        auto r = fw(L).invoker(Origin::Luau).invoke("validate.run", args);
        if (!r) fail(L, r.error());
        auto doc = refl::JsonDocument::parse(r->result);
        i64 errors = 0;
        if (doc) {
            errors = json::getInteger(doc->root(), "errors").value_or(0);
            for (refl::JsonValue issue : doc->root().get("issues").elements()) {
                HELIOS_LOG_WARN(LogTools, "[luau] {}: {}: {}", json::getString(issue, "file").value_or(""),
                                json::getString(issue, "path").value_or(""), json::getString(issue, "message").value_or(""));
            }
        }
        lua_pushnumber(L, static_cast<double>(errors));
        return 1;
    }
};

Result<std::unique_ptr<Automation>> Automation::create(Framework& framework) {
    std::unique_ptr<Automation> a(new Automation(framework));
    script::VmConfig cfg;
    cfg.name = "editor";
    cfg.profile = script::HostProfile::Editor;
    cfg.heapLimitBytes = 256u << 20;
    // Batch automation (validation sweeps, bulk edits) runs far longer than a game callback: no
    // fuel limits, a 30 s wall kill and a 60 s backstop against runaway loops.
    script::FuelBudget budget;
    budget.fuelPerResume = 0;
    budget.fuelKill = 0;
    budget.fuelPerTick = 0;
    budget.wallSoftNanos = 0;
    budget.wallKillNanos = 30'000'000'000ull;
    budget.wallPerTickNanos = 0;
    budget.wallBackstopNanos = 60'000'000'000ull;
    cfg.budget = budget;
    Automation* self = a.get();
    cfg.onPrint = [self](std::string_view, std::string_view text) { self->m_log.emplace_back(text); };
    HELIOS_TRY_ASSIGN(a->m_vm, script::ScriptVm::create(cfg, [self](script::Binder& b) {
        void* ud = self;
        b.function("Editor", "cmd", &AutomationBindings::cmd, {10}, ud);
        b.function("Editor", "transaction", &AutomationBindings::transaction, {10}, ud);
        b.function("Editor", "undo", &AutomationBindings::undo, {10}, ud);
        b.function("Editor", "redo", &AutomationBindings::redo, {10}, ud);
        b.function("Editor", "find", &AutomationBindings::find, {10}, ud);
        b.function("Editor", "log", &AutomationBindings::log, {5}, ud);
        b.function("Record", "get", &AutomationBindings::recordGet, {5}, ud);
        b.function("Record", "set", &AutomationBindings::recordSet, {10}, ud);
        b.function("Validate", "run", &AutomationBindings::validateRun, {20}, ud);
    }));
    return a;
}

Automation::Automation(Framework& framework) noexcept : m_framework(&framework) {}

Automation::~Automation() = default;

Result<AutomationResult> Automation::run(std::string_view chunkName, std::string_view source) {
    m_log.clear();
    std::string name = std::format("run{}_", ++m_runs);
    for (char c : chunkName) name.push_back((std::isalnum(static_cast<unsigned char>(c)) != 0) ? c : '_');
    HELIOS_TRY(m_vm->loadModule(name, source));
    auto r = m_vm->instantiateModule(name);
    if (m_framework->inGroup()) m_framework->cancelGroup();
    if (!r) {
        const script::ScriptError& e = m_vm->lastError();
        return Error{r.error().code, e.isError() ? e.toString() : r.error().message};
    }
    AutomationResult out;
    out.log = std::move(m_log);
    m_log.clear();
    return out;
}

} // namespace helios::tf

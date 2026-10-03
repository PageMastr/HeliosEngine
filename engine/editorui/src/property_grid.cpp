#include "helios/editorui/property_grid.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <vector>

#include "helios/editorui/localize.h"
#include "helios/editorui/ui_test.h"
#include "helios/reflect/json.h"
#include "helios/reflect/path.h"
#include "helios/reflect/serialize.h"
#include "helios/toolsfw/framework.h"
#include "helios/toolsfw/json_util.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "misc/cpp/imgui_stdlib.h"

namespace helios::edui {

using refl::FieldInfo;
using refl::Kind;
using refl::PropertyPath;
using refl::TypeInfo;

namespace {

std::string f64Json(f64 v) {
    char buf[40];
    return std::string(buf, refl::formatJsonF64(v, buf));
}

std::string f32Json(f32 v) {
    char buf[40];
    return std::string(buf, refl::formatJsonF32(v, buf));
}

/// SameLine() when an item `width` pixels wide still fits in the cell, else a new line (the
/// layout lint flags items that leave their window).
void sameLineIfFits(f32 width) {
    ImGui::SameLine();
    if (ImGui::GetContentRegionAvail().x < width) ImGui::NewLine();
}

f32 smallButtonWidth(const char* label) {
    return ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f;
}

/// Row scope for the item table: "row[<path>]".
struct RowScope {
    explicit RowScope(const std::string& path) {
        UiTest::pushScope("row[" + path + "]");
        ImGui::PushID(path.c_str());
    }
    ~RowScope() {
        ImGui::PopID();
        UiTest::popScope();
    }
    RowScope(const RowScope&) = delete;
    RowScope& operator=(const RowScope&) = delete;
};

bool isTuple(const TypeInfo& t) {
    return t.kind == Kind::Builtin && t.hasFlag(refl::TypeFlags::Tuple) && !t.fields.empty() && t.fields.size() <= 4 &&
           std::all_of(t.fields.begin(), t.fields.end(), [](const FieldInfo& f) { return f.type().kind == Kind::F32; });
}

bool isInline(const TypeInfo& t) {
    return refl::isScalarKind(t.kind) || t.kind == Kind::String || t.kind == Kind::Name || t.kind == Kind::Enum ||
           t.kind == Kind::Flags || t.kind == Kind::Set || (t.kind == Kind::Builtin && (isTuple(t) || t.fields.empty()));
}

} // namespace

struct PropertyGrid::Impl {
    PropertyGrid& grid;
    tf::Framework& fw;
    tf::CommandInvoker& invoker;
    const tf::Document& doc;
    std::string docKey;

    // ---- commits -------------------------------------------------------------------------------
    void commit(std::string_view command, std::string args) {
        auto r = invoker.invoke(command, args);
        grid.m_error = r ? std::string() : r.error().message;
    }
    std::string argsHead(const std::string& path) const {
        return std::format(R"({{"doc":{},"path":{})", tf::json::quote(docKey), tf::json::quote(path));
    }
    void set(const std::string& path, const std::string& json, bool continuous = false) {
        std::string args = argsHead(path) + ",\"value\":" + json;
        if (continuous) {
            // One undo step per gesture: every commit of the drag shares a merge key.
            const ImGuiID id = ImGui::GetItemID();
            if (grid.m_activeId != id) {
                grid.m_activeId = id;
                ++grid.m_gesture;
            }
            args += ",\"mergeKey\":" + tf::json::quote(std::format("grid:{}:{}:{}", docKey, path, grid.m_gesture));
        }
        commit("doc.setProperty", args + "}");
    }
    void endGesture() {
        if (ImGui::IsItemDeactivated() && grid.m_activeId == ImGui::GetItemID()) grid.m_activeId = 0;
    }
    void insert(const std::string& path) { commit("doc.insertElement", argsHead(path) + "}"); }
    void remove(const std::string& path) { commit("doc.remove", argsHead(path) + "}"); }
    void move(const std::string& path, u64 to) { commit("doc.moveElement", argsHead(path) + std::format(",\"to\":{}}}", to)); }

    // ---- rows ----------------------------------------------------------------------------------
    static std::string tooltipFor(const FieldInfo* f, const TypeInfo& t) {
        std::string tip;
        if (f) {
            tip = std::string(f->name);
            if (!f->doc.empty()) tip += "\n" + std::string(f->doc);
            if (const auto* u = f->attr<refl::attrs::Unit>()) tip += std::format("\nUnit: {}", u->unit);
            if (const auto* r = f->attr<refl::attrs::Range>()) tip += std::format("\nRange: {} .. {}", f64Json(r->min), f64Json(r->max));
            if (refl::hasFlag(f->flags, refl::FieldFlags::ServerOnly)) tip += "\nServer only: never sent to clients";
            if (refl::hasFlag(f->flags, refl::FieldFlags::ClientOnly)) tip += "\nClient only";
        }
        tip += std::format("\nType: {}", t.qualifiedName);
        return tip;
    }

    /// Label cell. Returns true when a tree node is open (compound values).
    bool nameCell(const char* label, const std::string& tooltip, const FieldInfo* f, bool compound, bool defaultOpen) {
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        bool open = false;
        if (compound) {
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding;
            if (defaultOpen) flags |= ImGuiTreeNodeFlags_DefaultOpen;
            open = ImGui::TreeNodeEx("##name", flags, "%s", tr(label));
        } else {
            ImGui::TreeNodeEx("##name", ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth |
                                            ImGuiTreeNodeFlags_FramePadding, "%s", tr(label));
        }
        UiTest::setSegment("name");
        UiTest::annotate({}, tooltip);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", tooltip.c_str());
        if (ImGui::BeginPopupContextItem("##nameMenu")) {
            if (ImGui::MenuItem(tr("Copy Path"))) ImGui::SetClipboardText(currentPath.c_str());
            ImGui::EndPopup();
        }
        if (f) {
            const bool server = refl::hasFlag(f->flags, refl::FieldFlags::ServerOnly);
            const bool client = refl::hasFlag(f->flags, refl::FieldFlags::ClientOnly);
            if (server || client) {
                ImGui::SameLine();
                ImGui::TextColored(ImGui::GetStyleColorVec4(server ? ImGuiCol_PlotHistogram : ImGuiCol_CheckMark), "%s",
                                   tr(server ? "server" : "client"));
            }
        }
        return open;
    }

    std::string currentPath;

    void row(const TypeInfo& t, const void* v, const FieldInfo* f, const std::string& path, const char* label,
             const std::function<void()>& extra = {}) {
        RowScope scope(path);
        currentPath = path;
        ImGui::TableNextRow();
        const bool readOnly = f && refl::hasFlag(f->flags, refl::FieldFlags::ReadOnly);
        const std::string tooltip = tooltipFor(f, t);
        // Optionals: a check box engages them; the contained value is edited in place.
        if (t.kind == Kind::Optional) {
            const bool has = t.ops->has(v);
            const TypeInfo& e = t.element();
            const bool compound = has && !isInline(e);
            const bool open = nameCell(label, tooltip, f, compound, false);
            ImGui::TableSetColumnIndex(1);
            if (readOnly) ImGui::BeginDisabled();
            bool engaged = has;
            if (ImGui::Checkbox("##set", &engaged)) {
                if (engaged) {
                    refl::Value def(e);
                    set(path, refl::toJson(e, def.data(), refl::JsonStyle::Compact));
                } else {
                    set(path, "null");
                }
            }
            UiTest::annotate(has ? "set" : "unset", tr("Set or clear the optional value"));
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", tr("Set or clear the optional value"));
            if (has && isInline(e)) {
                ImGui::SameLine();
                inlineEditor(e, t.ops->get(const_cast<void*>(v)), f, path);
            }
            if (readOnly) ImGui::EndDisabled();
            if (open) {
                children(e, t.ops->get(const_cast<void*>(v)), path);
                ImGui::TreePop();
            }
            return;
        }
        const bool compound = !isInline(t);
        const bool open = nameCell(label, tooltip, f, compound, t.kind == Kind::Struct && path.find('/') == std::string::npos);
        ImGui::TableSetColumnIndex(1);
        if (readOnly) ImGui::BeginDisabled();
        if (compound) {
            summary(t, v, path);
        } else {
            inlineEditor(t, v, f, path);
        }
        if (extra) extra();
        if (readOnly) ImGui::EndDisabled();
        if (open) {
            children(t, v, path);
            ImGui::TreePop();
        }
    }

    /// Value cell of a compound row: element counts, add buttons, variant selection.
    void summary(const TypeInfo& t, const void* v, const std::string& path) {
        switch (t.kind) {
        case Kind::List:
        case Kind::KeyedList:
        case Kind::Array: {
            const usize n = t.ops->size(v);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", std::format("{} {}", n, tr(n == 1 ? "item" : "items")).c_str());
            if (t.kind != Kind::Array) {
                const char* add = tr("+ Add##add");
                sameLineIfFits(smallButtonWidth(add));
                if (ImGui::SmallButton(add)) insert(path);
                UiTest::annotate({}, tr("Add an element"));
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", tr("Add an element"));
            }
            break;
        }
        case Kind::Map: {
            ImGui::AlignTextToFramePadding();
            usize n = 0;
            t.ops->forEach(v, &n, [](void* user, const void*, const void*) { ++*static_cast<usize*>(user); });
            ImGui::TextDisabled("%s", std::format("{} {}", n, tr(n == 1 ? "entry" : "entries")).c_str());
            const char* add = tr("+ Add##add");
            sameLineIfFits(smallButtonWidth(add));
            if (ImGui::SmallButton(add)) {
                grid.m_newKey.clear();
                ImGui::OpenPopup("##newKey");
            }
            UiTest::annotate({}, tr("Add an entry"));
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", tr("Add an entry"));
            if (ImGui::BeginPopup("##newKey")) {
                ImGui::TextUnformatted(tr("Key"));
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
                if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
                const bool enter = ImGui::InputText("##key", &grid.m_newKey, ImGuiInputTextFlags_EnterReturnsTrue);
                UiTest::annotate(grid.m_newKey, tr("New key"));
                if ((ImGui::Button(tr("Add##confirm")) || enter) && !grid.m_newKey.empty()) {
                    refl::Value def(t.element());
                    set(PropertyPath::parse(path).value().withKey(grid.m_newKey).toString(),
                        refl::toJson(t.element(), def.data(), refl::JsonStyle::Compact));
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            break;
        }
        case Kind::Variant: {
            const u32 index = t.ops->index(v);
            const std::string_view active = index < t.alternatives.size() ? t.alternatives[index].name : std::string_view("?");
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##value", std::string(active).c_str())) {
                for (const refl::VariantAlt& alt : t.alternatives) {
                    const bool selected = alt.name == active;
                    if (ImGui::Selectable(std::string(alt.name).c_str(), selected) && !selected) {
                        set(path, std::format("{{{}:{{}}}}", tf::json::quote(alt.name)));
                    }
                }
                ImGui::EndCombo();
            }
            UiTest::annotate(active, tr("Variant alternative"));
            break;
        }
        default: ImGui::TextDisabled("%s", ""); break;
        }
    }

    void children(const TypeInfo& t, const void* v, const std::string& path) {
        const PropertyPath base = PropertyPath::parse(path).valueOr(PropertyPath());
        switch (t.kind) {
        case Kind::Struct:
        case Kind::Builtin:
            for (const FieldInfo& f : t.fields) {
                if (refl::hasFlag(f.flags, refl::FieldFlags::Hidden)) continue;
                row(f.type(), f.ptr(v), &f, base.withField(f.name).toString(), std::string(f.name).c_str());
            }
            break;
        case Kind::List:
        case Kind::KeyedList:
        case Kind::Array: {
            const usize n = t.ops->size(v);
            const TypeInfo& e = t.element();
            const FieldInfo* keyField = nullptr;
            // @keyed(field) lists name elements by their key field.
            std::string_view byField;
            if (t.kind == Kind::List) {
                auto ref = refl::resolve(doc.type(), doc.object(), base);
                if (ref && ref->field) {
                    if (const auto* k = ref->field->attr<refl::attrs::Keyed>()) byField = k->field;
                }
                if (!byField.empty()) keyField = e.field(byField);
            }
            for (usize i = 0; i < n; ++i) {
                const void* ev = t.ops->element(const_cast<void*>(v), i);
                std::string label;
                std::string elemPath;
                if (t.kind == Kind::KeyedList) {
                    const std::string key = refl::keyedKeyText(t.ops->keyAt(v, i));
                    label = "#" + key.substr(0, 8);
                    elemPath = base.withKeyed(key).toString();
                } else if (keyField && keyField->type().ops->keyToText) {
                    const std::string key = keyField->type().ops->keyToText(keyField->ptr(ev));
                    label = key;
                    elemPath = base.withKeyed(key).toString();
                } else {
                    label = std::format("[{}]", i);
                    elemPath = base.withIndex(i).toString();
                }
                const bool mutableList = t.kind != Kind::Array;
                row(e, ev, nullptr, elemPath, label.c_str(), [&, i, n, elemPath, mutableList] {
                    if (!mutableList) return;
                    ImGui::SameLine();
                    if (ImGui::SmallButton(tr("-##remove"))) remove(elemPath);
                    UiTest::annotate({}, tr("Remove element"));
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", tr("Remove element"));
                    if (i > 0) {
                        ImGui::SameLine();
                        if (ImGui::SmallButton(tr("Up##up"))) move(elemPath, i - 1);
                        UiTest::annotate({}, tr("Move element up"));
                    }
                    if (i + 1 < n) {
                        ImGui::SameLine();
                        if (ImGui::SmallButton(tr("Down##down"))) move(elemPath, i + 1);
                        UiTest::annotate({}, tr("Move element down"));
                    }
                });
            }
            break;
        }
        case Kind::Map: {
            struct Entry {
                std::string key;
                const void* value;
            };
            std::vector<Entry> entries;
            const TypeInfo& kt = t.key();
            struct Ctx {
                std::vector<Entry>* out;
                const TypeInfo* kt;
            } ctx{&entries, &kt};
            t.ops->forEach(v, &ctx, [](void* user, const void* key, const void* value) {
                auto* c = static_cast<Ctx*>(user);
                c->out->push_back({c->kt->ops->keyToText ? c->kt->ops->keyToText(key) : std::string("?"), value});
            });
            // Canonical order: by key text (std::map<Name, ...> iterates in interning order).
            std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.key < b.key; });
            for (const Entry& en : entries) {
                const std::string elemPath = base.withKey(en.key).toString();
                row(t.element(), en.value, nullptr, elemPath, en.key.c_str(), [&, elemPath] {
                    ImGui::SameLine();
                    if (ImGui::SmallButton(tr("-##remove"))) remove(elemPath);
                    UiTest::annotate({}, tr("Remove entry"));
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", tr("Remove entry"));
                });
            }
            break;
        }
        case Kind::Variant: {
            const u32 index = t.ops->index(v);
            if (index < t.alternatives.size()) {
                const refl::VariantAlt& alt = t.alternatives[index];
                children(alt.type(), t.ops->alt(const_cast<void*>(v)), base.withField(alt.name).toString());
            }
            break;
        }
        default: break;
        }
    }

    /// Editors of values that fit in one cell.
    void inlineEditor(const TypeInfo& t, const void* v, const FieldInfo* f, const std::string& path) {
        const std::string current = refl::toJson(t, v, refl::JsonStyle::Compact);
        const std::string tooltip = f ? tooltipFor(f, t) : std::string(t.qualifiedName);
        ImGui::SetNextItemWidth(-FLT_MIN);
        const refl::attrs::Range* range = f ? f->attr<refl::attrs::Range>() : nullptr;
        const refl::attrs::Unit* unit = f ? f->attr<refl::attrs::Unit>() : nullptr;
        const std::string unitSuffix = unit ? " " + std::string(unit->unit) : std::string();
        switch (t.kind) {
        case Kind::Bool: {
            bool b = *static_cast<const bool*>(v);
            if (ImGui::Checkbox("##value", &b)) set(path, b ? "true" : "false");
            break;
        }
        case Kind::F32: {
            f32 x = *static_cast<const f32*>(v);
            const f32 lo = range ? static_cast<f32>(range->min) : 0.0f;
            const f32 hi = range ? static_cast<f32>(range->max) : 0.0f;
            const f32 speed = std::max(0.01f, std::abs(x) * 0.005f);
            const std::string fmt = "%.6g" + unitSuffix;
            if (ImGui::DragFloat("##value", &x, speed, lo, hi, fmt.c_str(), range ? ImGuiSliderFlags_AlwaysClamp : 0)) {
                set(path, f32Json(x), true);
            }
            endGesture();
            break;
        }
        case Kind::F64: {
            f64 x = *static_cast<const f64*>(v);
            const f64 lo = range ? range->min : 0.0;
            const f64 hi = range ? range->max : 0.0;
            const std::string fmt = "%.6g" + unitSuffix;
            if (ImGui::DragScalar("##value", ImGuiDataType_Double, &x, static_cast<f32>(std::max(0.01, std::abs(x) * 0.005)),
                                  range ? &lo : nullptr, range ? &hi : nullptr, fmt.c_str(), range ? ImGuiSliderFlags_AlwaysClamp : 0)) {
                set(path, f64Json(x), true);
            }
            endGesture();
            break;
        }
        case Kind::I8:
        case Kind::I16:
        case Kind::I32:
        case Kind::I64:
        case Kind::U8:
        case Kind::U16:
        case Kind::U32:
        case Kind::U64: {
            const bool sign = refl::isSignedKind(t.kind);
            i64 s = refl::readIntegerBits(t, v);
            u64 u = static_cast<u64>(s);
            const ImGuiDataType dt = sign ? ImGuiDataType_S64 : ImGuiDataType_U64;
            const std::string fmt = (sign ? "%lld" : "%llu") + unitSuffix;
            // Type limits keep a drag from wrapping small integers.
            const u32 bits = static_cast<u32>(t.size * 8);
            i64 smin = bits >= 64 ? INT64_MIN : -(i64{1} << (bits - 1));
            i64 smax = bits >= 64 ? INT64_MAX : (i64{1} << (bits - 1)) - 1;
            u64 umin = 0;
            u64 umax = bits >= 64 ? UINT64_MAX : (u64{1} << bits) - 1;
            if (range) {
                smin = std::max(smin, static_cast<i64>(range->min));
                smax = std::min(smax, static_cast<i64>(range->max));
                umin = std::max<u64>(umin, static_cast<u64>(std::max(0.0, range->min)));
                umax = std::min<u64>(umax, static_cast<u64>(std::max(0.0, range->max)));
            }
            const bool changed = sign ? ImGui::DragScalar("##value", dt, &s, 0.25f, &smin, &smax, fmt.c_str(), ImGuiSliderFlags_AlwaysClamp)
                                      : ImGui::DragScalar("##value", dt, &u, 0.25f, &umin, &umax, fmt.c_str(), ImGuiSliderFlags_AlwaysClamp);
            if (changed) set(path, sign ? std::to_string(s) : std::to_string(u), true);
            endGesture();
            break;
        }
        case Kind::Enum: {
            const i64 value = refl::readIntegerBits(t, v);
            const refl::EnumValue* cur = t.enumByValue(value);
            const std::string preview = cur ? std::string(cur->name) : std::to_string(value);
            if (ImGui::BeginCombo("##value", preview.c_str())) {
                for (const refl::EnumValue& ev : t.enumValues) {
                    const bool selected = ev.value == value;
                    if (ImGui::Selectable(std::string(ev.name).c_str(), selected) && !selected) set(path, tf::json::quote(ev.name));
                    if (!ev.doc.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", std::string(ev.doc).c_str());
                }
                ImGui::EndCombo();
            }
            break;
        }
        case Kind::Flags: {
            const u64 bits = static_cast<u64>(refl::readIntegerBits(t, v));
            bool first = true;
            for (const refl::EnumValue& ev : t.enumValues) {
                const u64 bit = static_cast<u64>(ev.value);
                if (bit == 0 || (bit & (bit - 1)) != 0) continue;
                const std::string flagLabel(ev.name);
                if (!first) {
                    sameLineIfFits(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x +
                                   ImGui::CalcTextSize(flagLabel.c_str()).x);
                }
                first = false;
                bool on = (bits & bit) != 0;
                ImGui::PushID(static_cast<int>(bit));
                if (ImGui::Checkbox(flagLabel.c_str(), &on)) set(path, std::to_string(on ? (bits | bit) : (bits & ~bit)));
                UiTest::annotate(on ? "true" : "false", ev.doc.empty() ? std::string_view(ev.name) : ev.doc);
                ImGui::PopID();
            }
            return;  // one item per flag; annotated above
        }
        case Kind::String:
        case Kind::Name: {
            std::string text = t.kind == Kind::String ? *static_cast<const std::string*>(v) : std::string(static_cast<const Name*>(v)->view());
            ImGui::InputText("##value", &text);
            if (ImGui::IsItemDeactivatedAfterEdit()) set(path, tf::json::quote(text));
            break;
        }
        default: {
            if (isTuple(t)) {
                float c[4] = {0, 0, 0, 0};
                const usize n = t.fields.size();
                for (usize i = 0; i < n; ++i) c[i] = *static_cast<const f32*>(t.fields[i].ptr(v));
                bool changed = false;
                switch (n) {
                case 2: changed = ImGui::DragFloat2("##value", c, 0.01f); break;
                case 3: changed = ImGui::DragFloat3("##value", c, 0.01f); break;
                default: changed = ImGui::DragFloat4("##value", c, 0.01f); break;
                }
                if (changed) {
                    std::string json = "[";
                    for (usize i = 0; i < n; ++i) json += (i ? "," : "") + f32Json(c[i]);
                    set(path, json + "]", true);
                }
                endGesture();
                break;
            }
            // Vocabulary values (LocString, AssetRef, RecordRef, Guid, Duration, sets, ...): their
            // JSON text; strings are edited without quotes.
            const bool quoted = current.size() >= 2 && current.front() == '"';
            std::string text = current;
            if (quoted) {
                auto doc = refl::JsonDocument::parse(current);
                if (doc) text = std::string(doc->root().asString());
            }
            ImGui::InputText("##value", &text);
            if (ImGui::IsItemDeactivatedAfterEdit()) set(path, quoted ? tf::json::quote(text) : text);
            break;
        }
        }
        UiTest::annotate(current, tooltip);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s", tooltip.c_str());
    }
};

void PropertyGrid::draw(tf::Framework& framework, tf::CommandInvoker& invoker, const tf::Document& doc) {
    Impl impl{*this, framework, invoker, doc, doc.id().toString(), {}};
    UiTest::pushScope("Grid");
    // Header: record identity. $name is editable (doc.rename); $rid never changes.
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(tr("Record"));
    ImGui::SameLine();
    std::string name = doc.name();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    ImGui::InputText("##recordName", &name);
    UiTest::annotate(doc.name(), tr("Record name ($name)"));
    if (ImGui::IsItemDeactivatedAfterEdit() && name != doc.name() && !name.empty()) {
        auto r = invoker.invoke("doc.rename", std::format(R"({{"doc":{},"name":{}}})", tf::json::quote(impl.docKey), tf::json::quote(name)));
        m_error = r ? std::string() : r.error().message;
    }
    // Type and file, wrapped to the panel width (a narrow Inspector must not clip them).
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", std::format("{}  {}{}", doc.type().qualifiedName, doc.relativePath(), doc.dirty() ? "  *" : "").c_str());
    ImGui::PopStyleColor();
    if (!m_error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_PlotHistogramHovered));
        ImGui::TextWrapped("%s", m_error.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Separator();

    // The name column fits the longest top-level label (the layout lint checks clipping).
    f32 nameWidth = ImGui::CalcTextSize(tr("Property")).x;
    const ImGuiStyle& style = ImGui::GetStyle();
    for (const FieldInfo& f : doc.type().fields) {
        f32 w = ImGui::CalcTextSize(tr(std::string(f.name).c_str())).x;
        const bool server = refl::hasFlag(f.flags, refl::FieldFlags::ServerOnly);
        const bool client = refl::hasFlag(f.flags, refl::FieldFlags::ClientOnly);
        if (server || client) w += style.ItemSpacing.x + ImGui::CalcTextSize(tr(server ? "server" : "client")).x;
        nameWidth = std::max(nameWidth, w);
    }
    // Tree arrow, frame padding and two levels of nesting.
    nameWidth += ImGui::GetFontSize() + style.FramePadding.x * 4 + style.IndentSpacing * 2;
    const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable("##grid", 2, flags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Property"), ImGuiTableColumnFlags_WidthFixed, nameWidth);
        ImGui::TableSetupColumn(tr("Value"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const FieldInfo& f : doc.type().fields) {
            if (refl::hasFlag(f.flags, refl::FieldFlags::Hidden)) continue;
            impl.row(f.type(), f.ptr(doc.object()), &f, std::string(f.name), std::string(f.name).c_str());
        }
        ImGui::EndTable();
    }
    UiTest::popScope();
}

} // namespace helios::edui

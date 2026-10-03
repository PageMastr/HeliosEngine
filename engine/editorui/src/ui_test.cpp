#include "helios/editorui/ui_test.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <unordered_map>

#include "helios/editorui/theme.h"
#include "helios/reflect/json.h"
#include "helios/toolsfw/command.h"
#include "helios/toolsfw/json_util.h"

#include "imgui_item_hooks.h"

namespace helios::edui {

f32 UiRect::overlap(const UiRect& o) const noexcept {
    const f32 ow = std::min(right(), o.right()) - std::max(x, o.x);
    const f32 oh = std::min(bottom(), o.bottom()) - std::max(y, o.y);
    return (ow > 0 && oh > 0) ? ow * oh : 0.0f;
}

// ---------------------------------------------------------------------------------------------
// Keys
// ---------------------------------------------------------------------------------------------
namespace {
constexpr std::string_view kKeyNames[] = {
    "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z",
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "f10", "f11", "f12",
    "enter", "escape", "tab", "backspace", "delete", "space", "up", "down", "left", "right", "home", "end", "pageup", "pagedown",
    "ctrl", "shift", "alt", "super"};

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}
} // namespace

std::vector<std::string_view> uiKeyNames() {
    return {std::begin(kKeyNames), std::end(kKeyNames)};
}

Result<std::pair<u32, std::string>> parseChord(std::string_view chord) {
    u32 mods = 0;
    std::string key;
    std::string_view rest = chord;
    while (!rest.empty()) {
        const usize plus = rest.find('+', 1);  // "+" alone is not a key we support
        const std::string part = lower(rest.substr(0, plus));
        rest = plus == std::string_view::npos ? std::string_view() : rest.substr(plus + 1);
        if (part == "ctrl" || part == "control") {
            mods |= kModCtrl;
        } else if (part == "shift") {
            mods |= kModShift;
        } else if (part == "alt") {
            mods |= kModAlt;
        } else if (part == "super" || part == "cmd" || part == "win") {
            mods |= kModSuper;
        } else if (part == "esc") {
            key = "escape";
        } else if (part == "return") {
            key = "enter";
        } else if (part == "del") {
            key = "delete";
        } else {
            key = part;
        }
    }
    if (chord.size() > 1 && chord.back() == '+') return Error{ErrorCode::InvalidArgument, std::format("chord '{}' ends with '+'", chord)};
    if (key.empty()) {
        // A lone modifier ("ctrl") is a key too.
        if (mods == kModCtrl) return std::pair<u32, std::string>{0, "ctrl"};
        if (mods == kModShift) return std::pair<u32, std::string>{0, "shift"};
        if (mods == kModAlt) return std::pair<u32, std::string>{0, "alt"};
        if (mods == kModSuper) return std::pair<u32, std::string>{0, "super"};
        return Error{ErrorCode::InvalidArgument, std::format("chord '{}' has no key", chord)};
    }
    if (std::find(std::begin(kKeyNames), std::end(kKeyNames), key) == std::end(kKeyNames)) {
        return Error{ErrorCode::InvalidArgument, std::format("unknown key '{}' in '{}'", key, chord)};
    }
    return std::pair<u32, std::string>{mods, key};
}

// ---------------------------------------------------------------------------------------------
// Item table
// ---------------------------------------------------------------------------------------------
struct UiTest::Impl {
    struct Pending {
        UiItem item;
        ImGuiID windowId = 0;
        std::string scope;
        std::string segment;
        bool fixedSegment = false;  ///< Set by setSegment(); ItemInfo does not override it.
    };
    UiTest* owner = nullptr;
    HeliosImGuiItemHooks hooks{};
    std::vector<Pending> building;
    std::unordered_map<ImGuiID, usize> byId;
    std::unordered_map<ImGuiID, std::string> lastPaths;
    std::unordered_map<ImGuiID, std::string> windowPaths;
    std::unordered_map<ImGuiID, std::string> popupNames;
    std::vector<std::string> scopes;
    std::optional<CaptureRequest> capture;
    std::optional<Result<std::string>> captureResult;

    static Impl* current() {
        ImGuiContext* ctx = ImGui::GetCurrentContext();
        if (!ctx || !ctx->TestEngineHookItems || !ctx->TestEngine) return nullptr;
        return static_cast<Impl*>(static_cast<HeliosImGuiItemHooks*>(ctx->TestEngine)->user);
    }

    /// Path segment of a label: the id after "###" or "##" when there is one (stable under
    /// translation and pseudo-localization), else the visible text.
    static std::string displayName(std::string_view name) {
        if (const usize p = name.find("###"); p != std::string_view::npos) return std::string(name.substr(p + 3));
        const usize h = name.find("##");
        if (h == std::string_view::npos) return std::string(name);
        if (h + 2 < name.size()) return std::string(name.substr(h + 2));
        return std::string(name.substr(0, h));
    }

    std::string openerPath(ImGuiID id) const {
        if (const auto it = popupNames.find(id); it != popupNames.end()) return it->second;
        if (const auto it = byId.find(id); it != byId.end()) {
            const Pending& p = building[it->second];
            return p.item.window + (p.scope.empty() ? "" : "/" + p.scope) + "/" + (p.segment.empty() ? std::format("#{:08x}", id) : p.segment);
        }
        if (const auto it = lastPaths.find(id); it != lastPaths.end()) return it->second;
        return {};
    }

    std::string windowPath(ImGuiContext& g, ImGuiWindow* w) {
        if (const auto it = windowPaths.find(w->ID); it != windowPaths.end()) return it->second;
        std::string path;
        const std::string_view name = w->Name;
        if (name == "##MainMenuBar") {
            path = "MainMenu";
        } else if (w->Flags & ImGuiWindowFlags_Tooltip) {
            path = "Tooltip";
        } else if (w->Flags & ImGuiWindowFlags_Popup) {
            for (const ImGuiPopupData& p : g.OpenPopupStack) {
                if (p.Window == w) path = openerPath(p.PopupId);
            }
            if (path.empty()) path = "Popup/" + displayName(name);
        } else if ((w->Flags & ImGuiWindowFlags_ChildWindow) && w->ParentWindow && !w->DockIsActive) {
            const std::string parent = windowPath(g, w->ParentWindow);
            std::string_view child = name;
            const std::string_view parentName = w->ParentWindow->Name;
            if (child.starts_with(parentName) && child.size() > parentName.size() && child[parentName.size()] == '/') {
                child.remove_prefix(parentName.size() + 1);
            }
            // "<label>_<8 hex id>" or "<8 hex id>".
            if (child.size() >= 9 && child[child.size() - 9] == '_') child.remove_suffix(9);
            path = parent + "/" + (child.empty() ? std::string("child") : displayName(child));
        } else if (name.starts_with("WindowOverViewport")) {
            path = "Dock";
        } else {
            path = displayName(name);
        }
        windowPaths.emplace(w->ID, path);
        return path;
    }

    void itemAdd(ImGuiContext* ctx, ImGuiID id, const ImRect& bb, const ImGuiLastItemData* data) {
        ImGuiContext& g = *ctx;
        ImGuiWindow* w = g.CurrentWindow;
        if (!w || id == 0 || byId.contains(id)) return;
        Pending p;
        p.windowId = w->ID;
        p.item.id = id;
        p.item.rect = UiRect{bb.Min.x, bb.Min.y, bb.GetWidth(), bb.GetHeight()};
        p.item.window = windowPath(g, w);
        if (id == w->ID) {
            p.item.kind = "window";
            p.segment.clear();
        } else {
            p.item.kind = "item";
            if (g.CurrentTabBar && ImGui::TabBarFindTabByID(g.CurrentTabBar, id)) p.item.kind = "tab";
        }
        if (data) {
            p.item.enabled = (data->ItemFlags & ImGuiItemFlags_Disabled) == 0;
            p.item.status = static_cast<u32>(data->StatusFlags);
        }
        p.item.enabled = p.item.enabled && (g.CurrentItemFlags & ImGuiItemFlags_Disabled) == 0;
        const ImRect clip = w->ClipRect;
        p.item.visible = bb.Overlaps(clip) && w->Active && !w->Hidden;
        for (const std::string& s : scopes) p.scope += (p.scope.empty() ? "" : "/") + s;
        byId.emplace(id, building.size());
        building.push_back(std::move(p));
    }

    void itemInfo(ImGuiContext* ctx, ImGuiID id, const char* label, ImGuiItemStatusFlags flags) {
        const auto it = byId.find(id);
        if (it == byId.end()) return;
        Pending& p = building[it->second];
        p.item.status |= static_cast<u32>(flags);
        if (label) {
            const std::string_view text = label;
            const usize hash = text.find("##");
            p.item.label = std::string(text.substr(0, hash));
            p.item.labeled = true;
            if (p.item.kind != "window" && !p.fixedSegment) p.segment = displayName(text);
            if (!p.item.label.empty() && ctx->Font) {
                p.item.labelWidth = ImGui::CalcTextSize(p.item.label.c_str()).x;
            }
        }
        if (p.item.kind == "item") {
            if (flags & ImGuiItemStatusFlags_Checkable) {
                p.item.kind = "check";
            } else if (flags & ImGuiItemStatusFlags_Openable) {
                ImGuiWindow* w = ctx->CurrentWindow;
                const bool menu = w && ((w->Flags & ImGuiWindowFlags_MenuBar) != 0 || (w->Flags & ImGuiWindowFlags_Popup) != 0 ||
                                        w->DC.NavLayerCurrent == ImGuiNavLayer_Menu);
                p.item.kind = menu ? "menu" : "tree";
            } else if (flags & ImGuiItemStatusFlags_Inputable) {
                p.item.kind = "input";
            }
        }
    }

    void finish(std::vector<UiItem>& out, std::vector<UiWindowInfo>& windows, ImGuiContext& g) {
        out.clear();
        lastPaths.clear();
        std::unordered_map<std::string, int> counts;
        for (Pending& p : building) {
            // Items of windows that ended up inactive (ImGui's implicit "Debug" window when
            // nothing was drawn into it) are not part of the frame.
            const ImGuiWindow* win = ImGui::FindWindowByID(p.windowId);
            if (!win || !win->Active) continue;
            UiItem item = std::move(p.item);
            if (item.kind == "window") {
                item.path = item.window;
            } else {
                std::string segment = p.segment.empty() ? std::format("#{:08x}", item.id) : p.segment;
                item.path = item.window + (p.scope.empty() ? "" : "/" + p.scope) + "/" + segment;
            }
            const int n = ++counts[item.path];
            if (n > 1) item.path += std::format("[{}]", n);
            lastPaths.emplace(item.id, item.path);
            out.push_back(std::move(item));
        }
        building.clear();
        byId.clear();
        windowPaths.clear();
        windows.clear();
        for (ImGuiWindow* w : g.Windows) {
            if (!w->Active) continue;
            UiWindowInfo info;
            info.path = windowPath(g, w);
            info.hidden = w->Hidden;
            info.rect = UiRect{w->Pos.x, w->Pos.y, w->Size.x, w->Size.y};
            // What the user can scroll: a visible scroll bar, or the mouse wheel vertically.
            info.scrollX = w->ScrollbarX;
            info.scrollY = w->ScrollbarY || (w->ScrollMax.y > 0.0f && (w->Flags & ImGuiWindowFlags_NoScrollWithMouse) == 0);
            info.navFocusable = (w->Flags & ImGuiWindowFlags_NoNavFocus) == 0;
            info.inFocusOrder = w->FocusOrder != -1;
            windows.push_back(std::move(info));
        }
        windowPaths.clear();
        scopes.clear();
    }
};

namespace {

void hookItemAdd(void* user, ImGuiContext* ctx, ImGuiID id, const ImRect& bb, const ImGuiLastItemData* data) {
    static_cast<UiTest::Impl*>(user)->itemAdd(ctx, id, bb, data);
}
void hookItemInfo(void* user, ImGuiContext* ctx, ImGuiID id, const char* label, ImGuiItemStatusFlags flags) {
    static_cast<UiTest::Impl*>(user)->itemInfo(ctx, id, label, flags);
}
const char* hookFindLabel(void*, ImGuiContext*, ImGuiID) {
    return nullptr;
}

} // namespace

UiTest::UiTest(ImGuiContext* context, InputSink sink)
    : m_context(context), m_sink(std::move(sink)), m_impl(std::make_unique<Impl>()) {
    m_impl->owner = this;
    m_impl->hooks.itemAdd = &hookItemAdd;
    m_impl->hooks.itemInfo = &hookItemInfo;
    m_impl->hooks.findLabel = &hookFindLabel;
    m_impl->hooks.user = m_impl.get();
}

UiTest::~UiTest() {
    cancelAll();
    setEnabled(false);
}

void UiTest::setEnabled(bool enabled) {
    m_enabled = enabled;
    if (!m_context) return;
    m_context->TestEngine = enabled ? &m_impl->hooks : nullptr;
    m_context->TestEngineHookItems = enabled;
}

const UiItem* UiTest::find(std::string_view query) const {
    if (query.starts_with("**/")) {
        const std::string_view suffix = query.substr(2);  // "/<rest>"
        const UiItem* found = nullptr;
        for (const UiItem& i : m_items) {
            if (std::string_view(i.path).ends_with(suffix)) {
                if (found) return nullptr;  // ambiguous
                found = &i;
            }
        }
        return found;
    }
    for (const UiItem& i : m_items) {
        if (i.path == query) return &i;
    }
    return nullptr;
}

void UiTest::annotate(std::string_view value, std::string_view tooltip) {
    Impl* impl = Impl::current();
    if (!impl) return;
    const auto it = impl->byId.find(GImGui->LastItemData.ID);
    if (it == impl->byId.end()) return;
    UiItem& item = impl->building[it->second].item;
    item.value = std::string(value);
    if (!tooltip.empty()) item.tooltip = std::string(tooltip);
}

void UiTest::markViewport() {
    Impl* impl = Impl::current();
    if (!impl) return;
    const auto it = impl->byId.find(GImGui->LastItemData.ID);
    if (it != impl->byId.end()) impl->building[it->second].item.kind = "viewport";
}

void UiTest::setSegment(std::string_view segment) {
    Impl* impl = Impl::current();
    if (!impl) return;
    const auto it = impl->byId.find(GImGui->LastItemData.ID);
    if (it != impl->byId.end()) {
        impl->building[it->second].segment = std::string(segment);
        impl->building[it->second].fixedSegment = true;
    }
}

void UiTest::pushScope(std::string_view segment) {
    if (Impl* impl = Impl::current()) impl->scopes.emplace_back(segment);
}

void UiTest::popScope() {
    if (Impl* impl = Impl::current(); impl && !impl->scopes.empty()) impl->scopes.pop_back();
}

void UiTest::namePopup(u32 popupId, std::string_view name) {
    if (Impl* impl = Impl::current()) impl->popupNames[popupId] = std::string(name);
}

// ---------------------------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------------------------
struct UiTest::Action {
    enum class Step : u8 { Continue, Retry, Done, Fail };
    std::string name;
    Completion done;
    std::function<Step(UiTest&, Action&)> run;
    int step = 0;
    u32 wait = 0;
    u32 age = 0;
    u32 timeout = 600;
    f32 x = 0, y = 0, x2 = 0, y2 = 0;
    std::vector<UiInputEvent> events;  // key sequences
    std::string result;
    Error error;
};

void UiTest::enqueue(std::unique_ptr<Action> action) {
    m_actions.push_back(std::move(action));
}

void UiTest::beginFrame() {
    ++m_frame;
    while (!m_actions.empty()) {
        Action& a = *m_actions.front();
        if (a.wait > 0) {
            --a.wait;
            return;
        }
        ++a.age;
        const Action::Step s = a.run(*this, a);
        if (s == Action::Step::Continue) {
            ++a.step;
            return;
        }
        if (s == Action::Step::Retry) {
            if (a.age > a.timeout) {
                // Keep what the action was waiting for ("no visible item ...") in the message.
                a.error = Error{ErrorCode::Timeout, a.error.message.empty()
                                                        ? std::format("{}: timed out after {} frames", a.name, a.timeout)
                                                        : std::format("{} (timed out after {} frames)", a.error.message, a.timeout)};
            } else {
                return;
            }
        }
        std::unique_ptr<Action> finished = std::move(m_actions.front());
        m_actions.pop_front();
        if (finished->done) {
            if (s == Action::Step::Done) {
                finished->done(finished->result.empty() ? std::string("null") : finished->result);
            } else {
                finished->done(finished->error);
            }
        }
        // The next action starts on the next frame (one step per frame keeps runs deterministic).
        return;
    }
}

void UiTest::endFrame() {
    if (!m_enabled || !m_context) return;
    m_impl->finish(m_items, m_windows, *m_context);
}

void UiTest::cancelAll() {
    while (!m_actions.empty()) {
        std::unique_ptr<Action> a = std::move(m_actions.front());
        m_actions.pop_front();
        if (a->done) a->done(Error{ErrorCode::Cancelled, a->name + ": cancelled"});
    }
    m_impl->capture.reset();
    m_impl->captureResult.reset();
}

namespace {

using Step = UiTest::Action::Step;

std::string pointJson(std::string_view path, f32 x, f32 y) {
    return std::format(R"({{"path":{},"x":{},"y":{}}})", tf::json::quote(path), static_cast<int>(std::lround(x)),
                       static_cast<int>(std::lround(y)));
}

/// Resolves a visible item's center, retrying until it appears.
bool locate(UiTest& t, UiTest::Action& a, const std::string& path, f32& x, f32& y) {
    const UiItem* item = t.find(path);
    if (!item || !item->visible || item->rect.empty()) {
        a.error = Error{ErrorCode::NotFound, std::format("{}: no visible item '{}'", a.name, path)};
        return false;
    }
    // The center of the visible part (items may be partly clipped by their window).
    x = std::floor(item->rect.x + item->rect.w * 0.5f);
    y = std::floor(item->rect.y + item->rect.h * 0.5f);
    for (const UiWindowInfo& w : t.windows()) {
        if (w.path == item->window) {
            const f32 x0 = std::max(item->rect.x, w.rect.x), x1 = std::min(item->rect.right(), w.rect.right());
            const f32 y0 = std::max(item->rect.y, w.rect.y), y1 = std::min(item->rect.bottom(), w.rect.bottom());
            if (x1 > x0 && y1 > y0) {
                x = std::floor((x0 + x1) * 0.5f);
                y = std::floor((y0 + y1) * 0.5f);
            }
        }
    }
    return true;
}

UiInputEvent mouse(UiInputEvent::Type type, f32 x, f32 y, u8 button = 0) {
    UiInputEvent e;
    e.type = type;
    e.x = x;
    e.y = y;
    e.button = button;
    return e;
}

} // namespace

void UiTest::click(std::string path, u8 button, Completion done) {
    auto a = std::make_unique<Action>();
    a->name = "ui.click";
    a->done = std::move(done);
    a->run = [path, button](UiTest& t, Action& a) -> Step {
        switch (a.step) {
        case 0:
            if (!locate(t, a, path, a.x, a.y)) return Step::Retry;
            t.m_sink(mouse(UiInputEvent::Type::MouseMove, a.x, a.y));
            return Step::Continue;
        case 1: t.m_sink(mouse(UiInputEvent::Type::MouseDown, a.x, a.y, button)); return Step::Continue;
        case 2:
            t.m_sink(mouse(UiInputEvent::Type::MouseUp, a.x, a.y, button));
            a.wait = 2;
            return Step::Continue;
        default: a.result = pointJson(path, a.x, a.y); return Step::Done;
        }
    };
    enqueue(std::move(a));
}

void UiTest::doubleClick(std::string path, Completion done) {
    auto a = std::make_unique<Action>();
    a->name = "ui.doubleClick";
    a->done = std::move(done);
    a->run = [path](UiTest& t, Action& a) -> Step {
        switch (a.step) {
        case 0:
            if (!locate(t, a, path, a.x, a.y)) return Step::Retry;
            t.m_sink(mouse(UiInputEvent::Type::MouseMove, a.x, a.y));
            return Step::Continue;
        case 1:
        case 3: t.m_sink(mouse(UiInputEvent::Type::MouseDown, a.x, a.y)); return Step::Continue;
        case 2: t.m_sink(mouse(UiInputEvent::Type::MouseUp, a.x, a.y)); return Step::Continue;
        case 4:
            t.m_sink(mouse(UiInputEvent::Type::MouseUp, a.x, a.y));
            a.wait = 2;
            return Step::Continue;
        default: a.result = pointJson(path, a.x, a.y); return Step::Done;
        }
    };
    enqueue(std::move(a));
}

void UiTest::hover(std::string path, Completion done) {
    auto a = std::make_unique<Action>();
    a->name = "ui.hover";
    a->done = std::move(done);
    a->run = [path](UiTest& t, Action& a) -> Step {
        if (a.step == 0) {
            if (!locate(t, a, path, a.x, a.y)) return Step::Retry;
            t.m_sink(mouse(UiInputEvent::Type::MouseMove, a.x, a.y));
            a.wait = 2;
            return Step::Continue;
        }
        a.result = pointJson(path, a.x, a.y);
        return Step::Done;
    };
    enqueue(std::move(a));
}

void UiTest::moveTo(f32 x, f32 y, Completion done) {
    auto a = std::make_unique<Action>();
    a->name = "ui.move";
    a->done = std::move(done);
    a->run = [x, y](UiTest& t, Action& a) -> Step {
        if (a.step == 0) {
            t.m_sink(mouse(UiInputEvent::Type::MouseMove, x, y));
            a.wait = 2;
            return Step::Continue;
        }
        a.result = pointJson("", x, y);
        return Step::Done;
    };
    enqueue(std::move(a));
}

void UiTest::drag(std::string from, std::string to, f32 dx, f32 dy, Completion done) {
    auto a = std::make_unique<Action>();
    a->name = "ui.drag";
    a->done = std::move(done);
    constexpr int kMoves = 6;
    a->run = [from, to, dx, dy](UiTest& t, Action& a) -> Step {
        if (a.step == 0) {
            if (!locate(t, a, from, a.x, a.y)) return Step::Retry;
            if (to.empty()) {
                a.x2 = a.x + dx;
                a.y2 = a.y + dy;
            } else if (!locate(t, a, to, a.x2, a.y2)) {
                return Step::Retry;
            }
            t.m_sink(mouse(UiInputEvent::Type::MouseMove, a.x, a.y));
            return Step::Continue;
        }
        if (a.step == 1) {
            t.m_sink(mouse(UiInputEvent::Type::MouseDown, a.x, a.y));
            return Step::Continue;
        }
        if (a.step <= 1 + kMoves) {
            const f32 f = static_cast<f32>(a.step - 1) / kMoves;
            t.m_sink(mouse(UiInputEvent::Type::MouseMove, std::floor(a.x + (a.x2 - a.x) * f), std::floor(a.y + (a.y2 - a.y) * f)));
            return Step::Continue;
        }
        if (a.step == 2 + kMoves) {
            t.m_sink(mouse(UiInputEvent::Type::MouseUp, a.x2, a.y2));
            a.wait = 2;
            return Step::Continue;
        }
        a.result = std::format(R"({{"from":[{},{}],"to":[{},{}]}})", std::lround(a.x), std::lround(a.y), std::lround(a.x2), std::lround(a.y2));
        return Step::Done;
    };
    enqueue(std::move(a));
}

void UiTest::type(std::string text, Completion done) {
    auto a = std::make_unique<Action>();
    a->name = "ui.type";
    a->done = std::move(done);
    // Text goes to the focused text field; wait until one has keyboard focus (a field focused
    // this frame becomes active on the next one), like a user who waits for the caret.
    a->timeout = 60;
    a->run = [text](UiTest& t, Action& a) -> Step {
        if (a.step == 0) {
            if (!ImGui::GetIO().WantTextInput) {
                a.error = Error{ErrorCode::InvalidState, "ui.type: no text field has keyboard focus"};
                return Step::Retry;
            }
            UiInputEvent e;
            e.type = UiInputEvent::Type::Text;
            e.text = text;
            t.m_sink(e);
            a.wait = 2;
            return Step::Continue;
        }
        return Step::Done;
    };
    enqueue(std::move(a));
}

void UiTest::key(std::string chord, u32 repeat, Completion done) {
    auto a = std::make_unique<Action>();
    a->name = "ui.key";
    a->done = std::move(done);
    auto parsed = parseChord(chord);
    if (!parsed) {
        if (a->done) a->done(parsed.error());
        return;
    }
    const u32 mods = parsed->first;
    const std::string key = parsed->second;
    // Press the modifiers, the key, release in reverse order: one event per frame.
    static constexpr std::pair<u32, std::string_view> kMods[] = {{kModCtrl, "ctrl"}, {kModShift, "shift"}, {kModAlt, "alt"}, {kModSuper, "super"}};
    u32 held = 0;
    for (const auto& [bit, name] : kMods) {
        if (!(mods & bit)) continue;
        held |= bit;
        UiInputEvent e;
        e.type = UiInputEvent::Type::KeyDown;
        e.key = std::string(name);
        e.mods = held;
        a->events.push_back(e);
    }
    UiInputEvent down;
    down.type = UiInputEvent::Type::KeyDown;
    down.key = key;
    down.mods = held;
    UiInputEvent up = down;
    up.type = UiInputEvent::Type::KeyUp;
    for (u32 i = 0; i < std::max(repeat, 1u); ++i) {
        a->events.push_back(down);
        a->events.push_back(up);
    }
    for (auto it = std::rbegin(kMods); it != std::rend(kMods); ++it) {
        if (!(mods & it->first)) continue;
        held &= ~it->first;
        UiInputEvent e;
        e.type = UiInputEvent::Type::KeyUp;
        e.key = std::string(it->second);
        e.mods = held;
        a->events.push_back(e);
    }
    a->run = [chord](UiTest& t, Action& a) -> Step {
        const usize i = static_cast<usize>(a.step);
        if (i < a.events.size()) {
            t.m_sink(a.events[i]);
            if (i + 1 == a.events.size()) a.wait = 2;
            return Step::Continue;
        }
        a.result = tf::json::quote(chord);
        return Step::Done;
    };
    enqueue(std::move(a));
}

void UiTest::scroll(std::string path, f32 steps, Completion done) {
    auto a = std::make_unique<Action>();
    a->name = "ui.scroll";
    a->done = std::move(done);
    a->run = [path, steps](UiTest& t, Action& a) -> Step {
        switch (a.step) {
        case 0:
            if (!locate(t, a, path, a.x, a.y)) return Step::Retry;
            t.m_sink(mouse(UiInputEvent::Type::MouseMove, a.x, a.y));
            return Step::Continue;
        case 1: {
            UiInputEvent e = mouse(UiInputEvent::Type::Wheel, a.x, steps);
            t.m_sink(e);
            a.wait = 2;
            return Step::Continue;
        }
        default: a.result = pointJson(path, a.x, a.y); return Step::Done;
        }
    };
    enqueue(std::move(a));
}

void UiTest::waitFor(std::string path, std::optional<bool> enabled, std::optional<std::string> value, u32 timeoutFrames,
                     Completion done) {
    auto a = std::make_unique<Action>();
    a->name = "ui.waitFor";
    a->done = std::move(done);
    a->timeout = timeoutFrames;
    a->run = [path, enabled, value](UiTest& t, Action& a) -> Step {
        const UiItem* item = t.find(path);
        if (!item) {
            a.error = Error{ErrorCode::Timeout, std::format("ui.waitFor: '{}' did not appear", path)};
            return Step::Retry;
        }
        if ((enabled && item->enabled != *enabled) || (value && item->value != *value)) {
            a.error = Error{ErrorCode::Timeout, std::format("ui.waitFor: '{}' is enabled={} value={}", path, item->enabled, item->value)};
            return Step::Retry;
        }
        a.result = std::format(R"({{"path":{},"frames":{},"value":{}}})", tf::json::quote(item->path), a.age, tf::json::quote(item->value));
        return Step::Done;
    };
    enqueue(std::move(a));
}

void UiTest::waitFrames(u32 frames, Completion done) {
    auto a = std::make_unique<Action>();
    a->name = "ui.frames";
    a->done = std::move(done);
    a->wait = frames;
    a->run = [frames](UiTest&, Action& a) -> Step {
        a.result = std::to_string(frames);
        return Step::Done;
    };
    enqueue(std::move(a));
}

void UiTest::capture(std::string target, std::string file, Completion done) {
    auto a = std::make_unique<Action>();
    a->name = "ui.capture";
    a->done = std::move(done);
    // Let auto-sized windows, scrolling and hover states settle first.
    a->wait = 3;
    a->run = [target, file](UiTest& t, Action& a) -> Step {
        if (a.step == 0) {
            UiRect rect;
            if (target == "window" || target.empty()) {
                const ImGuiIO& io = ImGui::GetIO();
                rect = UiRect{0, 0, io.DisplaySize.x * io.DisplayFramebufferScale.x, io.DisplaySize.y * io.DisplayFramebufferScale.y};
            } else {
                bool found = false;
                for (const UiWindowInfo& w : t.windows()) {
                    if (w.path == target && !w.hidden) {
                        rect = w.rect;
                        found = true;
                    }
                }
                if (!found) {
                    const UiItem* item = t.find(target);
                    if (!item) {
                        a.error = Error{ErrorCode::NotFound, std::format("ui.capture: no window or item '{}'", target)};
                        return Step::Retry;
                    }
                    rect = item->rect;
                }
            }
            t.m_impl->captureResult.reset();
            t.m_impl->capture = CaptureRequest{rect, file};
            return Step::Continue;
        }
        if (!t.m_impl->captureResult) return Step::Retry;
        Result<std::string> r = std::move(*t.m_impl->captureResult);
        t.m_impl->captureResult.reset();
        if (!r) {
            a.error = r.error();
            return Step::Fail;
        }
        a.result = std::move(*r);
        return Step::Done;
    };
    enqueue(std::move(a));
}

const UiTest::CaptureRequest* UiTest::captureRequest() const noexcept {
    return m_impl->capture ? &*m_impl->capture : nullptr;
}

void UiTest::finishCapture(const Result<std::string>& result) {
    m_impl->capture.reset();
    m_impl->captureResult = result;
}

// ---------------------------------------------------------------------------------------------
// Lints
// ---------------------------------------------------------------------------------------------
std::vector<LintIssue> UiTest::lint(const LintOptions& options) const {
    std::vector<LintIssue> issues;
    const auto add = [&](std::string rule, std::string path, std::string message) {
        issues.push_back({std::move(rule), std::move(path), std::move(message)});
    };
    std::unordered_map<std::string_view, const UiWindowInfo*> windows;
    for (const UiWindowInfo& w : m_windows) windows.emplace(w.path, &w);

    std::vector<const UiItem*> leaves;
    for (const UiItem& i : m_items) {
        if (i.kind == "window" || !i.visible || i.rect.empty()) continue;
        leaves.push_back(&i);
        // Text clipped inside its item: the visible label is wider than the item.
        if (i.labeled && !i.label.empty() && i.kind != "input" && i.labelWidth > i.rect.w + 1.0f) {
            add("clipped", i.path, std::format("label '{}' is {:.0f} px wide in a {:.0f} px item", i.label, i.labelWidth, i.rect.w));
        }
        // An interactive item with no label and no tooltip (ImGui chrome reports no label at all).
        if (i.labeled && i.label.empty() && i.tooltip.empty() && i.kind != "viewport") {
            add("unlabeled", i.path, "interactive item without a label or tooltip");
        }
        // A widget outside its window on an axis the window cannot scroll (ImGui chrome such as
        // resize borders sits on the window edge by design).
        if (const auto it = windows.find(i.window); i.labeled && it != windows.end()) {
            const UiWindowInfo& w = *it->second;
            const bool outX = i.rect.x < w.rect.x - 1.0f || i.rect.right() > w.rect.right() + 1.0f;
            const bool outY = i.rect.y < w.rect.y - 1.0f || i.rect.bottom() > w.rect.bottom() + 1.0f;
            if ((outX && !w.scrollX) || (outY && !w.scrollY)) {
                add("outside", i.path, std::format("item [{:.0f},{:.0f} {:.0f}x{:.0f}] leaves window '{}'", i.rect.x, i.rect.y, i.rect.w,
                                                   i.rect.h, w.path));
            }
        }
    }
    // Overlapping interactive items of one window (containment is nesting, not overlap).
    for (usize a = 0; a < leaves.size(); ++a) {
        for (usize b = a + 1; b < leaves.size(); ++b) {
            const UiItem& x = *leaves[a];
            const UiItem& y = *leaves[b];
            // Widgets only: ImGui chrome (column resizers, splitters) overlaps by design.
            if (x.window != y.window || !x.labeled || !y.labeled) continue;
            if (x.rect.contains(y.rect) || y.rect.contains(x.rect)) continue;
            // More than a pixel on both axes: selectables and table rows touch by design and can
            // share a sub-pixel seam at fractional scales.
            const f32 ox = std::min(x.rect.right(), y.rect.right()) - std::max(x.rect.x, y.rect.x);
            const f32 oy = std::min(x.rect.bottom(), y.rect.bottom()) - std::max(x.rect.y, y.rect.y);
            if (ox > 1.0f && oy > 1.0f) {
                add("overlap", x.path, std::format("overlaps '{}'", y.path));
            }
        }
    }
    if (options.theme) {
        for (const ContrastIssue& c : checkContrast(*options.theme)) {
            add("contrast", "theme/" + options.theme->name + "/" + c.foreground + "-on-" + c.background,
                std::format("contrast {:.2f}:1 below {:.1f}:1", c.ratio, c.required));
        }
    }
    if (options.commands) {
        for (const std::string& id : options.commands->unexposedCommands(true)) {
            add("unexposed", "command/" + id, "no menu, toolbar or context menu exposes the command");
        }
    }
    for (const std::string& panel : options.panels) {
        const auto it = windows.find(panel);
        if (it == windows.end()) {
            add("unreachable", panel, "panel is not open");
        } else if (!it->second->navFocusable || !it->second->inFocusOrder) {
            add("unreachable", panel, "keyboard focus cycling (Ctrl+Tab) cannot reach the panel");
        }
    }
    return issues;
}

std::string lintIssuesJson(const std::vector<LintIssue>& issues) {
    refl::JsonWriter w(refl::JsonStyle::Compact);
    w.beginArray();
    for (const LintIssue& i : issues) {
        w.beginObject();
        w.key("rule");
        w.string(i.rule);
        w.key("path");
        w.string(i.path);
        w.key("message");
        w.string(i.message);
        w.endObject();
    }
    w.endArray();
    return w.take();
}

std::string UiTest::itemsJson(std::string_view filter) const {
    refl::JsonWriter w(refl::JsonStyle::Compact);
    w.beginArray();
    for (const UiItem& i : m_items) {
        if (!filter.empty() && i.path.find(filter) == std::string::npos) continue;
        w.beginObject();
        w.key("path");
        w.string(i.path);
        w.key("window");
        w.string(i.window);
        w.key("label");
        w.string(i.label);
        w.key("kind");
        w.string(i.kind);
        w.key("rect");
        w.beginArray(true);
        w.number(i.rect.x);
        w.number(i.rect.y);
        w.number(i.rect.w);
        w.number(i.rect.h);
        w.endArray();
        w.key("enabled");
        w.boolean(i.enabled);
        w.key("visible");
        w.boolean(i.visible);
        w.key("value");
        w.string(i.value);
        w.key("tooltip");
        w.string(i.tooltip);
        w.endObject();
    }
    w.endArray();
    return w.take();
}

} // namespace helios::edui

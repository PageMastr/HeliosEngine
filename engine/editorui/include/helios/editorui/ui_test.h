#pragma once
// Editor UI test harness core (07 §4.4, helios-uitest). Built in-house on edui hooks, following the
// design of Dear ImGui Test Engine (items addressed by path, queued input, wait helpers) without its
// code (docs/adr/ADR-0.18-imgui-test-engine-licence.md).
//
// * Item table: while enabled, every interactive item ImGui submits is recorded each frame with a
//   stable test path built from its window, the semantic scopes edui widgets push and the widget's
//   id ("Inspector/Grid/row[mass]/value"): the part after "###" or "##" of its label when present,
//   else its visible text. It also records the rect, kind, enabled state, value text and tooltip.
//   The hooks cost one branch per item when the harness is off.
//   Budget (07 §4.4): the item table adds ≤ 0.2 ms per frame for the shell with a record in the
//   property grid at 1920 × 1080; editorui_tests' "perf: item table" case measures it.
// * Input: ui.* actions queue input events (mouse, keys, text) that the host feeds through the SDL3
//   backend seam, one step per frame, so they take the same path as a real mouse and keyboard.
// * Waits and captures complete after whole frames; with the host's fixed time step the whole run
//   is deterministic.
// * Layout lints: clipped text, overlapping items, items outside a non-scrolling window, unlabeled
//   interactive items, low-contrast theme tokens, commands no menu exposes, panels keyboard focus
//   cannot reach.
//
// Threading: owner (UI) thread only.

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "helios/core/result.h"
#include "helios/core/types.h"

struct ImGuiContext;

namespace helios::tf {
class CommandBus;
}

namespace helios::edui {

struct Theme;

struct UiRect {
    f32 x = 0, y = 0, w = 0, h = 0;
    f32 right() const noexcept { return x + w; }
    f32 bottom() const noexcept { return y + h; }
    bool empty() const noexcept { return w <= 0 || h <= 0; }
    bool contains(const UiRect& o, f32 tolerance = 0.5f) const noexcept {
        return o.x >= x - tolerance && o.y >= y - tolerance && o.right() <= right() + tolerance &&
               o.bottom() <= bottom() + tolerance;
    }
    /// Intersection area.
    f32 overlap(const UiRect& o) const noexcept;
};

struct UiItem {
    u32 id = 0;
    std::string path;     ///< Stable test path.
    std::string window;   ///< Path of the window the item belongs to.
    std::string label;    ///< Visible label ("" when the widget has none).
    std::string kind;     ///< window, item, check, tree, input, tab, menu, viewport.
    UiRect rect;          ///< Display pixels.
    f32 labelWidth = 0;   ///< Width of the visible label at the current font size.
    bool enabled = true;
    bool visible = true;  ///< Intersects its window's visible area.
    bool labeled = false; ///< The widget reported a label (ImGui chrome such as scrollbars does not).
    std::string value;    ///< Annotated by edui widgets (current value as text).
    std::string tooltip;  ///< Annotated tooltip.
    u32 status = 0;       ///< ImGuiItemStatusFlags.
};

struct UiWindowInfo {
    std::string path;
    UiRect rect;
    bool scrollX = false;  ///< The window can scroll horizontally.
    bool scrollY = false;
    bool navFocusable = true;
    bool inFocusOrder = false;
    bool hidden = false;  ///< Open but not shown (a docked panel behind another tab).
};

struct LintIssue {
    std::string rule;  ///< clipped, overlap, outside, unlabeled, contrast, unexposed, unreachable.
    std::string path;
    std::string message;
};

/// One synthetic input event, delivered by the host through the SDL3 backend seam.
struct UiInputEvent {
    enum class Type : u8 { MouseMove, MouseDown, MouseUp, Wheel, KeyDown, KeyUp, Text, Focus };
    Type type = Type::MouseMove;
    f32 x = 0, y = 0;       ///< MouseMove: display pixels. Wheel: y = wheel steps.
    u8 button = 0;          ///< 0 left, 1 right, 2 middle.
    std::string key;        ///< KeyDown/KeyUp: key name ("a", "enter", "ctrl", "f2", "tab", ...).
    u32 mods = 0;           ///< Modifier bits held (kModCtrl | ...).
    std::string text;       ///< Text: UTF-8.
    bool focused = true;    ///< Focus.
};

inline constexpr u32 kModCtrl = 1, kModShift = 2, kModAlt = 4, kModSuper = 8;

/// A chord "ctrl+shift+p" -> modifier bits + key name. InvalidArgument for unknown keys.
Result<std::pair<u32, std::string>> parseChord(std::string_view chord);
/// Key names parseChord() accepts (the host maps each to an SDL keycode and scancode).
std::vector<std::string_view> uiKeyNames();

class UiTest {
public:
    using Completion = std::function<void(const Result<std::string>&)>;
    using InputSink = std::function<void(const UiInputEvent&)>;
    /// Called when a capture action is ready; the host renders the frame, crops `rect`, writes the
    /// PNG and reports back through finishCapture().
    struct CaptureRequest {
        UiRect rect;
        std::string file;
    };

    UiTest(ImGuiContext* context, InputSink sink);
    ~UiTest();
    UiTest(const UiTest&) = delete;
    UiTest& operator=(const UiTest&) = delete;

    /// Installs or removes the item hooks.
    void setEnabled(bool enabled);
    bool enabled() const noexcept { return m_enabled; }

    /// Before ImGui::NewFrame(): runs the current action's step (which may inject input).
    void beginFrame();
    /// After ImGui::EndFrame()/Render(): publishes this frame's item table and window list.
    void endFrame();
    u64 frame() const noexcept { return m_frame; }

    /// The last complete frame's items and windows.
    const std::vector<UiItem>& items() const noexcept { return m_items; }
    const std::vector<UiWindowInfo>& windows() const noexcept { return m_windows; }
    /// Exact path, or a unique path ending in "/<query>" when the query starts with "**/".
    const UiItem* find(std::string_view query) const;

    // ---- annotations (edui widgets; no-ops while disabled) -----------------------------------
    /// Value text and tooltip of the last submitted item.
    static void annotate(std::string_view value, std::string_view tooltip = {});
    /// Marks the last item as a viewport (masked in UI goldens: 03's goldens own the renderer).
    static void markViewport();
    /// Replaces the last item's path segment (its label by default), for stable paths of items
    /// whose label is data or translated text.
    static void setSegment(std::string_view segment);
    /// Semantic path segments for the items that follow ("Grid", "row[mass]").
    static void pushScope(std::string_view segment);
    static void popScope();
    /// Names a popup (by its ImGui id) for item paths inside it.
    static void namePopup(u32 popupId, std::string_view name);

    // ---- actions (ui.*) ----------------------------------------------------------------------
    void click(std::string path, u8 button, Completion done);
    void doubleClick(std::string path, Completion done);
    void hover(std::string path, Completion done);
    void moveTo(f32 x, f32 y, Completion done);
    /// Drag from an item's center to another item's center, or by (dx, dy) pixels when `to` is empty.
    void drag(std::string from, std::string to, f32 dx, f32 dy, Completion done);
    void type(std::string text, Completion done);
    /// Presses a chord: modifiers down, the key down/up `repeat` times, modifiers up, one event
    /// per frame ("ctrl+tab" x3 walks the Ctrl+Tab window list three steps).
    void key(std::string chord, u32 repeat, Completion done);
    void scroll(std::string path, f32 steps, Completion done);
    /// Waits until the item exists (and, when given, is enabled / has the value).
    void waitFor(std::string path, std::optional<bool> enabled, std::optional<std::string> value, u32 timeoutFrames,
                 Completion done);
    void waitFrames(u32 frames, Completion done);
    /// Captures `target` ("window" or a window/item path) into a PNG file after the UI settles.
    void capture(std::string target, std::string file, Completion done);
    /// Pending capture of this frame (the host checks after ImGui::Render()).
    const CaptureRequest* captureRequest() const noexcept;
    void finishCapture(const Result<std::string>& result);
    /// Drops queued actions (they complete with Cancelled).
    void cancelAll();
    usize pendingActions() const noexcept { return m_actions.size(); }

    // ---- lints ---------------------------------------------------------------------------------
    struct LintOptions {
        std::vector<std::string> panels;         ///< Window paths that must be reachable.
        const tf::CommandBus* commands = nullptr;
        const Theme* theme = nullptr;
    };
    std::vector<LintIssue> lint(const LintOptions& options) const;

    /// Item table as JSON (ui.items): [{path, window, label, kind, rect: [x, y, w, h], enabled, ...}].
    std::string itemsJson(std::string_view filter = {}) const;

    struct Impl;    ///< Internal.
    struct Action;  ///< Internal: one queued ui.* action.

private:
    void enqueue(std::unique_ptr<Action> action);

    ImGuiContext* m_context;
    InputSink m_sink;
    bool m_enabled = false;
    u64 m_frame = 0;
    std::unique_ptr<Impl> m_impl;
    std::vector<UiItem> m_items;
    std::vector<UiWindowInfo> m_windows;
    std::deque<std::unique_ptr<Action>> m_actions;
};

/// JSON of lint issues: [{"rule", "path", "message"}].
std::string lintIssuesJson(const std::vector<LintIssue>& issues);

} // namespace helios::edui

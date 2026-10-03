#pragma once
// The editor's UI-scale policy for per-monitor DPI (07 §1.3: a panel moved from a 100 % to a 200 %
// monitor stays sharp). Pure functions; used by EditorHost on SDL's display-scale events.

#include <algorithm>
#include <cmath>
#include <optional>

#include "helios/core/types.h"

namespace helios::edui::detail {

inline constexpr f32 kMinUiScale = 0.5f;
inline constexpr f32 kMaxUiScale = 4.0f;

/// The UI scale to switch to when the window's display scale is `displayScale` (from
/// SDL_GetWindowDisplayScale, at start and on SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED). None when a
/// scale was forced (--scale, HELIOS_EDITOR_SCALE, test mode), the display reports no scale, or
/// the clamped scale equals `current`.
inline std::optional<f32> scaleForDisplay(f32 current, f32 displayScale, bool forced) noexcept {
    if (forced || !(displayScale > 0.0f) || !std::isfinite(displayScale)) return std::nullopt;
    const f32 s = std::clamp(displayScale, kMinUiScale, kMaxUiScale);
    if (s == current) return std::nullopt;
    return s;
}

/// Who owns the UI scale. A scale forced at start (--scale, HELIOS_EDITOR_SCALE, test mode) or
/// chosen under View > UI Scale stays when the window moves to another display; otherwise the
/// scale follows the window's display.
struct ScalePolicy {
    bool forced = false;      ///< Forced at start.
    bool userChosen = false;  ///< The user picked a scale in this session.

    /// The UI scale to switch to on a display-scale change (see scaleForDisplay).
    std::optional<f32> onDisplayScale(f32 current, f32 displayScale) const noexcept {
        return scaleForDisplay(current, displayScale, forced || userChosen);
    }
    /// The user picked `scale`: returns it clamped, and display changes no longer move it.
    f32 choose(f32 scale) noexcept {
        userChosen = true;
        return std::clamp(scale, kMinUiScale, kMaxUiScale);
    }
};

} // namespace helios::edui::detail

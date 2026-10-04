#pragma once
// Dear ImGui item hooks for the editor UI test harness (07 §4.4). imgui.cpp, built with
// IMGUI_ENABLE_TEST_ENGINE (third_party/CMakeLists.txt), reports every item it submits to the
// ImGuiTestEngineHook_* functions while a context has TestEngineHookItems set. Helios defines those
// functions in imgui_item_hooks.cpp (compiled into tp_imgui, so every image that links ImGui links):
// they forward to the table stored in ImGuiContext::TestEngine, and do nothing when it is null.
//
// This is Helios code (MIT). Dear ImGui Test Engine itself is not used or vendored (see
// docs/adr/ADR-0.18-imgui-test-engine-licence.md).

#include "imgui.h"
#include "imgui_internal.h"

struct HeliosImGuiItemHooks {
    void (*itemAdd)(void* user, ImGuiContext* ctx, ImGuiID id, const ImRect& bb, const ImGuiLastItemData* item);
    void (*itemInfo)(void* user, ImGuiContext* ctx, ImGuiID id, const char* label, ImGuiItemStatusFlags flags);
    const char* (*findLabel)(void* user, ImGuiContext* ctx, ImGuiID id);
    void* user;
};

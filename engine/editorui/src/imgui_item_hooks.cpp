// Forwarders of Dear ImGui's item hooks (see imgui_item_hooks.h). Compiled into tp_imgui.

#include "imgui_item_hooks.h"

namespace {
HeliosImGuiItemHooks* hooks(ImGuiContext* ctx) {
    return ctx ? static_cast<HeliosImGuiItemHooks*>(ctx->TestEngine) : nullptr;
}
} // namespace

void ImGuiTestEngineHook_ItemAdd(ImGuiContext* ctx, ImGuiID id, const ImRect& bb, const ImGuiLastItemData* item_data) {
    if (HeliosImGuiItemHooks* h = hooks(ctx); h && h->itemAdd) h->itemAdd(h->user, ctx, id, bb, item_data);
}

void ImGuiTestEngineHook_ItemInfo(ImGuiContext* ctx, ImGuiID id, const char* label, ImGuiItemStatusFlags flags) {
    if (HeliosImGuiItemHooks* h = hooks(ctx); h && h->itemInfo) h->itemInfo(h->user, ctx, id, label, flags);
}

void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...) {}

const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext* ctx, ImGuiID id) {
    if (HeliosImGuiItemHooks* h = hooks(ctx); h && h->findLabel) return h->findLabel(h->user, ctx, id);
    return nullptr;
}

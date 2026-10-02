/* Dear ImGui build configuration for the addon host (third_party/imgui/manifest.json points here).
 * ImGui misuse by an addon must never take the game down: a failed assertion is logged once per
 * call site (xi_imgui_assert, host/addons/gui.cpp) and ImGui carries on. */
#pragma once

#ifdef __cplusplus
extern "C"
#endif
    void
    xi_imgui_assert(const char* expr, const char* file, int line);

#define IM_ASSERT(e) ((e) ? (void)0 : xi_imgui_assert(#e, __FILE__, __LINE__))
#define IMGUI_DISABLE_OBSOLETE_KEYIO
#define IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS

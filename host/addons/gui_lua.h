/* Lua binding of Ashita v4's IGuiManager (AshitaCore:GetGuiManager(), wrapped by Ashita's libs/imgui.lua).
 *
 * The manager is a plain table of functions, one per lua_State, cached in that state's registry. Its
 * Lua-facing conventions follow Ashita's annotations (addons/libs/annotations/SDK/IGuiManager*.lua):
 * pointer parameters are tables read/written at [1] (arrays at [1]..[N]), ImVec2/ImVec4 go in as
 * {x, y}/{x, y, z, w} tables and come out as multiple numbers, io/style/fonts/draw lists/viewports are
 * live objects, printf-style functions take one string, a function ImGui lacks is nil.
 *
 * Widget and drawing functions do nothing (and return their defaults) unless xi_gui_in_frame() says an
 * ImGui frame is open, so addons calling imgui outside their present event can't upset ImGui's stacks. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lua_State lua_State;

/* Pushes this state's GuiManager table (created on first use). */
void xi_gui_lua_push_manager(lua_State* L);

/* Call before lua_close(L): drops the state from the size-constraint callback slot. */
void xi_gui_lua_forget_state(lua_State* L);

/* IGuiManager:GetVisible()/SetVisible(): whether addons' ImGui output should be shown (starts 1). */
int xi_gui_lua_visible(void);
void xi_gui_lua_set_visible(int visible);

/* Provided by the host's frame code (gui.cpp): 1 between the host's NewFrame and Render. */
int xi_gui_in_frame(void);

#ifdef __cplusplus
}
#endif

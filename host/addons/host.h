/* The addon host's internals, shared by its C files (not by the rest of host64: addons.h). */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "xi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lua_State lua_State;

/* Which API an addon is written for. */
enum
{
    XI_KIND_XI,       /* ours: xi.* */
    XI_KIND_ASHITA,   /* Ashita v4 */
    XI_KIND_WINDOWER, /* Windower 4 */
    XI_KINDS,
};
extern const char* const xi_kind_name[XI_KINDS]; /* "xi", "ashita", "windower" */

typedef struct Addon
{
    char name[64];
    int kind;
    char dir[1024];  /* its folder, host path, trailing '/' */
    char file[1024]; /* its main .lua */
    lua_State* L;
    int hooks;       /* registry ref: the table xi.lua returns (raise, frame, ...) */
    int dead;        /* faulted in native code: its state is abandoned, never entered again */
    int unloading;   /* unload requested while it (or another addon) was running */
    /* errors: an addon failing continuously for 5 s is unloaded */
    uint64_t err_since, err_last;
    char err_text[256];
    unsigned err_repeats;
} Addon;

/* --addon-harness: no game running (chat lines to stdout, commands routed directly, no GPU). */
extern int xi_headless;

/* --- registry (core.c) ----------------------------------------------------------------------- */

unsigned xi_addon_count(void);
Addon* xi_addon_at(unsigned i);
Addon* xi_addon_find(const char* name);
/* kind -1: the first folder (xi, ashita, windower) that has it. 1 on success; errors go to chat. */
int xi_addon_load(const char* name, int kind);
/* Unload now, or as soon as no Lua is running. */
void xi_addon_unload(Addon* a);
void xi_addon_reload(Addon* a);
/* The addon whose Lua is running on this thread (NULL outside Lua). */
Addon* xi_current(void);
/* An error from an addon (Lua error text, first line to chat once, all to the log). */
void xi_addon_error(Addon* a, const char* where, const char* msg);

/* <data dir>/<kind>/ (ashita/, windower/, xi/): that ecosystem's install layout, trailing '/'. */
const char* xi_kind_root(int kind);

/* --- events (core.c) ------------------------------------------------------------------------- */

/* One event raised to every addon, in load order. The C side fills what the event has; each
 * addon's Lua sees it as a table (xi.lua) and may set blocked, a modified buffer or mode. The
 * compat layers (ashita.lua, windower.lua) turn it into their own events. */
typedef struct XiEvent
{
    const char* name; /* command, text_in, text_out, packet_in, packet_out, key, mouse, frame, ... */
    int mode, injected, blocked;
    uint32_t id;
    const uint8_t* data; size_t size;      /* the original: command line, chat text, packet */
    uint8_t* mod; size_t mod_size, mod_cap; /* the modified copy, updated from Lua (mod_cap 0: none) */
    int mode_mod;
    const uint8_t* chunk; size_t chunk_size; /* packets: the whole buffer's packets */
    uint32_t sequence;
    int x, y, delta, msg; /* mouse: window message, game pixels, wheel */
    uint32_t key, flags, vk; int down; /* keys: DIK code, flags, virtual key */
    int handled;          /* any addon returned true */
} XiEvent;

void xi_raise(XiEvent* e);
/* A drawing event with no data (present, postrender). */
void xi_frame_draw(const char* name);
/* Only addons of one kind (-1: all). */
void xi_raise_kind(XiEvent* e, int kind);

/* --- hooks and chat (hooks.c) ---------------------------------------------------------------- */

void xi_hooks_init(void);
void xi_hooks_frame(void);            /* chat lines and commands queued for the game */
/* The chat input line: text, open state. */
int xi_chat_input(char* out, size_t n);
int xi_chat_input_open(void);
void xi_chat_set_input(const char* text);
/* Packets queued by addons (whole packets: header included). */
void xi_packet_inject(int outgoing, const uint8_t* p, size_t n);
/* The last packet of an id seen in a direction (0x200 bytes max): size, or 0. */
size_t xi_packet_last(int outgoing, uint16_t id, uint8_t* out, size_t cap, uint64_t* when_ms);
/* A plain buffer (0x1C header, packets) through the pipeline, as the packet hooks do (the harness). */
size_t xi_packets_process(int outgoing, const uint8_t* buf, size_t size, uint8_t* out, size_t cap);

/* --- commands (cmd.c) ------------------------------------------------------------------------ */

/* A line from the command line (or injected): 1 if the host or an addon handled it (the game
 * doesn't see it). */
int xi_command(const char* line, int mode, int injected);
/* Runs a script file (scripts/<name>.txt of a kind, or a path): each line as if typed. */
int xi_exec_script(const char* name, int kind);
void xi_cmd_frame(void); /* scripts' waits, the boot script on the first frame */
/* Aliases: /alias add /foo /bar. */
void xi_alias_set(const char* name, const char* expansion);

/* --- input (input.c) ------------------------------------------------------------------------- */

void xi_input_init(void);
/* Binds (Ashita's /bind syntax, Windower's bind): key text like "^!f1". 1 if parsed. */
int xi_bind(const char* key, const char* command, int down_too);
int xi_unbind(const char* key);
void xi_bind_list(void);
/* DIK key state as the game last read it. */
int xi_key_down(uint32_t dik);

/* --- the overlay (gui.cpp) ------------------------------------------------------------------- */

void xi_gui_init(void);
/* Between the addons' frame events and the frame going out. */
void xi_gui_begin(uint32_t w, uint32_t h);
void xi_gui_end(void);
int xi_gui_in_frame(void);
/* Around one addon's drawing event: what it leaves open in ImGui is closed (and logged). */
void xi_gui_addon_begin(void);
void xi_gui_addon_end(Addon* a);
/* Input: 1 when the overlay takes it (the game must not see it). */
int xi_gui_mouse(int msg, int x, int y, int delta);
int xi_gui_key(uint32_t vk, int down, uint32_t scancode);
int xi_gui_text(const char* utf8);
int xi_gui_wants_keyboard(void);
int xi_gui_wants_mouse(void);
/* Objects an addon owns (text objects, primitives, textures) freed when it unloads. */
void xi_gui_free_owned(Addon* a);

/* --- Lua (lua_xi.c) -------------------------------------------------------------------------- */

/* Pushes the native xi table for an addon's state. */
void xi_lua_open(lua_State* L, Addon* a);
/* Embedded Lua sources (generated/addons_lua.c from host/addons/lua/). */
const char* xi_embedded(const char* name, size_t* size);

#ifdef __cplusplus
}
#endif

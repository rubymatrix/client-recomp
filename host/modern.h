/* Config > Display, Menus, Modern and Addons (modern.c): pages of the game's own Config menu, drawn and
 * run by the game's menu code - its window, cursor, sounds and open/close effects. Menus shows or
 * hides items of the game's menus (each as it next opens); Modern switches this port's additions:
 * the scene effects (gfx_metal.m's settings file), draw distance, the frame rate and the
 * interface's shape, each showing at once. */
#pragma once

#include <stdint.h>

typedef struct ModernSetup
{
    const char* game; /* the FINAL FANTASY XI folder as the game sees it: the menu DAT the layouts are cut from */
    const char* data_dir;  /* modern.cfg: the frame rate, the interface's shape and what Menus hides */
    uint32_t* fps_divisor; /* host64's: 1 60 fps, 2 30 */
    int fps_given, ui_aspect_given; /* on the command line: modern.cfg does not override them */
    const char* settings_reg; /* the game's display settings (0001/0002, 0003/0004, 0037/0038): Config > Display writes them */
} ModernSetup;

/* Reads modern.cfg, applying what the command line did not give. */
void modern_init(const ModernSetup* setup);
/* From the Present hook, on the game's thread: puts the items and pages into the game's menus once
 * its menu data is loaded (and again if it reloads it). */
void modern_frame(void);

/* Config > Addons: the addon host's installed addons, a row each (host/addons/manage.c). Each call
 * is on the game's thread. */
typedef struct ModernAddons
{
    int (*scan)(void);           /* the folders read again: the count */
    const char* (*name)(int i);  /* by name, without case */
    const char* (*kind)(int i);  /* "xi", "Ashita", "Windower" */
    int (*is_new)(int i);        /* installed since the page last closed */
    int (*get)(int i);           /* on (or asked to be) */
    void (*set)(int i, int on);  /* loads or unloads on the next frame, and remembers it */
    void (*seen)(void);          /* the page closed: none is new */
} ModernAddons;

/* Before the menus are in (modern_frame's first): with NULL or never called, no Addons page. */
void modern_set_addons(const ModernAddons* ops);

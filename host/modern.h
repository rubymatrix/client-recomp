/* Config > Menus and Config > Modern (modern.c): pages of the game's own Config menu, drawn and
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

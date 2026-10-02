#pragma once

#include <windows.h>

/* Map the retail FFXiMain.dll at 0x10000000 and wire it to the translation. NULL on failure
 * (reason on stderr). Does not run the game's DllMain. */
HMODULE rt_load_image(const char* retail_path);

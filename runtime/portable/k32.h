/* KERNEL32 shims (k32.c). game_dir: the FINAL FANTASY XI folder (GetModuleFileNameA on the game
 * module answers <game_dir>\FFXiMain.dll, as retail does); exe_path: what the host process is
 * called, as the guest sees it. */
#pragma once

void k32_init(const char* game_dir, const char* exe_path);
/* A translated module mapped besides FFXiMain (FFXi.dll): GetModuleHandleA, GetModuleFileNameA
 * and GetProcAddress answer for it. */
void k32_add_module(const char* name, uint32_t base);
/* Files, directories, synchronisation, threads, time (k32_io.c). */
void k32_io_init(void);
void k32_misc_init(void); /* k32_misc.c */

/* The registry (reg.c): .reg files in `reg export` format, loaded in order, then the overlay the
 * game's own changes are saved to (may be NULL: changes stay in memory). */
void reg_init(const char* const* files, unsigned nfiles, const char* overlay);
/* A .reg loaded after the overlay, so its values win over what the game saved (a launcher's settings). */
void reg_load_final(const char* path);
/* ole32 / OLEAUT32 (ole.c): COM plumbing, CoCreateInstance of the translated modules' classes. */
void ole_init(void);
/* A COM class a translated module serves (DllGetClassObject of the module mapped at module_base). */
void ole_register_class(const uint8_t clsid[16], uint32_t module_base);

/* A DWORD value by full path ("HKEY_LOCAL_MACHINE\SOFTWARE\..."), for the host; 1 if found. */
int reg_get_dword(const char* path, const char* name, uint32_t* out);
/* A string value, the same way (NUL-terminated, truncated to n). */
int reg_get_string(const char* path, const char* name, char* out, size_t n);
/* Sets a string value (REG_SZ), creating the key: what the host knows better than the imported file. */
void reg_set_string(const char* path, const char* name, const char* value);

/* The translated image's file name (default FFXiMain.dll): an EXE (ffxivgame.exe) is also the
 * process image, at EXE_HANDLE. */
void k32_set_image_name(const char* name);

/* kernel32 calls only FINAL FANTASY XIV makes (k32_xiv.c) */
void k32_xiv_init(void);
/* The arguments after the exe path in GetCommandLine, and the value GetTickCount starts from
 * (ffxivgame.exe's launch argument is keyed to it: tools/xiv_args.py). */
void k32_set_command_line(const char* args);
void k32_set_tick_base(uint32_t tick);
/* XIV_CPUS when set, else 0 (each query its own default) */
unsigned k32_cpus(void);
/* Sleep calls returning to `ret` only yield (lifting a frame limiter) */
void k32_skip_sleep_at(uint32_t ret);
/* Sleep calls returning to `ret` sleep until one period after the last such call woke */
void k32_pace_sleep_at(uint32_t ret, uint64_t period_ns);

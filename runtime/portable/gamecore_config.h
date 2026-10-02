/* What the host tells our own gamecore (gamecore_slots.c). */
#pragma once

#include <stdint.h>

/* Register the implemented slots; call before gamecore_init. */
void gamecore_slots_init(void); /* session and lobby (gamecore_slots.c) */
void gamecore_files_init(void); /* paths, files, registry, timers (gamecore_files.c) */
void gamecore_presence_init(void); /* presence client, records, mail, text (gamecore_presence.c) */

/* The viewer folder as the guest sees it ("C:\...\<viewer>"): the root of every path the
 * core hands out (slot 126). */
void gamecore_set_root(const char* guest_viewer_dir);
/* A patch.ver (0x120 bytes, encrypted as retail's with no registry key) carrying the client
 * version string, e.g. "30260903_0", for an install that ships none (private-server installs
 * launched without the viewer). 0 if the string does not fit. */
int gamecore_make_patch_ver(const char* version, uint8_t out[0x120]);
/* The AFK logout timer's check, run from the per-frame pump (gamecore_files.c). */
void gamecore_idle_tick(void);

/* The session value V for this sign-in: 16 bytes, no NUL, byte-identical to the session value in
 * LSB's account table (LSB's lobby checks the passwords built from it). */
void gamecore_set_session(const uint8_t v[16]);
/* The lobby's IPv4 address (host byte order), which the lobby's host name resolves to. Default
 * 127.0.0.1; the hosts set it to their account server (host64: --server). */
void gamecore_set_lobby(uint32_t ipv4_host_order);
/* The command line gamecore reports (GetlpCmdLine): empty, or " /game eAZcFcB -net 3
 * -port <lobby view port>" for a LandSandBoat sign-in. Before gamecore_init. */
void gamecore_set_cmdline(const char* text);
/* The exit code and message the game left (slots 969/1026), after GameStart returns. */
int32_t gamecore_exit_code(const char** message);

/* Discord Rich Presence (discord.c): the character, job and zone on the player's Discord profile,
 * sent over the Discord client's local IPC socket (no SDK). Off with FFXI_DISCORD=0; the
 * application is FFXI_DISCORD_APP_ID's, else the built-in one. FFXI_DISCORD_NAME=0 leaves the
 * character's name out. The activity clears when the game exits (Discord sees the
 * socket close). */
#pragma once

/* Reads the settings. Nothing connects until the first frame (of the sign-in screen or the game). */
void discord_init(void);
/* From the Present hook, on the game's thread (guest lock held): every few seconds reads the game
 * state and, when it changed, sends it. Connects (and reconnects) to Discord on its own. */
void discord_frame(void);
/* From the sign-in screen's loop, before the game runs: "Signing in", without reading the game. */
void discord_signin_frame(void);

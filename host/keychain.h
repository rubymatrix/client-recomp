/* Saved sign-in passwords in the OS's credential store: the macOS keychain (generic passwords
 * under one service). Elsewhere nothing is saved yet: every call fails and the player types the
 * password each time. */
#pragma once

#include <stddef.h>

/* The password saved for key, into out. 0 when there is none (or no store). */
int keychain_get(const char* key, char* out, size_t n);
/* Saves (or replaces) key's password. 0 on failure. */
int keychain_set(const char* key, const char* password);
void keychain_delete(const char* key);

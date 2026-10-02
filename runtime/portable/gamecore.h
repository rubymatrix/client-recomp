/* Our own gamecore (R3, decided 2026-09-24): what
 * the retail core library gives FFXi.dll and FFXiMain.dll, implemented natively.
 *
 * Two surfaces (specs/FFXI.gamecore-surface.txt):
 *   - the ICoreCom object FFXi.dll's GameStart receives. FFXi.dll calls six methods; the
 *     object answers only the US interface id, so FFXi.dll records region 1, as a retail
 *     US client does;
 *   - the 1,560-slot common function table, which FFXi.dll hands FFXiMain through the parameter
 *     block. It lives in guest memory; each slot is a thunk (thunk.h) named "gamecore!+0xDISP",
 *     so a slot with no implementation traps with its offset on first call.
 *
 * The slot implementations (gamecore_slots.c) follow the per-slot specifications in
 * specs/gamecore-slots.*.txt. */
#pragma once

#include <stdint.h>

#include "thunk.h"

#define GAMECORE_SLOTS 1560u

/* Builds the table and the object in guest memory. Call after the guest window exists. */
void gamecore_init(void);
/* The ICoreCom object (a guest address), to pass to IFFXiEntry::GameStart. */
uint32_t gamecore_object(void);
/* The common function table (a guest address). */
uint32_t gamecore_table(void);

/* Slot implementations register here (by byte offset into the table, as the docs number them). */
typedef struct GamecoreSlot
{
    uint32_t disp;
    Shim fn;
} GamecoreSlot;
void gamecore_register(const GamecoreSlot* slots); /* NULL-terminated (fn == NULL) */

/* The presence values slot 195 sets (-2 = keep), read back by slots 189/190/204/206/207. */
void presence_update(int32_t chan, int32_t sub, int32_t flag, int32_t mode, int32_t word);

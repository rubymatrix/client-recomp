/* The ground snap of remote actors (mobs, NPCs, other players): CharaActor_groundSnap, slot 0x298,
 * the per-frame position apply of every non-local chara.
 *
 * Ray start. CharaActor_probeGround casts its ray from 2 above the mover y and the snap takes the first
 * surface it meets going down, either winding. In a low cave a ceiling 1-2 over the floor is that first
 * surface, and the actor climbs onto it and up its slope until the client's 4-yalm rule warps it back.
 * We start the ray 1 above instead (RAY_UP_SHIFT): the mover y is lowered by 1 for the call and put
 * back after; groundSnap reads it only as the ray start (a hit sets y from the hit, a miss keeps last
 * frame's drawn y) and never writes the mover. The server's ClientProbe.RayUp matches. Not for actors
 * whose snap is off for good ((id & 0xE0000000) with chara+0x2af0 bit 3 clear), which take the mover y
 * as their y.
 *
 * Snap log (XIV_SNAPLOG=<file>): one line per snap of each logged actor, also wrapping
 * RaptureActor_setPosition (the position the snap writes):
 *
 *   epoch_ms id mover=(x,y,z) target=(x,y,z) drawn=(x,y,z) dy=drawn.y-mover.y [JUMP prev_y]
 *
 * epoch_ms is the wall clock (the server's METEOR_SHADOWLOG uses it too, so the two line up). mover is
 * the position the snap probes from (unshifted), target the last 0xCF hop (mover+0x50), drawn what
 * RaptureActor_setPosition got. JUMP marks a drawn y more than 0.5 off the
 * previous frame's. XIV_SNAPLOG_IDS=4410015B,4410015C limits it to those actor ids; default every
 * 0x4xxxxxxx actor (battle npcs). See client/docs/npc_movement_client.md for the snap rules. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "funcs.h"
#include "runtime.h"

#define CHARA_ID 0xdc
#define CHARA_MOVER 0xbf0
#define CHARA_SNAP_FLAGS 0x2af0
#define MOVER_TARGET 0x50
#define MAX_IDS 32
#define MAX_TRACKED 256

/* the ray starts 2 - RAY_UP_SHIFT above the mover y */
static const float RAY_UP_SHIFT = 1.0f;

static FILE* g_out;
static uint32_t g_ids[MAX_IDS];
static int g_nids;

/* the logged snap in progress (groundSnap runs on the game thread, one at a time) */
static uint32_t g_cur_id;
static int g_have_drawn;
static float g_drawn[3];

static struct
{
    uint32_t id;
    float y;
} g_last[MAX_TRACKED];

static float rdf(uint32_t a)
{
    float v;
    memcpy(&v, GUEST_PTR(a), 4);
    return v;
}

static void wrf(uint32_t a, float v) { memcpy(GUEST_PTR(a), &v, 4); }

static int logged(uint32_t id)
{
    if (!g_out)
        return 0;
    if (!g_nids)
        return (id >> 28) == 4;
    for (int i = 0; i < g_nids; i++)
        if (g_ids[i] == id)
            return 1;
    return 0;
}

/* previous drawn y of id, NAN the first time; stores y */
static float swap_last(uint32_t id, float y)
{
    int free_slot = -1;
    for (int i = 0; i < MAX_TRACKED; i++)
    {
        if (g_last[i].id == id)
        {
            float prev = g_last[i].y;
            g_last[i].y = y;
            return prev;
        }
        if (!g_last[i].id && free_slot < 0)
            free_slot = i;
    }
    if (free_slot >= 0)
    {
        g_last[free_slot].id = id;
        g_last[free_slot].y = y;
    }
    return NAN;
}

static void w_actor_setpos(Guest* g)
{
    if (g_cur_id && !g_have_drawn)
    {
        uint32_t p = rt_arg(g, 0);
        g_drawn[0] = rdf(p), g_drawn[1] = rdf(p + 4), g_drawn[2] = rdf(p + 8);
        g_have_drawn = 1;
    }
    rt_orig_actor_setpos(g);
}

static void w_snap_ground(Guest* g)
{
    uint32_t chara = g->ecx, id = rd32(chara + CHARA_ID), p = rt_arg(g, 0);
    float mx = rdf(p), my = rdf(p + 4), mz = rdf(p + 8);
    int shift = !((id & 0xE0000000u) && !(rd32(chara + CHARA_SNAP_FLAGS) & 8));
    int log = logged(id);
    if (log)
    {
        g_cur_id = id;
        g_have_drawn = 0;
    }
    if (shift)
        wrf(p + 4, my - RAY_UP_SHIFT);
    rt_orig_snap_ground(g);
    if (shift)
        wrf(p + 4, my);
    if (!log)
        return;
    g_cur_id = 0;
    if (!g_have_drawn)
        return;
    uint32_t tg = chara + CHARA_MOVER + MOVER_TARGET;
    float tx = rdf(tg), ty = rdf(tg + 4), tz = rdf(tg + 8);
    float prev = swap_last(id, g_drawn[1]);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long long ms = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    fprintf(g_out, "%lld %08X mover=(%.3f,%.3f,%.3f) target=(%.3f,%.3f,%.3f) drawn=(%.3f,%.3f,%.3f) dy=%.2f", ms, id, mx, my, mz, tx, ty, tz,
            g_drawn[0], g_drawn[1], g_drawn[2], g_drawn[1] - my);
    if (!isnan(prev) && fabsf(g_drawn[1] - prev) > 0.5f)
        fprintf(g_out, " JUMP %.2f", prev);
    fputc('\n', g_out);
}

void xiv_groundsnap_init(void)
{
    rt_wrap_snap_ground = w_snap_ground;
    const char* path = getenv("XIV_SNAPLOG");
    if (!path || !*path)
        return;
    g_out = fopen(path, "w");
    if (!g_out)
    {
        rt_log("[snaplog] cannot open %s\n", path);
        return;
    }
    setvbuf(g_out, NULL, _IOLBF, 0);
    const char* ids = getenv("XIV_SNAPLOG_IDS");
    for (const char* s = ids; s && *s && g_nids < MAX_IDS;)
    {
        char* end;
        unsigned long v = strtoul(s, &end, 16);
        if (end == s)
            break;
        g_ids[g_nids++] = (uint32_t)v;
        s = *end ? end + 1 : end;
    }
    rt_wrap_actor_setpos = w_actor_setpos;
    rt_log("[snaplog] on: %s, %s\n", path, g_nids ? "listed ids" : "every 0x4xxxxxxx actor");
}

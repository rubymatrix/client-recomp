/* The game-data layer: FFXiMain's live structures, found by pattern and read in place (game.h).
 *
 * Patterns and what they resolved to in our 2026-09-03 image (generated/FFXiMain.unpacked.dll,
 * image base 0x10000000); tests/game_test.c checks every one of these against the image:
 *
 *   pointer            source   match       resolves to
 *   entity map         Ashita   0x10003A17  +9  = 0x10480AF0  entity_t*[0x900] (static array)
 *   entity map size    Ashita   0x10095988  +3  = 0x900
 *   local player index ours     0x101CC4F0  +2  = 0x10485F7A  u16 target index (the game's own
 *                                                             "local player entity" getter)
 *   party              Ashita   0x10202BA4  +23 = 0x10663AD8  party_t (18 x 0x7C); every member's
 *                                                             AllianceInfo = 0x10664390
 *   party status icons Ashita   0x10098F56  +9  = 0x104809A0  partystatusicons_t (0xF0, rep movsd 0x3C)
 *   player             Ashita   0x1014735E  +28 = 0x10485638  player_t (the 2nd hit, 0x10147407,
 *                                                             reads MPMax: 0x1048563C; use hit 0)
 *   target             Ashita   0x10088BD6  +45 = 0x10578478  -> targetwindow_t*;
 *                                                0x1057876C (+0x2F4) -> target_t* (the
 *                                                auto-follow pattern's +9 reads the same global)
 *   character block    Ashita   0x100EB555  +1  = 0x104DFD98  -> char block; inventory_t at +0x9868
 *                               (+8, the lea's disp; the same code stores char+0x19304 = pItem)
 *   auto-follow        Ashita   0x1001F253  +25 = 0x10487F58  autofollow_t (static)
 *   cast bar           Ashita   0x1007B4E0  +5  = 0x1057817C  -> castbar_t* (null when no bar)
 *   key items (have)   Ashita   0x10097E50  +22 = 0x1047FEB8  u32[128]; packet 0x055 block 1
 *   key items (seen)   ours     0x10097EB0  +22 = 0x104800B8  u32[128]; packet 0x055 block 2
 *   job level          Ashita   0x100F0FB0  fn: char+0x1A628+job (job < 16), char+0x1A660+job (< 24)
 *   master level       Ashita   0x100F0FF0  fn: char+0x1A67C+job (job < 24)
 *   job master flags   ours     0x100F1020  fn: bit job of u32 char+0x1A678
 *   known spells       Ashita   0x100F00A0  (3rd hit) fn: char+0x2CF54 (0x80 bytes) if char+0x2CFD4
 *   known abilities    Ashita   0x100E6020  fn: char+0x2CDE8 (0x160 bytes) if char+0x2CF49 && +0x2CF4A
 *   ability recasts    Ashita   0x1022471B  +25 = 0x10666EC0  abilityrecast_t[31], u32 timer[31] at +0xF8
 *   spell recasts      Ashita   0x1022B290  +2  = 0x106671E0  int16_t[1025] (loop ends 0x106679E2)
 *   pet MP%            Ashita   0x100875F5  +10 = 0x10482F33  u8
 *   pet status block   Ashita   0x1009CEE0  +61 = 0x10482F28  0x18 bytes from packet 0x068 (+4):
 *                                                +8 u16 pet index, +0xB MP%, +0xC u32 TP
 *   SetTarget          Ashita   0x1007A617  (4 hits, use 0) call at +0x16 -> 0x10157B40, thiscall on
 *                                                [0x1057876C] (target_t*) with (entity_t*, 1, 0)
 *
 * The function-shaped ones (job levels, known spells/abilities, master flags) are decoded from
 * their instruction bytes (the offsets they index with) and read directly: no guest call, no
 * side effects. If an instruction doesn't decode as expected we fall back to calling the
 * function through guest_call (unless XI_GAME_NO_GUEST_CALL, as in the standalone test).
 *
 * Layout checks beyond the SDK's static_asserts (all from the code in this image): entity_t
 * Render.Flags0 at +0x120, Render.Flags3 at +0x12C, MonstrosityFlag at +0x210, PetTargetIndex at
 * +0x1FA; target_t Targets[0].IsModelActor at +0x21; targetwindow_t DeathFlag at +0x65;
 * castbar_t CastType at +0x20; partymember_t stride 0x7C; inventory_t pItem at +0xFA9C and
 * ContainerMaxCapacity, TreasurePoolStatus, Equipment, ContainerUpdateCounter, SearchComment,
 * CraftStatus all appear as char-block displacements (0x9868 + field) in the code.
 *
 * Per-frame read cache: not implemented. Every getter is a pointer walk plus a mapped check,
 * cheap enough for per-call use. Where it would matter: xi_game_entity_by_server_id (a linear
 * scan of 0x900 entries; addons that map ids every frame should cache a frame's id -> index
 * table), and Lua proxies over the entity array in render loops. */
#include "game.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xi.h"

#ifndef XI_GAME_NO_GUEST_CALL
#include "gthread.h"
#endif

#include "game_fields.inc"

#define XI_CONTAINERS XI_COUNT_inventory_Containers   /* 18 */
#define XI_CONTAINER_SLOTS XI_COUNT_items_Items       /* 81 */

/* --- safe guest access ----------------------------------------------------------------------- */

int xi_game_rd(uint32_t a, void* out, uint32_t n)
{
    if (a == 0 || (uint64_t)a + n > 0x100000000ull || !xi_mapped(a, n))
        return 0;
    memcpy(out, GUEST_PTR(a), n);
    return 1;
}

int xi_game_wr(uint32_t a, const void* in, uint32_t n)
{
    if (a == 0 || (uint64_t)a + n > 0x100000000ull || !xi_mapped(a, n))
        return 0;
    memcpy(GUEST_PTR(a), in, n);
    return 1;
}

uint32_t xi_game_rd32(uint32_t a)
{
    uint32_t v = 0;
    xi_game_rd(a, &v, 4);
    return v;
}

uint16_t xi_game_rd16(uint32_t a)
{
    uint16_t v = 0;
    xi_game_rd(a, &v, 2);
    return v;
}

uint8_t xi_game_rd8(uint32_t a)
{
    uint8_t v = 0;
    xi_game_rd(a, &v, 1);
    return v;
}

/* --- patterns -------------------------------------------------------------------------------- */

enum { RD_U32, RD_MATCH }; /* value = u32 at match+offset, or the match address itself */

typedef struct pattern_def
{
    const char* name;
    const char* pattern;
    int32_t offset;
    uint32_t count;
    int kind;
} pattern_def;

static const pattern_def patterns[XI_P_COUNT] = {
    [XI_P_ENTITY_MAP] = {"entitymap", "8B560C8B042A8B0485", 9, 0, RD_U32},
    [XI_P_ENTITY_COUNT] = {"entitymap.size", "4781FF????????7C??89", 3, 0, RD_U32},
    [XI_P_PLAYER_INDEX] = {"player.index", "66A1????????6685C074??0FBFC08B0485????????85C075??33C0C3", 2, 0, RD_U32},
    [XI_P_PARTY] = {"party", "0FBEC38D0C5256578BF58D0448", 23, 0, RD_U32},
    [XI_P_PARTY_ICONS] = {"party.statusicons", "B93C0000008D7004BF????????F3A5", 9, 0, RD_U32},
    [XI_P_PLAYER] = {"player", "6A018D44242C68808080806683????506683????5152E8????????A1", 28, 0, RD_U32},
    [XI_P_TARGET] = {"target.target", "53568BF18B480433DB3BCB75065E33C05B59C38B0D", 45, 0, RD_U32},
    [XI_P_CHAR] = {"inventory", "A1????????8D9488????????8990????????E9", 1, 0, RD_U32},
    [XI_P_INVENTORY_OFS] = {"inventory.offset", "A1????????8D9488????????8990????????E9", 8, 0, RD_U32},
    [XI_P_AUTOFOLLOW] = {"autofollow", "8BCFE8????????8B0D????????E8????????8BE885ED750CB9", 25, 0, RD_U32},
    [XI_P_CASTBAR] = {"castbar", "85F674??A1????????85C074??8B4808", 5, 0, RD_U32},
    [XI_P_KEYITEMS] = {"player.haskeyitem", "8B44240485C07C??3D????????7D??8B4C24088B1485", 22, 0, RD_U32},
    [XI_P_KEYITEMS_SEEN] = {"player.seenkeyitem", "8B44240485C07C??3D????????7D??8B4C24088B1485", 22, 1, RD_U32},
    [XI_P_JOBLEVEL_FN] = {"player.joblevels", "8B0D????????85C974??8B4424043C1073??25FF000000", 0, 0, RD_MATCH},
    [XI_P_MASTERLEVEL_FN] = {"player.jobmasterlevels", "8B0D????????85C974??8B4424043C1873??25FF000000", 0, 0, RD_MATCH},
    [XI_P_MASTERFLAG_FN] = {"player.jobmasterflags", "8B15????????85D274??8A4C240480F91873??B801000000D3E02382????????F7D81BC0F7D8C3", 0, 0, RD_MATCH},
    [XI_P_SPELLS_FN] = {"player.hasspell", "A1????????85C075??C38A88????????84C975??33C0C305????????C3", 0, 2, RD_MATCH},
    [XI_P_ABILITIES_FN] = {"player.hasability", "A1????????85C074??8A88????????84C974??8A88????????84C974??05????????C333C0C3", 0, 0, RD_MATCH},
    [XI_P_RECAST_ABILITY] = {"recast.abilities", "894124E9????????8B46??6A006A00508BCEE8", 25, 0, RD_U32},
    [XI_P_RECAST_SPELL] = {"recast.spells", "56BE????????66833E00??????????????662906", 2, 0, RD_U32},
    [XI_P_PET_MP] = {"player.petmp", "84C0750333C0C333C0A0????????C3", 10, 0, RD_U32},
    [XI_P_PET_BLOCK] = {"player.pettp", "8B44240C5657668B48048D70048BC183E03F83E802", 61, 0, RD_U32},
    [XI_P_SET_TARGET] = {"target.settarget", "8B0D????????83C40885C974??85C074??6A006A0150E8", 0, 0, RD_MATCH},
};

enum { ST_NEW, ST_OK, ST_MISSING };

static struct
{
    int state;
    uint32_t match; /* the pattern hit plus offset */
    uint32_t value;
} ptrs[XI_P_COUNT];

void xi_game_reset(void)
{
    memset(ptrs, 0, sizeof ptrs);
}

static void resolve(int which)
{
    uint32_t base, size, text, text_size;
    xi_image(&base, &size, &text, &text_size);
    if (text == 0 || text_size == 0)
        return; /* the image isn't loaded yet: try again next time */
    const pattern_def* p = &patterns[which];
    uint32_t a = xi_find_pattern(text, text_size, p->pattern, p->offset, p->count);
    if (a == 0)
    {
        ptrs[which].state = ST_MISSING;
        xi_log("game: pattern '%s' not found; its getters return nothing", p->name);
        return;
    }
    ptrs[which].match = a;
    ptrs[which].value = p->kind == RD_U32 ? xi_game_rd32(a) : a;
    ptrs[which].state = ST_OK;
}

uint32_t xi_game_ptr_value(int which)
{
    if (which < 0 || which >= XI_P_COUNT)
        return 0;
    if (ptrs[which].state == ST_NEW)
        resolve(which);
    return ptrs[which].state == ST_OK ? ptrs[which].value : 0;
}

uint32_t xi_game_ptr_match(int which)
{
    if (which < 0 || which >= XI_P_COUNT)
        return 0;
    xi_game_ptr_value(which);
    return ptrs[which].match;
}

const char* xi_game_ptr_name(int which)
{
    return which >= 0 && which < XI_P_COUNT ? patterns[which].name : NULL;
}

/* --- descriptors and path lookup ------------------------------------------------------------- */

int xi_game_struct_count(void)
{
    return XI_S_COUNT;
}

const xi_struct* xi_game_struct(int id)
{
    return id >= 0 && id < XI_S_COUNT ? &xi_game_structs[id] : NULL;
}

int xi_game_struct_id(const char* name)
{
    if (!name)
        return -1;
    for (int i = 0; i < XI_S_COUNT; i++)
        if (strcmp(xi_game_structs[i].name, name) == 0)
            return i;
    return -1;
}

const xi_field* xi_game_field_at(int index)
{
    return index >= 0 && index < (int)(sizeof xi_game_fields / sizeof xi_game_fields[0]) ? &xi_game_fields[index] : NULL;
}

static const xi_field* field_in(int sid, const char* name, size_t n)
{
    const xi_struct* s = xi_game_struct(sid);
    if (!s)
        return NULL;
    for (unsigned i = 0; i < s->nfields; i++)
    {
        const xi_field* f = &xi_game_fields[s->first + i];
        if (strlen(f->name) == n && memcmp(f->name, name, n) == 0)
            return f;
    }
    return NULL;
}

/* Every plain path of every struct (nested non-array structs flattened with '.'), hashed. */
typedef struct path_entry
{
    char* path;
    int16_t sid;
    const xi_field* f;
    uint32_t extra;
} path_entry;

#define HASH_SIZE 8192u
static path_entry* entries;
static int nentries, capentries;
static int32_t hash_slots[HASH_SIZE]; /* entry index + 1 */
static int hash_built;

static uint32_t hash_path(int sid, const char* s, size_t n)
{
    uint32_t h = 2166136261u ^ (uint32_t)sid;
    for (size_t i = 0; i < n; i++)
        h = (h ^ (uint8_t)s[i]) * 16777619u;
    return h;
}

static void add_path(int sid, const char* path, const xi_field* f, uint32_t extra)
{
    if (nentries == capentries)
    {
        int cap = capentries ? capentries * 2 : 2048;
        path_entry* e = realloc(entries, (size_t)cap * sizeof *e);
        if (!e)
            return;
        entries = e;
        capentries = cap;
    }
    size_t n = strlen(path);
    char* copy = malloc(n + 1);
    if (!copy)
        return;
    memcpy(copy, path, n + 1);
    uint32_t h = hash_path(sid, path, n);
    for (uint32_t k = 0; k < HASH_SIZE; k++)
    {
        uint32_t slot = (h + k) & (HASH_SIZE - 1);
        if (hash_slots[slot] == 0)
        {
            entries[nentries] = (path_entry){copy, (int16_t)sid, f, extra};
            hash_slots[slot] = ++nentries;
            return;
        }
    }
    free(copy); /* table full: the walker still finds it */
}

static void add_struct_paths(int root, int sid, const char* prefix, uint32_t extra, int depth)
{
    const xi_struct* s = xi_game_struct(sid);
    if (!s || depth > 6)
        return;
    for (unsigned i = 0; i < s->nfields; i++)
    {
        const xi_field* f = &xi_game_fields[s->first + i];
        char path[256];
        int n = snprintf(path, sizeof path, "%s%s%s", prefix, *prefix ? "." : "", f->name);
        if (n <= 0 || n >= (int)sizeof path)
            continue;
        add_path(root, path, f, extra);
        if (f->type == XI_T_STRUCT && f->count == 1)
            add_struct_paths(root, f->sub, path, extra + f->offset, depth + 1);
    }
}

static void build_hash(void)
{
    if (hash_built)
        return;
    hash_built = 1;
    for (int sid = 0; sid < XI_S_COUNT; sid++)
        add_struct_paths(sid, sid, "", 0, 0);
}

static int find_entry(int sid, const char* path)
{
    build_hash();
    size_t n = strlen(path);
    uint32_t h = hash_path(sid, path, n);
    for (uint32_t k = 0; k < HASH_SIZE; k++)
    {
        uint32_t slot = (h + k) & (HASH_SIZE - 1);
        int32_t e = hash_slots[slot];
        if (e == 0)
            return -1;
        path_entry* pe = &entries[e - 1];
        if (pe->sid == sid && strcmp(pe->path, path) == 0)
            return e - 1;
    }
    return -1;
}

/* "A.B[3].C": walks nested structs, applying subscripts of struct arrays. A trailing "[n]" is
 * allowed on a struct array only (extra then points at element n); scalar arrays take `sub`. */
static const xi_field* walk(int sid, const char* path, uint32_t* extra)
{
    uint32_t off = 0;
    const char* p = path;
    for (;;)
    {
        size_t n = strcspn(p, ".[");
        const xi_field* f = field_in(sid, p, n);
        if (!f)
            return NULL;
        p += n;
        uint32_t idx = 0;
        int subscripted = 0;
        if (*p == '[')
        {
            char* end;
            unsigned long v = strtoul(p + 1, &end, 0);
            if (*end != ']' || v >= f->count || f->type != XI_T_STRUCT)
                return NULL;
            idx = (uint32_t)v;
            subscripted = 1;
            p = end + 1;
        }
        if (*p == '\0')
        {
            *extra = off + (subscripted ? idx * f->stride : 0);
            return f;
        }
        if (*p != '.' || f->type != XI_T_STRUCT)
            return NULL;
        off += f->offset + idx * f->stride;
        sid = f->sub;
        p++;
    }
}

const xi_field* xi_game_lookup(int sid, const char* path, uint32_t* extra)
{
    uint32_t dummy;
    if (!extra)
        extra = &dummy;
    *extra = 0;
    if (!path || sid < 0 || sid >= XI_S_COUNT)
        return NULL;
    if (!strchr(path, '['))
    {
        int e = find_entry(sid, path);
        if (e >= 0)
        {
            *extra = entries[e].extra;
            return entries[e].f;
        }
    }
    return walk(sid, path, extra);
}

int xi_game_path_id(int sid, const char* path)
{
    if (!path || sid < 0 || sid >= XI_S_COUNT || strchr(path, '['))
        return -1;
    return find_entry(sid, path);
}

const xi_field* xi_game_path_field(int id, uint32_t* extra, int* sid)
{
    if (id < 0 || id >= nentries)
        return NULL;
    if (extra)
        *extra = entries[id].extra;
    if (sid)
        *sid = entries[id].sid;
    return entries[id].f;
}

/* --- generic read / write -------------------------------------------------------------------- */

static uint32_t type_size(int t)
{
    switch (t)
    {
    case XI_T_U8: case XI_T_I8: return 1;
    case XI_T_U16: case XI_T_I16: return 2;
    case XI_T_U64: return 8;
    default: return 4;
    }
}

static int read_scalar(int t, uint32_t a, double* out)
{
    uint8_t b[8];
    if (!xi_game_rd(a, b, type_size(t)))
        return 0;
    switch (t)
    {
    case XI_T_U8: *out = b[0]; break;
    case XI_T_I8: *out = (int8_t)b[0]; break;
    case XI_T_U16: { uint16_t v; memcpy(&v, b, 2); *out = v; break; }
    case XI_T_I16: { int16_t v; memcpy(&v, b, 2); *out = v; break; }
    case XI_T_I32: { int32_t v; memcpy(&v, b, 4); *out = v; break; }
    case XI_T_U64: { uint64_t v; memcpy(&v, b, 8); *out = (double)v; break; }
    case XI_T_F32: { float v; memcpy(&v, b, 4); *out = v; break; }
    default: { uint32_t v; memcpy(&v, b, 4); *out = v; break; }
    }
    return 1;
}

static int write_scalar(int t, uint32_t a, double v)
{
    uint8_t b[8];
    uint32_t n = type_size(t);
    if (t == XI_T_F32)
    {
        float f = (float)v;
        memcpy(b, &f, 4);
    }
    else
    {
        /* integers wrap like a C cast from int64 (negative values into unsigned fields too) */
        int64_t i = isfinite(v) ? (int64_t)v : 0;
        if (v >= 9223372036854775807.0)
            i = (int64_t)(uint64_t)v;
        uint64_t u = (uint64_t)i;
        memcpy(b, &u, 8); /* little-endian: the low n bytes */
    }
    return xi_game_wr(a, b, n);
}

uint32_t xi_game_field_addr(const xi_field* f, uint32_t base, uint32_t extra, int32_t sub)
{
    if (!f || base == 0)
        return 0;
    if (sub >= 0 && (uint32_t)sub >= f->count)
        return 0;
    return base + extra + f->offset + (sub > 0 ? (uint32_t)sub * f->stride : 0);
}

int xi_game_read(const xi_field* f, uint32_t base, uint32_t extra, int32_t sub, xi_value* out)
{
    out->kind = XI_V_NIL;
    out->len = 0;
    out->sid = -1;
    uint32_t a = xi_game_field_addr(f, base, extra, sub);
    if (a == 0)
        return XI_V_NIL;
    switch (f->type)
    {
    case XI_T_STRUCT:
        if (!xi_mapped(a, f->stride))
            return XI_V_NIL;
        out->kind = XI_V_ADDR;
        out->num = a;
        out->sid = f->sub;
        return out->kind;
    case XI_T_CHARS:
    case XI_T_BYTES:
        if (sub >= 0)
        {
            if (!read_scalar(f->elem, a, &out->num))
                return XI_V_NIL;
            out->kind = XI_V_NUM;
            return out->kind;
        }
        else
        {
            uint32_t n = f->count < sizeof out->str ? f->count : (uint32_t)sizeof out->str - 1;
            if (!xi_game_rd(a, out->str, n))
                return XI_V_NIL;
            if (f->type == XI_T_CHARS)
            {
                uint32_t k = 0;
                while (k < n && out->str[k])
                    k++;
                n = k;
            }
            out->str[n] = '\0';
            out->len = n;
            out->kind = XI_V_STR;
            return out->kind;
        }
    case XI_T_BITS:
    {
        double unit;
        if (!read_scalar(f->elem, a, &unit))
            return XI_V_NIL;
        uint64_t u = (uint64_t)unit;
        uint64_t mask = f->bit_width >= 64 ? ~0ull : ((1ull << f->bit_width) - 1);
        out->num = (double)((u >> f->bit_lo) & mask);
        out->kind = XI_V_NUM;
        return out->kind;
    }
    default:
        if (f->count > 1 && sub < 0)
        {
            if (!xi_mapped(a, f->count * f->stride))
                return XI_V_NIL;
            out->kind = XI_V_ARRAY;
            out->num = a;
            out->len = f->count;
            return out->kind;
        }
        if (!read_scalar(f->type, a, &out->num))
            return XI_V_NIL;
        out->kind = XI_V_NUM;
        return out->kind;
    }
}

int xi_game_write_num(const xi_field* f, uint32_t base, uint32_t extra, int32_t sub, double v)
{
    uint32_t a = xi_game_field_addr(f, base, extra, sub);
    if (a == 0)
        return 0;
    switch (f->type)
    {
    case XI_T_STRUCT:
        return 0;
    case XI_T_CHARS:
    case XI_T_BYTES:
        return sub >= 0 ? write_scalar(f->elem, a, v) : 0;
    case XI_T_BITS:
    {
        double unit;
        if (!read_scalar(f->elem, a, &unit))
            return 0;
        uint64_t u = (uint64_t)unit;
        uint64_t mask = f->bit_width >= 64 ? ~0ull : ((1ull << f->bit_width) - 1);
        uint64_t nv = (uint64_t)(int64_t)v & mask;
        u = (u & ~(mask << f->bit_lo)) | (nv << f->bit_lo);
        return write_scalar(f->elem, a, (double)u);
    }
    default:
        if (f->count > 1 && sub < 0)
            return 0; /* a whole numeric array takes a subscript */
        return write_scalar(f->type, a, v);
    }
}

int xi_game_write_str(const xi_field* f, uint32_t base, uint32_t extra, const char* s, size_t len)
{
    if (!f || (f->type != XI_T_CHARS && f->type != XI_T_BYTES))
        return 0;
    uint32_t a = xi_game_field_addr(f, base, extra, -1);
    if (a == 0 || !xi_mapped(a, f->count))
        return 0;
    uint8_t buf[1024];
    uint32_t n = f->count < sizeof buf ? f->count : (uint32_t)sizeof buf;
    if (f->type == XI_T_CHARS)
    {
        memset(buf, 0, n);
        size_t k = len < n - 1 ? len : n - 1; /* keep a terminator */
        memcpy(buf, s, k);
    }
    else
    {
        if (!xi_game_rd(a, buf, n))
            return 0;
        memcpy(buf, s, len < n ? len : n);
    }
    return xi_game_wr(a, buf, n);
}

/* --- structures ------------------------------------------------------------------------------ */

uint32_t xi_game_entity_count(void)
{
    uint32_t n = xi_game_ptr_value(XI_P_ENTITY_COUNT);
    return n <= 0x10000 ? n : 0; /* sanity: the immediate is 0x900 */
}

uint32_t xi_game_entity(uint32_t index)
{
    uint32_t map = xi_game_ptr_value(XI_P_ENTITY_MAP);
    if (map == 0 || index >= xi_game_entity_count())
        return 0;
    uint32_t e = xi_game_rd32(map + index * 4);
    return e && xi_mapped(e, XI_SIZE_entity_t) ? e : 0;
}

int32_t xi_game_player_index(void)
{
    uint32_t g = xi_game_ptr_value(XI_P_PLAYER_INDEX);
    if (g == 0)
        return -1;
    uint16_t v;
    if (!xi_game_rd(g, &v, 2))
        return -1;
    int32_t i = (int16_t)v; /* the game sign-extends it */
    return i > 0 && (uint32_t)i < xi_game_entity_count() ? i : -1;
}

int32_t xi_game_entity_by_server_id(uint32_t id)
{
    uint32_t map = xi_game_ptr_value(XI_P_ENTITY_MAP), n = xi_game_entity_count();
    if (map == 0 || id == 0 || !xi_mapped(map, n * 4))
        return -1;
    const uint32_t* arr = (const uint32_t*)GUEST_PTR(map);
    for (uint32_t i = 0; i < n; i++)
    {
        uint32_t e = arr[i], sid;
        if (e && xi_game_rd(e + XI_OFS_entity_ServerId, &sid, 4) && sid == id)
            return (int32_t)i;
    }
    return -1;
}

uint32_t xi_game_party(void)
{
    uint32_t p = xi_game_ptr_value(XI_P_PARTY);
    return p && xi_mapped(p, XI_SIZE_party_t) ? p : 0;
}

uint32_t xi_game_party_member(uint32_t i)
{
    uint32_t p = xi_game_party();
    return p && i < 18 ? p + i * XI_SIZE_partymember_t : 0;
}

uint32_t xi_game_alliance(void)
{
    uint32_t m = xi_game_party_member(0);
    uint32_t a = m ? xi_game_rd32(m) : 0; /* Members[0].AllianceInfo */
    return a && xi_mapped(a, XI_SIZE_allianceinfo_t) ? a : 0;
}

uint32_t xi_game_party_icons(void)
{
    uint32_t p = xi_game_ptr_value(XI_P_PARTY_ICONS);
    return p && xi_mapped(p, XI_SIZE_partystatusicons_t) ? p : 0;
}

uint32_t xi_game_player(void)
{
    uint32_t p = xi_game_ptr_value(XI_P_PLAYER);
    return p && xi_mapped(p, XI_SIZE_player_t) ? p : 0;
}

uint32_t xi_game_target_window(void)
{
    uint32_t g = xi_game_ptr_value(XI_P_TARGET);
    uint32_t p = g ? xi_game_rd32(g) : 0;
    return p && xi_mapped(p, XI_SIZE_targetwindow_t) ? p : 0;
}

uint32_t xi_game_target(void)
{
    uint32_t g = xi_game_ptr_value(XI_P_TARGET);
    uint32_t p = g ? xi_game_rd32(g + 0x2F4) : 0;
    return p && xi_mapped(p, XI_SIZE_target_t) ? p : 0;
}

uint32_t xi_game_char(void)
{
    uint32_t g = xi_game_ptr_value(XI_P_CHAR);
    return g ? xi_game_rd32(g) : 0;
}

uint32_t xi_game_inventory(void)
{
    uint32_t c = xi_game_char(), ofs = xi_game_ptr_value(XI_P_INVENTORY_OFS);
    if (c == 0 || ofs == 0 || ofs > 0x100000)
        return 0;
    return xi_mapped(c + ofs, XI_SIZE_inventory_t) ? c + ofs : 0;
}

uint32_t xi_game_container_item(uint32_t container, uint32_t slot)
{
    uint32_t inv = xi_game_inventory();
    if (inv == 0 || container >= XI_CONTAINERS || slot >= XI_CONTAINER_SLOTS)
        return 0;
    return inv + container * XI_SIZE_items_t + slot * XI_SIZE_item_t;
}

uint32_t xi_game_autofollow(void)
{
    uint32_t p = xi_game_ptr_value(XI_P_AUTOFOLLOW);
    return p && xi_mapped(p, XI_SIZE_autofollow_t) ? p : 0;
}

uint32_t xi_game_castbar(void)
{
    uint32_t g = xi_game_ptr_value(XI_P_CASTBAR);
    uint32_t p = g ? xi_game_rd32(g) : 0;
    return p && xi_mapped(p, XI_SIZE_castbar_t) ? p : 0;
}

int xi_game_party_member_icons(uint32_t member, int16_t out[32])
{
    uint32_t p = xi_game_party_icons();
    if (p == 0 || member >= 5)
        return 0;
    uint8_t e[48]; /* statusiconsentry_t: ServerId, TargetIndex, u64 BitMask, u8 StatusIcons[32] */
    if (!xi_game_rd(p + member * 48, e, 48))
        return 0;
    uint64_t mask;
    memcpy(&mask, e + 8, 8);
    for (int i = 0; i < 32; i++)
        out[i] = (int16_t)(e[16 + i] | (((mask >> (2 * i)) & 3) << 8));
    return 1;
}

int xi_game_container_count(uint32_t container)
{
    uint32_t inv = xi_game_inventory();
    if (inv == 0 || container >= XI_CONTAINERS)
        return -1;
    int n = 0;
    for (uint32_t s = 1; s < XI_CONTAINER_SLOTS; s++)
    {
        uint16_t id = xi_game_rd16(inv + container * XI_SIZE_items_t + s * XI_SIZE_item_t);
        if (id != 0 && id != 0xFFFF)
            n++;
    }
    return n;
}

int xi_game_container_max(uint32_t container)
{
    uint32_t inv = xi_game_inventory();
    if (inv == 0 || container >= XI_CONTAINERS)
        return -1;
    uint8_t v;
    return xi_game_rd(inv + XI_OFS_inventory_ContainerMaxCapacity + container, &v, 1) ? v : -1;
}

/* --- bit fields and function-shaped pointers ------------------------------------------------- */

static int bit_at(uint32_t table, uint32_t nbytes, uint32_t bit)
{
    if (table == 0 || bit / 8 >= nbytes)
        return 0;
    uint8_t b;
    if (!xi_game_rd(table + bit / 8, &b, 1))
        return 0;
    return (b >> (bit & 7)) & 1;
}

static int key_bit(int which, uint32_t id)
{
    uint32_t t = xi_game_ptr_value(which);
    if (t == 0)
        return -1;
    if (id >= 128 * 32)
        return 0;
    uint32_t w;
    if (!xi_game_rd(t + (id / 32) * 4, &w, 4))
        return -1;
    return (w >> (id & 31)) & 1;
}

int xi_game_key_item(uint32_t id) { return key_bit(XI_P_KEYITEMS, id); }
int xi_game_key_item_seen(uint32_t id) { return key_bit(XI_P_KEYITEMS_SEEN, id); }

/* An imm32 at fn+at, if the bytes before it are `op` (the instruction's opcode and ModRM/SIB). */
static int imm_after(uint32_t fn, uint32_t at, const uint8_t* op, uint32_t oplen, uint32_t* imm)
{
    uint8_t b[16];
    if (fn == 0 || oplen > 8 || !xi_game_rd(fn + at - oplen, b, oplen + 4))
        return 0;
    if (memcmp(b, op, oplen) != 0)
        return 0;
    memcpy(imm, b + oplen, 4);
    return 1;
}

#ifndef XI_GAME_NO_GUEST_CALL
static uint32_t call_guest(uint32_t fn, unsigned nargs, uint32_t a0)
{
    uint32_t args[1] = {a0};
    return guest_call(fn, nargs, args);
}
#endif

/* The spell / ability field: {bits, nbytes} if the game says its data has arrived. */
static uint32_t spells_field(uint32_t* nbytes)
{
    uint32_t fn = xi_game_ptr_value(XI_P_SPELLS_FN), g, flag, bits;
    static const uint8_t op_a1[] = {0xA1}, op_flag[] = {0x8A, 0x88}, op_add[] = {0x05};
    if (!imm_after(fn, 1, op_a1, 1, &g) || !imm_after(fn, 12, op_flag, 2, &flag) || !imm_after(fn, 24, op_add, 1, &bits))
    {
#ifndef XI_GAME_NO_GUEST_CALL
        if (fn)
        {
            *nbytes = 0x80;
            return call_guest(fn, 0, 0);
        }
#endif
        return 0;
    }
    uint32_t c = xi_game_rd32(g);
    if (c == 0 || xi_game_rd8(c + flag) == 0)
        return 0;
    *nbytes = flag > bits && flag - bits <= 0x400 ? flag - bits : 0x80;
    return c + bits;
}

static uint32_t abilities_field(uint32_t* nbytes)
{
    uint32_t fn = xi_game_ptr_value(XI_P_ABILITIES_FN), g, f1, f2, bits;
    static const uint8_t op_a1[] = {0xA1}, op_flag[] = {0x8A, 0x88}, op_add[] = {0x05};
    if (!imm_after(fn, 1, op_a1, 1, &g) || !imm_after(fn, 11, op_flag, 2, &f1) || !imm_after(fn, 21, op_flag, 2, &f2) ||
        !imm_after(fn, 30, op_add, 1, &bits))
    {
#ifndef XI_GAME_NO_GUEST_CALL
        if (fn)
        {
            *nbytes = 0x160;
            return call_guest(fn, 0, 0);
        }
#endif
        return 0;
    }
    uint32_t c = xi_game_rd32(g);
    if (c == 0 || xi_game_rd8(c + f1) == 0 || xi_game_rd8(c + f2) == 0)
        return 0;
    *nbytes = 0x160; /* the setter bounds bit/8 < 0x160 (0xB00 bits) */
    return c + bits;
}

int xi_game_has_spell_data(void)
{
    uint32_t n;
    return spells_field(&n) != 0;
}

int xi_game_spell_known(uint32_t id)
{
    uint32_t n = 0, t = spells_field(&n);
    return bit_at(t, n, id);
}

int xi_game_has_ability_data(void)
{
    uint32_t n;
    return abilities_field(&n) != 0;
}

int xi_game_ability_bit(uint32_t bit)
{
    uint32_t n = 0, t = abilities_field(&n);
    return bit_at(t, n, bit);
}

int xi_game_job_level(uint32_t job)
{
    uint32_t fn = xi_game_ptr_value(XI_P_JOBLEVEL_FN), g, lo, hi;
    static const uint8_t op_ecx[] = {0x8B, 0x0D}, op_mov[] = {0x8A, 0x84, 0x08};
    if (job >= 0x100)
        return -1;
    if (imm_after(fn, 2, op_ecx, 2, &g) && imm_after(fn, 0x1A, op_mov, 3, &lo) && imm_after(fn, 0x2B, op_mov, 3, &hi) &&
        xi_game_rd8(fn + 0x0F) == 0x10 && xi_game_rd8(fn + 0x20) == 0x18)
    {
        uint32_t c = xi_game_rd32(g);
        uint8_t v = 0;
        if (c == 0 || (job < 0x18 && !xi_game_rd(c + (job < 0x10 ? lo : hi) + job, &v, 1)))
            return -1;
        return v; /* jobs >= 0x18: 0, as the game returns */
    }
#ifndef XI_GAME_NO_GUEST_CALL
    if (fn)
        return (int)(call_guest(fn, 1, job) & 0xFF);
#endif
    return -1;
}

int xi_game_master_level(uint32_t job)
{
    uint32_t fn = xi_game_ptr_value(XI_P_MASTERLEVEL_FN), g, lo;
    static const uint8_t op_ecx[] = {0x8B, 0x0D}, op_mov[] = {0x8A, 0x84, 0x08};
    if (job >= 0x100)
        return -1;
    if (imm_after(fn, 2, op_ecx, 2, &g) && imm_after(fn, 0x1A, op_mov, 3, &lo) && xi_game_rd8(fn + 0x0F) == 0x18)
    {
        uint32_t c = xi_game_rd32(g);
        uint8_t v = 0;
        if (c == 0 || (job < 0x18 && !xi_game_rd(c + lo + job, &v, 1)))
            return -1;
        return v;
    }
#ifndef XI_GAME_NO_GUEST_CALL
    if (fn)
        return (int)(call_guest(fn, 1, job) & 0xFF);
#endif
    return -1;
}

int64_t xi_game_master_flags(void)
{
    uint32_t fn = xi_game_ptr_value(XI_P_MASTERFLAG_FN), g, ofs;
    static const uint8_t op_edx[] = {0x8B, 0x15}, op_and[] = {0x23, 0x82};
    if (!imm_after(fn, 2, op_edx, 2, &g) || !imm_after(fn, 28, op_and, 2, &ofs))
        return -1; /* a bit test per job; no single value to call for */
    uint32_t c = xi_game_rd32(g), v;
    if (c == 0 || !xi_game_rd(c + ofs, &v, 4))
        return -1;
    return v;
}

int xi_game_ability_recast(uint32_t i, uint32_t* timer, uint32_t* timer_id, uint32_t* recast, uint32_t* calc1,
                           int32_t* calc2)
{
    uint32_t o = xi_game_ptr_value(XI_P_RECAST_ABILITY);
    uint8_t e[8];
    uint32_t t;
    if (o == 0 || i >= 31 || !xi_game_rd(o + i * 8, e, 8) || !xi_game_rd(o + 31 * 8 + i * 4, &t, 4))
        return 0;
    uint16_t r;
    int16_t c2;
    memcpy(&r, e, 2);
    memcpy(&c2, e + 4, 2);
    if (timer) *timer = t;
    if (timer_id) *timer_id = e[3];
    if (recast) *recast = r;
    if (calc1) *calc1 = e[2];
    if (calc2) *calc2 = c2;
    return 1;
}

int32_t xi_game_spell_recast(uint32_t id)
{
    uint32_t t = xi_game_ptr_value(XI_P_RECAST_SPELL);
    int16_t v;
    if (t == 0 || id > 1024 || !xi_game_rd(t + id * 2, &v, 2))
        return -1;
    return v > 0 ? v : 0;
}

int xi_game_pet(uint32_t* mpp, uint32_t* tp, uint32_t* index)
{
    uint32_t blk = xi_game_ptr_value(XI_P_PET_BLOCK), mp = xi_game_ptr_value(XI_P_PET_MP);
    int32_t pi = xi_game_player_index();
    uint32_t pe = pi >= 0 ? xi_game_entity((uint32_t)pi) : 0;
    if (blk == 0 || pe == 0)
        return 0;
    uint16_t want = xi_game_rd16(pe + XI_OFS_entity_PetTargetIndex), have;
    if (want == 0 || !xi_game_rd(blk + 8, &have, 2) || have != want)
        return 0; /* the game's own check (0x100875D0) */
    if (mpp) *mpp = mp ? xi_game_rd8(mp) : xi_game_rd8(blk + 0xB);
    if (tp) *tp = xi_game_rd32(blk + 0xC);
    if (index) *index = want;
    return 1;
}

int xi_game_set_target(uint32_t index)
{
#ifdef XI_GAME_NO_GUEST_CALL
    (void)index;
    return 0;
#else
    uint32_t site = xi_game_ptr_value(XI_P_SET_TARGET), g, rel;
    static const uint8_t op_ecx[] = {0x8B, 0x0D}, op_call[] = {0x50, 0xE8};
    if (!imm_after(site, 2, op_ecx, 2, &g) || !imm_after(site, 0x17, op_call, 2, &rel))
        return 0;
    uint32_t fn = site + 0x1B + rel, target = xi_game_rd32(g), ent = xi_game_entity(index);
    if (target == 0 || ent == 0)
        return 0;
    uint32_t args[3] = {ent, 1, 0};
    guest_thiscall(fn, target, 3, args);
    return 1;
#endif
}

/* Ashita resolves "player.loginstatus" to a manager object ([0x10621808] in this image) whose
 * status field isn't in the SDK, so we infer the same three states from what we can see. */
int xi_game_login_status(void)
{
    if (xi_game_char() == 0)
        return 0;
    uint32_t pl = xi_game_player();
    uint32_t zoning = pl ? xi_game_rd32(pl + XI_OFS_player_IsZoning) : 0;
    int32_t pi = xi_game_player_index();
    if (zoning || pi < 0 || xi_game_entity((uint32_t)pi) == 0)
        return 1;
    return 2;
}

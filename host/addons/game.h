/* The game-data layer of the addon host: FFXiMain's live structures, read and written in place.
 *
 * Shaped after Ashita v4's IMemoryManager (IEntity, IParty, IPlayer, ITarget, IInventory,
 * IAutoFollow, ICastBar, IRecast) and general enough for Windower's windower.ffxi getters.
 *
 * - Every structure is found lazily, at first use, by a byte pattern over the image's .text
 *   (Ashita's own ashita.pointers.ini patterns where it has one), never by a fixed address. The
 *   pattern result is cached; the pointers it leads to are re-read on every call, because the
 *   structures move and are null before login and while zoning.
 * - Every guest read and write is checked with xi_mapped first: a bad pointer reads as "absent"
 *   (functions return 0 / nil), never a fault.
 * - Field layouts come from Ashita's SDK headers (plugins/sdk/ffxi/ headers) through
 *   tools/gen_game_fields.py -> game_fields.inc: one descriptor table and one generic
 *   getter/setter path (xi_game_read / xi_game_write) instead of a function per field.
 *
 * Addresses here are guest addresses (uint32_t). Callers hold the guest lock (addons run on the
 * game thread with it held). */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- field descriptors (game_fields.inc) ----------------------------------------------------- */

typedef enum xi_type
{
    XI_T_U8,
    XI_T_I8,
    XI_T_U16,
    XI_T_I16,
    XI_T_U32,
    XI_T_I32,
    XI_T_U64,
    XI_T_F32,
    XI_T_PTR,    /* uintptr_t in the SDK: a 32-bit guest pointer */
    XI_T_CHARS,  /* char/int8_t array (or a name in uint8_t): a NUL-terminated string without a subscript */
    XI_T_BYTES,  /* uint8_t array: raw bytes (a Lua string) without a subscript, a number with one */
    XI_T_STRUCT, /* nested struct (or array of them); `sub` is its struct id */
    XI_T_BITS,   /* bitfield: `bit_width` bits from `bit_lo` of a unit of type `elem`, `stride` bytes */
} xi_type;

typedef struct xi_field
{
    const char* name;
    uint32_t offset;   /* from the start of the containing struct */
    uint8_t type;      /* xi_type */
    uint8_t elem;      /* element type of CHARS/BYTES arrays, unit type of BITS, else == type */
    uint16_t count;    /* array length (1 for scalars) */
    uint32_t stride;   /* element size in bytes */
    uint8_t bit_lo, bit_width;
    int16_t sub;       /* struct id of an XI_T_STRUCT field, else -1 */
} xi_field;

typedef struct xi_struct
{
    const char* name;
    uint32_t size;
    uint16_t first, nfields; /* its fields are xi_game_field_at(first .. first + nfields - 1) */
} xi_struct;

int xi_game_struct_count(void);
const xi_struct* xi_game_struct(int id);
int xi_game_struct_id(const char* name); /* -1 if unknown */
const xi_field* xi_game_field_at(int index);

/* A field by path within a struct: "ServerId", "Movement.LocalPosition.X", "Look.Hair",
 * "AbilityInfo[3].Recast", "Targets[1].Index", "UnityInfo.Bits.Points". *extra receives the
 * offset of the field's containing struct from `sid`'s start (nonzero for nested paths). Plain
 * (bracket-free) paths are hashed; bracketed ones are walked. NULL if the path doesn't exist. */
const xi_field* xi_game_lookup(int sid, const char* path, uint32_t* extra);

/* The same, as a small integer handle for hot paths (Lua caches these): >= 0, or -1. Only plain
 * paths get handles. */
int xi_game_path_id(int sid, const char* path);
const xi_field* xi_game_path_field(int id, uint32_t* extra, int* sid);

/* --- values ---------------------------------------------------------------------------------- */

typedef enum xi_vkind
{
    XI_V_NIL,  /* absent: null structure, unmapped memory, bad subscript */
    XI_V_NUM,  /* num */
    XI_V_STR,  /* str[0 .. len) (CHARS stop at the first NUL; BYTES are the whole array) */
    XI_V_ADDR, /* a nested struct: num is its guest address, sid its struct id */
    XI_V_ARRAY /* a numeric array read without a subscript: the caller iterates 0 .. len-1 */
} xi_vkind;

typedef struct xi_value
{
    int kind;
    int sid;
    double num;    /* U64 loses precision above 2^53: use the dedicated decoders (party icons) */
    uint32_t len;
    char str[512]; /* longest string field is 0x84 bytes (SearchComment) */
} xi_value;

/* Reads field f of the struct at guest address `base` (+ extra). sub < 0: no subscript (whole
 * string for CHARS/BYTES, XI_V_ARRAY for numeric arrays, XI_V_ADDR for structs). Returns kind. */
int xi_game_read(const xi_field* f, uint32_t base, uint32_t extra, int32_t sub, xi_value* out);
/* Writes a number (or, for CHARS/BYTES without a subscript, a string of `len` bytes). Returns 1
 * if written. CHARS are NUL-terminated within the field and zero-filled after. */
int xi_game_write_num(const xi_field* f, uint32_t base, uint32_t extra, int32_t sub, double v);
int xi_game_write_str(const xi_field* f, uint32_t base, uint32_t extra, const char* s, size_t len);
/* The guest address of field f (element `sub`, or element 0 if sub < 0), 0 if out of range. */
uint32_t xi_game_field_addr(const xi_field* f, uint32_t base, uint32_t extra, int32_t sub);

/* --- safe guest access ----------------------------------------------------------------------- */

int xi_game_rd(uint32_t a, void* out, uint32_t n); /* 1 if [a, a+n) is mapped and was copied */
int xi_game_wr(uint32_t a, const void* in, uint32_t n);
uint32_t xi_game_rd32(uint32_t a); /* 0 if unmapped */
uint16_t xi_game_rd16(uint32_t a);
uint8_t xi_game_rd8(uint32_t a);

/* --- where things are (guest addresses; 0 = not available right now) ------------------------ */

typedef enum xi_game_ptr
{
    XI_P_ENTITY_MAP,     /* entity_t* [entity count] (static array) */
    XI_P_ENTITY_COUNT,   /* the entity map's size (0x900), an immediate */
    XI_P_PLAYER_INDEX,   /* u16 global: the local player's target index (ours, not Ashita's) */
    XI_P_PARTY,          /* party_t (static) */
    XI_P_PARTY_ICONS,    /* partystatusicons_t (static) */
    XI_P_PLAYER,         /* player_t (static) */
    XI_P_TARGET,         /* global holding targetwindow_t*; target_t* is the global 0x2F4 after it */
    XI_P_CHAR,           /* global holding the character block pointer */
    XI_P_INVENTORY_OFS,  /* inventory_t's offset in the character block, an immediate */
    XI_P_AUTOFOLLOW,     /* autofollow_t (static) */
    XI_P_CASTBAR,        /* global holding castbar_t* */
    XI_P_KEYITEMS,       /* u32[128] "have" bits (4096 key items) */
    XI_P_KEYITEMS_SEEN,  /* u32[128] "examined" bits */
    XI_P_JOBLEVEL_FN,    /* uint8_t __cdecl (int job): match = the function */
    XI_P_MASTERLEVEL_FN, /* uint8_t __cdecl (int job) */
    XI_P_MASTERFLAG_FN,  /* bool __cdecl (int job): tests the job-master bit mask */
    XI_P_SPELLS_FN,      /* uint8_t* __cdecl (void): known-spell bits or null */
    XI_P_ABILITIES_FN,   /* uint8_t* __cdecl (void): known ability bits or null */
    XI_P_RECAST_ABILITY, /* ability recast object: abilityrecast_t[31], then u32 timers[31] */
    XI_P_RECAST_SPELL,   /* int16_t[1025] spell recast timers (1/60 s) */
    XI_P_PET_MP,         /* u8 pet MP%, in the pet status block (0x068 copy) */
    XI_P_PET_BLOCK,      /* pet status block: +8 u16 pet index, +0xB MP%, +0xC u32 TP */
    XI_P_SET_TARGET,     /* a call site of target_t::SetTarget(entity_t*, 1, 0) (thiscall) */
    XI_P_COUNT
} xi_game_ptr;

/* The resolved value for a pointer (pattern hit, then its read), cached; 0 if the pattern
 * didn't hit. Resolves on first use. */
uint32_t xi_game_ptr_value(int which);
const char* xi_game_ptr_name(int which);
/* The raw pattern match address (for diagnostics and tests). */
uint32_t xi_game_ptr_match(int which);
/* Forget every cached pattern result (tests; a reloaded image). */
void xi_game_reset(void);

uint32_t xi_game_entity_count(void);             /* 0x900, or 0 */
uint32_t xi_game_entity(uint32_t index);         /* entity_t*, 0 if absent */
int32_t xi_game_player_index(void);              /* the local player's target index, -1 */
int32_t xi_game_entity_by_server_id(uint32_t id); /* target index, -1 (linear scan) */
uint32_t xi_game_party(void);                    /* party_t */
uint32_t xi_game_party_member(uint32_t i);       /* partymember_t (0..17), 0 if out of range */
uint32_t xi_game_alliance(void);                 /* allianceinfo_t (Members[0].AllianceInfo) */
uint32_t xi_game_party_icons(void);              /* partystatusicons_t */
uint32_t xi_game_player(void);                   /* player_t */
uint32_t xi_game_target(void);                   /* target_t* */
uint32_t xi_game_target_window(void);            /* targetwindow_t* */
uint32_t xi_game_char(void);                     /* the character block, 0 before login */
uint32_t xi_game_inventory(void);                /* inventory_t */
uint32_t xi_game_container_item(uint32_t container, uint32_t slot); /* item_t */
uint32_t xi_game_autofollow(void);               /* autofollow_t */
uint32_t xi_game_castbar(void);                  /* castbar_t*, 0 when no bar is up */

/* Status icon ids of party member `member` (0..4): 32 entries (low byte | 2 bits of BitMask << 8); 255 = none. */
int xi_game_party_member_icons(uint32_t member, int16_t out[32]);

int xi_game_container_count(uint32_t container);     /* occupied slots 1..80, -1 */
int xi_game_container_max(uint32_t container);       /* ContainerMaxCapacity[c], -1 */

int xi_game_key_item(uint32_t id);      /* 1/0; -1 if the table isn't found */
int xi_game_key_item_seen(uint32_t id);
int xi_game_has_spell_data(void);
int xi_game_spell_known(uint32_t id);   /* bit `id` of the 1024-bit spell field */
int xi_game_has_ability_data(void);
int xi_game_ability_bit(uint32_t bit);  /* bit `bit` of the 0xB00-bit ability field */
int xi_game_job_level(uint32_t job);    /* -1 if unavailable */
int xi_game_master_level(uint32_t job);
int64_t xi_game_master_flags(void);     /* u32 bit per job, -1 */

/* Ability recast slot i (0..30): the abilityrecast_t fields and the live timer (1/60 s). */
int xi_game_ability_recast(uint32_t i, uint32_t* timer, uint32_t* timer_id, uint32_t* recast,
                           uint32_t* calc1, int32_t* calc2);
int32_t xi_game_spell_recast(uint32_t id); /* 1/60 s, -1 */

int xi_game_pet(uint32_t* mpp, uint32_t* tp, uint32_t* index); /* 1 if the player has a pet */

/* Targets entity `index` the way the game's own call sites do (target_t::SetTarget(entity, 1, 0)
 * through guest_thiscall). 1 if called. Not available in XI_GAME_NO_GUEST_CALL builds. */
int xi_game_set_target(uint32_t index);

/* Ashita's GetLoginStatus: 0 not logged in (no character block), 1 logging in / zoning (block
 * present, no local player entity yet), 2 in game. Heuristic, see game.c. */
int xi_game_login_status(void);

#ifdef __cplusplus
}
#endif

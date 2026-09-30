/* Game resources from the retail DATs: items, abilities, spells, status icons, key items, zones,
 * jobs and the game's string tables, for the addon host (Ashita's IResourceManager and Windower's
 * res tables are both built from this).
 *
 * Everything loads lazily on first use and stays cached for the process. Not thread-safe: call it
 * from one thread (the game thread) or guard it yourself.
 *
 * Strings are returned as the game stores them: Shift-JIS with FFXI's own codes (element icons,
 * auto-translate phrases, the 0x85 Latin row, ...), NUL-terminated. res_utf8() converts them.
 *
 * Languages follow Ashita's numbering: 0 = default (res_set_default_lang, English unless changed),
 * 1 = Japanese, 2 = English.
 *
 * File ids: VTABLE.DAT / ROMn/VTABLEn.DAT say which ROM folder holds a file id (a table's byte is
 * n for the ROMn it belongs to), FTABLE.DAT / ROMn/FTABLEn.DAT give `dir << 7 | file` for
 * ROMn/<dir>/<file>.DAT. Overlay folders (res_init) shadow any of these paths. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum
{
    RES_LANG_DEFAULT = 0,
    RES_LANG_JA = 1,
    RES_LANG_EN = 2,
};

/* The retail install folder, and folders whose files shadow it (same relative paths, e.g.
 * <overlay>/ROM/118/106.DAT); the first overlay that has a file wins. Copies the strings. Calling
 * it again drops every cache. */
void res_init(const char* game_dir, const char* const* overlays, int overlay_count);
void res_shutdown(void);
/* Which language RES_LANG_DEFAULT means (RES_LANG_JA or RES_LANG_EN). */
void res_set_default_lang(int lang);

/* --- files ----------------------------------------------------------------------------------- */

/* The host path of a file id (through the overlays). Returns 1, or 0 if the id has no file. */
int res_file_path(uint32_t file_id, char* out, size_t n);
/* The relative path ("ROM3/12/34.DAT") of a file id. Returns 1 or 0. */
int res_file_relpath(uint32_t file_id, char* out, size_t n);
/* A whole file, malloc'd (free() it). NULL if missing. */
uint8_t* res_file_read(uint32_t file_id, size_t* size);

/* --- text ------------------------------------------------------------------------------------ */

enum
{
    RES_UTF8_KEEP_CODES = 1, /* keep control bytes and unknown codes as they are (else dropped) */
    RES_UTF8_PUA_ICONS = 2,  /* element icons (EF 1F..28) as U+E000..E009, as Windower's res text has them */
    RES_UTF8_NO_ICONS = 4,   /* drop element icons instead of naming them */
};
/* Game text (Shift-JIS + FFXI codes) to UTF-8: 0x07 and 0x0A become '\n', element icons
 * (EF 1F..26) their element names, EF 27/28 '{' '}', auto-translate phrases (FD .. FD) "{phrase}",
 * the FFXI Latin row (85 xx) its Latin-1 / cp1252 characters. Dialog codes (7F xx yy) and other
 * control bytes are kept with RES_UTF8_KEEP_CODES, dropped otherwise. Converts `len` bytes (or up
 * to the NUL when len is (size_t)-1). Writes at most cap-1 bytes plus a NUL; returns the length
 * the whole conversion needs (excluding the NUL), like snprintf. */
size_t res_utf8(const char* in, size_t len, char* out, size_t cap, int flags);
/* The same, malloc'd (free() it); NULL for NULL. */
char* res_utf8_dup(const char* in, int flags);
/* One Shift-JIS double-byte character to its Unicode code point (0 if unmapped). */
uint32_t res_sjis_char(uint8_t lead, uint8_t trail);
/* An auto-translate phrase by its code bytes (FD <kind> <lang> <b2> <b3> FD, give the middle four),
 * raw game text with its @-references resolved; NULL if unknown. */
const char* res_auto_translate(uint8_t kind, uint8_t lang, uint8_t b2, uint8_t b3);

/* --- string tables (Ashita's names: "zones.names", "buffs.names", "keyitems.names", ...) ------ */

/* The raw string, or NULL. Key-item tables are indexed by key item id. English-only tables answer
 * Japanese requests in English. Dialog tables
 * ("action.messages", "monsters.abilities") hold codes with NUL argument bytes: use res_string_n
 * for their full length. */
const char* res_string(const char* table, uint32_t index, int lang);
/* The same with its length in bytes (*len; NUL-terminated after that too). */
const char* res_string_n(const char* table, uint32_t index, int lang, size_t* len);
/* The index of the first entry equal to `str` (ASCII case-insensitive), or -1. */
int32_t res_string_find(const char* table, const char* str, int lang);
/* Entries in the table (for keyed tables: 1 + the highest key), 0 if unknown. */
uint32_t res_string_count(const char* table, int lang);
/* The table names this module knows, NULL-terminated. */
const char* const* res_string_tables(void);

/* --- icons ------------------------------------------------------------------------------------ */

/* An icon as stored after its record: a 16-byte name and a Windows DIB (BITMAPINFOHEADER, palette
 * for 8 bpp, bottom-up rows). */
typedef struct ResIcon
{
    uint32_t size;        /* bytes after this u32 in the record: type + name + DIB */
    uint8_t type;         /* 0x91 on every icon seen */
    char name[17];        /* "recepi  0001    ", "sts_iconst00_32 " */
    int32_t width, height;
    uint16_t bpp;         /* 8 (items) or 32 (status icons) */
    uint8_t* dib;         /* malloc'd: BITMAPINFOHEADER onward (res_icon_free) */
    uint32_t dib_size;
} ResIcon;
void res_icon_free(ResIcon* icon);
/* The icon as top-down RGBA8 (width*height*4, malloc'd). FFXI's alpha is 0..0x80; scaled to
 * 0..255. NULL if the DIB isn't one we can read. */
uint8_t* res_icon_rgba(const ResIcon* icon);

/* --- items ----------------------------------------------------------------------------------- */

enum
{
    RES_ITEM_GENERAL,
    RES_ITEM_USABLE,
    RES_ITEM_WEAPON,
    RES_ITEM_ARMOR,
    RES_ITEM_PUPPET,
    RES_ITEM_CURRENCY,
    RES_ITEM_SLIP,        /* Maze tabulae, storage slips (Windower: "Maze") */
    RES_ITEM_MONSTROSITY,
    RES_ITEM_INSTINCT,
};

typedef struct ResMonAbility
{
    uint16_t id;
    int8_t level;
    uint8_t unknown;
} ResMonAbility;

typedef struct ResItem
{
    uint32_t id;
    uint8_t kind;          /* RES_ITEM_* */
    uint8_t legacy;        /* from 0xC00-byte records (kind-specific fields left 0) */
    uint16_t flags, stack, type, resource_id, targets;

    /* weapons, armor (level/slots/races/jobs also instincts) */
    uint16_t level, slots, races;
    uint32_t jobs;
    uint16_t superior_level, shield_size;
    uint8_t max_charges;
    uint16_t cast_time;    /* raw; seconds = cast_time / 4 (also usable items) */
    uint16_t cast_delay;   /* seconds */
    uint32_t recast_delay; /* seconds */
    uint16_t base_item_id;
    uint8_t item_level;
    uint8_t item_level_unknown; /* the byte after item_level (1 on many i119 items) */
    uint16_t damage;
    int16_t delay;         /* negative on some ammo (a delay modifier) */
    uint16_t dps;
    uint8_t skill, jug_size;
    uint32_t weapon_unknown;
    uint8_t range, area_range, area_shape, area_cursor;

    /* general */
    uint16_t element;      /* 0xFFFF none */
    uint32_t storage;
    /* instinct */
    uint16_t instinct_cost;
    /* monstrosity */
    uint16_t monstrosity_id;
    char monstrosity_name[0x21];
    uint8_t monstrosity_data[0x0C];
    ResMonAbility monstrosity_abilities[16];
    /* puppet */
    uint16_t puppet_slot;
    uint32_t puppet_elements;
    /* usable */
    uint16_t usable0;
    uint32_t usable1, usable2;

    uint32_t article;

    /* [0] = English, [1] = Japanese (the Japanese files carry only name and description); raw text,
     * never NULL ("" when missing) */
    const char* name[2];
    const char* description[2];
    const char* log_singular[2];
    const char* log_plural[2];

    /* the decoded record up to its string block (everything above, undecoded fields too) */
    const uint8_t* raw;
    uint16_t raw_size;

    /* where the record is, for the icon */
    uint32_t file_id;
    uint32_t record;
    uint32_t record_size;
} ResItem;

const ResItem* res_item(uint32_t id);
/* By name (ASCII case-insensitive; also matches the log names), in `lang`. */
const ResItem* res_item_by_name(const char* name, int lang);
/* Every item, in id order: *count of them. */
const ResItem* const* res_items(uint32_t* count);
int res_item_icon(uint32_t id, ResIcon* out);
/* "General", "Usable", "Weapon", "Armor", "Automaton", "Gil", "Maze", "Monstrosity", "Instinct" */
const char* res_item_kind_name(int kind);

/* --- spells and abilities (file 81: mgc_ and comm chunks) ------------------------------------ */

typedef struct ResSpell
{
    uint16_t index;
    uint16_t type;          /* 1 white 2 black 3 summoning 4 ninjutsu 5 song 6 blue 7 geomancy 8 trust */
    uint16_t element, targets, skill, mp_cost;
    uint8_t cast_time;      /* quarter seconds */
    uint8_t recast_delay;   /* quarter seconds */
    int16_t levels[24];     /* by job id; -1 can't learn; see job_point_mask */
    uint16_t id;            /* recast id */
    uint16_t icon_nq, icon_hq;
    uint8_t requirements;
    int8_t range;
    uint8_t area_range, area_shape, cursor_target;
    uint32_t area_flags;
    uint32_t job_point_mask;
    uint8_t raw[0x64];      /* the decoded record */
    const char* name[2];    /* [0] English, [1] Japanese; "" when missing */
    const char* description[2];
} ResSpell;

typedef struct ResAbility
{
    uint16_t id;
    uint8_t type;           /* 1 job ability, 2 pet command, 3 weapon skill, 6 blood pact rage, ... */
    uint8_t element;        /* low 3 bits the element; 0x0F-ish values = none */
    uint16_t icon_id, mp_cost, recast_id, targets;
    int16_t tp_cost;        /* -1 none */
    uint8_t menu_category, monster_level;
    int8_t range;
    uint8_t area_range, area_shape, cursor_target;
    uint8_t raw[0x30];
    const char* name[2];
    const char* description[2];
} ResAbility;

/* Spell by index (0..1023). NULL when out of range or unnamed ("." / empty in both languages). */
const ResSpell* res_spell(uint32_t index);
const ResSpell* res_spell_by_name(const char* name, int lang);
/* Ability by id: weapon skills 1-255, job abilities and pet commands 0x200 + id, ... */
const ResAbility* res_ability(uint32_t id);
const ResAbility* res_ability_by_name(const char* name, int lang);
/* The first job ability using recast timer `id`. */
const ResAbility* res_ability_by_recast(uint32_t id);
uint32_t res_spell_count(void);   /* records (1024) */
uint32_t res_ability_count(void); /* records (0xB00) */
/* The same table Windower names: "JobAbility", "WeaponSkill", "BloodPactRage", ... ("" unknown) */
const char* res_ability_type_name(int type);
/* "WhiteMagic", "BlackMagic", "SummonerPact", "Ninjutsu", "BardSong", "BlueMagic", "Geomancy",
 * "Trust" ("" unknown) */
const char* res_spell_type_name(int type);

/* --- status icons / buffs -------------------------------------------------------------------- */

typedef struct ResStatus
{
    uint16_t index;         /* record index in the icon file */
    uint16_t id;            /* the buff id (differs from index for a few) */
    uint8_t can_cancel;
    uint8_t hide_timer;
    const char* description[2];
    /* from the buffs.names tables by id */
    const char* name[2];
    const char* log_name;   /* English only */
    uint32_t record;
} ResStatus;

const ResStatus* res_status_by_index(uint32_t index);
/* By buff id; a status with a name but no icon record still comes back (index 0xFFFF). */
const ResStatus* res_status(uint32_t id);
uint32_t res_status_count(void);  /* icon records */
int res_status_icon(uint32_t id, ResIcon* out);

/* --- key items, zones, jobs ------------------------------------------------------------------ */

typedef struct ResKeyItem
{
    uint32_t id;
    uint32_t category;      /* the entry's second number */
    const char* name[2];
    const char* plural;     /* English only */
    const char* description[2];
} ResKeyItem;
const ResKeyItem* res_key_item(uint32_t id);

typedef struct ResZone
{
    uint32_t id;
    const char* name[2];
    const char* search;     /* "PhanauetCh" */
    const char* abbr;       /* English only */
} ResZone;
const ResZone* res_zone(uint32_t id);

typedef struct ResJob
{
    uint32_t id;
    const char* name[2];
    const char* abbr[2];
} ResJob;
const ResJob* res_job(uint32_t id);

#ifdef __cplusplus
}
#endif

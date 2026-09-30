/* Game resources from the retail DATs. See res.h.
 *
 * Formats (checked against the September 2026 retail install):
 *
 * Items: records of 0x1400 bytes (0xC00 in older clients), every byte rotated right by 5. A 0x10-byte
 * header (id u32, flags u16, pad, stack, type, resource id, targets), kind-specific fields, then a
 * string block: u32 count, count x {u32 offset, u32 kind} relative to the block; kind 0 is text at
 * offset + 0x1C, kind 1 a u32 at offset. English records hold name, article, log name singular,
 * log name plural, description; Japanese ones name and description. The icon follows at +0x280
 * (u32 size, u8 type, 16-byte name, DIB).
 *
 * Spells/abilities: file 81 is a chunked DAT (4-byte name, u32 type | size/16 << 7, 8 bytes pad);
 * "mgc_" holds 0x64-byte spell records and "comm" 0x30-byte ability records, each scrambled: with
 * n = |popcount(r[2]) - popcount(r[11]) + popcount(r[12])| % 5, every byte except 2, 11 and 12
 * (stored plain, they are the key) is rotated right by {7,1,6,2,5}[n].
 *
 * Status icons: 0x1800-byte records, the first 0x280 bytes scrambled like spells; id u16,
 * can-cancel u8, hide-timer u8, a string block (the description) at +4, the icon at +0x280
 * (not scrambled).
 *
 * d_msg string tables: "d_msg" header of 0x40 bytes; u16 at +0x0A = 1 means every byte after the
 * header is XOR 0xFF; +0x1C index size, +0x20 fixed entry size, +0x24 data size, +0x28 count.
 * Entries are either at a fixed stride or found through an index of {u32 offset, u32 size} pairs
 * (offsets from the data start). An entry is a string block like the items'.
 *
 * Dialog files: u32 size | 0x10000000, then everything XOR 0x80: u32 offsets (the first / 4 is the
 * count) to NUL-terminated strings.
 *
 * Auto-translate (55665 English, 55545 Japanese): categories of {4-byte key, 32-byte name, 32-byte
 * name, u32 count, u32 byte size} followed by entries {4-byte key, u8 length, text} (Japanese
 * entries carry a second length + reading). Keys are {2, lang, category, index}. Texts "@J6",
 * "@A68", "@C2B", "@Y223" reference jobs / zones / spells / abilities by hex id. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "res.h"
#include "res_sjis.h"

/* ---- state ----------------------------------------------------------------------------------- */

#define MAX_OVERLAYS 16

static char* g_dir;
static char* g_overlay[MAX_OVERLAYS];
static int g_overlays;
static int g_default_lang = RES_LANG_EN;

static int li(int lang) /* Ashita language -> our [0] English / [1] Japanese */
{
    if (lang == RES_LANG_DEFAULT)
        lang = g_default_lang;
    return lang == RES_LANG_JA ? 1 : 0;
}

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t* p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static size_t nlen(const char* s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n])
        ++n;
    return n;
}
static uint8_t rotr(uint8_t b, int n) { return n ? (uint8_t)(b >> n | b << (8 - n)) : b; }
static int popc(uint8_t b)
{
    int n = 0;
    for (; b; b &= (uint8_t)(b - 1))
        ++n;
    return n;
}

/* ---- arena for strings and copies that live as long as the caches ---------------------------- */

typedef struct Chunk
{
    struct Chunk* next;
    size_t used, cap;
    /* data follows */
} Chunk;
static Chunk* g_arena;

static void* arena_alloc(size_t n)
{
    n = (n + 7) & ~(size_t)7;
    if (!g_arena || g_arena->used + n > g_arena->cap)
    {
        size_t cap = n > (1u << 20) ? n : (1u << 20);
        Chunk* c = malloc(sizeof(Chunk) + cap);
        if (!c)
            return NULL;
        c->next = g_arena;
        c->used = 0;
        c->cap = cap;
        g_arena = c;
    }
    void* p = (char*)(g_arena + 1) + g_arena->used;
    g_arena->used += n;
    return p;
}

static const char* arena_strn(const char* s, size_t n)
{
    if (!n)
        return "";
    char* p = arena_alloc(n + 1);
    if (!p)
        return "";
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

static void arena_free(void)
{
    while (g_arena)
    {
        Chunk* n = g_arena->next;
        free(g_arena);
        g_arena = n;
    }
}

/* ---- files ----------------------------------------------------------------------------------- */

static char* dupstr(const char* s)
{
    size_t n = strlen(s);
    char* p = malloc(n + 2);
    if (!p)
        return NULL;
    memcpy(p, s, n + 1);
    if (n && s[n - 1] != '/' && s[n - 1] != '\\')
        p[n] = '/', p[n + 1] = 0;
    return p;
}

static int file_exists(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f)
        return 0;
    fclose(f);
    return 1;
}

/* A relative path through the overlays, then the install. */
static int resolve(const char* rel, char* out, size_t n)
{
    for (int i = 0; i < g_overlays; ++i)
    {
        snprintf(out, n, "%s%s", g_overlay[i], rel);
        if (file_exists(out))
            return 1;
    }
    if (!g_dir)
        return 0;
    snprintf(out, n, "%s%s", g_dir, rel);
    return 1;
}

static uint8_t* read_path(const char* path, size_t* size)
{
    FILE* f = fopen(path, "rb");
    if (!f)
        return NULL;
    long n = -1;
    if (!fseek(f, 0, SEEK_END))
        n = ftell(f);
    uint8_t* p = NULL;
    if (n > 0 && !fseek(f, 0, SEEK_SET) && (p = malloc((size_t)n)) != NULL)
        if (fread(p, 1, (size_t)n, f) != (size_t)n)
        {
            free(p);
            p = NULL;
        }
    fclose(f);
    if (p && size)
        *size = (size_t)n;
    return p;
}

static uint8_t* read_rel(const char* rel, size_t* size)
{
    char path[1024];
    return resolve(rel, path, sizeof path) ? read_path(path, size) : NULL;
}

/* VTABLE / FTABLE per ROM folder (1 = ROM, n = ROMn) */
static struct
{
    uint8_t* v;
    size_t vn;
    uint8_t* f;
    size_t fn;
} g_tab[10];
static int g_tabs_loaded;

static void load_tables(void)
{
    if (g_tabs_loaded)
        return;
    g_tabs_loaded = 1;
    for (int r = 1; r <= 9; ++r)
    {
        char vr[64], fr[64];
        if (r == 1)
            snprintf(vr, sizeof vr, "VTABLE.DAT"), snprintf(fr, sizeof fr, "FTABLE.DAT");
        else
            snprintf(vr, sizeof vr, "ROM%d/VTABLE%d.DAT", r, r), snprintf(fr, sizeof fr, "ROM%d/FTABLE%d.DAT", r, r);
        g_tab[r].v = read_rel(vr, &g_tab[r].vn);
        g_tab[r].f = read_rel(fr, &g_tab[r].fn);
    }
}

int res_file_relpath(uint32_t id, char* out, size_t n)
{
    load_tables();
    for (int r = 1; r <= 9; ++r)
    {
        if (!g_tab[r].v || id >= g_tab[r].vn || g_tab[r].v[id] != r || (size_t)id * 2 + 2 > g_tab[r].fn)
            continue;
        uint16_t x = rd16(g_tab[r].f + (size_t)id * 2);
        if (r == 1)
            snprintf(out, n, "ROM/%u/%u.DAT", x >> 7, x & 0x7f);
        else
            snprintf(out, n, "ROM%d/%u/%u.DAT", r, x >> 7, x & 0x7f);
        return 1;
    }
    return 0;
}

int res_file_path(uint32_t id, char* out, size_t n)
{
    char rel[64];
    return res_file_relpath(id, rel, sizeof rel) && resolve(rel, out, n);
}

uint8_t* res_file_read(uint32_t id, size_t* size)
{
    char path[1024];
    return res_file_path(id, path, sizeof path) ? read_path(path, size) : NULL;
}

/* ---- string blocks (items, d_msg entries, status icons) -------------------------------------- */

typedef struct Sub
{
    uint32_t kind;       /* 0 text, 1 number */
    uint32_t value;      /* kind 1 */
    const char* text;    /* kind 0, NUL-terminated within the block's bounds (else NULL) */
    uint32_t len;
} Sub;

/* Whether blk (n bytes available) starts a plausible string block. */
static int block_ok(const uint8_t* blk, size_t n)
{
    if (n < 12)
        return 0;
    uint32_t cnt = rd32(blk);
    if (cnt < 1 || cnt > 32 || 4 + (size_t)cnt * 8 > n || rd32(blk + 4) != 4 + cnt * 8)
        return 0;
    uint32_t prev = 0;
    for (uint32_t i = 0; i < cnt; ++i)
    {
        uint32_t o = rd32(blk + 4 + i * 8), k = rd32(blk + 8 + i * 8);
        if (k > 1 || o < prev || o + (k ? 4u : 0x1Cu) > n)
            return 0;
        prev = o;
    }
    return 1;
}

/* Up to max subs of the block; returns how many. */
static int block_subs(const uint8_t* blk, size_t n, Sub* out, int max)
{
    if (n < 4)
        return 0;
    uint32_t cnt = rd32(blk);
    int k = 0;
    for (uint32_t i = 0; i < cnt && k < max && 4 + (size_t)i * 8 + 8 <= n; ++i)
    {
        uint32_t o = rd32(blk + 4 + i * 8), kind = rd32(blk + 8 + i * 8);
        Sub s = {kind, 0, NULL, 0};
        if (kind == 1)
        {
            if ((size_t)o + 4 <= n)
                s.value = rd32(blk + o);
        }
        else if ((size_t)o + 0x1C <= n)
        {
            const char* t = (const char*)blk + o + 0x1C;
            const char* z = memchr(t, 0, n - o - 0x1C);
            if (z)
            {
                s.text = t;
                s.len = (uint32_t)(z - t);
            }
        }
        out[k++] = s;
    }
    return k;
}

/* ---- d_msg and dialog files ------------------------------------------------------------------ */

typedef struct MsgFile
{
    uint32_t fid;
    int dialog;          /* dialog format: entries are plain strings */
    uint8_t* data;
    size_t size;
    uint32_t count;
    uint32_t* off;       /* entry start */
    uint32_t* len;       /* entry size */
} MsgFile;

static MsgFile* g_msg;
static int g_msgs, g_msg_cap;

static MsgFile* msg_file(uint32_t fid)
{
    for (int i = 0; i < g_msgs; ++i)
        if (g_msg[i].fid == fid)
            return g_msg[i].data ? &g_msg[i] : NULL;
    if (g_msgs == g_msg_cap)
    {
        int cap = g_msg_cap ? g_msg_cap * 2 : 32;
        MsgFile* m = realloc(g_msg, sizeof *m * (size_t)cap);
        if (!m)
            return NULL;
        g_msg = m;
        g_msg_cap = cap;
    }
    MsgFile* m = &g_msg[g_msgs++];
    memset(m, 0, sizeof *m);
    m->fid = fid;
    size_t n = 0;
    uint8_t* d = res_file_read(fid, &n);
    if (!d)
        return NULL;
    if (n >= 0x40 && !memcmp(d, "d_msg", 5))
    {
        if (rd16(d + 0x0A) == 1)
            for (size_t i = 0x40; i < n; ++i)
                d[i] ^= 0xFF;
        uint32_t idx = rd32(d + 0x1C), stride = rd32(d + 0x20), cnt = rd32(d + 0x28);
        if (cnt > n / 4)
            cnt = 0;
        m->off = malloc(sizeof(uint32_t) * (cnt + 1));
        m->len = malloc(sizeof(uint32_t) * (cnt + 1));
        if (!m->off || !m->len)
            cnt = 0;
        for (uint32_t i = 0; i < cnt; ++i)
        {
            uint64_t o, s;
            if (idx)
            {
                if (0x40 + (size_t)i * 8 + 8 > n)
                    break;
                o = (uint64_t)rd32(d + 0x40 + i * 8) + 0x40 + idx;
                s = rd32(d + 0x44 + i * 8);
            }
            else
            {
                o = 0x40 + (uint64_t)i * stride;
                s = stride;
            }
            if (o > n)
                o = n;
            if (o + s > n)
                s = n - o;
            m->off[i] = (uint32_t)o;
            m->len[i] = (uint32_t)s;
            m->count = i + 1;
        }
    }
    else if (n >= 8 && (rd32(d) & 0xF0000000u) == 0x10000000u)
    {
        /* entries are delimited by the offsets only (codes carry NUL argument bytes): copy each
         * into a new buffer followed by a NUL */
        m->dialog = 1;
        for (size_t i = 4; i < n; ++i)
            d[i] ^= 0x80;
        uint32_t first = rd32(d + 4), cnt = first / 4;
        if (4 + (size_t)first > n)
            cnt = 0;
        m->off = malloc(sizeof(uint32_t) * (cnt + 1));
        m->len = malloc(sizeof(uint32_t) * (cnt + 1));
        uint8_t* nd = malloc(n + cnt + 1);
        if (!m->off || !m->len || !nd)
            cnt = 0;
        size_t w = 0;
        for (uint32_t i = 0; i < cnt; ++i)
        {
            uint32_t o = rd32(d + 4 + i * 4) + 4;
            uint32_t e = i + 1 < cnt ? rd32(d + 8 + i * 4) + 4 : (uint32_t)n;
            if (o > n)
                o = (uint32_t)n;
            if (e > n || e < o)
                e = o;
            m->off[i] = (uint32_t)w;
            m->len[i] = e - o;
            memcpy(nd + w, d + o, e - o);
            w += e - o;
            nd[w++] = 0;
            m->count = i + 1;
        }
        free(d);
        d = nd ? nd : malloc(1);
        n = w;
    }
    else
    {
        free(d);
        return NULL;
    }
    m->data = d;
    m->size = n;
    return m;
}

/* Sub-string `sub` of entry `i`: text (NULL if none) with its length in *len, and, for number
 * subs, *num. */
static const char* msg_sub_n(MsgFile* m, uint32_t i, int sub, uint32_t* num, int* is_num, size_t* len)
{
    if (is_num)
        *is_num = 0;
    if (!m || i >= m->count)
        return NULL;
    const uint8_t* e = m->data + m->off[i];
    size_t n = m->len[i];
    if (m->dialog)
    {
        if (sub != 0)
            return NULL;
        if (len)
            *len = n;
        return (const char*)e;
    }
    Sub s[16];
    int k = block_subs(e, n, s, 16);
    if (sub >= k)
        return NULL;
    if (s[sub].kind == 1)
    {
        if (num)
            *num = s[sub].value;
        if (is_num)
            *is_num = 1;
        return NULL;
    }
    if (len)
        *len = s[sub].len;
    return s[sub].text;
}

static const char* msg_sub(MsgFile* m, uint32_t i, int sub, uint32_t* num, int* is_num)
{
    return msg_sub_n(m, i, sub, num, is_num, NULL);
}

/* ---- named string tables (Ashita's datmap names and file ids) -------------------------------- */

typedef struct StrTable
{
    const char* name;
    uint32_t en, ja;     /* file ids, 0 = none */
    uint8_t en_sub, ja_sub;
    uint8_t keyed;       /* entries keyed by their first (number) sub-string */
} StrTable;

static const StrTable g_tables[] = {
    {"abilities.names", 55701, 55581, 0, 0, 0},
    {"abilities.descriptions", 55733, 55613, 0, 0, 0},
    {"spells.names", 55702, 55582, 0, 0, 0},
    {"spells.descriptions", 55734, 55614, 0, 0, 0},
    {"action.messages", 7027, 7026, 0, 0, 0},
    {"augments", 55692, 55572, 0, 0, 0},
    {"buffs.names", 55732, 55605, 0, 0, 0},
    {"buffs.names_log", 55732, 0, 1, 0, 0},
    {"commands.help", 55687, 55567, 0, 0, 0},
    {"days", 55658, 55538, 0, 0, 0},
    {"directions", 55659, 55539, 0, 0, 0},
    {"emotes", 55676, 55556, 0, 0, 0},
    {"equipment.slots", 55471, 0, 0, 0, 0},
    {"equipment.slots_old", 55666, 55546, 0, 0, 0},
    {"jobpoints", 55694, 55574, 0, 0, 0},
    {"jobpoints.gifts", 55674, 55554, 0, 0, 0},
    {"jobs.names", 55467, 55536, 0, 0, 0},
    {"jobs.names_abbr", 55468, 55468, 0, 0, 0},
    {"keyitems.names", 55697, 55577, 4, 1, 1},
    {"keyitems.names_plural", 55697, 0, 5, 0, 1},
    {"keyitems.descriptions", 55697, 55577, 6, 2, 1},
    {"merits", 55686, 55566, 0, 0, 0},
    {"monsters.abilities", 7035, 7034, 0, 0, 0},
    {"monsters.groups", 55690, 55570, 0, 0, 0},
    {"monster.groups_plural", 55690, 0, 1, 0, 0},
    {"moonphases", 55660, 55540, 0, 0, 0},
    {"mounts.names", 55681, 55561, 0, 0, 0},
    {"mounts.descriptions", 55682, 55562, 0, 0, 0},
    {"races", 55469, 55469, 0, 0, 0},
    {"regions", 55654, 55534, 0, 0, 0},
    {"titles", 55704, 55584, 0, 0, 0},
    {"weather", 55657, 55537, 0, 0, 0},
    {"weather.effects", 55657, 0, 1, 0, 0},
    {"zones.names", 55465, 55535, 0, 0, 0},
    {"zones.names_abbr", 55661, 0, 0, 0, 0},
    {"zones.names_search", 55466, 55466, 0, 0, 0},
};
#define NTABLES (sizeof g_tables / sizeof g_tables[0])

static const char* g_table_names[NTABLES + 1];

const char* const* res_string_tables(void)
{
    for (size_t i = 0; i < NTABLES; ++i)
        g_table_names[i] = g_tables[i].name;
    g_table_names[NTABLES] = NULL;
    return g_table_names;
}

/* keyed tables: key -> entry, per file */
typedef struct KeyMap
{
    uint32_t fid;
    uint32_t n;          /* 1 + highest key */
    int32_t* entry;
} KeyMap;
static KeyMap g_keymap[8];
static int g_keymaps;

static KeyMap* keymap(MsgFile* m)
{
    for (int i = 0; i < g_keymaps; ++i)
        if (g_keymap[i].fid == m->fid)
            return &g_keymap[i];
    if (g_keymaps == (int)(sizeof g_keymap / sizeof g_keymap[0]))
        return NULL;
    KeyMap* k = &g_keymap[g_keymaps++];
    k->fid = m->fid;
    k->n = 0;
    for (uint32_t i = 0; i < m->count; ++i)
    {
        uint32_t v = 0;
        int isn = 0;
        msg_sub(m, i, 0, &v, &isn);
        if (isn && v < 0x100000 && v + 1 > k->n)
            k->n = v + 1;
    }
    k->entry = malloc(sizeof(int32_t) * (k->n ? k->n : 1));
    if (!k->entry)
    {
        k->n = 0;
        return k;
    }
    for (uint32_t i = 0; i < k->n; ++i)
        k->entry[i] = -1;
    for (uint32_t i = 0; i < m->count; ++i)
    {
        uint32_t v = 0;
        int isn = 0;
        msg_sub(m, i, 0, &v, &isn);
        if (isn && v < k->n && k->entry[v] < 0)
            k->entry[v] = (int32_t)i;
    }
    return k;
}

static const StrTable* find_table(const char* name)
{
    if (!name)
        return NULL;
    for (size_t i = 0; i < NTABLES; ++i)
    {
        const char *a = g_tables[i].name, *b = name;
        while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b))
            ++a, ++b;
        if (!*a && !*b)
            return &g_tables[i];
    }
    return NULL;
}

static const char* table_get_n(const StrTable* t, uint32_t index, int l, size_t* len)
{
    if (l && !t->ja)
        l = 0; /* English-only table */
    uint32_t fid = l ? t->ja : t->en;
    int sub = l ? t->ja_sub : t->en_sub;
    MsgFile* m = fid ? msg_file(fid) : NULL;
    if (!m)
        return NULL;
    if (t->keyed)
    {
        KeyMap* k = keymap(m);
        if (!k || index >= k->n || k->entry[index] < 0)
            return NULL;
        index = (uint32_t)k->entry[index];
    }
    return msg_sub_n(m, index, sub, NULL, NULL, len);
}

static const char* table_get(const StrTable* t, uint32_t index, int l)
{
    return table_get_n(t, index, l, NULL);
}

static uint32_t table_count(const StrTable* t, int l)
{
    if (l && !t->ja)
        l = 0;
    uint32_t fid = l ? t->ja : t->en;
    MsgFile* m = fid ? msg_file(fid) : NULL;
    if (!m)
        return 0;
    if (t->keyed)
    {
        KeyMap* k = keymap(m);
        return k ? k->n : 0;
    }
    return m->count;
}

const char* res_string(const char* table, uint32_t index, int lang)
{
    const StrTable* t = find_table(table);
    return t ? table_get(t, index, li(lang)) : NULL;
}

const char* res_string_n(const char* table, uint32_t index, int lang, size_t* len)
{
    const StrTable* t = find_table(table);
    size_t n = 0;
    const char* s = t ? table_get_n(t, index, li(lang), &n) : NULL;
    if (len)
        *len = s ? n : 0;
    return s;
}

uint32_t res_string_count(const char* table, int lang)
{
    const StrTable* t = find_table(table);
    return t ? table_count(t, li(lang)) : 0;
}

static int ieq(const char* a, const char* b)
{
    for (; *a && *b; ++a, ++b)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
    return *a == *b;
}

int32_t res_string_find(const char* table, const char* str, int lang)
{
    const StrTable* t = find_table(table);
    if (!t || !str)
        return -1;
    int l = li(lang);
    uint32_t n = table_count(t, l);
    for (uint32_t i = 0; i < n; ++i)
    {
        const char* s = table_get(t, i, l);
        if (s && ieq(s, str))
            return (int32_t)i;
    }
    return -1;
}

static const char* tbl(const char* name, uint32_t index, int l)
{
    const char* s = res_string(name, index, l ? RES_LANG_JA : RES_LANG_EN);
    return s ? s : "";
}

/* ---- auto-translate -------------------------------------------------------------------------- */

typedef struct AtEntry
{
    uint16_t key;        /* category << 8 | index (index 0 = the category's own name) */
    const char* text;
} AtEntry;
static AtEntry* g_at[2];
static uint32_t g_at_n[2];
static int g_at_loaded[2];

static int at_cmp(const void* a, const void* b)
{
    return (int)((const AtEntry*)a)->key - (int)((const AtEntry*)b)->key;
}

/* "@J6" and friends to the name they reference (raw text); anything else as it is. */
static const char* at_resolve(const char* s, size_t n, int l)
{
    if (n >= 3 && s[0] == '@')
    {
        uint32_t v = 0;
        size_t i = 2;
        for (; i < n && isxdigit((unsigned char)s[i]); ++i)
            v = v * 16 + (uint32_t)(isdigit((unsigned char)s[i]) ? s[i] - '0' : (tolower((unsigned char)s[i]) - 'a' + 10));
        if (i == n)
        {
            const char* r = NULL;
            switch (s[1])
            {
            case 'J': r = tbl("jobs.names", v, l); break;
            case 'A': r = tbl("zones.names", v, l); break;
            case 'C': r = tbl("spells.names", v, l); break;
            case 'Y': r = tbl("abilities.names", v, l); break;
            }
            if (r && *r)
                return r;
        }
    }
    return arena_strn(s, n);
}

static void at_load(int l)
{
    if (g_at_loaded[l])
        return;
    g_at_loaded[l] = 1;
    size_t n = 0;
    uint8_t* d = res_file_read(l ? 55545 : 55665, &n);
    if (!d)
        return;
    uint32_t cap = 0, cnt = 0;
    AtEntry* e = NULL;
    size_t o = 0;
    while (o + 0x4C <= n)
    {
        uint32_t sz = rd32(d + o + 0x48);
        size_t p = o + 0x4C, end = p + sz;
        if (end > n)
            end = n;
        /* the category itself: its second name */
        const uint8_t* ks = d + o;
        for (int pass = 0;; pass = 1)
        {
            if (cnt == cap)
            {
                cap = cap ? cap * 2 : 1024;
                AtEntry* ne = realloc(e, sizeof *e * cap);
                if (!ne)
                    goto done;
                e = ne;
            }
            if (!pass)
            {
                const char* t = (const char*)d + o + 0x24;
                size_t tl = nlen(t, 0x20);
                e[cnt].key = (uint16_t)(ks[2] << 8);
                e[cnt].text = arena_strn(t, tl);
                ++cnt;
                continue;
            }
            if (p + 5 > end)
                break;
            const uint8_t* k = d + p;
            size_t len = d[p + 4];
            const char* t = (const char*)d + p + 5;
            if (p + 5 + len > end)
                break;
            size_t tl = nlen(t, len);
            p += 5 + len;
            if (l && p < end)
                p += 1 + (size_t)d[p]; /* Japanese: the reading */
            e[cnt].key = (uint16_t)(k[2] << 8 | k[3]);
            e[cnt].text = at_resolve(t, tl, l);
            ++cnt;
        }
        o = end > o + 0x4C ? end : o + 0x4C;
    }
done:
    free(d);
    if (e)
        qsort(e, cnt, sizeof *e, at_cmp);
    g_at[l] = e;
    g_at_n[l] = cnt;
}

const char* res_auto_translate(uint8_t kind, uint8_t lang, uint8_t b2, uint8_t b3)
{
    if (kind != 2)
        return NULL;
    int l = lang == 1 ? 1 : 0;
    at_load(l);
    AtEntry key = {(uint16_t)(b2 << 8 | b3), NULL};
    AtEntry* e = g_at[l] ? bsearch(&key, g_at[l], g_at_n[l], sizeof key, at_cmp) : NULL;
    return e ? e->text : NULL;
}

/* ---- text to UTF-8 --------------------------------------------------------------------------- */

uint32_t res_sjis_char(uint8_t lead, uint8_t trail)
{
    int row;
    if (lead >= 0x81 && lead <= 0x9F)
        row = lead - 0x81;
    else if (lead >= 0xE0 && lead <= 0xFC)
        row = lead - 0xE0 + 31;
    else
        return 0;
    if (trail < 0x40 || trail > 0xFC)
        return 0;
    return res_sjis_table[row][trail - 0x40];
}

/* cp1252's 0x80-0x9F */
static const uint16_t g_cp1252[32] = {
    0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
    0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178,
};

/* FFXI's 0x85 row: Latin characters in cp1252 order from 0x80. */
static uint32_t ffxi_latin(uint8_t t)
{
    uint32_t c;
    if (t >= 0x40 && t <= 0x7E)
        c = t + 0x40u;
    else if (t >= 0x80 && t <= 0xC0)
        c = t + 0x3Fu;
    else
        return 0;
    return c < 0xA0 ? g_cp1252[c - 0x80] : c;
}

static const char* const g_elements[8] = {"Fire", "Ice", "Wind", "Earth", "Lightning", "Water", "Light", "Dark"};

typedef struct Out
{
    char* p;
    size_t cap, n;
} Out;

static void put(Out* o, const char* s, size_t n)
{
    for (size_t i = 0; i < n; ++i, ++o->n)
        if (o->n + 1 < o->cap)
            o->p[o->n] = s[i];
}

static void put_cp(Out* o, uint32_t c)
{
    char b[4];
    size_t n;
    if (c < 0x80)
        b[0] = (char)c, n = 1;
    else if (c < 0x800)
        b[0] = (char)(0xC0 | c >> 6), b[1] = (char)(0x80 | (c & 0x3F)), n = 2;
    else if (c < 0x10000)
        b[0] = (char)(0xE0 | c >> 12), b[1] = (char)(0x80 | (c >> 6 & 0x3F)), b[2] = (char)(0x80 | (c & 0x3F)), n = 3;
    else
        b[0] = (char)(0xF0 | c >> 18), b[1] = (char)(0x80 | (c >> 12 & 0x3F)), b[2] = (char)(0x80 | (c >> 6 & 0x3F)),
        b[3] = (char)(0x80 | (c & 0x3F)), n = 4;
    put(o, b, n);
}

static void convert(Out* o, const uint8_t* s, size_t n, int flags, int depth)
{
    int keep = flags & RES_UTF8_KEEP_CODES;
    for (size_t i = 0; i < n;)
    {
        uint8_t b = s[i];
        if (b == 0x0A || b == 0x07)
        {
            put(o, "\n", 1);
            ++i;
        }
        else if (b == 0x7F && i + 2 < n)
        {
            /* dialog code with one argument byte */
            if (keep)
                put(o, (const char*)s + i, 3);
            i += 3;
        }
        else if (b < 0x20 || b == 0x7F)
        {
            if (keep || b == 0x09)
                put(o, (const char*)s + i, 1);
            ++i;
        }
        else if (b < 0x80)
        {
            put(o, (const char*)s + i, 1);
            ++i;
        }
        else if (b >= 0xA1 && b <= 0xDF)
        {
            put_cp(o, 0xFF61u + (b - 0xA1u));
            ++i;
        }
        else if (b == 0xEF && i + 1 < n && s[i + 1] >= 0x1F && s[i + 1] <= 0x28)
        {
            uint8_t t = s[i + 1];
            if (flags & RES_UTF8_NO_ICONS)
                ;
            else if (flags & RES_UTF8_PUA_ICONS)
                put_cp(o, 0xE000u + (t - 0x1Fu));
            else if (t <= 0x26)
                put(o, g_elements[t - 0x1F], strlen(g_elements[t - 0x1F]));
            else
                put(o, t == 0x27 ? "{" : "}", 1);
            i += 2;
        }
        else if (b == 0xFD && i + 5 < n && s[i + 5] == 0xFD)
        {
            const char* t = depth < 2 ? res_auto_translate(s[i + 1], s[i + 2], s[i + 3], s[i + 4]) : NULL;
            if (t)
            {
                put(o, "{", 1);
                convert(o, (const uint8_t*)t, strlen(t), flags, depth + 1);
                put(o, "}", 1);
            }
            else if (keep)
                put(o, (const char*)s + i, 6);
            else
                put(o, "{?}", 3);
            i += 6;
        }
        else if (i + 1 < n && ((b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC)))
        {
            uint32_t c = b == 0x85 ? ffxi_latin(s[i + 1]) : res_sjis_char(b, s[i + 1]);
            if (c)
                put_cp(o, c);
            else if (keep)
                put(o, (const char*)s + i, 2);
            else
                put_cp(o, 0xFFFD);
            i += 2;
        }
        else
        {
            if (keep)
                put(o, (const char*)s + i, 1);
            ++i;
        }
    }
}

size_t res_utf8(const char* in, size_t len, char* out, size_t cap, int flags)
{
    Out o = {out, out ? cap : 0, 0};
    if (in)
    {
        if (len == (size_t)-1)
            len = strlen(in);
        convert(&o, (const uint8_t*)in, len, flags, 0);
    }
    if (o.cap)
        o.p[o.n < o.cap ? o.n : o.cap - 1] = 0;
    return o.n;
}

char* res_utf8_dup(const char* in, int flags)
{
    if (!in)
        return NULL;
    size_t n = res_utf8(in, (size_t)-1, NULL, 0, flags);
    char* p = malloc(n + 1);
    if (p)
        res_utf8(in, (size_t)-1, p, n + 1, flags);
    return p;
}

/* ---- icons ----------------------------------------------------------------------------------- */

void res_icon_free(ResIcon* icon)
{
    if (!icon)
        return;
    free(icon->dib);
    memset(icon, 0, sizeof *icon);
}

/* The icon at `at` in record `rec` of file `fid` (record size rs); rot = byte rotation of the icon. */
static int read_icon(uint32_t fid, uint32_t rec, uint32_t rs, uint32_t at, int rot, ResIcon* out)
{
    memset(out, 0, sizeof *out);
    char path[1024];
    if (!res_file_path(fid, path, sizeof path))
        return 0;
    FILE* f = fopen(path, "rb");
    if (!f)
        return 0;
    size_t avail = rs - at;
    uint8_t* buf = malloc(avail);
    int ok = buf && !fseek(f, (long)((uint64_t)rec * rs + at), SEEK_SET) && fread(buf, 1, avail, f) == avail;
    fclose(f);
    if (!ok)
    {
        free(buf);
        return 0;
    }
    for (size_t i = 0; i < avail && rot; ++i)
        buf[i] = rotr(buf[i], rot);
    uint32_t size = rd32(buf);
    if (size < 17 + 40 || size > avail - 4)
    {
        free(buf);
        return 0;
    }
    out->size = size;
    out->type = buf[4];
    memcpy(out->name, buf + 5, 16);
    out->name[16] = 0;
    const uint8_t* dib = buf + 21;
    out->dib_size = size - 17;
    out->width = (int32_t)rd32(dib + 4);
    out->height = (int32_t)rd32(dib + 8);
    out->bpp = rd16(dib + 14);
    out->dib = malloc(out->dib_size);
    if (!out->dib)
    {
        free(buf);
        memset(out, 0, sizeof *out);
        return 0;
    }
    memcpy(out->dib, dib, out->dib_size);
    free(buf);
    return 1;
}

uint8_t* res_icon_rgba(const ResIcon* icon)
{
    if (!icon || !icon->dib || icon->dib_size < 40)
        return NULL;
    const uint8_t* d = icon->dib;
    uint32_t hs = rd32(d);
    int32_t w = icon->width, h = icon->height;
    int bottom_up = h > 0;
    if (h < 0)
        h = -h;
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024 || hs > icon->dib_size)
        return NULL;
    uint32_t ncol = 0;
    if (icon->bpp <= 8)
    {
        ncol = rd32(d + 32);
        if (!ncol)
            ncol = 1u << icon->bpp;
    }
    const uint8_t* pal = d + hs;
    const uint8_t* px = pal + ncol * 4;
    size_t stride = (((size_t)w * icon->bpp + 31) / 32) * 4;
    if ((size_t)(px - d) + stride * (size_t)h > icon->dib_size)
        return NULL;
    if (icon->bpp != 8 && icon->bpp != 32)
        return NULL;
    uint8_t* out = malloc((size_t)w * (size_t)h * 4);
    if (!out)
        return NULL;
    for (int32_t y = 0; y < h; ++y)
    {
        const uint8_t* row = px + stride * (size_t)(bottom_up ? h - 1 - y : y);
        uint8_t* o = out + (size_t)y * (size_t)w * 4;
        for (int32_t x = 0; x < w; ++x, o += 4)
        {
            const uint8_t* c = icon->bpp == 8 ? (row[x] < ncol ? pal + row[x] * 4 : pal) : row + x * 4;
            unsigned a = c[3] * 2u;
            o[0] = c[2];
            o[1] = c[1];
            o[2] = c[0];
            o[3] = (uint8_t)(a > 255 ? 255 : a);
        }
    }
    return out;
}

/* ---- items ----------------------------------------------------------------------------------- */

static const struct
{
    uint8_t kind;
    uint32_t en, ja;
    uint16_t block;      /* where the string block is in 0x1400-byte records */
} g_item_files[] = {
    {RES_ITEM_GENERAL, 73, 4, 0x1C},
    {RES_ITEM_USABLE, 74, 5, 0x1C},
    {RES_ITEM_WEAPON, 75, 6, 0x3C},
    {RES_ITEM_ARMOR, 76, 7, 0x30},
    {RES_ITEM_PUPPET, 77, 8, 0x1C},
    {RES_ITEM_CURRENCY, 91, 9, 0x14},
    {RES_ITEM_SLIP, 55667, 55547, 0x54},
    {RES_ITEM_ARMOR, 55668, 55548, 0x30},
    {RES_ITEM_MONSTROSITY, 55669, 55549, 0x74},
    {RES_ITEM_INSTINCT, 55670, 55550, 0x2C},
    {RES_ITEM_GENERAL, 55671, 55551, 0x1C},
    {RES_ITEM_GENERAL, 55675, 55555, 0x1C},
};
#define NITEMFILES (sizeof g_item_files / sizeof g_item_files[0])
#define ITEM_HEAD 0x280  /* strings end before the icon */

static ResItem* g_items;
static ResItem** g_item_list;
static uint32_t g_nitems, g_items_cap;
static int32_t* g_item_idx; /* id (0..65535) -> g_items index */
static int g_items_loaded;

/* Record size of an item file: 0x1400 or 0xC00, by what divides it and the 0xFF terminator. */
static uint32_t item_record_size(FILE* f, long size)
{
    static const uint32_t sizes[2] = {0x1400, 0xC00};
    for (int i = 0; i < 2; ++i)
    {
        uint32_t rs = sizes[i];
        if (size <= 0 || size % rs)
            continue;
        uint8_t b;
        if (fseek(f, (long)rs - 1, SEEK_SET) || fread(&b, 1, 1, f) != 1)
            continue;
        if (rotr(b, 5) == 0xFF)
            return rs;
    }
    return 0;
}

static size_t find_block(const uint8_t* r, size_t n, size_t want)
{
    if (want && want < n && block_ok(r + want, n - want))
        return want;
    for (size_t o = 0x0C; o + 12 <= n && o < 0x200; o += 2)
        if (block_ok(r + o, n - o))
            return o;
    return 0;
}

static void parse_item_fields(ResItem* it, const uint8_t* r, int kind)
{
    switch (kind)
    {
    case RES_ITEM_GENERAL:
        it->element = rd16(r + 0x10);
        it->storage = rd16(r + 0x12);
        break;
    case RES_ITEM_USABLE:
        it->cast_time = rd16(r + 0x10);
        it->usable0 = rd16(r + 0x12);
        it->usable1 = rd32(r + 0x14);
        it->usable2 = rd32(r + 0x18);
        break;
    case RES_ITEM_WEAPON:
    case RES_ITEM_ARMOR:
    case RES_ITEM_INSTINCT:
        it->level = rd16(r + 0x10);
        it->slots = rd16(r + 0x12);
        it->races = rd16(r + 0x14);
        it->jobs = rd32(r + 0x18);
        if (kind == RES_ITEM_INSTINCT)
        {
            it->instinct_cost = rd16(r + 0x1C);
            break;
        }
        it->superior_level = rd16(r + 0x1C);
        it->shield_size = rd16(r + 0x1E);
        {
            const uint8_t* p = r + 0x20;
            if (kind == RES_ITEM_WEAPON)
            {
                it->damage = rd16(r + 0x20);
                it->delay = (int16_t)rd16(r + 0x22);
                it->dps = rd16(r + 0x24);
                it->skill = r[0x26];
                it->jug_size = r[0x27];
                it->weapon_unknown = rd32(r + 0x28);
                p = r + 0x2C;
            }
            it->max_charges = p[0];
            it->cast_time = p[1];
            it->cast_delay = rd16(p + 2);
            it->recast_delay = rd32(p + 4);
            it->base_item_id = rd16(p + 8);
            it->item_level = p[10];
            it->item_level_unknown = p[11];
            it->range = p[12];
            it->area_range = p[13];
            it->area_shape = p[14];
            it->area_cursor = p[15];
        }
        break;
    case RES_ITEM_PUPPET:
        it->puppet_slot = rd16(r + 0x10);
        it->puppet_elements = rd32(r + 0x14);
        break;
    case RES_ITEM_MONSTROSITY:
        it->monstrosity_id = rd16(r + 4);
        memcpy(it->monstrosity_name, r + 8, 0x20);
        it->monstrosity_name[0x20] = 0;
        memcpy(it->monstrosity_data, r + 0x28, 0x0C);
        for (int i = 0; i < 16; ++i)
        {
            const uint8_t* a = r + 0x34 + i * 4;
            it->monstrosity_abilities[i].id = rd16(a);
            it->monstrosity_abilities[i].level = (int8_t)a[2];
            it->monstrosity_abilities[i].unknown = a[3];
        }
        break;
    }
}

static ResItem* item_slot(uint32_t id)
{
    if (id > 0xFFFF)
        return NULL;
    if (g_item_idx[id] >= 0)
        return &g_items[g_item_idx[id]];
    if (g_nitems == g_items_cap)
    {
        uint32_t cap = g_items_cap ? g_items_cap * 2 : 32768;
        ResItem* n = realloc(g_items, sizeof *n * cap);
        if (!n)
            return NULL;
        g_items = n;
        g_items_cap = cap;
    }
    ResItem* it = &g_items[g_nitems];
    memset(it, 0, sizeof *it);
    it->id = id;
    it->element = 0xFFFF;
    for (int l = 0; l < 2; ++l)
        it->name[l] = it->description[l] = it->log_singular[l] = it->log_plural[l] = "";
    g_item_idx[id] = (int32_t)g_nitems++;
    return it;
}

static void load_item_file(uint32_t fid, int kind, uint16_t want, int l)
{
    char path[1024];
    if (!res_file_path(fid, path, sizeof path))
        return;
    FILE* f = fopen(path, "rb");
    if (!f)
        return;
    long size = -1;
    if (!fseek(f, 0, SEEK_END))
        size = ftell(f);
    uint32_t rs = item_record_size(f, size);
    if (!rs)
    {
        fclose(f);
        return;
    }
    int legacy = rs == 0xC00;
    uint32_t count = (uint32_t)(size / rs);
    uint8_t r[ITEM_HEAD];
    for (uint32_t i = 0; i < count; ++i)
    {
        if (fseek(f, (long)((uint64_t)i * rs), SEEK_SET) || fread(r, 1, sizeof r, f) != sizeof r)
            break;
        for (size_t k = 0; k < sizeof r; ++k)
            r[k] = rotr(r[k], 5);
        uint32_t id = rd32(r);
        if (id > 0xFFFF || (id == 0 && i > 0))
            continue;
        size_t blk = find_block(r, sizeof r, legacy ? 0 : want);
        if (!blk)
            continue;
        Sub s[16];
        int ns = block_subs(r + blk, sizeof r - blk, s, 16);
        const char* text[8];
        uint32_t tlen[8];
        int nt = 0, have_num = 0;
        uint32_t num = 0;
        for (int k = 0; k < ns; ++k)
            if (s[k].kind == 1)
            {
                if (!have_num)
                    num = s[k].value, have_num = 1;
            }
            else if (nt < 8)
                text[nt] = s[k].text ? s[k].text : "", tlen[nt] = s[k].text ? s[k].len : 0, ++nt;
        if (!nt)
            continue;
        ResItem* it = item_slot(id);
        if (!it)
            break;
        if (l == 0 || !it->file_id)
        {
            it->kind = (uint8_t)kind;
            it->legacy = (uint8_t)legacy;
            it->file_id = fid;
            it->record = i;
            it->record_size = rs;
            if (kind != RES_ITEM_MONSTROSITY)
            {
                it->flags = rd16(r + 4);
                if (legacy)
                {
                    it->stack = rd16(r + 6);
                    it->type = rd16(r + 8);
                    it->resource_id = rd16(r + 0x0A);
                    it->targets = rd16(r + 0x0C);
                }
                else
                {
                    it->stack = rd16(r + 8);
                    it->type = rd16(r + 0x0A);
                    it->resource_id = rd16(r + 0x0C);
                    it->targets = rd16(r + 0x0E);
                }
            }
            if (!legacy || kind == RES_ITEM_MONSTROSITY)
                parse_item_fields(it, r, kind);
            if (have_num)
                it->article = num;
            uint8_t* raw = arena_alloc(blk);
            if (raw)
            {
                memcpy(raw, r, blk);
                it->raw = raw;
                it->raw_size = (uint16_t)blk;
            }
        }
        it->name[l] = arena_strn(text[0], tlen[0]);
        if (nt >= 4)
        {
            it->log_singular[l] = arena_strn(text[1], tlen[1]);
            it->log_plural[l] = arena_strn(text[2], tlen[2]);
            it->description[l] = arena_strn(text[3], tlen[3]);
        }
        else if (nt >= 2)
            it->description[l] = arena_strn(text[nt - 1], tlen[nt - 1]);
    }
    fclose(f);
}

static int item_id_cmp(const void* a, const void* b)
{
    uint32_t x = (*(ResItem* const*)a)->id, y = (*(ResItem* const*)b)->id;
    return x < y ? -1 : x > y;
}

static void load_items(void)
{
    if (g_items_loaded)
        return;
    g_items_loaded = 1;
    g_item_idx = malloc(sizeof(int32_t) * 0x10000);
    if (!g_item_idx)
        return;
    for (uint32_t i = 0; i < 0x10000; ++i)
        g_item_idx[i] = -1;
    for (int l = 0; l < 2; ++l)
        for (size_t i = 0; i < NITEMFILES; ++i)
            load_item_file(l ? g_item_files[i].ja : g_item_files[i].en, g_item_files[i].kind, g_item_files[i].block, l);
    g_item_list = malloc(sizeof(ResItem*) * (g_nitems ? g_nitems : 1));
    if (g_item_list)
    {
        for (uint32_t i = 0; i < g_nitems; ++i)
            g_item_list[i] = &g_items[i];
        qsort(g_item_list, g_nitems, sizeof(ResItem*), item_id_cmp);
    }
}

const ResItem* res_item(uint32_t id)
{
    load_items();
    if (!g_item_idx || id > 0xFFFF || g_item_idx[id] < 0)
        return NULL;
    return &g_items[g_item_idx[id]];
}

const ResItem* res_item_by_name(const char* name, int lang)
{
    load_items();
    if (!name || !g_item_list)
        return NULL;
    int l = li(lang);
    for (uint32_t i = 0; i < g_nitems; ++i)
        if (ieq(g_item_list[i]->name[l], name))
            return g_item_list[i];
    for (uint32_t i = 0; i < g_nitems; ++i)
    {
        const ResItem* it = g_item_list[i];
        if ((*it->log_singular[l] && ieq(it->log_singular[l], name)) || (*it->log_plural[l] && ieq(it->log_plural[l], name)))
            return it;
    }
    return NULL;
}

const ResItem* const* res_items(uint32_t* count)
{
    load_items();
    if (count)
        *count = g_item_list ? g_nitems : 0;
    return (const ResItem* const*)g_item_list;
}

int res_item_icon(uint32_t id, ResIcon* out)
{
    const ResItem* it = res_item(id);
    if (!it || !out)
        return 0;
    return read_icon(it->file_id, it->record, it->record_size, ITEM_HEAD, 5, out);
}

const char* res_item_kind_name(int kind)
{
    static const char* const n[] = {"General", "Usable", "Weapon", "Armor", "Automaton", "Gil", "Maze", "Monstrosity", "Instinct"};
    return kind >= 0 && kind < (int)(sizeof n / sizeof n[0]) ? n[kind] : "";
}

/* ---- spells and abilities -------------------------------------------------------------------- */

static ResSpell* g_spells;
static ResAbility* g_abilities;
static uint32_t g_nspells, g_nabilities;
static int g_spdata_loaded;

static void unscramble(uint8_t* r, size_t n)
{
    static const int rot[5] = {7, 1, 6, 2, 5};
    if (n < 13)
        return;
    uint8_t k2 = r[2], k11 = r[11], k12 = r[12];
    int d = popc(k2) - popc(k11) + popc(k12);
    int s = rot[(d < 0 ? -d : d) % 5];
    for (size_t i = 0; i < n; ++i)
        r[i] = rotr(r[i], s);
    r[2] = k2, r[11] = k11, r[12] = k12;
}

static const char* name_or_empty(const char* s)
{
    return s ? s : "";
}

static void load_spdata(void)
{
    if (g_spdata_loaded)
        return;
    g_spdata_loaded = 1;
    size_t n = 0;
    uint8_t* d = res_file_read(81, &n);
    if (!d)
        return;
    for (size_t o = 0; o + 16 <= n;)
    {
        uint32_t info = rd32(d + o + 4);
        size_t len = (size_t)(info >> 7) * 16;
        if (len < 16 || o + len > n)
            break;
        uint8_t* p = d + o + 16;
        size_t sz = len - 16;
        if (!memcmp(d + o, "mgc_", 4) && !g_spells)
        {
            g_nspells = (uint32_t)(sz / 0x64);
            g_spells = calloc(g_nspells ? g_nspells : 1, sizeof *g_spells);
            for (uint32_t i = 0; g_spells && i < g_nspells; ++i)
            {
                ResSpell* s = &g_spells[i];
                uint8_t* r = s->raw;
                memcpy(r, p + (size_t)i * 0x64, 0x64);
                unscramble(r, 0x64);
                s->index = rd16(r);
                s->type = rd16(r + 2);
                s->element = rd16(r + 4);
                s->targets = rd16(r + 6);
                s->skill = rd16(r + 8);
                s->mp_cost = rd16(r + 0x0A);
                s->cast_time = r[0x0C];
                s->recast_delay = r[0x0D];
                for (int j = 0; j < 24; ++j)
                    s->levels[j] = (int16_t)rd16(r + 0x0E + j * 2);
                s->id = rd16(r + 0x3E);
                s->icon_nq = rd16(r + 0x40);
                s->icon_hq = rd16(r + 0x42);
                s->requirements = r[0x44];
                s->range = (int8_t)r[0x45];
                s->area_range = r[0x46];
                s->area_shape = r[0x47];
                s->cursor_target = r[0x48];
                s->area_flags = rd32(r + 0x4C);
                s->job_point_mask = rd32(r + 0x5C);
                for (int l = 0; l < 2; ++l)
                {
                    s->name[l] = name_or_empty(res_string("spells.names", i, l ? RES_LANG_JA : RES_LANG_EN));
                    s->description[l] = name_or_empty(res_string("spells.descriptions", i, l ? RES_LANG_JA : RES_LANG_EN));
                }
            }
        }
        else if (!memcmp(d + o, "comm", 4) && !g_abilities)
        {
            g_nabilities = (uint32_t)(sz / 0x30);
            g_abilities = calloc(g_nabilities ? g_nabilities : 1, sizeof *g_abilities);
            for (uint32_t i = 0; g_abilities && i < g_nabilities; ++i)
            {
                ResAbility* a = &g_abilities[i];
                uint8_t* r = a->raw;
                memcpy(r, p + (size_t)i * 0x30, 0x30);
                unscramble(r, 0x30);
                a->id = rd16(r);
                a->type = r[2];
                a->element = r[3];
                a->icon_id = rd16(r + 4);
                a->mp_cost = rd16(r + 6);
                a->recast_id = rd16(r + 8);
                a->targets = rd16(r + 0x0A);
                a->tp_cost = (int16_t)rd16(r + 0x0C);
                a->menu_category = r[0x0E];
                a->monster_level = r[0x0F];
                a->range = (int8_t)r[0x10];
                a->area_range = r[0x11];
                a->area_shape = r[0x12];
                a->cursor_target = r[0x13];
                for (int l = 0; l < 2; ++l)
                {
                    a->name[l] = name_or_empty(res_string("abilities.names", i, l ? RES_LANG_JA : RES_LANG_EN));
                    a->description[l] = name_or_empty(res_string("abilities.descriptions", i, l ? RES_LANG_JA : RES_LANG_EN));
                }
            }
        }
        o += len;
    }
    free(d);
}

static int unnamed(const char* const name[2])
{
    for (int l = 0; l < 2; ++l)
        if (name[l][0] && strcmp(name[l], "."))
            return 0;
    return 1;
}

const ResSpell* res_spell(uint32_t index)
{
    load_spdata();
    if (index >= g_nspells || !g_spells || unnamed(g_spells[index].name))
        return NULL;
    return &g_spells[index];
}

const ResAbility* res_ability(uint32_t id)
{
    load_spdata();
    if (id >= g_nabilities || !g_abilities || unnamed(g_abilities[id].name))
        return NULL;
    return &g_abilities[id];
}

const ResSpell* res_spell_by_name(const char* name, int lang)
{
    load_spdata();
    int l = li(lang);
    for (uint32_t i = 0; name && g_spells && i < g_nspells; ++i)
        if (g_spells[i].name[l][0] && ieq(g_spells[i].name[l], name))
            return &g_spells[i];
    return NULL;
}

const ResAbility* res_ability_by_name(const char* name, int lang)
{
    load_spdata();
    int l = li(lang);
    for (uint32_t i = 0; name && g_abilities && i < g_nabilities; ++i)
        if (g_abilities[i].name[l][0] && ieq(g_abilities[i].name[l], name))
            return &g_abilities[i];
    return NULL;
}

const ResAbility* res_ability_by_recast(uint32_t id)
{
    load_spdata();
    for (uint32_t i = 0x200; g_abilities && i < g_nabilities; ++i)
        if (g_abilities[i].recast_id == id && !unnamed(g_abilities[i].name) && g_abilities[i].type != 3)
            return &g_abilities[i];
    return NULL;
}

uint32_t res_spell_count(void)
{
    load_spdata();
    return g_nspells;
}

uint32_t res_ability_count(void)
{
    load_spdata();
    return g_nabilities;
}

const char* res_ability_type_name(int type)
{
    switch (type)
    {
    case 1: return "JobAbility";
    case 2: return "PetCommand";
    case 3: return "WeaponSkill";
    case 6: return "BloodPactRage";
    case 8: return "CorsairRoll";
    case 9: return "CorsairShot";
    case 10: return "BloodPactWard";
    case 11: return "Samba";
    case 12: return "Waltz";
    case 13: return "Step";
    case 14: return "Flourish1";
    case 15: return "Scholar";
    case 16: return "Jig";
    case 17: return "Flourish2";
    case 18: return "Monster";
    case 19: return "Flourish3";
    case 21: return "Rune";
    case 22: return "Ward";
    case 23: return "Effusion";
    }
    return "";
}

const char* res_spell_type_name(int type)
{
    static const char* const n[] = {"", "WhiteMagic", "BlackMagic", "SummonerPact", "Ninjutsu", "BardSong", "BlueMagic", "Geomancy", "Trust"};
    return type >= 0 && type < (int)(sizeof n / sizeof n[0]) ? n[type] : "";
}

/* ---- status icons / buffs -------------------------------------------------------------------- */

#define STATUS_RS 0x1800
#define STATUS_HEAD 0x280

static ResStatus* g_status_rec;     /* by icon record */
static uint32_t g_nstatus_rec;
static ResStatus* g_status_id;      /* by buff id */
static uint32_t g_nstatus_id;
static int g_status_loaded;

static void load_status_file(uint32_t fid, int l)
{
    char path[1024];
    if (!res_file_path(fid, path, sizeof path))
        return;
    FILE* f = fopen(path, "rb");
    if (!f)
        return;
    long size = -1;
    if (!fseek(f, 0, SEEK_END))
        size = ftell(f);
    uint32_t count = size > 0 ? (uint32_t)(size / STATUS_RS) : 0;
    if (!g_status_rec && count)
    {
        g_status_rec = calloc(count, sizeof *g_status_rec);
        g_nstatus_rec = g_status_rec ? count : 0;
        for (uint32_t i = 0; i < g_nstatus_rec; ++i)
        {
            ResStatus* s = &g_status_rec[i];
            s->index = (uint16_t)i;
            s->id = 0xFFFF;
            s->record = 0xFFFFFFFFu;
            s->description[0] = s->description[1] = s->name[0] = s->name[1] = s->log_name = "";
        }
    }
    uint8_t r[STATUS_HEAD];
    for (uint32_t i = 0; i < count && i < g_nstatus_rec; ++i)
    {
        if (fseek(f, (long)((uint64_t)i * STATUS_RS), SEEK_SET) || fread(r, 1, sizeof r, f) != sizeof r)
            break;
        unscramble(r, sizeof r);
        ResStatus* s = &g_status_rec[i];
        if (l == 0 || s->record == 0xFFFFFFFFu)
        {
            s->id = rd16(r);
            s->can_cancel = r[2];
            s->hide_timer = r[3];
            s->record = i;
        }
        Sub sub[4];
        int n = block_ok(r + 4, sizeof r - 4) ? block_subs(r + 4, sizeof r - 4, sub, 4) : 0;
        for (int k = 0; k < n; ++k)
            if (sub[k].kind == 0 && sub[k].text)
            {
                s->description[l] = arena_strn(sub[k].text, sub[k].len);
                break;
            }
    }
    fclose(f);
}

static void load_status(void)
{
    if (g_status_loaded)
        return;
    g_status_loaded = 1;
    load_status_file(87, 0);
    load_status_file(12, 1);
    uint32_t n = res_string_count("buffs.names", RES_LANG_EN);
    for (uint32_t i = 0; i < g_nstatus_rec; ++i)
        if (g_status_rec[i].id != 0xFFFF && g_status_rec[i].id + 1u > n)
            n = g_status_rec[i].id + 1u;
    g_status_id = calloc(n ? n : 1, sizeof *g_status_id);
    if (!g_status_id)
        return;
    g_nstatus_id = n;
    for (uint32_t i = 0; i < n; ++i)
    {
        ResStatus* s = &g_status_id[i];
        s->index = 0xFFFF;
        s->id = (uint16_t)i;
        s->record = 0xFFFFFFFFu;
        s->description[0] = s->description[1] = "";
    }
    for (uint32_t i = 0; i < g_nstatus_rec; ++i)
    {
        ResStatus* r = &g_status_rec[i];
        if (r->id < n && g_status_id[r->id].index == 0xFFFF)
            g_status_id[r->id] = *r;
    }
    for (uint32_t i = 0; i < n; ++i)
    {
        ResStatus* s = &g_status_id[i];
        s->name[0] = tbl("buffs.names", i, 0);
        s->name[1] = tbl("buffs.names", i, 1);
        s->log_name = tbl("buffs.names_log", i, 0);
        if (s->index < g_nstatus_rec)
        {
            ResStatus* r = &g_status_rec[s->index];
            r->name[0] = s->name[0];
            r->name[1] = s->name[1];
            r->log_name = s->log_name;
        }
    }
}

const ResStatus* res_status_by_index(uint32_t index)
{
    load_status();
    return index < g_nstatus_rec ? &g_status_rec[index] : NULL;
}

const ResStatus* res_status(uint32_t id)
{
    load_status();
    if (id >= g_nstatus_id)
        return NULL;
    const ResStatus* s = &g_status_id[id];
    if (s->index == 0xFFFF && !s->name[0][0] && !s->name[1][0])
        return NULL;
    return s;
}

uint32_t res_status_count(void)
{
    load_status();
    return g_nstatus_rec;
}

int res_status_icon(uint32_t id, ResIcon* out)
{
    const ResStatus* s = res_status(id);
    if (!s || s->index == 0xFFFF || !out)
        return 0;
    return read_icon(87, s->index, STATUS_RS, STATUS_HEAD, 0, out);
}

/* ---- key items, zones, jobs ------------------------------------------------------------------ */

static ResKeyItem* g_ki;
static uint32_t g_nki;
static int g_ki_loaded;

static void load_key_items(void)
{
    if (g_ki_loaded)
        return;
    g_ki_loaded = 1;
    uint32_t n = res_string_count("keyitems.names", RES_LANG_EN);
    uint32_t nj = res_string_count("keyitems.names", RES_LANG_JA);
    if (nj > n)
        n = nj;
    g_ki = calloc(n ? n : 1, sizeof *g_ki);
    if (!g_ki)
        return;
    g_nki = n;
    MsgFile* m = msg_file(55697);
    KeyMap* k = m ? keymap(m) : NULL;
    for (uint32_t i = 0; i < n; ++i)
    {
        ResKeyItem* e = &g_ki[i];
        e->id = i;
        e->name[0] = tbl("keyitems.names", i, 0);
        e->name[1] = tbl("keyitems.names", i, 1);
        e->plural = tbl("keyitems.names_plural", i, 0);
        e->description[0] = tbl("keyitems.descriptions", i, 0);
        e->description[1] = tbl("keyitems.descriptions", i, 1);
        if (k && i < k->n && k->entry[i] >= 0)
        {
            int isn = 0;
            msg_sub(m, (uint32_t)k->entry[i], 1, &e->category, &isn);
        }
    }
}

const ResKeyItem* res_key_item(uint32_t id)
{
    load_key_items();
    if (id >= g_nki || (!g_ki[id].name[0][0] && !g_ki[id].name[1][0]))
        return NULL;
    return &g_ki[id];
}

static ResZone* g_zones;
static uint32_t g_nzones;
static ResJob* g_jobs;
static uint32_t g_njobs;
static int g_zj_loaded;

static void load_zones_jobs(void)
{
    if (g_zj_loaded)
        return;
    g_zj_loaded = 1;
    uint32_t n = res_string_count("zones.names", RES_LANG_EN);
    g_zones = calloc(n ? n : 1, sizeof *g_zones);
    g_nzones = g_zones ? n : 0;
    for (uint32_t i = 0; i < g_nzones; ++i)
    {
        g_zones[i].id = i;
        g_zones[i].name[0] = tbl("zones.names", i, 0);
        g_zones[i].name[1] = tbl("zones.names", i, 1);
        g_zones[i].search = tbl("zones.names_search", i, 0);
        g_zones[i].abbr = tbl("zones.names_abbr", i, 0);
    }
    n = res_string_count("jobs.names", RES_LANG_EN);
    g_jobs = calloc(n ? n : 1, sizeof *g_jobs);
    g_njobs = g_jobs ? n : 0;
    for (uint32_t i = 0; i < g_njobs; ++i)
    {
        g_jobs[i].id = i;
        g_jobs[i].name[0] = tbl("jobs.names", i, 0);
        g_jobs[i].name[1] = tbl("jobs.names", i, 1);
        g_jobs[i].abbr[0] = tbl("jobs.names_abbr", i, 0);
        g_jobs[i].abbr[1] = tbl("jobs.names_abbr", i, 1);
    }
}

const ResZone* res_zone(uint32_t id)
{
    load_zones_jobs();
    return id < g_nzones ? &g_zones[id] : NULL;
}

const ResJob* res_job(uint32_t id)
{
    load_zones_jobs();
    return id < g_njobs ? &g_jobs[id] : NULL;
}

/* ---- init ------------------------------------------------------------------------------------ */

void res_shutdown(void)
{
    free(g_dir);
    g_dir = NULL;
    for (int i = 0; i < g_overlays; ++i)
        free(g_overlay[i]);
    g_overlays = 0;
    for (int r = 0; r < 10; ++r)
    {
        free(g_tab[r].v);
        free(g_tab[r].f);
    }
    memset(g_tab, 0, sizeof g_tab);
    g_tabs_loaded = 0;
    for (int i = 0; i < g_msgs; ++i)
    {
        free(g_msg[i].data);
        free(g_msg[i].off);
        free(g_msg[i].len);
    }
    free(g_msg);
    g_msg = NULL;
    g_msgs = g_msg_cap = 0;
    for (int i = 0; i < g_keymaps; ++i)
        free(g_keymap[i].entry);
    g_keymaps = 0;
    for (int l = 0; l < 2; ++l)
    {
        free(g_at[l]);
        g_at[l] = NULL;
        g_at_n[l] = 0;
        g_at_loaded[l] = 0;
    }
    free(g_items);
    free(g_item_list);
    free(g_item_idx);
    g_items = NULL;
    g_item_list = NULL;
    g_item_idx = NULL;
    g_nitems = g_items_cap = 0;
    g_items_loaded = 0;
    free(g_spells);
    free(g_abilities);
    g_spells = NULL;
    g_abilities = NULL;
    g_nspells = g_nabilities = 0;
    g_spdata_loaded = 0;
    free(g_status_rec);
    free(g_status_id);
    g_status_rec = g_status_id = NULL;
    g_nstatus_rec = g_nstatus_id = 0;
    g_status_loaded = 0;
    free(g_ki);
    g_ki = NULL;
    g_nki = 0;
    g_ki_loaded = 0;
    free(g_zones);
    free(g_jobs);
    g_zones = NULL;
    g_jobs = NULL;
    g_nzones = g_njobs = 0;
    g_zj_loaded = 0;
    arena_free();
}

void res_init(const char* game_dir, const char* const* overlays, int overlay_count)
{
    res_shutdown();
    g_dir = game_dir ? dupstr(game_dir) : NULL;
    for (int i = 0; i < overlay_count && g_overlays < MAX_OVERLAYS; ++i)
        if (overlays[i] && *overlays[i])
            g_overlay[g_overlays++] = dupstr(overlays[i]);
}

void res_set_default_lang(int lang)
{
    g_default_lang = lang == RES_LANG_JA ? RES_LANG_JA : RES_LANG_EN;
}

/* host/addons/game.c against the unpacked image, no running game: every pattern resolves to the
 * address documented in game.c, and the generic field path reads and writes planted data.
 *
 * The image is mapped at its base inside a reserved 4 GB window (the same layout the runtime uses:
 * guest address a is at rt_guest_base + a); the host core's functions game.c needs (xi_mapped,
 * xi_image, xi_find_pattern, xi_log) are small stand-ins here. A fake heap at 0x20000000 holds a
 * character block and an entity, reached through the image's own globals.
 *
 *   clang -std=c11 -O1 -Wall -DRT_GUEST_WINDOW -DXI_GAME_NO_GUEST_CALL -Iruntime -Iruntime/portable \
 *       -Ihost/addons -o build/game_test tests/game_test.c host/addons/game.c
 *   build/game_test [generated/FFXiMain.unpacked.dll] */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "game.h"
#include "xi.h"

unsigned char* rt_guest_base;

static uint32_t img_base, img_size, text_start, text_size;
#define HEAP 0x20000000u
#define HEAP_SIZE 0x01000000u

static int failures, checks;

static void check(int ok, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    checks++;
    if (!ok)
    {
        printf("FAIL  ");
        vprintf(fmt, ap);
        printf("\n");
        failures++;
    }
    else if (getenv("VERBOSE"))
    {
        printf("ok    ");
        vprintf(fmt, ap);
        printf("\n");
    }
    va_end(ap);
}

/* --- host core stand-ins ---------------------------------------------------------------------- */

void xi_log(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("[addons] ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

void xi_log_once(const char* key, const char* fmt, ...)
{
    (void)key;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

int xi_mapped(uint32_t a, uint32_t n)
{
    uint64_t e = (uint64_t)a + n;
    return (a >= img_base && e <= (uint64_t)img_base + img_size) || (a >= HEAP && e <= (uint64_t)HEAP + HEAP_SIZE);
}

void xi_image(uint32_t* base, uint32_t* size, uint32_t* text, uint32_t* tsize)
{
    *base = img_base;
    *size = img_size;
    *text = text_start;
    *tsize = text_size;
}

static int parse_pattern(const char* p, int16_t* out)
{
    int n = 0;
    while (*p)
    {
        if (*p == ' ')
        {
            p++;
            continue;
        }
        if (p[0] == '?' && p[1] == '?')
            out[n++] = -1;
        else
        {
            char h[3] = {p[0], p[1], 0};
            out[n++] = (int16_t)strtol(h, NULL, 16);
        }
        p += 2;
    }
    return n;
}

uint32_t xi_find_pattern(uint32_t start, uint32_t size, const char* pattern, int32_t offset, uint32_t count)
{
    int16_t pat[256];
    int n = parse_pattern(pattern, pat);
    const uint8_t* m = GUEST_PTR(start);
    for (uint32_t i = 0; i + n <= size; i++)
    {
        int k = 0;
        while (k < n && (pat[k] < 0 || m[i + k] == pat[k]))
            k++;
        if (k == n && count-- == 0)
            return start + i + (uint32_t)offset;
    }
    return 0;
}

static uint32_t hits(const char* pattern)
{
    uint32_t n = 0;
    while (xi_find_pattern(text_start, text_size, pattern, 0, n))
        n++;
    return n;
}

/* --- a minimal PE mapper ---------------------------------------------------------------------- */

static int load_image(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* file = malloc((size_t)n);
    if (!file || fread(file, 1, (size_t)n, f) != (size_t)n)
        return 0;
    fclose(f);
    uint32_t pe = *(uint32_t*)(file + 0x3C);
    uint16_t nsec = *(uint16_t*)(file + pe + 6), optsize = *(uint16_t*)(file + pe + 20);
    const uint8_t* opt = file + pe + 24;
    img_base = *(uint32_t*)(opt + 28);
    img_size = *(uint32_t*)(opt + 56);
    uint32_t hdrsize = *(uint32_t*)(opt + 60);
    memcpy(GUEST_PTR(img_base), file, hdrsize);
    const uint8_t* sec = opt + optsize;
    for (int i = 0; i < nsec; i++, sec += 40)
    {
        uint32_t vsize = *(uint32_t*)(sec + 8), va = *(uint32_t*)(sec + 12);
        uint32_t rawsize = *(uint32_t*)(sec + 16), raw = *(uint32_t*)(sec + 20);
        uint32_t copy = rawsize < vsize ? rawsize : vsize;
        if (copy && raw + copy <= (uint32_t)n)
            memcpy(GUEST_PTR(img_base + va), file + raw, copy);
        if (memcmp(sec, ".text", 6) == 0)
        {
            text_start = img_base + va;
            text_size = vsize;
        }
    }
    free(file);
    return text_size != 0;
}

/* --- helpers ---------------------------------------------------------------------------------- */

static void w8(uint32_t a, uint8_t v) { *GUEST_PTR(a) = v; }
static void w16(uint32_t a, uint16_t v) { memcpy(GUEST_PTR(a), &v, 2); }
static void w32(uint32_t a, uint32_t v) { memcpy(GUEST_PTR(a), &v, 4); }
static void wf(uint32_t a, float v) { memcpy(GUEST_PTR(a), &v, 4); }

static double num(int sid, uint32_t base, const char* path, int32_t sub)
{
    uint32_t extra;
    const xi_field* f = xi_game_lookup(sid, path, &extra);
    xi_value v;
    if (!f || xi_game_read(f, base, extra, sub, &v) != XI_V_NUM)
        return -12345.0;
    return v.num;
}

static const char* str(int sid, uint32_t base, const char* path)
{
    static xi_value v;
    uint32_t extra;
    const xi_field* f = xi_game_lookup(sid, path, &extra);
    if (!f || xi_game_read(f, base, extra, -1, &v) != XI_V_STR)
        return "(nil)";
    return v.str;
}

static int setn(int sid, uint32_t base, const char* path, int32_t sub, double val)
{
    uint32_t extra;
    const xi_field* f = xi_game_lookup(sid, path, &extra);
    return f && xi_game_write_num(f, base, extra, sub, val);
}

int main(int argc, char** argv)
{
    const char* path = argc > 1 ? argv[1] : "generated/FFXiMain.unpacked.dll";
    rt_guest_base = mmap(NULL, 1ull << 32, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
    if (rt_guest_base == MAP_FAILED)
    {
        printf("cannot reserve the 4 GB window\n");
        return 2;
    }
    if (!load_image(path))
    {
        printf("cannot load %s\n", path);
        return 2;
    }
    printf("image 0x%08X size 0x%X, .text 0x%08X size 0x%X\n", img_base, img_size, text_start, text_size);

    /* 1. every pattern: the match and the value it resolves to, for the 2026-09-03 image */
    static const struct
    {
        int which;
        uint32_t match, value, nhits;
    } want[] = {
        {XI_P_ENTITY_MAP, 0x10003A17 + 9, 0x10480AF0, 1},
        {XI_P_ENTITY_COUNT, 0x10095988 + 3, 0x900, 1},
        {XI_P_PLAYER_INDEX, 0x101CC4F0 + 2, 0x10485F7A, 1},
        {XI_P_PARTY, 0x10202BA4 + 23, 0x10663AD8, 1},
        {XI_P_PARTY_ICONS, 0x10098F56 + 9, 0x104809A0, 1},
        {XI_P_PLAYER, 0x1014735E + 28, 0x10485638, 2},
        {XI_P_TARGET, 0x10088BD6 + 45, 0x10578478, 1},
        {XI_P_CHAR, 0x100EB555 + 1, 0x104DFD98, 1},
        {XI_P_INVENTORY_OFS, 0x100EB555 + 8, 0x9868, 1},
        {XI_P_AUTOFOLLOW, 0x1001F253 + 25, 0x10487F58, 1},
        {XI_P_CASTBAR, 0x1007B4E0 + 5, 0x1057817C, 1},
        {XI_P_KEYITEMS, 0x10097E50 + 22, 0x1047FEB8, 2},
        {XI_P_KEYITEMS_SEEN, 0x10097EB0 + 22, 0x104800B8, 2},
        {XI_P_JOBLEVEL_FN, 0x100F0FB0, 0x100F0FB0, 1},
        {XI_P_MASTERLEVEL_FN, 0x100F0FF0, 0x100F0FF0, 1},
        {XI_P_MASTERFLAG_FN, 0x100F1020, 0x100F1020, 1},
        {XI_P_SPELLS_FN, 0x100F00A0, 0x100F00A0, 3},
        {XI_P_ABILITIES_FN, 0x100E6020, 0x100E6020, 1},
        {XI_P_RECAST_ABILITY, 0x1022471B + 25, 0x10666EC0, 1},
        {XI_P_RECAST_SPELL, 0x1022B290 + 2, 0x106671E0, 1},
        {XI_P_PET_MP, 0x100875F5 + 10, 0x10482F33, 1},
        {XI_P_PET_BLOCK, 0x1009CEE0 + 61, 0x10482F28, 1},
        {XI_P_SET_TARGET, 0x1007A617, 0x1007A617, 4},
    };
    /* the pattern strings, to count hits (same order as game.c's table) */
    static const char* const pats[XI_P_COUNT] = {
        [XI_P_ENTITY_MAP] = "8B560C8B042A8B0485",
        [XI_P_ENTITY_COUNT] = "4781FF????????7C??89",
        [XI_P_PLAYER_INDEX] = "66A1????????6685C074??0FBFC08B0485????????85C075??33C0C3",
        [XI_P_PARTY] = "0FBEC38D0C5256578BF58D0448",
        [XI_P_PARTY_ICONS] = "B93C0000008D7004BF????????F3A5",
        [XI_P_PLAYER] = "6A018D44242C68808080806683????506683????5152E8????????A1",
        [XI_P_TARGET] = "53568BF18B480433DB3BCB75065E33C05B59C38B0D",
        [XI_P_CHAR] = "A1????????8D9488????????8990????????E9",
        [XI_P_INVENTORY_OFS] = "A1????????8D9488????????8990????????E9",
        [XI_P_AUTOFOLLOW] = "8BCFE8????????8B0D????????E8????????8BE885ED750CB9",
        [XI_P_CASTBAR] = "85F674??A1????????85C074??8B4808",
        [XI_P_KEYITEMS] = "8B44240485C07C??3D????????7D??8B4C24088B1485",
        [XI_P_KEYITEMS_SEEN] = "8B44240485C07C??3D????????7D??8B4C24088B1485",
        [XI_P_JOBLEVEL_FN] = "8B0D????????85C974??8B4424043C1073??25FF000000",
        [XI_P_MASTERLEVEL_FN] = "8B0D????????85C974??8B4424043C1873??25FF000000",
        [XI_P_MASTERFLAG_FN] = "8B15????????85D274??8A4C240480F91873??B801000000D3E02382????????F7D81BC0F7D8C3",
        [XI_P_SPELLS_FN] = "A1????????85C075??C38A88????????84C975??33C0C305????????C3",
        [XI_P_ABILITIES_FN] = "A1????????85C074??8A88????????84C974??8A88????????84C974??05????????C333C0C3",
        [XI_P_RECAST_ABILITY] = "894124E9????????8B46??6A006A00508BCEE8",
        [XI_P_RECAST_SPELL] = "56BE????????66833E00??????????????662906",
        [XI_P_PET_MP] = "84C0750333C0C333C0A0????????C3",
        [XI_P_PET_BLOCK] = "8B44240C5657668B48048D70048BC183E03F83E802",
        [XI_P_SET_TARGET] = "8B0D????????83C40885C974??85C074??6A006A0150E8",
    };
    printf("\n%-24s %-10s %-10s %s\n", "pointer", "match+ofs", "value", "hits");
    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++)
    {
        uint32_t v = xi_game_ptr_value(want[i].which), m = xi_game_ptr_match(want[i].which);
        uint32_t h = hits(pats[want[i].which]);
        printf("%-24s 0x%08X 0x%08X %u\n", xi_game_ptr_name(want[i].which), m, v, h);
        check(m == want[i].match, "%s match 0x%08X, want 0x%08X", xi_game_ptr_name(want[i].which), m, want[i].match);
        check(v == want[i].value, "%s value 0x%08X, want 0x%08X", xi_game_ptr_name(want[i].which), v, want[i].value);
        check(h == want[i].nhits, "%s hits %u, want %u", xi_game_ptr_name(want[i].which), h, want[i].nhits);
    }
    /* the target_t global is also what the auto-follow pattern loads at +9 */
    uint32_t af = xi_find_pattern(text_start, text_size, pats[XI_P_AUTOFOLLOW], 9, 0);
    check(xi_game_rd32(af) == xi_game_ptr_value(XI_P_TARGET) + 0x2F4, "target_t global = target window global + 0x2F4 (0x%08X)",
          xi_game_rd32(af));
    /* SetTarget's call site: target_t global and the callee */
    {
        uint32_t site = xi_game_ptr_value(XI_P_SET_TARGET), rel = xi_game_rd32(site + 0x17);
        check(xi_game_rd32(site + 2) == 0x1057876C && site + 0x1B + rel == 0x10157B40, "SetTarget call site -> 0x%08X",
              site + 0x1B + rel);
    }
    /* the second player hit reads MPMax, 4 bytes on */
    check(xi_game_rd32(xi_find_pattern(text_start, text_size, pats[XI_P_PLAYER], 28, 1)) == 0x1048563C, "player hit 1 = MPMax");

    /* 2. descriptors */
    int s_entity = xi_game_struct_id("entity_t"), s_player = xi_game_struct_id("player_t"),
        s_inv = xi_game_struct_id("inventory_t"), s_member = xi_game_struct_id("partymember_t"),
        s_target = xi_game_struct_id("target_t"), s_item = xi_game_struct_id("item_t");
    check(xi_game_struct(s_entity)->size == 0x2AC, "sizeof entity_t");
    check(xi_game_struct(s_player)->size == 0x438, "sizeof player_t");
    check(xi_game_struct(s_inv)->size == 0x1057C, "sizeof inventory_t");
    uint32_t extra;
    const xi_field* f = xi_game_lookup(s_entity, "Render.Flags0", &extra);
    check(f && extra + f->offset == 0x120, "entity Render.Flags0 at 0x120");
    f = xi_game_lookup(s_entity, "PetTargetIndex", &extra);
    check(f && extra + f->offset == 0x1FA, "entity PetTargetIndex at 0x1FA");
    f = xi_game_lookup(s_target, "Targets[1].Index", &extra);
    check(f && extra + f->offset == 40, "target Targets[1].Index at 40");
    f = xi_game_lookup(s_player, "AbilityInfo[3].TimerId", &extra);
    check(f && extra + f->offset == 0xEC + 3 * 8 + 3, "player AbilityInfo[3].TimerId");
    f = xi_game_lookup(s_inv, "pItem", &extra);
    check(f && f->offset == 0x19304 - 0x9868, "inventory pItem = char+0x19304 in the code");
    check(xi_game_path_id(s_entity, "Movement.LocalPosition.X") >= 0, "path id for a nested path");
    check(xi_game_lookup(s_entity, "NoSuchField", NULL) == NULL, "unknown path");

    /* 3. planted data, reached through the image's own globals */
    uint32_t chr = HEAP, ent = HEAP + 0x100000, cast = HEAP + 0x110000, tgt = HEAP + 0x120000,
             twin = HEAP + 0x121000;
    w32(0x104DFD98, chr);
    check(xi_game_char() == chr, "char block through 0x104DFD98");
    uint32_t inv = xi_game_inventory();
    check(inv == chr + 0x9868, "inventory = char + 0x9868");
    uint32_t it = xi_game_container_item(0, 1);
    w16(it, 0x1234);
    w32(it + 4, 5);
    w16(xi_game_container_item(5, 3), 0x0010);
    w8(inv + 0x10029, 81);
    check(xi_game_container_count(0) == 1, "container 0 count");
    check(xi_game_container_max(0) == 81, "container 0 max");
    check(num(s_item, it, "Count", -1) == 5, "item count");
    check(num(s_inv, inv, "ContainerMaxCapacity", 0) == 81, "ContainerMaxCapacity[0]");

    /* entity 0x400 is the local player */
    uint32_t map = xi_game_ptr_value(XI_P_ENTITY_MAP);
    memset(GUEST_PTR(map), 0, 0x900 * 4);
    w32(map + 0x400 * 4, ent);
    w32(ent + 0x78, 0x01020304);
    memcpy(GUEST_PTR(ent + 0x7C), "Josh", 5);
    wf(ent + 4, 1.5f);
    w16(0x10485F7A, 0x400);
    w32(0x10485638 + 0x3AC, 0); /* player_t.IsZoning */
    check(xi_game_entity(0x400) == ent, "entity 0x400");
    check(xi_game_entity(0x401) == 0, "entity 0x401 absent");
    check(xi_game_entity(0x900) == 0, "entity out of range");
    check(xi_game_player_index() == 0x400, "local player index");
    check(xi_game_entity_by_server_id(0x01020304) == 0x400, "entity by server id");
    check(num(s_entity, ent, "Movement.LocalPosition.X", -1) == 1.5, "LocalPosition.X");
    check(strcmp(str(s_entity, ent, "Name"), "Josh") == 0, "entity Name");
    check(setn(s_entity, ent, "Render.Flags0", -1, 0x40000200) && xi_game_rd32(ent + 0x120) == 0x40000200, "write Render.Flags0");
    check(setn(s_entity, ent, "Animations", 3, 0x31313131) && xi_game_rd32(ent + 0x190 + 12) == 0x31313131,
          "write Animations[3]");
    check(num(s_entity, ent, "HPPercent", -1) == 0, "HPPercent");
    check(setn(s_entity, ent, "HPPercent", -1, 300) && xi_game_rd8(ent + 0xEC) == 300 % 256, "u8 write wraps");
    {
        uint32_t ex;
        const xi_field* nf = xi_game_lookup(s_entity, "Name", &ex);
        check(xi_game_write_str(nf, ent, ex, "AVeryLongNameThatDoesNotFitInTheField", 37) &&
                  strlen(str(s_entity, ent, "Name")) == 27,
              "Name write truncates to 27 + NUL");
    }
    check(xi_game_login_status() == 2, "login status 2 in game");
    w32(0x10485638 + 0x3AC, 1);
    check(xi_game_login_status() == 1, "login status 1 while zoning");
    w32(0x10485638 + 0x3AC, 0);

    /* player bitfields and nested paths */
    uint32_t pl = xi_game_player();
    check(setn(s_player, pl, "MeritPoints", -1, 75) && setn(s_player, pl, "IsLimitBreaker", -1, 1), "write bitfields");
    check(num(s_player, pl, "MeritPoints", -1) == 75 && num(s_player, pl, "AssimilationPoints", -1) == 0 &&
              num(s_player, pl, "IsLimitBreaker", -1) == 1 && xi_game_rd16(pl + 0x1F0) == (75 | 1 << 13),
          "bitfields read back (0x%04X)", xi_game_rd16(pl + 0x1F0));
    w32(pl + 0x54, (3u) | (12345u << 10));
    check(num(s_player, pl, "UnityInfo.Bits.Faction", -1) == 3 && num(s_player, pl, "UnityInfo.Bits.Points", -1) == 12345,
          "unity bits");
    w8(pl + 0xEC + 5 * 8 + 3, 42);
    check(num(s_player, pl, "AbilityInfo[5].TimerId", -1) == 42, "AbilityInfo[5].TimerId");
    w16(pl + 0x3F8 + 2, 33);
    check(num(s_player, pl, "Buffs", 1) == 33, "Buffs[1]");

    /* job levels, master levels and flags through the decoded functions */
    w8(chr + 0x1A628 + 3, 75);
    w8(chr + 0x1A660 + 17, 50);
    w8(chr + 0x1A67C + 22, 12);
    w32(chr + 0x1A678, 1u << 22);
    check(xi_game_job_level(3) == 75, "job level 3");
    check(xi_game_job_level(17) == 50, "job level 17 (second table)");
    check(xi_game_master_level(22) == 12, "master level 22");
    check(xi_game_master_flags() == (1 << 22), "master flags");

    /* key items, spells, abilities */
    w32(0x1047FEB8 + 4 * 2, 1u << 5); /* id 69 */
    w32(0x104800B8, 1u << 7);
    check(xi_game_key_item(69) == 1 && xi_game_key_item(70) == 0, "key item 69");
    check(xi_game_key_item_seen(7) == 1 && xi_game_key_item_seen(69) == 0, "key item seen 7");
    check(!xi_game_has_spell_data() && xi_game_spell_known(3) == 0, "no spell data yet");
    w8(chr + 0x2CFD4, 1);
    w8(chr + 0x2CF54, 1 << 3);
    check(xi_game_has_spell_data() && xi_game_spell_known(3) == 1 && xi_game_spell_known(4) == 0, "spell 3 known");
    w8(chr + 0x2CF49, 1);
    w8(chr + 0x2CF4A, 1);
    w8(chr + 0x2CDE8 + 0x200 / 8, 1 << 1); /* job ability 0x201 */
    check(xi_game_has_ability_data() && xi_game_ability_bit(0x201) == 1 && xi_game_ability_bit(0x200) == 0, "ability 0x201");

    /* recasts */
    uint32_t ro = xi_game_ptr_value(XI_P_RECAST_ABILITY);
    w16(ro + 2 * 8, 300);
    w8(ro + 2 * 8 + 3, 99);
    w32(ro + 0xF8 + 2 * 4, 1800);
    uint32_t t, id, rc, c1;
    int32_t c2;
    check(xi_game_ability_recast(2, &t, &id, &rc, &c1, &c2) && t == 1800 && id == 99 && rc == 300, "ability recast 2");
    w16(0x106671E0 + 2 * 1024, 600);
    check(xi_game_spell_recast(1024) == 600 && xi_game_spell_recast(1025) == -1, "spell recast 1024");

    /* pet: entity PetTargetIndex must match the block's pet index */
    w16(ent + 0x1FA, 0x700);
    w16(0x10482F28 + 8, 0x700);
    w8(0x10482F33, 88);
    w32(0x10482F28 + 0xC, 1500);
    uint32_t mpp, tp, pidx;
    check(xi_game_pet(&mpp, &tp, &pidx) && mpp == 88 && tp == 1500 && pidx == 0x700, "pet");
    w16(0x10482F28 + 8, 0x701);
    check(!xi_game_pet(&mpp, &tp, &pidx), "no pet when the index differs");

    /* cast bar, target */
    w32(0x1057817C, 0);
    check(xi_game_castbar() == 0, "no cast bar");
    w32(0x1057817C, cast);
    wf(cast + 0x1C, 0.25f);
    check(xi_game_castbar() == cast && num(xi_game_struct_id("castbar_t"), cast, "Percent", -1) == 0.25, "cast bar percent");
    w32(0x10578478, twin);
    w32(0x10578478 + 0x2F4, tgt);
    w32(tgt + 40, 0x123);
    check(xi_game_target() == tgt && xi_game_target_window() == twin, "target pointers");
    check(num(s_target, tgt, "Targets[1].Index", -1) == 0x123, "Targets[1].Index");

    /* party */
    check(xi_game_party() == 0x10663AD8 && xi_game_party_member(17) == 0x10663AD8 + 17 * 0x7C &&
              xi_game_party_member(18) == 0,
          "party members");
    w32(0x10663AD8, 0x10664390);
    check(xi_game_alliance() == 0x10664390, "alliance info via member 0");
    w32(0x10663AD8 + 0x7C + 0x18, 0xAABBCCDD);
    check(num(s_member, xi_game_party_member(1), "ServerId", -1) == (double)0xAABBCCDD, "member 1 server id");
    uint32_t ic = xi_game_party_icons();
    memset(GUEST_PTR(ic), 0xFF, 48);
    w8(ic + 16 + 0, 0x21);
    uint64_t mask = 0;
    mask |= 1ull << 0; /* icon 0 high bits = 1 -> 0x121 */
    memcpy(GUEST_PTR(ic + 8), &mask, 8);
    int16_t icons[32];
    check(xi_game_party_member_icons(0, icons) && icons[0] == 0x121 && icons[1] == 0xFF, "party icons decode (%d, %d)",
          icons[0], icons[1]);

    /* unmapped pointers never fault */
    w32(0x104DFD98, 0x70000000);
    check(xi_game_inventory() == 0 && xi_game_job_level(3) == -1 && xi_game_container_count(0) == -1,
          "unmapped char block reads as absent");
    w32(map + 0x400 * 4, 0x70000000);
    check(xi_game_entity(0x400) == 0, "unmapped entity pointer reads as absent");

    printf("\n%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}

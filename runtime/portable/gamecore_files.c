/* Our own gamecore, paths / files / registry group (R3): the slots specified in
 * specs/gamecore-slots.files.txt, all cdecl (RETC).
 *
 * The core's file cipher (used by patch.ver, option.bin, ContentsData and the dictionaries) is
 * implemented both ways; the decryption and its inverse were first checked in Python against the
 * retail patch.ver ("30260805_0", and a byte-identical round trip).
 *
 * Choices where the spec left a design decision (docs/client-native-arm64.md, R3.1 progress):
 * patch.ver keyed from the game's registry key's Interface value, as retail; option.bin read, not
 * written back; the profanity filter off (LSB checks names server-side); no storage server
 * ("NO DATA"); our own English wording for the message lines. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gthread.h"
#include "gwin.h"
#include "k32.h"
#include "plat.h"
#include "gamecore.h"
#include "gamecore_config.h"
#include "vfs.h"

/* --- the cipher -------------------------------------------------------------------------------- */
typedef struct CoreKey
{
    uint64_t k[32];
    uint8_t s[32]; /* byte sum of each k */
} CoreKey;

static uint32_t ror32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static uint32_t rol32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

/* The 32 round keys from K[0] (K[j] = 5 K[j-1]) and their byte sums. */
static void core_key_k0(uint64_t k0, CoreKey* key)
{
    key->k[0] = k0;
    for (int j = 1; j < 32; ++j)
        key->k[j] = key->k[j - 1] * 5;
    for (int j = 0; j < 32; ++j)
    {
        uint8_t sum = 0;
        for (int i = 0; i < 8; ++i)
            sum = (uint8_t)(sum + (uint8_t)(key->k[j] >> (8 * i)));
        key->s[j] = sum;
    }
}

/* The key schedule: a 64-bit key to K[0], then the round keys. */
static void core_key(uint64_t key64, CoreKey* key)
{
    uint32_t lo = (uint32_t)key64, hi = (uint32_t)(key64 >> 32);
    uint32_t a = ror32(hi, 16), b = rol32(lo, 8);
    uint8_t s[8];
    memcpy(s, &a, 4); /* little-endian hosts only: x86-64 and arm64 */
    memcpy(s + 4, &b, 4);
    s[0] = (uint8_t)(s[0] + 0x45);
    for (int i = 1; i < 8; ++i)
    {
        uint8_t c = (uint8_t)(s[i] + s[i - 1] - 0x2c);
        s[i] = (uint8_t)((s[i - 1] << 2) ^ c ^ 0x45);
    }
    uint64_t k0;
    memcpy(&k0, s, 8);
    core_key_k0(k0, key);
}

static uint64_t tweak(uint32_t ctr)
{
    uint64_t t = ctr;
    for (int i = 0; i < 3; ++i)
        t = (t << 10) | ctr;
    return t + 0xa1652347u;
}

/* Decrypts n bytes (a multiple of 8) under round keys; 1 if the trailer's checksum matches. */
static int core_decrypt_with(const uint8_t* in, uint8_t* out, uint32_t n, const CoreKey* kp)
{
    CoreKey key = *kp;
    uint32_t sum = 0, before = 0;
    for (uint32_t off = 0; off < n; off += 8)
    {
        if (off == n - 8)
            before = sum;
        unsigned idx = (off >> 3) & 31;
        uint8_t x[8], d = (uint8_t)(((off >> 3) ^ 0x45) & 0xff);
        for (int k = 0; k < 8; ++k)
        {
            uint8_t c = in[off + k];
            x[k] = (uint8_t)(((c ^ d) + 0xc0 - key.s[idx]) & 0xff);
            d = c;
        }
        uint64_t v;
        memcpy(&v, x, 8);
        v -= key.k[idx];
        v ^= tweak(off);
        v ^= key.k[idx];
        v = (v >> 32) | (v << 32);
        memcpy(out + off, &v, 8);
        sum += out[off] + out[off + 4];
    }
    uint32_t check;
    memcpy(&check, out + n - 4, 4);
    return n >= 8 && before == check;
}

static int core_decrypt(const uint8_t* in, uint8_t* out, uint32_t n, uint64_t key64)
{
    CoreKey key;
    core_key(key64, &key);
    return core_decrypt_with(in, out, n, &key);
}

/* The byte stage of one block's decryption, as a little-endian u64 (it depends on the key only
 * through the byte sum S). */
static uint64_t byte_stage(const uint8_t* block, uint32_t ctr, uint8_t s)
{
    uint8_t x[8], d = (uint8_t)(((ctr >> 3) ^ 0x45) & 0xff);
    for (int k = 0; k < 8; ++k)
    {
        x[k] = (uint8_t)(((block[k] ^ d) + 0xc0 - s) & 0xff);
        d = block[k];
    }
    uint64_t v;
    memcpy(&v, x, 8);
    return v;
}

/* Every K with ((v - m*K) ^ (m*K)) == t, found bit by bit from the least significant: bit i of
 * m*K depends only on bits 0..i of K. Calls found(K) for each; stops when it returns 1. */
static int solve_bits(uint64_t v, uint64_t t, uint64_t m, uint64_t k, int bit, int (*found)(uint64_t, void*), void* ctx)
{
    if (bit == 64)
        return found(k, ctx);
    uint64_t mask = bit == 63 ? ~0ull : (1ull << (bit + 1)) - 1;
    for (uint64_t b = 0; b < 2; ++b)
    {
        uint64_t kk = k | (b << bit), mk = m * kk;
        if ((((v - mk) ^ mk) & mask) == (t & mask) && solve_bits(v, t, m, kk, bit + 1, found, ctx))
            return 1;
    }
    return 0;
}

typedef struct Recover
{
    const uint8_t* data;
    uint8_t s0;
    uint64_t k0;
} Recover;

static uint8_t byte_sum(uint64_t k)
{
    uint8_t s = 0;
    for (int i = 0; i < 8; ++i)
        s = (uint8_t)(s + (uint8_t)(k >> (8 * i)));
    return s;
}

static int recover_found(uint64_t k0, void* p)
{
    Recover* r = (Recover*)p;
    if (byte_sum(k0) != r->s0)
        return 0;
    uint64_t k1 = k0 * 5, v1 = byte_stage(r->data + 8, 8, byte_sum(k1));
    if (((v1 - k1) ^ k1) != tweak(8))
        return 0;
    r->k0 = k0;
    return 1;
}

/* Known-plaintext key recovery (specs/gamecore-slots.files.txt, slot 1171):
 * the first two plaintext blocks of patch.ver are zero, so block 0 fixes K[0] up to its byte sum
 * S0 (tried in turn) and block 1, under K[1] = 5 K[0], confirms it. No registry needed; the same
 * key the registry path derives, for every retail title. */
static int core_recover_k0(const uint8_t* data, uint64_t* k0)
{
    Recover r = { data, 0, 0 };
    for (unsigned s0 = 0; s0 < 256; ++s0)
    {
        r.s0 = (uint8_t)s0;
        if (solve_bits(byte_stage(data, 0, (uint8_t)s0), tweak(0), 1, 0, 0, recover_found, &r))
        {
            *k0 = r.k0;
            return 1;
        }
    }
    return 0;
}

/* The inverse. plain: n bytes whose last 8 are the trailer {payload length, checksum}; the
 * checksum is filled in here. */
static void core_encrypt(uint8_t* plain, uint8_t* out, uint32_t n, uint64_t key64)
{
    uint32_t sum = 0;
    for (uint32_t off = 0; off + 8 < n; off += 8)
        sum += plain[off] + plain[off + 4];
    memcpy(plain + n - 4, &sum, 4);
    CoreKey key;
    core_key(key64, &key);
    for (uint32_t off = 0; off < n; off += 8)
    {
        unsigned idx = (off >> 3) & 31;
        uint64_t v;
        memcpy(&v, plain + off, 8);
        v = (v >> 32) | (v << 32);
        v ^= key.k[idx];
        v ^= tweak(off);
        v += key.k[idx];
        uint8_t x[8], d = (uint8_t)(((off >> 3) ^ 0x45) & 0xff);
        memcpy(x, &v, 8);
        for (int k = 0; k < 8; ++k)
        {
            uint8_t c = (uint8_t)(((x[k] + key.s[idx] + 0x40) & 0xff) ^ d);
            out[off + k] = c;
            d = c;
        }
    }
}

/* --- paths: slots 126 and 127 -------------------------------------------------------------------
 * Root: the viewer folder, a guest (Windows) path with a trailing backslash. */
static char g_root[512] = "C:\\Program Files (x86)\\PlayOnline\\SquareEnix\\PlayOnlineViewer\\";

void gamecore_set_root(const char* guest_viewer_dir)
{
    snprintf(g_root, sizeof g_root - 1, "%s", guest_viewer_dir);
    size_t n = strlen(g_root);
    if (n && g_root[n - 1] != '\\')
        strcat(g_root, "\\");
}

static const char* const EU_LANG[] = { "EN", "FR", "DE", "IT", "ES", "EL", "PT" };

static int title_component(int32_t extra, char* out, size_t n)
{
    if (extra <= 0 || extra >= 0x400)
        return 0;
    if (extra == 1)
        snprintf(out, n, "FinalFantasyXI\\");
    else if (extra == 2)
        snprintf(out, n, "TetraMaster\\");
    else if (extra == 3)
        snprintf(out, n, "Janhourou\\");
    else
        snprintf(out, n, "Contents%04d\\", extra);
    return 1;
}

/* The path for an id (see the table in the spec); "" for an unknown id. home = "homeNN" with
 * the member index 0. extra_str is slot 126's 4th argument as a string (ids 0x18/0x19). */
static void core_path(uint32_t id, int32_t extra, const char* extra_str, char* out, size_t n)
{
    const int lang = 1; /* slot 421: the US/English client */
    const char* home = "home00";
    char t[64] = "";
    out[0] = 0;
    switch (id)
    {
    case 0x00: snprintf(out, n, "%susr\\%s\\", g_root, home); break;
    case 0x01: snprintf(out, n, "%spub\\%s\\mail\\s\\b\\", g_root, home); break;
    case 0x02: snprintf(out, n, "%spub\\%s\\mail\\s\\a\\", g_root, home); break;
    case 0x03: snprintf(out, n, "%spub\\%s\\mail\\r\\b\\", g_root, home); break;
    case 0x04: snprintf(out, n, "%spub\\%s\\mail\\r\\a\\", g_root, home); break;
    case 0x05: snprintf(out, n, "%spub\\%s\\msg\\s\\b\\", g_root, home); break;
    case 0x06: snprintf(out, n, "%spub\\%s\\msg\\s\\a\\", g_root, home); break;
    case 0x07: snprintf(out, n, "%spub\\%s\\msg\\r\\b\\", g_root, home); break;
    case 0x08: snprintf(out, n, "%spub\\%s\\msg\\r\\a\\", g_root, home); break;
    case 0x09: snprintf(out, n, "%stmp\\", g_root); break;
    case 0x0a: snprintf(out, n, "%susr\\", g_root); break;
    case 0x0b: snprintf(out, n, "%spatch\\", g_root); break;
    case 0x0c: snprintf(out, n, "%susr\\all\\", g_root); break;
    case 0x0d: snprintf(out, n, "%spub\\%s\\mail\\uidl\\", g_root, home); break;
    case 0x0e: snprintf(out, n, "%sdata\\", g_root); break;
    case 0x0f: snprintf(out, n, "%sdata\\utf\\", g_root); break;
    case 0x10: snprintf(out, n, "%spub\\%s\\mail\\adr\\", g_root, home); break;
    case 0x11: snprintf(out, n, "%spub\\%s\\", g_root, home); break;
    case 0x12: snprintf(out, n, "%spub\\all\\", g_root); break;
    case 0x13: snprintf(out, n, "%sdata\\dic\\", g_root); break;
    case 0x14:
        if (lang >= 2 && lang <= 8)
            snprintf(out, n, "%sEU\\%s\\db\\", g_root, EU_LANG[lang - 2]);
        else
            snprintf(out, n, "%sdata\\db\\", g_root);
        break;
    case 0x15: snprintf(out, n, "%sdata\\icon\\", g_root); break;
    case 0x16: snprintf(out, n, "%spub\\", g_root); break;
    case 0x17:
        if (lang >= 2 && lang <= 8)
            snprintf(out, n, "%sEU\\%s\\doc\\", g_root, EU_LANG[lang - 2]);
        else
            snprintf(out, n, "%sdata\\doc\\", g_root);
        break;
    case 0x18: snprintf(out, n, "%spub\\%s\\open\\%s\\", g_root, home, extra_str ? extra_str : "Unknown"); break;
    case 0x19: snprintf(out, n, "%spub\\%s\\hidden\\%s\\", g_root, home, extra_str ? extra_str : "Unknown"); break;
    case 0x1a: snprintf(out, n, "%sexport\\drv\\", g_root); break;
    case 0x1b: snprintf(out, n, "%sdefault\\pub\\home\\", g_root); break;
    case 0x1c: snprintf(out, n, "%sdata\\code\\", g_root); break;
    case 0x1d: snprintf(out, n, "%spub\\%s\\open\\", g_root, home); break;
    case 0x1e: snprintf(out, n, "%sdefault\\usr\\home\\", g_root); break;
    case 0x1f: snprintf(out, n, "%spub\\%s\\open\\GreetingCards\\", g_root, home); break;
    case 0x20: snprintf(out, n, "%spub\\%s\\open\\SaveFiles\\", g_root, home); break;
    case 0x21:
        title_component(extra, t, sizeof t);
        snprintf(out, n, "%spub\\%s\\open\\ScreenShots\\%s", g_root, home, t);
        break;
    case 0x22: snprintf(out, n, "%spub\\%s\\open\\etc\\", g_root, home); break;
    case 0x23: snprintf(out, n, "%sdata\\font\\", g_root); break;
    case 0x24:
        title_component(extra, t, sizeof t);
        snprintf(out, n, "%spub\\%s\\open\\ContentsData\\%s", g_root, home, t);
        break;
    case 0x25: snprintf(out, n, "%spub\\%s\\log\\", g_root, home); break;
    case 0x26:
        if (lang >= 2 && lang <= 8)
            snprintf(out, n, "%sEU\\%s\\", g_root, EU_LANG[lang - 2]);
        else
            snprintf(out, n, "%sdefault\\pub\\home\\", g_root);
        break;
    default: break;
    }
}

/* the result into a guest buffer of `size` (size-1 usable), returning strlen as written */
static uint32_t put(uint32_t buf, int32_t size, const char* s)
{
    if (!buf || size < 2)
        return 0;
    size_t n = strlen(s);
    if (n > (size_t)size - 1)
        n = (size_t)size - 1;
    memcpy(GUEST_PTR(buf), s, n);
    wr8(buf + (uint32_t)n, 0);
    return (uint32_t)n;
}

/* 126 GetPath(id, buf, size, extra) */
static void s126_path(Guest* g)
{
    uint32_t id = ARG(0);
    char p[1024];
    if (id > 0x26)
        RETC(0);
    uint32_t extra = ARG(3);
    core_path(id, (int32_t)extra, (id == 0x18 || id == 0x19) && extra ? (const char*)GUEST_PTR(extra) : NULL, p, sizeof p);
    RETC(put(ARG(1), (int32_t)ARG(2), p));
}

/* 127 GetFileName(id, buf, size, extra) */
static void s127_file_name(Guest* g)
{
    static const struct
    {
        uint32_t dir;
        const char* name;
    } FILES[] = {
        { 0x1a, "sqsound.irx" }, { 0x17, "polerr.bin" }, { 0x17, "sqpolcts.bin" }, { 0x1c, "sqpolexe.bin" },
        { 0x1c, "sqpolkey.bin" }, { 0x1c, "sqpoliop.bin" }, { 0x13, "entrynw.dic" }, { 0x13, "entry.dic" },
        { 0x23, "fnt%03d.bin" }, { 0x13, "entry_f.dic" }, { 0x13, "entry_b.dic" }, { 0x13, "entryz.dic" },
        { 0x13, "vulgar2.dic" }, { 0x13, "entryu.dic" }, { 0x13, "vulgaru.dic" },
    };
    uint32_t id = ARG(0);
    if (id > 0xe)
        RETC(0);
    char dir[1024], name[64], full[1100];
    core_path(FILES[id].dir, 0, NULL, dir, sizeof dir);
    snprintf(name, sizeof name, FILES[id].name, ARG(3));
    snprintf(full, sizeof full, "%s%s", dir, name);
    RETC(put(ARG(1), (int32_t)ARG(2), full));
}

/* --- 1171: patch.ver ----------------------------------------------------------------------------
 * PatchVerDecrypt(src[0x120], out, outsize, product). Retail keys it from the game's
 * registry key (its Interface\<product> value); here the key is recovered from the file
 * itself (decided 2026-09-24: no dependency on the registry), which yields the same key. */
static void s1171_patch_ver(Guest* g)
{
    uint32_t src = ARG(0), out = ARG(1);
    int32_t outsize = (int32_t)ARG(2), product = (int32_t)ARG(3);
    uint8_t plain[0x120];
    uint64_t k0;
    CoreKey key;
    if (core_recover_k0(GUEST_PTR(src), &k0))
    {
        core_key_k0(k0, &key);
        if (core_decrypt_with(GUEST_PTR(src), plain, 0x120, &key))
        {
            const char* v = (const char*)plain + 0x18;
            size_t len = strnlen(v, 0x100);
            if ((size_t)outsize <= len)
                RETC(0xFFFFFFFFu);
            memcpy(GUEST_PTR(out), v, len + 1);
            RETC(0);
        }
    }
    rt_log("[recomp] gamecore: patch.ver (product %d) did not decrypt\n", product);
    RETC(0xFFFFFFFFu);
}

/* The plaintext layout retail's files have (spec, slot 1171 step 5): zeros, the version string at
 * 0x18, the payload length 0x118 at 0x118, the checksum at 0x11c. key64 = seed 0 + product 1, what
 * retail itself falls back to without a registry value; any key decrypts here (key recovery). */
int gamecore_make_patch_ver(const char* version, uint8_t out[0x120])
{
    uint8_t plain[0x120] = { 0 };
    size_t len = strlen(version);
    if (len >= 0x100)
        return 0;
    memcpy(plain + 0x18, version, len);
    uint32_t payload = 0x118;
    memcpy(plain + 0x118, &payload, 4);
    core_encrypt(plain, out, 0x120, 1);
    return 1;
}

/* --- 907-913: option.bin --------------------------------------------------------------------------
 * Loaded synchronously into a guest-heap image with the item pointers relocated, so FFXiMain can
 * read item+0x18 directly. Not written back (910 reports success). */
static uint32_t g_opt; /* the image (guest), 0 = not loaded */

static void opt_free(void)
{
    if (g_opt)
        gheap_free(g_opt);
    g_opt = 0;
}

static int opt_load(void)
{
    char dir[1024], path[1100], host[1400];
    core_path(0x11, 0, NULL, dir, sizeof dir);
    snprintf(path, sizeof path, "%soption.bin", dir);
    size_t n = 0;
    unsigned char* f = vfs_host_path(path, host, sizeof host) ? plat_read_file(host, &n) : NULL;
    if (!f || n < 16 || n % 8)
    {
        free(f);
        return -1;
    }
    uint32_t img = gheap_alloc((uint32_t)n, 0);
    int ok = core_decrypt(f, GUEST_PTR(img), (uint32_t)n, 0);
    free(f);
    if (!ok)
    {
        gheap_free(img);
        return -10243;
    }
    uint32_t count = rd32(img);
    for (uint32_t i = 0; i < count; ++i)
    {
        uint32_t item = img + 8 + 0x20 * i;
        if (rd16(item + 0x0a) == 1) /* string: default and current are offsets */
        {
            wr32(item + 0x14, img + rd32(item + 0x14));
            wr32(item + 0x18, img + rd32(item + 0x18));
        }
        wr32(item + 0x1c, img + rd32(item + 0x1c));
    }
    g_opt = img;
    return 1;
}

static void s907_opt_begin(Guest* g) { RETC(1); }

static void s908_opt_load(Guest* g)
{
    if (g_opt)
        RETC(0); /* polls after completion return 0 */
    int r = opt_load();
    RETC((uint32_t)r);
}

static void s910_opt_save(Guest* g) { RETC(1); }
static void s911_opt_end(Guest* g) { opt_free(); RETC(1); }

static void s912_opt_find(Guest* g)
{
    if (!g_opt)
        RETC(0);
    uint64_t key;
    memcpy(&key, ARGP(0), 8);
    uint32_t count = rd32(g_opt), lo = 0, hi = count;
    while (lo < hi)
    {
        uint32_t mid = (lo + hi) / 2, item = g_opt + 8 + 0x20 * mid;
        uint64_t k = rd64(item);
        if (k == key)
            RETC(item);
        if (k < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    RETC(0);
}

static void s913_opt_set(Guest* g)
{
    uint32_t item = ARG(0), value = ARG(1);
    if (!item || !value)
        RETC(0);
    if (rd16(item + 0x0a) == 0)
    {
        int32_t v = (int32_t)rd32(value), lo = (int32_t)rd32(item + 0x0c), hi = (int32_t)rd32(item + 0x10);
        if ((lo || hi) && (v < lo || v > hi))
            RETC(0);
        wr32(item + 0x18, (uint32_t)v);
        RETC(1);
    }
    if (rd16(item + 0x0a) == 1)
    {
        const char* s = (const char*)GUEST_PTR(value);
        uint32_t max = rd32(item + 0x0c);
        if (strlen(s) > max)
            RETC(0);
        memcpy(GUEST_PTR(rd32(item + 0x18)), s, strlen(s) + 1);
        RETC(1);
    }
    RETC(0);
}

/* --- small ones ---------------------------------------------------------------------------------- */
static void s697_date_format(Guest* g) { RETC(1); }
static void s698_time_format(Guest* g) { RETC(1); }

/* 688 GetTime(int tm[9]): UTC, full year; returns 1 (server time known). */
static void s688_online_time(Guest* g)
{
    PlatTime t;
    uint64_t ms = plat_wall_ms();
    plat_split_time(ms, 0, &t);
    static const int CUM[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    int leap = (t.year % 4 == 0 && t.year % 100 != 0) || t.year % 400 == 0;
    uint32_t tm = ARG(0), v[9] = { (uint32_t)t.second, (uint32_t)t.minute, (uint32_t)t.hour, (uint32_t)t.day,
                                    (uint32_t)(t.month - 1), (uint32_t)t.year, (uint32_t)t.day_of_week,
                                    (uint32_t)(CUM[t.month - 1] + t.day - 1 + (leap && t.month > 2)), 0 };
    for (int i = 0; i < 9; ++i)
        wr32(tm + 4 * i, v[i]);
    RETC(1);
}

/* 477 IconPath(buf, size, kind, icon) */
static void s477_icon_path(Guest* g)
{
    uint32_t kind = ARG(2), i = ARG(3) & 0xFFFF;
    char dir[1024], full[1100];
    core_path(0x15, 0, NULL, dir, sizeof dir);
    int32_t sub;
    if (kind == 0)
        snprintf(full, sizeof full, "%sdownload\\hnf%03u.png", dir, i / 8), sub = (int32_t)(i % 8);
    else if (kind == 1)
        snprintf(full, sizeof full, "%scicn_l%03u.png", dir, i / 4), sub = (int32_t)(i % 4);
    else if (kind == 2)
        snprintf(full, sizeof full, "%scicn_s%03u.png", dir, i / 4), sub = (int32_t)(i % 4);
    else
        snprintf(full, sizeof full, "%s", dir), sub = -1;
    put(ARG(0), (int32_t)ARG(1), full);
    RETC((uint32_t)sub);
}

/* 475 FormatId(buf, lo, hi): a member id as "NN-NNNNNNNN" */
static void s475_format_id(Guest* g)
{
    uint32_t buf = ARG(0);
    uint64_t v = ((uint64_t)ARG(2) << 32) | ARG(1);
    char d[16], out[20];
    size_t o = 0;
    if (v >= (1ull << 44))
    {
        wr8(buf, 0);
        RETC(buf);
    }
    snprintf(d, sizeof d, "%014llu", (unsigned long long)v);
    if (d[0] != '0')
        out[o++] = d[0];
    out[o++] = d[1];
    out[o++] = d[2];
    out[o++] = '-';
    const char* rest = d + 3;
    while (*rest == '0' && rest[1])
        rest++;
    while (*rest)
        out[o++] = *rest++;
    out[o] = 0;
    memcpy(GUEST_PTR(buf), out, o + 1);
    RETC(buf + (uint32_t)o);
}

/* 463/464/465: the server-side config backup. There is no storage server: nothing stored. */
static void s463_backup_cancel(Guest* g) { RETC(0); }

static void s464_backup_query(Guest* g)
{
    if (!ARG(0))
        RETC(0xFFFFEC00u);
    wr32(ARG(0), 0);
    RETC(0);
}

static void s465_backup_poll(Guest* g) { RETC(100); }

/* --- screenshots and ContentsData ----------------------------------------------------------------- */
static int valid_name(const char* s)
{
    size_t n = strlen(s);
    if (n < 1 || n >= 31)
        return 0;
    for (; *s; ++s)
    {
        unsigned char c = (unsigned char)*s;
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '#' || c == '-' ||
                c == '.' || c == '@' || c == '^' || c == '_'))
            return 0;
    }
    return 1;
}

static void s1136_valid_name(Guest* g) { RETC(valid_name(ARGS(0))); }

/* counts files (and their bytes) under a guest directory, two levels deep */
static void count_files(const char* host, int depth, uint32_t* files, uint64_t* bytes)
{
    PlatDir* d = plat_dir_open(host);
    if (!d)
        return;
    for (const char* e; (e = plat_dir_next(d));)
    {
        char sub[1600];
        PlatStat st;
        snprintf(sub, sizeof sub, "%s%c%s", host, plat_path_sep, e);
        if (!plat_stat(sub, &st))
            continue;
        if (st.is_dir)
        {
            if (depth < 2)
                count_files(sub, depth + 1, files, bytes);
        }
        else
        {
            (*files)++;
            *bytes += st.size;
        }
    }
    plat_dir_close(d);
}

static uint32_t g_scan_files, g_scan_param;
static uint64_t g_scan_bytes;

static void scan(uint32_t id, uint32_t param)
{
    char dir[1024], host[1400];
    core_path(id, 0, NULL, dir, sizeof dir);
    g_scan_files = 0;
    g_scan_bytes = 0;
    g_scan_param = param;
    if (vfs_host_path(dir, host, sizeof host))
        count_files(host, 1, &g_scan_files, &g_scan_bytes);
}

static void s1135_shot_scan(Guest* g) { scan(0x21, ARG(1)); RETC(0); }
static void s1134_shot_poll(Guest* g) { RETC(g_scan_files >= 200 ? 0xFFFFE3E5u : 201 - g_scan_files); }
static void s1296_cdata_scan(Guest* g) { scan(0x24, ARG(0)); RETC(0); }

static void s1295_cdata_poll(Guest* g)
{
    if (g_scan_files >= 99)
        RETC(0xFFFFE3E0u);
    if (g_scan_bytes + g_scan_param > 0x800000u)
        RETC(0xFFFFE3E1u);
    RETC(1);
}

static void s1299_cdata_size(Guest* g)
{
    int32_t size = (int32_t)ARG(0);
    RETC(size <= 0 || size % 8 ? 0xFFFFE3E4u : (uint32_t)size + 0x208);
}

/* ContentsData key: 0x37564 + the signed bytes of the file name from its end down to index 1,
 * stopping before the last separator */
static uint64_t cdata_key(const char* name)
{
    int64_t k = 0x37564;
    size_t n = strlen(name);
    for (size_t i = n; i-- > 1;)
    {
        if (name[i] == '\\' || name[i] == '/')
            break;
        k += (signed char)name[i];
    }
    return (uint64_t)k;
}

/* 1298 CDataEncode(src, size, out, cap, title, name) */
static void s1298_cdata_encode(Guest* g)
{
    uint32_t src = ARG(0), out = ARG(2);
    int32_t size = (int32_t)ARG(1), cap = (int32_t)ARG(3);
    if (size <= 0 || size % 8 || cap % 8 || cap < size + 0x208)
        RETC(0xFFFFE3E4u);
    uint64_t key = cdata_key(ARGS(5));
    uint8_t header[0x200] = { 0 };
    strncpy((char*)header, ARGS(4), 255);
    memcpy(header + 0x108, &size, 4);
    uint32_t hl = 0x1f8;
    memcpy(header + 0x1f8, &hl, 4);
    core_encrypt(header, GUEST_PTR(out), 0x200, key);
    uint32_t pn = (uint32_t)size + 8;
    uint8_t* payload = (uint8_t*)malloc(pn);
    memcpy(payload, GUEST_PTR(src), (size_t)size);
    memcpy(payload + size, &size, 4);
    core_encrypt(payload, GUEST_PTR(out + 0x200), pn, key);
    free(payload);
    RETC((uint32_t)size + 0x208);
}

/* 1297 CDataDecode(in, insize, out, cap, name, title, titlecap) */
static void s1297_cdata_decode(Guest* g)
{
    uint32_t in = ARG(0), out = ARG(2), title = ARG(5);
    int32_t insize = (int32_t)ARG(1), cap = (int32_t)ARG(3), titlecap = (int32_t)ARG(6);
    if (insize < 0x200)
        RETC(0xFFFFE3E2u);
    uint64_t key = cdata_key(ARGS(4));
    uint8_t header[0x200];
    if (!core_decrypt(GUEST_PTR(in), header, 0x200, key))
        RETC(0xFFFFE3E2u);
    int32_t size;
    memcpy(&size, header + 0x108, 4);
    if (out)
    {
        if (cap < size + 8)
            RETC(0xFFFFE3E4u);
        if (insize < size + 0x208 || !core_decrypt(GUEST_PTR(in + 0x200), GUEST_PTR(out), (uint32_t)size + 8, key))
            RETC(0xFFFFE3E2u);
    }
    if (title && titlecap > 0)
        strncpy((char*)GUEST_PTR(title), (const char*)header, (size_t)titlecap);
    RETC((uint32_t)size);
}

/* --- message lines (1071) and notices (1303): our own English wording ------------------------------ */
static const char* msg_line(uint32_t id)
{
    switch (id)
    {
    case 0x01: return "Sent you a friend request.";
    case 0x09: return "Accepted your friend request.";
    case 0x0a: return "Turned down your friend request.";
    case 0x0b: return "Removed.";
    case 0x0c: return "Asked to be removed.";
    case 0x0e: return "Invited you to a group.";
    case 0x0f: return "Accepted the group invite.";
    case 0x10: return "Turned down the group invite.";
    case 0x12: return "No longer in the group.";
    case 0x13: return "The group has been dissolved.";
    default: return "";
    }
}

static void s1071_msg_line(Guest* g)
{
    if ((int32_t)ARG(2) > 0)
        put(ARG(1), (int32_t)ARG(2), msg_line(ARG(0)));
    RETC(0);
}

static void s1303_notice(Guest* g)
{
    uint32_t id = ARG(0), buf = ARG(1), arg = ARG(3);
    int32_t size = (int32_t)ARG(2);
    char s[512];
    if (size <= 0)
        RETC(0);
    switch (id)
    {
    case 0: snprintf(s, sizeof s, "Add as a friend?"); break;
    case 3: snprintf(s, sizeof s, "%s keeps their message memo private.", arg ? (const char*)GUEST_PTR(arg + 0xa0) : ""); break;
    case 4: snprintf(s, sizeof s, "That name is taken. Choose a different one."); break;
    case 7: snprintf(s, sizeof s, "You are no longer in group [%s].", arg ? (const char*)GUEST_PTR(arg) : "unknown"); break;
    case 8: snprintf(s, sizeof s, "Group [%s] has been dissolved.", arg ? (const char*)GUEST_PTR(arg) : "unknown"); break;
    case 9: snprintf(s, sizeof s, "Accept the invite to group [%s]?", arg ? (const char*)GUEST_PTR(arg) : "unknown"); break;
    default: s[0] = 0; break;
    }
    put(buf, size, s);
    RETC(0);
}

/* --- the AFK logout timer (1140-1150), checked from the per-frame pump ---------------------------- */
static int g_idle_on, g_idle_warned, g_idle_expired;
static uint32_t g_idle_period, g_idle_warn_cb, g_idle_expire_cb;
static uint64_t g_idle_deadline;

static uint64_t now_ms(void) { return rt_monotonic_ns() / 1000000u; }
static void idle_restart(void) { g_idle_deadline = now_ms() + g_idle_period; g_idle_warned = g_idle_expired = 0; }

static void s1140_idle_enable(Guest* g) { g_idle_on = (int)ARG(0); idle_restart(); RETC(0); }

static void s1141_idle_period(Guest* g)
{
    if (!ARG(0))
        g_idle_on = 0;
    g_idle_period = ARG(0);
    idle_restart();
    RETC(0);
}

static void s1149_idle_callbacks(Guest* g) { g_idle_warn_cb = ARG(0); g_idle_expire_cb = ARG(1); RETC(0); }
static void s1148_idle_hook(Guest* g) { RETC(1); }
static void s1150_go_offline(Guest* g) { RETC(0); } /* there is no online session to drop */

/* Run from slots 705/818 (gamecore_slots.c), with the guest lock held: warn 60 s before, then
 * expire; the callbacks are guest cdecl functions taking one argument (0). */
void gamecore_idle_tick(void)
{
    if (!g_idle_on)
        return;
    uint64_t now = now_ms();
    if (!g_idle_warned)
    {
        uint64_t rem = g_idle_deadline > now ? g_idle_deadline - now : 0;
        if (rem > 0 && rem <= 60000)
        {
            g_idle_warned = 1;
            if (g_idle_warn_cb)
            {
                uint32_t a[1] = { 0 };
                guest_call(g_idle_warn_cb, 1, a);
            }
        }
    }
    else if (!g_idle_expired && now >= g_idle_deadline)
    {
        g_idle_expired = 1;
        if (g_idle_expire_cb)
        {
            uint32_t a[1] = { 0 };
            guest_call(g_idle_expire_cb, 1, a);
        }
    }
}

/* --- the profanity filter: off (the retail default), dictionaries accepted as they are ------------- */
static void s362_dic_open(Guest* g) { RETC(ARG(0)); }
static void s363_dic_filter(Guest* g) { RETC(0); }

static void s1109_name_dic_open(Guest* g)
{
    uint32_t buf = ARG(0);
    int32_t size = (int32_t)ARG(1);
    uint32_t total = 0x20;
    for (int i = 0; i < 4; ++i)
        total += rd32(buf + 4 + 8 * (uint32_t)i);
    RETC(buf && (uint32_t)size == total ? buf : 0);
}

static void s1110_name_forbidden(Guest* g) { RETC(0); }

static const GamecoreSlot FILES_SLOTS[] = {
    { 0x1f8, s126_path },
    { 0x1fc, s127_file_name },
    { 0x124c, s1171_patch_ver },
    { 0xe2c, s907_opt_begin },
    { 0xe34, s907_opt_begin }, /* 909: the same function */
    { 0xe30, s908_opt_load },
    { 0xe38, s910_opt_save },
    { 0xe3c, s911_opt_end },
    { 0xe40, s912_opt_find },
    { 0xe44, s913_opt_set },
    { 0xae4, s697_date_format },
    { 0xae8, s698_time_format },
    { 0xac0, s688_online_time },
    { 0x774, s477_icon_path },
    { 0x76c, s475_format_id },
    { 0x73c, s463_backup_cancel },
    { 0x740, s464_backup_query },
    { 0x744, s465_backup_poll },
    { 0x11c0, s1136_valid_name },
    { 0x1438, s1136_valid_name }, /* 1294: the same body */
    { 0x11bc, s1135_shot_scan },
    { 0x11b8, s1134_shot_poll },
    { 0x1440, s1296_cdata_scan },
    { 0x143c, s1295_cdata_poll },
    { 0x144c, s1299_cdata_size },
    { 0x1448, s1298_cdata_encode },
    { 0x1444, s1297_cdata_decode },
    { 0x10bc, s1071_msg_line },
    { 0x145c, s1303_notice },
    { 0x11d0, s1140_idle_enable },
    { 0x11d4, s1141_idle_period },
    { 0x11f4, s1149_idle_callbacks },
    { 0x11f0, s1148_idle_hook },
    { 0x11f8, s1150_go_offline },
    { 0x5a8, s362_dic_open },
    { 0x5ac, s363_dic_filter },
    { 0x1154, s1109_name_dic_open },
    { 0x1158, s1110_name_forbidden },
    { 0, NULL },
};

void gamecore_files_init(void)
{
    gamecore_register(FILES_SLOTS);
}

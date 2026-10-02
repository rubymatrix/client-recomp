/* Our own gamecore, the presence group (R3): presence client, handle and character records, mail,
 * text, the Help Desk FAQ and misc slots. Each follows
 * specs/gamecore-slots.presence.txt; its section 3 is the summary this file
 * implements. All cdecl (RETC).
 *
 * The model, from that spec: the online client features (friends, mail, FAQ, server-side settings)
 * are off - slot 836 says so - and every async request FFXiMain can still start finishes at once
 * with an empty result. What world entry needs is real: a valid handle record 0 (173), the
 * presence values 195 sets (189/190/204/206/207), and slot 177's record for the selected
 * character, built from FFXiMain's own character table. Slot 1132 is the text-input conversion
 * every typed character goes through. */
#include <stdio.h>
#include <string.h>

#include "build.h" /* FFXI_CHARS_PTR, per retail build */
#include "gamecore.h"
#include "gamecore_config.h"

/* --- presence (1b): what 195 last set, read back by 189/190/204/206/207 ---------------------------------- */
static int32_t g_chan = 0, g_sub = -1, g_flag = 1, g_mode = 0;
static uint32_t g_word = 0x3E8;

void presence_update(int32_t chan, int32_t sub, int32_t flag, int32_t mode, int32_t word)
{
    if (chan != -2)
        g_chan = chan;
    if (sub != -2)
        g_sub = sub;
    if (flag == 0 || flag == 1)
        g_flag = flag;
    if (mode != -2)
        g_mode = mode;
    if (word >= 0 && word <= 0x3FF)
        g_word = (uint32_t)word;
}

static void s189_presence_sub(Guest* g) { RETC((uint32_t)g_sub); }
static void s190_presence_chan(Guest* g) { RETC((uint32_t)g_chan); }
static void s204_presence_flag(Guest* g) { RETC((uint32_t)g_flag); }
static void s206_presence_mode(Guest* g) { RETC((uint32_t)g_mode); }
static void s207_presence_word(Guest* g) { RETC(g_word); }

/* --- records ---------------------------------------------------------------------------------------------- */
/* 167/169 (0x10023da0 / 0x10023f90): friend lists A (200) and B (100), empty */
static void s167_friend_a(Guest* g)
{
    if (ARG(0) >= 200)
        RETC(0xFFFFE3F9u);
    memset(ARGP(1), 0, 0xB0);
    wr32(ARG(1) + 8, (ARG(0) & 0xFF) << 20);
    RETC(0);
}

static void s169_friend_b(Guest* g)
{
    if (ARG(0) >= 100)
        RETC(0xFFFFE3F9u);
    memset(ARGP(1), 0, 0xB0);
    RETC(0);
}

/* 173 (0x1001cc40): handle record a0 (0x28 bytes). Handle 0 is the one this session uses. */
static void s173_handle(Guest* g)
{
    if (ARG(0) >= 64)
        RETC(0xFFFFE3EAu);
    uint32_t r = ARG(1);
    memset(GUEST_PTR(r), 0, 0x28);
    if (ARG(0) == 0)
    {
        wr32(r, 1); /* valid */
        strcpy((char*)GUEST_PTR(r + 8), "FFXI");
    }
    RETC(0);
}

/* 177 (0x100227d0): character record a0 (0x68 bytes), REQUIRED for world entry (spec 1a). Built
 * from FFXiMain's character table: entry = [[FFXI_CHARS_PTR] + 0x13820 + a0 * 0x8c], contentId =
 * d[+4], charId = w[+8] | b[+0xf] << 16, world = w[+0xa]. Registered to handle 0 (q[0x10] bit 15). */
#define FFXI_MAX_CHARS 16

static void s177_character(Guest* g)
{
    uint32_t i = ARG(0), r = ARG(1);
    if (i >= 64)
        RETC(0xFFFFE3FDu);
    memset(GUEST_PTR(r), 0, 0x68);
    uint32_t base = rd32(FFXI_CHARS_PTR) /* FFXiMain sits at its preferred base */;
    if (!base || i >= FFXI_MAX_CHARS)
        RETC(0);
    uint32_t e = base + 0x13820 + i * 0x8C;
    uint32_t content = rd32(e + 4), char_id = rd16(e + 8) | (uint32_t)rd8(e + 0xF) << 16, world = rd16(e + 0xA);
    if (!content)
        RETC(0);
    wr8(r, 1);
    wr16(r + 2, 1); /* FFXI character */
    wr32(r + 4, (char_id & 0xFFFF) | (world & 0xFFFF) << 16 | ((char_id >> 16) & 0xFF) << 24);
    wr32(r + 8, content);
    wr32(r + 0x10, 0x8000); /* registered, handle 0 */
    RETC(0);
}

/* 283 (0x1001ac60): the signed-in account id, a u64 in edx:eax; non-zero */
static void s283_account_id(Guest* g)
{
    g->edx = 0;
    RETC(0x5349474Eu); /* "SIGN" */
}

/* --- async requests (the spec's generic rule): start -> handle 0, poll -> done ------------------------------ */
static void s_start(Guest* g) { RETC(0); }
static void s_poll_done(Guest* g) { RETC(1); }
static void s_zero(Guest* g) { RETC(0); }

/* 208/209: look up a user; "found" is the id asked for */
static uint32_t g_lookup_lo, g_lookup_hi;
static void s208_lookup_begin(Guest* g)
{
    g_lookup_lo = ARG(1), g_lookup_hi = ARG(2);
    RETC(0);
}

static void s209_lookup_poll(Guest* g)
{
    if (ARG(1))
        wr32(ARG(1), g_lookup_lo), wr32(ARG(1) + 4, g_lookup_hi);
    if (ARG(2))
        wr32(ARG(2), 0);
    RETC(1);
}

/* --- 258 (0x10012420): gamecore's own sprintf (not libc's), exactly as the spec describes ---------------------- */
static void s258_sprintf(Guest* g)
{
    uint32_t dst = ARG(0), out = dst, limit = dst + (uint32_t)(int32_t)ARG(1) - 1, fmt = ARG(2), arg = 3;
    char pad = ' ';
    for (uint32_t f = fmt; rd8(f); )
    {
        if (out >= limit)
        {
            wr8(out, 0);
            RETC(0xFFFFFFFFu);
        }
        uint8_t c = rd8(f++);
        if (c != '%')
        {
            wr8(out++, c);
            continue;
        }
        int width = 0;
        uint8_t d = rd8(f);
        if (d == '0')
        {
            pad = '0';
            width = rd8(f + 1) - '0';
            f += 2;
        }
        else if (d >= '1' && d <= '9')
        {
            pad = ' ';
            width = d - '0';
            f++;
        }
        uint8_t t = rd8(f);
        if (!t)
            break;
        f++;
        if (t == 'd' || t == 'x' || t == 'X')
        {
            uint32_t v = ARG(arg++);
            char digits[16];
            int n = 0;
            if (t == 'd' && (int32_t)v < 0)
            {
                wr8(out++, '-'); /* without the limit check, as retail */
                v = (uint32_t)(-(int32_t)v);
            }
            const char* hex = t == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            do
            {
                digits[n++] = t == 'd' ? (char)('0' + v % 10) : hex[v & 0xF];
                v = t == 'd' ? v / 10 : v >> 4;
            } while (v);
            for (int p = n; p < width; ++p)
            {
                if (out >= limit)
                {
                    wr8(out, 0);
                    RETC(0xFFFFFFFFu);
                }
                wr8(out++, (uint8_t)pad);
            }
            while (n)
            {
                if (out >= limit)
                {
                    wr8(out, 0);
                    RETC(0xFFFFFFFFu);
                }
                wr8(out++, (uint8_t)digits[--n]);
            }
        }
        else if (t == 's')
        {
            for (uint32_t s = ARG(arg++); rd8(s); ++s)
            {
                if (out >= limit)
                {
                    wr8(out, 0);
                    RETC(0xFFFFFFFFu);
                }
                wr8(out++, rd8(s));
            }
        }
        else if (t == 'c')
            wr8(out++, (uint8_t)ARG(arg++)); /* without the limit check */
        else
            wr8(out++, t); /* literally, no argument */
    }
    wr8(out, 0);
    RETC(out - dst);
}

/* --- mail, paths, PIB, text -------------------------------------------------------------------------------- */
static void s270_mail_header(Guest* g)
{
    memset(ARGP(0), 0, 0x48);
    RETC(ARG(12));
}

static void s271_split(Guest* g)
{
    if (ARG(2))
        wr8(ARG(2), 0);
    RETC(0);
}

static void s272_concat(Guest* g)
{
    if (ARG(4))
        wr8(ARG(4), 0);
    RETC(0);
}

static void s278_fetch_headers(Guest* g)
{
    memset(ARGP(0), 0, ARG(1) * 0x48);
    RETC(0);
}

static void s282_unread(Guest* g) { RETC(rd16(ARG(0) + 0x3E) >> 15); }

static void s28x_mail_path(Guest* g)
{
    wr8(ARG(1), 0);
    RETC(0);
}

static uint32_t g_mail_callback;
static void s288_mail_callback(Guest* g)
{
    g_mail_callback = ARG(0);
    RETC(ARG(0));
}

static void s291_decode_index(Guest* g) { RETC(0xFFFFEC00u); }

/* 297 (0x10020720): query node op; 299: link n nodes */
static void s297_node_op(Guest* g)
{
    uint32_t n = ARG(0);
    wr32(n + 4, ARG(1));
    if (ARG(1) == 7)
    {
        wr8(n + 0xC, rd8(n + 0xC) | 1);
        if (ARG(2))
            wr8(n + 0xF, 1), wr32(n + 0x10, ARG(2));
    }
    RETC(0);
}

static void s299_link_nodes(Guest* g)
{
    int32_t n = (int32_t)ARG(0);
    uint32_t last = 0;
    for (int32_t i = 0; i < n; ++i)
    {
        uint32_t node = ARG(1 + (uint32_t)i);
        if (!node)
            continue;
        wr32(node, last);
        last = node;
    }
    RETC(last);
}

/* 836 (0x100458c0): the in-game online client stays off (no friend list or mail jobs) */
static void s836_online_client(Guest* g) { RETC(0); }

/* 921..931: the async file channels (mail files) */
static void s_file_fail(Guest* g) { RETC(0xFFFFD800u); }
static void s923_close(Guest* g) { RETC(1); }

static void s930_list_poll(Guest* g)
{
    if (ARG(1))
        wr32(ARG(1), 0);
    RETC(1);
}

/* 1004/1005/1006: online error message text */
static void s1006_error_table(Guest* g) { RETC(ARG(0)); }

static void s1004_error_text(Guest* g)
{
    char msg[64];
    int n = snprintf(msg, sizeof msg, "Online error %d", (int32_t)ARG(1));
    uint32_t size = ARG(3);
    if (size)
    {
        uint32_t k = (uint32_t)n < size - 1 ? (uint32_t)n : size - 1;
        memcpy(GUEST_PTR(ARG(2)), msg, k);
        wr8(ARG(2) + k, 0);
    }
    RETC(0);
}

/* 1132 (0x1002e6f0): the text-input conversion every typed character goes through (the chat box
 * drops a character whose conversion is empty). The Latin path: ASCII is copied; a byte >= 0x80
 * becomes the pair 0x85, (b < 0xC0 ? b - 0x40 + (b == 0xBF) : b - 0x21). Returns the number of
 * converted characters. The Japanese path (half-width katakana tables in gamecore's data) is not
 * implemented: a US client sets LATIN. */
static void s1132_text_input(Guest* g)
{
    uint32_t src = ARG(0), dst = ARG(1), len = ARG(2), o = 0, count = 0;
    for (uint32_t i = 0; i < len;)
    {
        uint8_t b = rd8(src + i++);
        if (b < 0x80)
        {
            if (!b)
                break;
            wr8(dst + o++, b);
            continue;
        }
        count++;
        wr8(dst + o++, 0x85);
        wr8(dst + o++, (uint8_t)(b < 0xC0 ? b - 0x40 + (b == 0xBF) : b - 0x21));
    }
    wr8(dst + o, 0);
    RETC(count);
}

static void s1483_mail_text(Guest* g)
{
    if ((int32_t)ARG(2) > 0)
        wr8(ARG(1), 0);
    RETC(0);
}

static void s_settings_fail(Guest* g) { RETC(0xFFFFFF00u); } /* 1535/1536: no server-side settings */

#define SLOT(n, f) { 4 * (n), f }
static const GamecoreSlot PRESENCE[] = {
    SLOT(165, s_start), SLOT(166, s_poll_done), SLOT(167, s167_friend_a), SLOT(169, s169_friend_b),
    SLOT(173, s173_handle), SLOT(174, s_zero), SLOT(175, s_start), SLOT(176, s_poll_done), SLOT(177, s177_character),
    SLOT(179, s_start), SLOT(180, s_poll_done), SLOT(181, s_start), SLOT(182, s_poll_done), SLOT(183, s_zero),
    SLOT(184, s_zero), SLOT(185, s_zero), SLOT(186, s_zero), SLOT(187, s_zero), SLOT(188, s_zero),
    SLOT(189, s189_presence_sub), SLOT(190, s190_presence_chan), SLOT(197, s_zero), SLOT(199, s_zero),
    SLOT(204, s204_presence_flag), SLOT(206, s206_presence_mode), SLOT(207, s207_presence_word),
    SLOT(208, s208_lookup_begin), SLOT(209, s209_lookup_poll), SLOT(258, s258_sprintf),
    SLOT(270, s270_mail_header), SLOT(271, s271_split), SLOT(272, s272_concat), SLOT(273, s_start),
    SLOT(274, s_poll_done), SLOT(275, s_start), SLOT(276, s_poll_done), SLOT(277, s_zero),
    SLOT(278, s278_fetch_headers), SLOT(279, s_poll_done), SLOT(280, s_start), SLOT(281, s_poll_done),
    SLOT(282, s282_unread), SLOT(283, s283_account_id), SLOT(284, s28x_mail_path), SLOT(285, s28x_mail_path),
    SLOT(286, s28x_mail_path), SLOT(287, s28x_mail_path), SLOT(288, s288_mail_callback), SLOT(289, s_zero),
    SLOT(290, s_zero), SLOT(291, s291_decode_index), SLOT(292, s_zero), SLOT(293, s_zero), SLOT(294, s_zero),
    SLOT(295, s_zero), SLOT(296, s_zero), SLOT(297, s297_node_op), SLOT(298, s297_node_op),
    SLOT(299, s299_link_nodes), SLOT(300, s_zero), SLOT(301, s_start), SLOT(302, s_poll_done), SLOT(422, s_zero),
    SLOT(638, s_zero), SLOT(639, s_zero), SLOT(640, s_zero), SLOT(641, s_zero), SLOT(642, s_zero),
    SLOT(643, s_zero), SLOT(644, s_zero), SLOT(645, s_zero), SLOT(646, s_zero), SLOT(647, s_zero),
    SLOT(648, s_zero), SLOT(649, s_zero), SLOT(650, s_zero), SLOT(651, s_zero), SLOT(652, s_zero),
    SLOT(653, s_zero), SLOT(654, s_zero), SLOT(655, s_zero), SLOT(656, s_zero), SLOT(657, s_zero),
    SLOT(658, s_zero), SLOT(659, s_zero), SLOT(660, s_zero), SLOT(661, s_zero), SLOT(662, s_zero),
    SLOT(663, s_zero), SLOT(664, s_zero), SLOT(665, s_zero), SLOT(666, s_zero), SLOT(667, s_zero),
    SLOT(668, s_zero), SLOT(669, s_zero), SLOT(836, s836_online_client), SLOT(921, s_file_fail),
    SLOT(922, s_file_fail), SLOT(923, s923_close), SLOT(924, s_file_fail), SLOT(925, s_file_fail),
    SLOT(926, s_file_fail), SLOT(927, s_file_fail), SLOT(928, s_file_fail), SLOT(929, s_file_fail),
    SLOT(930, s930_list_poll), SLOT(931, s_start), SLOT(1004, s1004_error_text), SLOT(1005, s1004_error_text),
    SLOT(1006, s1006_error_table), SLOT(1132, s1132_text_input), SLOT(1152, s_zero), SLOT(1483, s1483_mail_text),
    SLOT(1535, s_settings_fail), SLOT(1536, s_settings_fail),
    { 0, NULL },
};

void gamecore_presence_init(void)
{
    gamecore_register(PRESENCE);
}

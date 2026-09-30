/* The game hooks the addon host stands on (recomp.py --wraps, meta/builds.json "wraps"):
 *
 *   parse_input    int __cdecl (const char* line, int mode): every typed line, macro line and menu
 *                  command. The host's commands and addons' command events run first; handled
 *                  lines never reach the game.
 *   write_line     void __cdecl (int mode, const char* text): the chat log's add-line. Addons see
 *                  each line (text_in) and may change or block it. The host writes its own lines
 *                  through it (queued until the chat log exists).
 *   packet_decrypt int __cdecl (out, capacity, key, table, in, size): after it, the plain buffer
 *                  (a 0x1C-byte header, then packets) goes through the addons (packet_in).
 *   packet_encrypt int __cdecl (out, capacity, key, in, size, table): before it, the same for the
 *                  buffer the game sends (packet_out).
 *
 * A packet starts with a u16: the id in bits 0-8, the size in 4-byte units in bits 9-15; then a
 * u16 sequence number. Addons may block packets, change them, and add their own (injected), which
 * go through the addons too with injected set. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "build.h"
#include "gthread.h"
#include "gwin.h"
#include "host.h"
#include "plat.h"

#if defined(FFXI_WRAP_PARSE_INPUT)
extern GuestFn rt_wrap_parse_input;
extern const GuestFn rt_orig_parse_input;
#endif
#if defined(FFXI_WRAP_WRITE_LINE)
extern GuestFn rt_wrap_write_line;
extern const GuestFn rt_orig_write_line;
#endif
#if defined(FFXI_WRAP_PACKET_DECRYPT)
extern GuestFn rt_wrap_packet_decrypt;
extern const GuestFn rt_orig_packet_decrypt;
#endif
#if defined(FFXI_WRAP_PACKET_ENCRYPT)
extern GuestFn rt_wrap_packet_encrypt;
extern const GuestFn rt_orig_packet_encrypt;
#endif

/* a guest string, at most n bytes, into out (terminated) */
static size_t guest_str(uint32_t a, char* out, size_t n)
{
    size_t i = 0;
    if (!a)
    {
        out[0] = 0;
        return 0;
    }
    while (i + 1 < n && xi_mapped(a + (uint32_t)i, 1))
    {
        char c = (char)rd8(a + (uint32_t)i);
        if (!c)
            break;
        out[i++] = c;
    }
    out[i] = 0;
    return i;
}

/* A scratch buffer in guest memory the host hands the game (replaced lines, packet buffers). */
static uint32_t guest_scratch(int which, uint32_t size)
{
    static uint32_t buf[4], cap[4];
    if (cap[which] < size)
    {
        if (buf[which])
            gheap_free(buf[which]);
        buf[which] = gheap_alloc(size, 1);
        cap[which] = buf[which] ? size : 0;
    }
    return buf[which];
}

/* --- the command line ------------------------------------------------------------------------ */

static int g_injecting; /* the line parse_input sees is one the host runs (queued commands) */

#if defined(FFXI_WRAP_PARSE_INPUT)
static void wrap_parse_input(Guest* g)
{
    char line[1024];
    uint32_t text = rt_arg(g, 0);
    int mode = (int)rt_arg(g, 1);
    guest_str(text, line, sizeof line);
    int injected = g_injecting;
    g_injecting = 0;
    if (!strncmp(line, "/shutdown", 9) || !strncmp(line, "/logout", 7))
    {
        /* the game is going: addons save their settings in unload */
        extern void addons_shutdown(void);
        if (!strncmp(line, "/shutdown", 9))
            addons_shutdown();
    }
    if (xi_command(line, mode, injected))
    {
        rt_return(g, 0, 0);
        return;
    }
    rt_orig_parse_input(g);
}
#endif

/* --- the chat log ---------------------------------------------------------------------------- */

typedef struct Queued
{
    int mode;
    int command; /* a line to run, not one to write */
    char* text;
    struct Queued* next;
} Queued;
static Queued *g_q_head, *g_q_tail;

static void enqueue(int mode, const char* text, int command)
{
    Queued* q = (Queued*)calloc(1, sizeof *q);
    q->mode = mode, q->command = command, q->text = strdup(text);
    if (g_q_tail)
        g_q_tail->next = q;
    else
        g_q_head = q;
    g_q_tail = q;
}

void xi_chat_write(int mode, const char* text)
{
    /* long lines are cut at a unit boundary: a colour code (0x1E/0x1F xx), a Shift-JIS pair and an
     * auto-translate phrase (0xFD .. 0xFD, 6 bytes) are never split */
    const unsigned char* p = (const unsigned char*)text;
    size_t n = strlen(text);
    while (n > 240)
    {
        size_t cut = 0, i = 0;
        while (i < n && i < 240)
        {
            size_t unit = 1;
            if ((p[i] == 0x1E || p[i] == 0x1F) && i + 1 < n)
                unit = 2;
            else if (p[i] == 0xFD && i + 5 < n)
                unit = 6;
            else if (((p[i] >= 0x81 && p[i] <= 0x9F) || (p[i] >= 0xE0 && p[i] <= 0xFC)) && i + 1 < n)
                unit = 2;
            if (i + unit > 240)
                break;
            i += unit;
            cut = i;
        }
        char piece[256];
        memcpy(piece, p, cut);
        piece[cut] = 0;
        enqueue(mode, piece, 0);
        p += cut, n -= cut;
    }
    enqueue(mode, (const char*)p, 0);
}

void xi_chat_queue(int mode, const char* line) { enqueue(mode, line, 1); }

static uint32_t g_write_line_fn, g_chatlog_global;

static int chat_log_ready(void)
{
#if defined(FFXI_WRAP_WRITE_LINE)
    if (!g_chatlog_global)
    {
        /* mov ecx, [global] at +0x17 of the function: the chat log object's global */
        if (xi_mapped(FFXI_WRAP_WRITE_LINE + 0x17, 6) && rd8(FFXI_WRAP_WRITE_LINE + 0x17) == 0x8B &&
            rd8(FFXI_WRAP_WRITE_LINE + 0x18) == 0x0D)
            g_chatlog_global = rd32(FFXI_WRAP_WRITE_LINE + 0x19);
        else
        {
            xi_log_once("chatlog", "the chat log writer at %08x is not the one known: host lines go to the log only",
                FFXI_WRAP_WRITE_LINE);
            g_chatlog_global = 0xFFFFFFFFu;
        }
    }
    return g_chatlog_global != 0xFFFFFFFFu && xi_mapped(g_chatlog_global, 4) && rd32(g_chatlog_global) != 0;
#else
    return 0;
#endif
}

static int g_writing; /* the host's own line: text_in sees it injected */

static void game_write(int mode, const char* text)
{
    if (xi_headless)
    {
        /* the harness: what the chat log would show, after text_in */
        XiEvent e;
        memset(&e, 0, sizeof e);
        static uint8_t mod[2048];
        size_t n = strlen(text);
        if (n >= sizeof mod)
            n = sizeof mod - 1;
        e.name = "text_in", e.mode = mode, e.mode_mod = mode, e.injected = 1;
        e.data = (const uint8_t*)text, e.size = n;
        memcpy(mod, text, n);
        e.mod = mod, e.mod_size = n, e.mod_cap = sizeof mod - 1;
        xi_raise(&e);
        if (!e.blocked)
            printf("[chat %d] %.*s\n", e.mode_mod, (int)e.mod_size, (const char*)e.mod);
        fflush(stdout);
        return;
    }
#if defined(FFXI_WRAP_WRITE_LINE)
    uint32_t s = gheap_strdup(text);
    uint32_t args[2] = { (uint32_t)mode, s };
    g_writing = 1;
    guest_call(FFXI_WRAP_WRITE_LINE, 2, args);
    g_writing = 0;
    gheap_free(s);
#else
    xi_log("chat: %s", text);
#endif
}

static void game_run(int mode, const char* line)
{
    if (xi_headless)
    {
        if (!xi_command(line, mode, 1))
            printf("[game] %s\n", line);
        fflush(stdout);
        return;
    }
#if defined(FFXI_WRAP_PARSE_INPUT)
    uint32_t s = gheap_strdup(line);
    uint32_t args[2] = { s, (uint32_t)mode };
    g_injecting = 1;
    guest_call(FFXI_WRAP_PARSE_INPUT, 2, args);
    g_injecting = 0;
    gheap_free(s);
#else
    (void)mode;
    xi_command(line, 1, 1);
#endif
}

#if defined(FFXI_WRAP_WRITE_LINE)
static void wrap_write_line(Guest* g)
{
    int mode = (int)rt_arg(g, 0);
    uint32_t text = rt_arg(g, 1);
    if (!xi_addon_count())
    {
        rt_orig_write_line(g);
        return;
    }
    static char line[2][2048];
    static uint8_t mod[2][2048];
    static int depth;
    if (depth >= 2)
    {
        rt_orig_write_line(g);
        return;
    }
    int d = depth++;
    size_t n = guest_str(text, line[d], sizeof line[d]);
    XiEvent e;
    memset(&e, 0, sizeof e);
    e.name = "text_in";
    e.mode = mode;
    e.mode_mod = mode;
    e.injected = g_writing;
    e.data = (const uint8_t*)line[d], e.size = n;
    memcpy(mod[d], line[d], n);
    e.mod = mod[d], e.mod_size = n, e.mod_cap = sizeof mod[d] - 1;
    xi_raise(&e);
    depth--;
    if (e.blocked)
    {
        rt_return(g, 0, 0);
        return;
    }
    if (e.mode_mod == mode && e.mod_size == n && !memcmp(e.mod, line[d], n))
    {
        rt_orig_write_line(g);
        return;
    }
    uint32_t buf = guest_scratch(0, (uint32_t)e.mod_size + 1);
    if (!buf)
    {
        rt_orig_write_line(g);
        return;
    }
    memcpy(GUEST_PTR(buf), e.mod, e.mod_size);
    wr8(buf + (uint32_t)e.mod_size, 0);
    wr32(g->esp + 4, (uint32_t)e.mode_mod);
    wr32(g->esp + 8, buf);
    rt_orig_write_line(g); /* cdecl: the caller's stack slots are the caller's; put them back */
    /* esp is past the return address now: the arguments sit at esp and esp + 4 */
    wr32(g->esp, (uint32_t)mode);
    wr32(g->esp + 4, text);
}
#endif

/* --- the chat input line --------------------------------------------------------------------- */

/* The chat manager: `mov ecx,[input]; push ..; push ebx; push ebx; mov edx,[ecx]; ...` (+2). In it
 * (2025-12-26), the input line's object at +0x7E30: its text at +0x6C4, length at +0x888; the
 * line's state at +0xF108 (1 while closed). */
static uint32_t chat_manager(void)
{
    static uint32_t global;
    if (!global)
    {
        uint32_t text, size;
        xi_image(NULL, NULL, &text, &size);
        uint32_t at = xi_find_pattern(text, size, "8B0D????????6A??53538B11536A??6A??536A", 2, 0);
        global = at ? rd32(at) : 0xFFFFFFFFu;
        xi_log("chat manager global: %08x", global);
    }
    if (global == 0xFFFFFFFFu || !xi_mapped(global, 4))
        return 0;
    uint32_t m = rd32(global);
    return m && xi_mapped(m, 0xF110) ? m : 0;
}

int xi_chat_input(char* out, size_t n)
{
    uint32_t m = chat_manager();
    if (!m || !n)
        return n ? (out[0] = 0, 0) : 0;
    uint32_t len = rd32(m + 0x7E30 + 0x888);
    if (len >= n)
        len = (uint32_t)n - 1;
    if (len > 0x100)
        len = 0x100;
    memcpy(out, GUEST_PTR(m + 0x7E30 + 0x6C4), len);
    out[len] = 0;
    return (int)len;
}

int xi_chat_input_open(void)
{
    uint32_t m = chat_manager();
    return m ? rd32(m + 0xF108) != 1 : 0;
}

void xi_chat_set_input(const char* text)
{
    uint32_t m = chat_manager();
    if (!m)
        return;
    size_t n = strlen(text);
    if (n > 0xFF)
        n = 0xFF;
    memcpy(GUEST_PTR(m + 0x7E30 + 0x6C4), text, n);
    wr8(m + 0x7E30 + 0x6C4 + (uint32_t)n, 0);
    wr32(m + 0x7E30 + 0x888, (uint32_t)n);
}

/* --- packets --------------------------------------------------------------------------------- */

typedef struct Last
{
    uint16_t size;
    uint64_t when;
    uint8_t data[0x200];
} Last;
static Last g_last[2][0x200];

typedef struct Inject
{
    size_t size;
    struct Inject* next;
    uint8_t data[];
} Inject;
static Inject* g_inject[2];

void xi_packet_inject(int outgoing, const uint8_t* p, size_t n)
{
    if (n < 4 || n > 0x1FC)
        return;
    size_t padded = (n + 3) & ~(size_t)3;
    Inject* in = (Inject*)calloc(1, sizeof *in + padded);
    in->size = padded;
    memcpy(in->data, p, n);
    /* the header's size is the padded length, in 4-byte units */
    uint16_t h = (uint16_t)((in->data[0] | in->data[1] << 8) & 0x1FF);
    h |= (uint16_t)((padded / 4) << 9);
    in->data[0] = (uint8_t)h, in->data[1] = (uint8_t)(h >> 8);
    Inject** tail = &g_inject[outgoing];
    while (*tail)
        tail = &(*tail)->next;
    *tail = in;
}

size_t xi_packet_last(int outgoing, uint16_t id, uint8_t* out, size_t cap, uint64_t* when_ms)
{
    Last* l = &g_last[outgoing ? 1 : 0][id & 0x1FF];
    size_t n = l->size < cap ? l->size : cap;
    memcpy(out, l->data, n);
    if (when_ms)
        *when_ms = l->when;
    return l->size ? n : 0;
}

/* One packet through the addons: 0 if blocked; else its (possibly changed) bytes in out. */
static size_t one_packet(int outgoing, const uint8_t* p, size_t size, const uint8_t* chunk, size_t chunk_size, int injected,
    uint8_t* out, size_t cap)
{
    XiEvent e;
    memset(&e, 0, sizeof e);
    e.name = outgoing ? "packet_out" : "packet_in";
    e.id = (uint32_t)((p[0] | p[1] << 8) & 0x1FF);
    e.sequence = (uint32_t)(p[2] | p[3] << 8);
    e.injected = injected;
    e.data = p, e.size = size;
    static uint8_t mod[0x200];
    memcpy(mod, p, size);
    e.mod = mod, e.mod_size = size, e.mod_cap = 0x1FC;
    e.chunk = chunk, e.chunk_size = chunk_size;
    xi_raise(&e);
    if (e.blocked)
        return 0;
    size_t n = e.mod_size;
    if (n < 4)
        return 0;
    size_t padded = (n + 3) & ~(size_t)3;
    if (padded > cap)
        return 0;
    memset(out, 0, padded);
    memcpy(out, e.mod, n);
    /* an addon that changed the size: the header follows (the id stays the one it wrote) */
    uint16_t h = (uint16_t)((out[0] | out[1] << 8) & 0x1FF);
    h |= (uint16_t)((padded / 4) << 9);
    out[0] = (uint8_t)h, out[1] = (uint8_t)(h >> 8);
    return padded;
}

/* The buffer (header + packets) through the addons into out; its new size. */
static size_t process(int outgoing, const uint8_t* buf, size_t size, uint8_t* out, size_t cap)
{
    if (size < 0x1C || cap < 0x1C)
    {
        memcpy(out, buf, size < cap ? size : cap);
        return size < cap ? size : cap;
    }
    memcpy(out, buf, 0x1C);
    size_t o = 0x1C, off = 0x1C;
    uint16_t seq = 0;
    uint64_t now = rt_monotonic_ns() / 1000000ull;
    while (off + 4 <= size)
    {
        const uint8_t* p = buf + off;
        size_t n = (size_t)((p[1] >> 1) & 0x7F) * 4;
        if (n < 4 || off + n > size)
            break;
        uint16_t id = (uint16_t)((p[0] | p[1] << 8) & 0x1FF);
        seq = (uint16_t)(p[2] | p[3] << 8);
        size_t w = one_packet(outgoing, p, n, buf + 0x1C, size - 0x1C, 0, out + o, cap - o);
        if (w)
        {
            Last* l = &g_last[outgoing][id];
            l->size = (uint16_t)w, l->when = now;
            memcpy(l->data, out + o, w);
        }
        o += w;
        off += n;
    }
    /* what doesn't parse is kept as it came */
    if (off < size && o + (size - off) <= cap)
    {
        memcpy(out + o, buf + off, size - off);
        o += size - off;
    }
    /* the addons' own packets, with the buffer's last sequence number */
    Inject** pp = &g_inject[outgoing];
    while (*pp)
    {
        Inject* in = *pp;
        if (o + in->size > cap)
            break; /* the rest wait for the next buffer */
        in->data[2] = (uint8_t)seq, in->data[3] = (uint8_t)(seq >> 8);
        size_t w = one_packet(outgoing, in->data, in->size, buf + 0x1C, size - 0x1C, 1, out + o, cap - o);
        o += w;
        *pp = in->next;
        free(in);
    }
    return o;
}

size_t xi_packets_process(int outgoing, const uint8_t* buf, size_t size, uint8_t* out, size_t cap)
{
    return process(outgoing, buf, size, out, cap);
}

static int packets_wanted(int outgoing)
{
    return xi_addon_count() || g_inject[outgoing];
}

static uint8_t g_pbuf[2][0x4000];

#if defined(FFXI_WRAP_PACKET_DECRYPT)
static void wrap_packet_decrypt(Guest* g)
{
    static uint64_t thread;
    if (!thread)
    {
        thread = plat_thread_id();
        xi_log("packet decrypt runs on thread %llu", (unsigned long long)thread);
    }
    uint32_t out = rt_arg(g, 0), capacity = rt_arg(g, 1);
    rt_orig_packet_decrypt(g);
    int32_t n = (int32_t)g->eax;
    if (n <= 0x1C || !packets_wanted(0) || (uint32_t)n > capacity || !xi_mapped(out, (uint32_t)n))
        return;
    size_t cap = capacity < sizeof g_pbuf[0] ? capacity : sizeof g_pbuf[0];
    size_t w = process(0, GUEST_PTR(out), (size_t)n, g_pbuf[0], cap);
    memcpy(GUEST_PTR(out), g_pbuf[0], w);
    g->eax = (uint32_t)w;
}
#endif

#if defined(FFXI_WRAP_PACKET_ENCRYPT)
static void wrap_packet_encrypt(Guest* g)
{
    uint32_t in = rt_arg(g, 3), size = rt_arg(g, 4);
    if (!packets_wanted(1) || size < 0x1C || size > sizeof g_pbuf[1] || !xi_mapped(in, size))
    {
        rt_orig_packet_encrypt(g);
        return;
    }
    /* the game's outgoing buffer is built in place with little room: the host's own, 0x2000 */
    uint32_t buf = guest_scratch(1, 0x2000);
    if (!buf)
    {
        rt_orig_packet_encrypt(g);
        return;
    }
    size_t w = process(1, GUEST_PTR(in), size, g_pbuf[1], 0x2000);
    memcpy(GUEST_PTR(buf), g_pbuf[1], w);
    wr32(g->esp + 4 + 12, buf);
    wr32(g->esp + 4 + 16, (uint32_t)w);
    rt_orig_packet_encrypt(g);
    wr32(g->esp + 12, in);
    wr32(g->esp + 16, size);
}
#endif

/* --- setup and frame ------------------------------------------------------------------------- */

void xi_hooks_init(void)
{
#if defined(FFXI_WRAP_PARSE_INPUT)
    rt_wrap_parse_input = wrap_parse_input;
#endif
#if defined(FFXI_WRAP_WRITE_LINE)
    rt_wrap_write_line = wrap_write_line;
#endif
#if defined(FFXI_WRAP_PACKET_DECRYPT)
    rt_wrap_packet_decrypt = wrap_packet_decrypt;
#endif
#if defined(FFXI_WRAP_PACKET_ENCRYPT)
    rt_wrap_packet_encrypt = wrap_packet_encrypt;
#endif
#if !defined(FFXI_WRAP_PARSE_INPUT) || !defined(FFXI_WRAP_WRITE_LINE) || !defined(FFXI_WRAP_PACKET_DECRYPT) || \
    !defined(FFXI_WRAP_PACKET_ENCRYPT)
    xi_log("build %s lacks some addon hooks (meta/builds.json wraps): commands, chat or packets are not seen", FFXI_BUILD);
#endif
    (void)g_write_line_fn;
}

void xi_hooks_frame(void)
{
    /* what is queued now, in order; what that queues runs next frame. Lines to write wait for the
     * chat log (they stay first, in order). */
    Queued* list = g_q_head;
    g_q_head = g_q_tail = NULL;
    Queued *keep = NULL, *keep_tail = NULL;
    int ready = -1;
    while (list)
    {
        Queued* q = list;
        list = q->next;
        q->next = NULL;
        if (!q->command)
        {
            if (ready < 0)
                ready = xi_headless || chat_log_ready();
            if (!ready)
            {
                if (keep_tail)
                    keep_tail->next = q;
                else
                    keep = q;
                keep_tail = q;
                continue;
            }
        }
        if (q->command)
            game_run(q->mode, q->text);
        else
            game_write(q->mode, q->text);
        free(q->text);
        free(q);
    }
    if (keep)
    {
        keep_tail->next = g_q_head;
        if (!g_q_head)
            g_q_tail = keep_tail;
        g_q_head = keep;
    }
}

/* Command routing: every line from the command line (typed, macro, menu) and every line the host
 * or an addon runs.
 *
 *   1. aliases expand (Ashita's /alias, Windower's alias), then route again;
 *   2. the host's own commands: //addon and /addon (load, unload, reload, list), Windower's //lua
 *      (load, unload, reload, list, command), //exec and /exec, //bind /bind //unbind /unbind,
 *      //alias /alias, //input (a line for the game), //echo (to the chat log);
 *   3. the addons' command event (every addon sees the line; one may block it);
 *   4. a line nobody handled that starts with // is an error, never chat the server would see;
 *   5. otherwise text_out (addons may change the line), then the game.
 *
 * Scripts (xi/scripts/boot.txt, ashita/scripts/default.txt, windower/scripts/init.txt on the first
 * frame; //exec <name>) run a line at a time; /wait n and //wait n delay the lines after them. A
 * Windower script's lines are console commands: those without a leading / get //. */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "plat.h"

#define CHAT_MODE 207

static void chatf(const char* fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    xi_chat_write(CHAT_MODE, line);
}

/* --- words ------------------------------------------------------------------------------------ */

/* Splits a line into words (quotes group; "" is a literal quote). Returns the count. */
static int split(const char* s, char words[][256], int max)
{
    int n = 0;
    while (*s && n < max)
    {
        while (*s == ' ' || *s == '\t')
            ++s;
        if (!*s)
            break;
        size_t o = 0;
        if (*s == '"')
        {
            ++s;
            while (*s && !(*s == '"' && s[1] != '"'))
            {
                if (*s == '"')
                    ++s;
                if (o + 1 < 256)
                    words[n][o++] = *s;
                ++s;
            }
            if (*s == '"')
                ++s;
        }
        else
            while (*s && *s != ' ' && *s != '\t')
            {
                if (o + 1 < 256)
                    words[n][o++] = *s;
                ++s;
            }
        words[n++][o] = 0;
    }
    return n;
}

/* The rest of the line after `skip` words. */
static const char* rest(const char* s, int skip)
{
    for (int i = 0; i < skip; ++i)
    {
        while (*s == ' ' || *s == '\t')
            ++s;
        if (*s == '"')
        {
            ++s;
            while (*s && !(*s == '"' && s[1] != '"'))
                s += *s == '"' ? 2 : 1;
            if (*s)
                ++s;
        }
        else
            while (*s && *s != ' ' && *s != '\t')
                ++s;
    }
    while (*s == ' ' || *s == '\t')
        ++s;
    return s;
}

static int kind_of(const char* s)
{
    for (int k = 0; k < XI_KINDS; ++k)
        if (!strcasecmp(s, xi_kind_name[k]))
            return k;
    return -1;
}

/* --- aliases ---------------------------------------------------------------------------------- */

typedef struct Alias
{
    char name[64];
    char* expansion;
    struct Alias* next;
} Alias;
static Alias* g_aliases;

void xi_alias_set(const char* name, const char* expansion)
{
    Alias** pp = &g_aliases;
    for (; *pp; pp = &(*pp)->next)
        if (!strcasecmp((*pp)->name, name))
            break;
    if (!expansion || !*expansion)
    {
        if (*pp)
        {
            Alias* a = *pp;
            *pp = a->next;
            free(a->expansion);
            free(a);
        }
        return;
    }
    if (!*pp)
    {
        *pp = (Alias*)calloc(1, sizeof **pp);
        snprintf((*pp)->name, sizeof (*pp)->name, "%s", name);
    }
    free((*pp)->expansion);
    (*pp)->expansion = strdup(expansion);
}

static const Alias* alias_of(const char* word)
{
    for (const Alias* a = g_aliases; a; a = a->next)
        if (!strcasecmp(a->name, word))
            return a;
    return NULL;
}

/* --- scripts ---------------------------------------------------------------------------------- */

typedef struct Line
{
    char* text;
    int kind;
    struct Line* next;
} Line;
typedef struct Script
{
    Line* lines;
    uint64_t resume_ms;
    struct Script* next;
} Script;
static Script* g_scripts;

static uint64_t now_ms(void) { return rt_monotonic_ns() / 1000000ull; }

int xi_exec_script(const char* name, int kind)
{
    char raw[1200], path[1200];
    const char* ext = strchr(name, '.') ? "" : ".txt";
    FILE* f = NULL;
    int kinds[3] = { kind, -1, -1 };
    if (kind < 0)
        kinds[0] = XI_KIND_XI, kinds[1] = XI_KIND_ASHITA, kinds[2] = XI_KIND_WINDOWER;
    for (int i = 0; i < 3 && !f && kinds[i] >= 0; ++i)
    {
        snprintf(raw, sizeof raw, "%sscripts/%s%s", xi_kind_root(kinds[i]), name, ext);
        f = fopen(xi_host_path(raw, path, sizeof path), "rb");
        if (f)
            kind = kinds[i];
    }
    if (!f)
        return 0;
    Script* s = (Script*)calloc(1, sizeof *s);
    Line** tail = &s->lines;
    char buf[1024];
    while (fgets(buf, sizeof buf, f))
    {
        char* p = buf;
        if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF)
            p += 3; /* a UTF-8 BOM */
        while (*p == ' ' || *p == '\t')
            ++p;
        size_t l = strlen(p);
        while (l && (p[l - 1] == '\n' || p[l - 1] == '\r' || p[l - 1] == ' ' || p[l - 1] == '\t'))
            p[--l] = 0;
        if (!l || p[0] == '#' || (p[0] == '-' && p[1] == '-'))
            continue;
        Line* ln = (Line*)calloc(1, sizeof *ln);
        char line[1100];
        if (kind == XI_KIND_WINDOWER && p[0] != '/')
            snprintf(line, sizeof line, "//%s", p);
        else
            snprintf(line, sizeof line, "%s", p);
        ln->text = strdup(line);
        ln->kind = kind;
        *tail = ln;
        tail = &ln->next;
    }
    fclose(f);
    Script** sp = &g_scripts;
    while (*sp)
        sp = &(*sp)->next;
    *sp = s;
    xi_log("script %s (%s)", path, xi_kind_name[kind]);
    return 1;
}

/* A wait line: its seconds, or -1. */
static double wait_of(const char* line)
{
    const char* p = line;
    while (*p == '/')
        ++p;
    if (strncasecmp(p, "wait", 4) || (p[4] && p[4] != ' '))
        return -1;
    return atof(p + 4);
}

void xi_cmd_frame(void)
{
    static int booted;
    if (!booted)
    {
        booted = 1;
        xi_exec_script("boot", XI_KIND_XI);
        xi_exec_script("default", XI_KIND_ASHITA);
        xi_exec_script("init", XI_KIND_WINDOWER);
    }
    uint64_t t = now_ms();
    for (Script** sp = &g_scripts; *sp;)
    {
        Script* s = *sp;
        while (s->lines && t >= s->resume_ms)
        {
            Line* ln = s->lines;
            s->lines = ln->next;
            double w = wait_of(ln->text);
            if (w >= 0)
                s->resume_ms = t + (uint64_t)(w * 1000.0);
            else
                xi_chat_queue(1, ln->text);
            free(ln->text);
            free(ln);
        }
        if (!s->lines)
        {
            *sp = s->next;
            free(s);
            continue;
        }
        sp = &s->next;
    }
}

/* --- the host's commands ---------------------------------------------------------------------- */

static void addon_list(void)
{
    unsigned n = xi_addon_count();
    chatf("addons loaded: %u", n);
    for (unsigned i = 0; i < n; ++i)
    {
        Addon* a = xi_addon_at(i);
        chatf("  %s (%s)%s", a->name, xi_kind_name[a->kind], a->dead ? " - stopped (crashed)" : "");
    }
}

/* //addon and /addon: load, unload, reload, list. kind_hint: Windower's //lua prefers its folder. */
static int addon_command(char w[][256], int n, int kind_hint)
{
    const char* op = n > 1 ? w[1] : "list";
    int kind = n > 3 ? kind_of(w[3]) : kind_hint;
    if (!strcasecmp(op, "load") || !strcasecmp(op, "l"))
    {
        if (n < 3)
            return chatf("usage: addon load <name> [xi|ashita|windower]"), 1;
        if (kind_hint >= 0 && n <= 3 && !xi_addon_load(w[2], kind_hint))
            return 1;
        if (kind_hint < 0 || n > 3)
            xi_addon_load(w[2], kind);
        return 1;
    }
    if (!strcasecmp(op, "unload") || !strcasecmp(op, "u"))
    {
        if (n < 3)
            return chatf("usage: addon unload <name>"), 1;
        Addon* a = xi_addon_find(w[2]);
        if (!a)
            chatf("addon: %s is not loaded", w[2]);
        else
            xi_addon_unload(a);
        return 1;
    }
    if (!strcasecmp(op, "reload") || !strcasecmp(op, "r"))
    {
        if (n < 3)
            return chatf("usage: addon reload <name>"), 1;
        Addon* a = xi_addon_find(w[2]);
        if (!a)
            xi_addon_load(w[2], kind);
        else
            xi_addon_reload(a);
        return 1;
    }
    if (!strcasecmp(op, "unloadall"))
    {
        while (xi_addon_count())
            xi_addon_unload(xi_addon_at(xi_addon_count() - 1));
        return 1;
    }
    if (!strcasecmp(op, "list") || !strcasecmp(op, "l"))
    {
        addon_list();
        return 1;
    }
    chatf("addon: load, unload, reload, list");
    return 1;
}

static int host_command(const char* line)
{
    int dbl = line[0] == '/' && line[1] == '/';
    const char* body = line + (dbl ? 2 : line[0] == '/' ? 1 : 0);
    char w[8][256];
    int n = split(body, w, 8);
    if (!n)
        return 0;
    const char* c = w[0];
    if (!strcasecmp(c, "addon") || (!dbl && !strcasecmp(c, "addons")))
        return addon_command(w, n, -1);
    if (dbl && !strcasecmp(c, "lua"))
    {
        /* Windower: lua load|l, unload|u, reload|r, list, command|c <name> <args> */
        if (n > 1 && (!strcasecmp(w[1], "command") || !strcasecmp(w[1], "c") || !strcasecmp(w[1], "invoke") ||
                         !strcasecmp(w[1], "i")))
        {
            if (n < 3)
                return chatf("usage: lua command <addon> <args>"), 1;
            char again[1100];
            snprintf(again, sizeof again, "//%s %s", w[2], rest(body, 3));
            return xi_command(again, 1, 1), 1;
        }
        return addon_command(w, n, XI_KIND_WINDOWER);
    }
    if (!strcasecmp(c, "load") && !dbl && n > 1)
        return xi_addon_load(w[1], XI_KIND_ASHITA), 1;
    if (!strcasecmp(c, "unload") && !dbl && n > 1)
    {
        Addon* a = xi_addon_find(w[1]);
        if (a)
            xi_addon_unload(a);
        return 1;
    }
    if (!strcasecmp(c, "reload") && !dbl && n > 1)
    {
        Addon* a = xi_addon_find(w[1]);
        if (a)
            xi_addon_reload(a);
        else
            xi_addon_load(w[1], XI_KIND_ASHITA);
        return 1;
    }
    if (!strcasecmp(c, "exec"))
    {
        if (n < 2 || !xi_exec_script(w[1], dbl ? -1 : XI_KIND_ASHITA))
            if (n < 2 || !xi_exec_script(w[1], -1))
                chatf("exec: no script %s", n > 1 ? w[1] : "");
        return 1;
    }
    if (!strcasecmp(c, "bind"))
    {
        if (n < 2 || !strcasecmp(w[1], "list"))
            return xi_bind_list(), 1;
        /* Ashita: /bind [down|up] key command; Windower: bind key [up] command */
        int arg = 1, up = 0;
        if (!strcasecmp(w[arg], "down"))
            ++arg;
        else if (!strcasecmp(w[arg], "up"))
            up = 1, ++arg;
        if (arg >= n)
            return chatf("usage: bind <key> <command>"), 1;
        const char* key = w[arg];
        int cmd_at = arg + 1;
        if (cmd_at < n && !strcasecmp(w[cmd_at], "up"))
            up = 1, ++cmd_at;
        const char* command = rest(body, cmd_at);
        if (!*command)
            return chatf("usage: bind <key> <command>"), 1;
        char cmdline[1100];
        /* a Windower console bind runs a console command: input /ma ... or another console line */
        if (dbl && command[0] != '/')
            snprintf(cmdline, sizeof cmdline, "//%s", command);
        else
            snprintf(cmdline, sizeof cmdline, "%s", command);
        if (!xi_bind(key, cmdline, up ? 2 : 1))
            chatf("bind: unknown key %s", key);
        return 1;
    }
    if (!strcasecmp(c, "unbind"))
    {
        if (n < 2)
            return chatf("usage: unbind <key>"), 1;
        if (!strcasecmp(w[1], "all"))
            return xi_unbind(NULL), 1;
        if (!xi_unbind(w[1]))
            chatf("unbind: %s is not bound", w[1]);
        return 1;
    }
    if (!strcasecmp(c, "alias"))
    {
        /* Ashita: /alias add|remove|list /name command; Windower: alias name command */
        int arg = 1;
        if (n > 1 && (!strcasecmp(w[1], "add") || !strcasecmp(w[1], "remove") || !strcasecmp(w[1], "list")))
        {
            if (!strcasecmp(w[1], "list"))
            {
                for (const Alias* a = g_aliases; a; a = a->next)
                    chatf("  %s -> %s", a->name, a->expansion);
                return 1;
            }
            if (!strcasecmp(w[1], "remove"))
                return n > 2 ? (xi_alias_set(w[2], NULL), 1) : 1;
            arg = 2;
        }
        if (n <= arg + 1)
            return chatf("usage: alias <name> <command>"), 1;
        char name[256];
        snprintf(name, sizeof name, "%s", w[arg]);
        /* Windower's names are console words (//name); Ashita's include their slash */
        if (dbl && name[0] != '/')
            snprintf(name, sizeof name, "//%s", w[arg]);
        const char* expansion = rest(body, arg + 1);
        char exp[1100];
        snprintf(exp, sizeof exp, "%s%s", dbl && expansion[0] != '/' ? "//" : "", expansion);
        xi_alias_set(name, exp);
        return 1;
    }
    if (dbl && !strcasecmp(c, "input"))
    {
        xi_chat_queue(1, rest(body, 1));
        return 1;
    }
    if (dbl && !strcasecmp(c, "echo"))
    {
        chatf("%s", rest(body, 1));
        return 1;
    }
    if (dbl && !strcasecmp(c, "wait"))
        return 1; /* only meaningful in scripts and send_command chains */
    return 0;
}

/* --- routing ---------------------------------------------------------------------------------- */

int xi_command(const char* in, int mode, int injected)
{
    static int depth;
    const char* line = in;
    while (*line == ' ' || *line == '\t')
        ++line;
    if (!*line)
        return 0;
    if (depth > 8)
    {
        chatf("command: aliases nested too deeply: %s", line);
        return 1;
    }
    /* aliases: the first word */
    {
        char first[256];
        size_t k = 0;
        while (line[k] && line[k] != ' ' && k + 1 < sizeof first)
            first[k] = line[k], ++k;
        first[k] = 0;
        const Alias* a = alias_of(first);
        if (a)
        {
            char again[2048];
            snprintf(again, sizeof again, "%s%s", a->expansion, line + k);
            depth++;
            int r = xi_command(again, mode, 1);
            depth--;
            if (!r)
                xi_chat_queue(mode, again); /* a line for the game: it runs through the game's own parser */
            return 1;
        }
    }
    if ((line[0] == '/') && host_command(line))
        return 1;

    XiEvent e;
    memset(&e, 0, sizeof e);
    e.name = "command";
    e.mode = mode;
    e.injected = injected;
    e.data = (const uint8_t*)line, e.size = strlen(line);
    xi_raise(&e);
    if (e.blocked || e.handled)
        return 1;
    if (line[0] == '/' && line[1] == '/')
    {
        chatf("unknown command: %.200s", line);
        return 1;
    }
    /* text leaving for the game (chat, or its own commands): addons may change or block it */
    static uint8_t mod[1024];
    XiEvent t;
    memset(&t, 0, sizeof t);
    t.name = "text_out";
    t.mode = mode;
    t.mode_mod = mode;
    t.injected = injected;
    t.data = (const uint8_t*)line, t.size = strlen(line);
    memcpy(mod, line, t.size);
    t.mod = mod, t.mod_size = t.size, t.mod_cap = sizeof mod - 1;
    xi_raise(&t);
    if (t.blocked)
        return 1;
    if (t.mod_size != t.size || memcmp(t.mod, line, t.size))
    {
        mod[t.mod_size] = 0;
        /* the changed line runs in the original's place (the host skips its events: injected) */
        xi_chat_queue(t.mode_mod, (const char*)mod);
        return 1;
    }
    return 0;
}

/* The addons Config > Addons lists (modern.c, through ModernAddons): every addon installed in the
 * kinds' folders (<root>/addons/<name>/<name>.lua; xi, then Ashita, then Windower when a name is in
 * more than one, as //addon load picks), which are on, and which are new.
 *
 * <data dir>/addons.cfg remembers them, a line each: "name = on" or "name = off". The ones on load
 * on the first frame (with the kinds' own scripts); one installed since the list was last shown is
 * new until the page closes, and off. A switch on the page loads or unloads on the next frame,
 * outside the menu's code, and is written to the file then; one that fails to load is written off,
 * so a broken addon is not tried again each start. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "addons.h"
#include "host.h"
#include "plat.h"
#include "../modern.h"

enum
{
    MAX_LISTED = 512,
    MAX_KNOWN = 1024,
};

typedef struct Known
{
    char name[64];
    int on;
} Known;

typedef struct Listed
{
    char name[64];
    int kind;
    int fresh; /* not in the file when listed */
    int want;  /* -1 nothing asked, else what the page asked for (until the next frame) */
} Listed;

static Known g_known[MAX_KNOWN];
static int g_nknown, g_loaded_file;
static Listed g_list[MAX_LISTED];
static int g_nlist, g_pending;

static void cfg_path(char* out, size_t n) { snprintf(out, n, "%saddons.cfg", xi_data_dir()); }

static Known* known(const char* name)
{
    for (int i = 0; i < g_nknown; ++i)
        if (!strcasecmp(g_known[i].name, name))
            return &g_known[i];
    return NULL;
}

static Known* remember(const char* name, int on)
{
    Known* k = known(name);
    if (!k && g_nknown < MAX_KNOWN)
    {
        k = &g_known[g_nknown++];
        snprintf(k->name, sizeof k->name, "%s", name);
    }
    if (k)
        k->on = on;
    return k;
}

static char* trim(char* s)
{
    while (*s == ' ' || *s == '\t')
        ++s;
    char* e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = 0;
    return s;
}

static void load_file(void)
{
    if (g_loaded_file)
        return;
    g_loaded_file = 1;
    char path[1200];
    cfg_path(path, sizeof path);
    size_t size;
    char* text = (char*)plat_read_file(path, &size);
    if (!text)
        return;
    char* buf = (char*)malloc(size + 1);
    memcpy(buf, text, size);
    buf[size] = 0;
    free(text);
    for (char* line = strtok(buf, "\n"); line; line = strtok(NULL, "\n"))
    {
        char* eq = strchr(line, '=');
        if (*trim(line) == '#' || !eq)
            continue;
        *eq = 0;
        char *name = trim(line), *value = trim(eq + 1);
        if (*name)
            remember(name, !strcasecmp(value, "on") || !strcmp(value, "1"));
    }
    free(buf);
}

static void save_file(void)
{
    char path[1200], tmp[1210];
    cfg_path(path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE* f = fopen(tmp, "w");
    if (!f)
        return;
    fprintf(f, "# Config > Addons: the addons that load with the game (on) and the ones known but left off.\n");
    for (int i = 0; i < g_nknown; ++i)
        fprintf(f, "%s = %s\n", g_known[i].name, g_known[i].on ? "on" : "off");
    fclose(f);
    plat_unlink(path);
    plat_rename(tmp, path);
}

static int by_name(const void* a, const void* b) { return strcasecmp(((const Listed*)a)->name, ((const Listed*)b)->name); }

static int listed(const char* name)
{
    for (int i = 0; i < g_nlist; ++i)
        if (!strcasecmp(g_list[i].name, name))
            return 1;
    return 0;
}

/* the folders as they are now; what the page asked for and the new marks carry over */
static int scan(void)
{
    load_file();
    Listed old[MAX_LISTED];
    int nold = g_nlist;
    memcpy(old, g_list, sizeof(Listed) * (size_t)nold);
    g_nlist = 0;
    static const int ORDER[] = { XI_KIND_XI, XI_KIND_ASHITA, XI_KIND_WINDOWER };
    for (int k = 0; k < 3; ++k)
    {
        char dir[1200], file[1400];
        snprintf(dir, sizeof dir, "%saddons", xi_kind_root(ORDER[k]));
        PlatDir* d = plat_dir_open(dir);
        const char* e;
        while (d && (e = plat_dir_next(d)) && g_nlist < MAX_LISTED)
        {
            PlatStat st;
            if (e[0] == '.' || !strcasecmp(e, "libs") || strlen(e) >= sizeof g_list[0].name || listed(e))
                continue;
            snprintf(file, sizeof file, "%s/%s/%s.lua", dir, e, e);
            if (!plat_stat(file, &st))
                continue;
            Listed* l = &g_list[g_nlist++];
            snprintf(l->name, sizeof l->name, "%s", e);
            l->kind = ORDER[k], l->fresh = !known(e), l->want = -1;
            for (int i = 0; i < nold; ++i)
                if (!strcasecmp(old[i].name, e))
                    l->fresh = old[i].fresh, l->want = old[i].want;
        }
        if (d)
            plat_dir_close(d);
    }
    qsort(g_list, (size_t)g_nlist, sizeof *g_list, by_name);
    return g_nlist;
}

static const char* name_of(int i) { return i >= 0 && i < g_nlist ? g_list[i].name : ""; }

static const char* kind_of(int i)
{
    static const char* const WORDS[XI_KINDS] = { "xi", "Ashita", "Windower" };
    return i >= 0 && i < g_nlist ? WORDS[g_list[i].kind] : "";
}

static int is_new(int i) { return i >= 0 && i < g_nlist && g_list[i].fresh; }

static int get(int i)
{
    if (i < 0 || i >= g_nlist)
        return 0;
    return g_list[i].want >= 0 ? g_list[i].want : xi_addon_find(g_list[i].name) != NULL;
}

static void set(int i, int on)
{
    if (i < 0 || i >= g_nlist)
        return;
    g_list[i].want = on;
    g_pending = 1;
}

/* the page closed: what it listed is no longer new */
static void seen(void)
{
    int changed = 0;
    for (int i = 0; i < g_nlist; ++i)
        if (g_list[i].fresh)
        {
            g_list[i].fresh = 0;
            if (!known(g_list[i].name))
                remember(g_list[i].name, 0), changed = 1;
        }
    if (changed)
        save_file();
}

static const ModernAddons OPS = { scan, name_of, kind_of, is_new, get, set, seen };

const ModernAddons* addons_menu(void) { return &OPS; }

/* Each frame (addons_frame): the first loads the ones on and tells of new ones; later, what the
 * page asked for. */
void xi_manage_frame(void)
{
    static int started;
    if (xi_headless)
        return; /* the harness loads what its script says */
    if (!started)
    {
        started = 1;
        scan();
        char fresh[400] = "";
        int nfresh = 0;
        for (int i = 0; i < g_nlist; ++i)
        {
            Known* k = known(g_list[i].name);
            if (k && k->on && !xi_addon_find(g_list[i].name) && !xi_addon_load(g_list[i].name, g_list[i].kind))
                k->on = 0, g_pending = 1; /* written below: not tried again */
            if (!k)
            {
                size_t n = strlen(fresh);
                if (n + strlen(g_list[i].name) + 3 < sizeof fresh)
                    snprintf(fresh + n, sizeof fresh - n, "%s%s", nfresh ? ", " : "", g_list[i].name);
                ++nfresh;
            }
        }
        if (nfresh)
        {
            char line[512];
            snprintf(line, sizeof line, "addon: %d new (%s): turn them on in Config > Addons", nfresh, fresh);
            xi_chat_write(207, line);
        }
        if (g_pending)
            save_file(), g_pending = 0;
        return;
    }
    if (!g_pending)
        return;
    g_pending = 0;
    int changed = 0;
    for (int i = 0; i < g_nlist; ++i)
    {
        Listed* l = &g_list[i];
        if (l->want < 0)
            continue;
        Addon* a = xi_addon_find(l->name);
        int on = l->want;
        l->want = -1;
        if (on && !a)
            on = xi_addon_load(l->name, l->kind);
        else if (!on && a)
            xi_addon_unload(a);
        Known* k = known(l->name);
        if (!k || k->on != on)
            remember(l->name, on), changed = 1;
    }
    if (changed)
        save_file();
}

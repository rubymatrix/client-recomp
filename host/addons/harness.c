/* host64 --addon-harness <script>: the addon host without the game running.
 *
 * The game's image is mapped and its DLLs initialised (patterns, the guest heap and guest calls
 * work), but GameStart never runs: no window, no GPU, no server, and the game's structures are
 * empty. Chat lines go to stdout as "[chat <mode>] text"; lines the game would run as
 * "[game] line". For testing the Lua layers against real addons (tools/addon_survey.py).
 *
 * Script lines ('#' starts a comment):
 *   load <name> [xi|ashita|windower]    unload <name>
 *   command <line>                      a line as if typed (the host's commands, the addons')
 *   frames <n>                          n frames (tasks, frame and drawing events)
 *   run <seconds>                       frames at 60 a second for that long (tasks that sleep)
 *   packet_in <hex> / packet_out <hex>  one packet (header included) through the pipeline
 *   text_in <mode> <text>               a line the chat log is given
 *   key <dik> <0|1>                     a key event
 *   mouse <message> <x> <y> [delta]     a mouse event
 *   xpad <buttons hex>                  pad 0 read by the game with these XInput buttons (prints what
 *                                       the game gets)
 *   lua <addon> <code>                  runs code in an addon's state (prints what it returns)
 *   echo <text>                         prints the text */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "addons.h"
#include "dinput.h"
#include "gthread.h"
#include "host.h"
#include "plat.h"

#include "lauxlib.h"
#include "lua.h"

static int hexbytes(const char* s, uint8_t* out, size_t cap)
{
    size_t n = 0;
    while (*s && n < cap)
    {
        while (*s && !isxdigit((unsigned char)*s))
            ++s;
        if (!s[0] || !s[1])
            break;
        char b[3] = { s[0], s[1], 0 };
        out[n++] = (uint8_t)strtoul(b, NULL, 16);
        s += 2;
    }
    return (int)n;
}

static void packet(int outgoing, const char* hex)
{
    uint8_t p[0x200], buf[0x400], out[0x4000];
    int n = hexbytes(hex, p, sizeof p);
    if (n < 4)
    {
        printf("[harness] packet: too short\n");
        return;
    }
    /* the packet's size field follows its length */
    size_t padded = ((size_t)n + 3) & ~(size_t)3;
    memset(buf, 0, sizeof buf);
    memcpy(buf + 0x1C, p, (size_t)n);
    uint16_t h = (uint16_t)((buf[0x1C] | buf[0x1D] << 8) & 0x1FF) | (uint16_t)((padded / 4) << 9);
    buf[0x1C] = (uint8_t)h, buf[0x1D] = (uint8_t)(h >> 8);
    size_t w = xi_packets_process(outgoing, buf, 0x1C + padded, out, sizeof out);
    printf("[harness] %s: %zu bytes in, %zu out", outgoing ? "packet_out" : "packet_in", 0x1C + padded, w);
    for (size_t i = 0x1C; i < w && i < 0x1C + 64; ++i)
        printf("%s%02x", i == 0x1C ? " : " : "", out[i]);
    printf("\n");
}

static void run_lua(const char* rest)
{
    char name[64];
    int k = 0;
    while (*rest && *rest != ' ' && k < 63)
        name[k++] = *rest++;
    name[k] = 0;
    while (*rest == ' ')
        ++rest;
    Addon* a = xi_addon_find(name);
    if (!a || a->dead)
    {
        printf("[harness] lua: %s is not loaded\n", name);
        return;
    }
    lua_State* L = a->L;
    int top = lua_gettop(L);
    char code[4200];
    snprintf(code, sizeof code, "return %s", rest);
    if (luaL_loadstring(L, code) && (lua_settop(L, top), luaL_loadstring(L, rest)))
    {
        printf("[harness] lua: %s\n", lua_tostring(L, -1));
        lua_settop(L, top);
        return;
    }
    gt_noyield(1);
    if (lua_pcall(L, 0, LUA_MULTRET, 0))
        printf("[harness] lua error: %s\n", lua_tostring(L, -1));
    else
        for (int i = top + 1; i <= lua_gettop(L); ++i)
        {
            lua_getglobal(L, "tostring");
            lua_pushvalue(L, i);
            lua_pcall(L, 1, 1, 0);
            printf("[harness] = %s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    gt_noyield(0);
    lua_settop(L, top);
}

int addons_harness(const char* script)
{
    FILE* f = fopen(script, "r");
    if (!f)
    {
        fprintf(stderr, "--addon-harness: cannot read %s\n", script);
        return 2;
    }
    xi_headless = 1;
    gt_lock();
    char line[4096];
    int errors = 0;
    while (fgets(line, sizeof line, f))
    {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r'))
            line[--l] = 0;
        char* p = line;
        while (*p == ' ' || *p == '\t')
            ++p;
        if (!*p || *p == '#')
            continue;
        char cmd[32];
        int k = 0;
        while (*p && *p != ' ' && k < 31)
            cmd[k++] = *p++;
        cmd[k] = 0;
        while (*p == ' ')
            ++p;
        fflush(stdout);
        if (!strcmp(cmd, "load"))
        {
            char name[64] = "", kind[16] = "";
            sscanf(p, "%63s %15s", name, kind);
            int kd = -1;
            for (int i = 0; i < XI_KINDS; ++i)
                if (!strcmp(kind, xi_kind_name[i]))
                    kd = i;
            if (!xi_addon_load(name, kd))
                errors++;
        }
        else if (!strcmp(cmd, "unload"))
        {
            Addon* a = xi_addon_find(p);
            if (a)
                xi_addon_unload(a);
        }
        else if (!strcmp(cmd, "command"))
        {
            if (!xi_command(p, 1, 0))
                printf("[game] %s\n", p);
        }
        else if (!strcmp(cmd, "frames") || !strcmp(cmd, "run"))
        {
            int n = !strcmp(cmd, "run") ? (int)(atof(p) * 60) : atoi(p);
            for (int i = 0; i < n; ++i)
            {
                addons_frame();
                if (!strcmp(cmd, "run"))
                    plat_sleep_ms(16);
            }
        }
        else if (!strcmp(cmd, "packet_in") || !strcmp(cmd, "packet_out"))
            packet(!strcmp(cmd, "packet_out"), p);
        else if (!strcmp(cmd, "text_in"))
        {
            int mode = atoi(p);
            while (*p && *p != ' ')
                ++p;
            while (*p == ' ')
                ++p;
            xi_chat_write(mode, p);
            addons_frame();
        }
        else if (!strcmp(cmd, "key"))
        {
            XiEvent e;
            memset(&e, 0, sizeof e);
            unsigned dik = 0;
            int down = 0;
            sscanf(p, "%x %d", &dik, &down);
            e.name = "key", e.key = dik, e.down = down;
            xi_raise(&e);
            printf("[harness] key %02x %s%s\n", dik, down ? "down" : "up", e.blocked || e.handled ? " (blocked)" : "");
        }
        else if (!strcmp(cmd, "mouse"))
        {
            XiEvent e;
            memset(&e, 0, sizeof e);
            unsigned msg = 0;
            sscanf(p, "%x %d %d %d", &msg, &e.x, &e.y, &e.delta);
            e.name = "mouse", e.msg = (int)msg;
            xi_raise(&e);
            printf("[harness] mouse %x%s\n", msg, e.blocked || e.handled ? " (blocked)" : "");
        }
        else if (!strcmp(cmd, "xpad"))
        {
            XPad x;
            memset(&x, 0, sizeof x);
            unsigned b = 0;
            sscanf(p, "%x", &b);
            x.buttons = (uint16_t)b;
            if (dinput_xpad_hook)
                dinput_xpad_hook(0, &x);
            printf("[harness] xpad %04x -> game sees %04x\n", b, x.buttons);
        }
        else if (!strcmp(cmd, "lua"))
            run_lua(p);
        else if (!strcmp(cmd, "echo"))
            printf("%s\n", p);
        else
            printf("[harness] unknown: %s\n", cmd);
        fflush(stdout);
    }
    fclose(f);
    addons_shutdown();
    /* the chat lines unload queued */
    xi_hooks_frame();
    gt_unlock();
    fflush(stdout);
    return errors ? 1 : 0;
}

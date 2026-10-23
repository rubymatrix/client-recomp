/* Win32 dialogs and their controls on AppKit, for the launcher programs (ffxivupdater.exe's
 * progress dialog, ffxivlogin.exe's browser dialog).
 *
 * user32.c serves the game: one SDL window, ANSI messages. The launcher programs instead build
 * dialogs from their DIALOG resources (CreateDialogParamW) and drive child controls with
 * messages: static text, push buttons, progress bars. Here each dialog is an NSWindow and each
 * control an AppKit view, behind HWNDs of their own range. The window calls are taken over
 * (thunk_override) and handed to user32.c's shims when the HWND is not one of ours; messages
 * share user32.c's per-thread queues.
 *
 * The window procedures follow Win32: a dialog's is DefDlgProc, which calls the program's dialog
 * procedure (DWL_DLGPROC; WTL replaces it with an ATL thunk, which runtime.c runs) and falls back
 * to the default handling; the controls' are built in. All are guest-callable (thunk_for), so
 * GetWindowLong(GWL_WNDPROC) and subclassing work as on Windows.
 *
 * AppKit runs on the main thread (SDL's, the host's loop): view work goes there and waits. Guest
 * code never runs on it; clicks and window closes are posted to the owning thread's queue.
 */
#import <AppKit/AppKit.h>
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gthread.h"
#include "gwin.h"
#include "k32.h"
#include "pe.h"
#include "plat.h"
#include "thunk.h"
#include "user32.h"

#define WM_DESTROY 0x0002
#define WM_MOVE 0x0003
#define WM_SIZE 0x0005
#define WM_SETFOCUS 0x0007
#define WM_ENABLE 0x000A
#define WM_SETTEXT 0x000C
#define WM_GETTEXT 0x000D
#define WM_GETTEXTLENGTH 0x000E
#define WM_CLOSE 0x0010
#define WM_QUIT 0x0012
#define WM_SHOWWINDOW 0x0018
#define WM_SETFONT 0x0030
#define WM_GETFONT 0x0031
#define WM_NCDESTROY 0x0082
#define WM_INITDIALOG 0x0110
#define WM_COMMAND 0x0111
#define WM_SYSCOMMAND 0x0112
#define WM_TIMER 0x0113

#define WS_VISIBLE 0x10000000u
#define WS_DISABLED 0x08000000u
#define WS_CHILD 0x40000000u
#define DS_SETFONT 0x40u
#define SS_TYPEMASK 0x1Fu
#define SS_NOPREFIX 0x80u
#define BS_TYPEMASK 0x0Fu
#define BS_OWNERDRAW 0x0Bu

#define PBM_SETRANGE 0x0401
#define PBM_SETPOS 0x0402
#define PBM_DELTAPOS 0x0403
#define PBM_SETSTEP 0x0404
#define PBM_STEPIT 0x0405
#define PBM_SETRANGE32 0x0406
#define PBM_GETRANGE 0x0407
#define PBM_GETPOS 0x0408

#define CAPTION_H 28 /* a titled NSWindow's title bar, in points: Win32 window rects include it */

/* --- windows ----------------------------------------------------------------------------------- */
typedef enum { UK_DIALOG, UK_STATIC, UK_BUTTON, UK_PROGRESS, UK_ANIMATE, UK_OTHER } UiKind;

#define UI_MAX 128
#define UI_BASE 0x00060010u
#define UI_STRIDE 0x10u
#define UI_TEXT 512

typedef struct UiWnd
{
    int used;
    uint32_t hwnd, parent, owner, id, style, exstyle, tid, order;
    UiKind kind;
    uint32_t wndproc, dlgproc, userdata, msgresult, dlguser, font, hinst;
    int x, y, w, h; /* children: in the parent's client area; dialogs: the client area on the screen */
    int visible, enabled;
    uint16_t text[UI_TEXT];
    int pmin, pmax, ppos, pstep;
    void* view; /* NSWindow* (dialogs) or NSView* (controls), retained */
} UiWnd;

static UiWnd g_ui[UI_MAX];

/* SetTimer's timers: WM_TIMER for our windows, and thread timers with a procedure */
typedef struct { uint32_t tid, hwnd, id, proc; uint64_t period_ns, due_ns; } Timer;
static Timer g_timers[32];

static uint32_t g_order;
static uint32_t g_focus;
static float g_font_pt = 12;

static UiWnd* ui(uint32_t hwnd)
{
    if (hwnd < UI_BASE || (hwnd - UI_BASE) % UI_STRIDE)
        return NULL;
    uint32_t i = (hwnd - UI_BASE) / UI_STRIDE;
    return i < UI_MAX && g_ui[i].used ? &g_ui[i] : NULL;
}

static UiWnd* ui_new(void)
{
    for (int i = 0; i < UI_MAX; ++i)
        if (!g_ui[i].used)
        {
            memset(&g_ui[i], 0, sizeof g_ui[i]);
            g_ui[i].used = 1;
            g_ui[i].hwnd = UI_BASE + (uint32_t)i * UI_STRIDE;
            g_ui[i].order = ++g_order;
            g_ui[i].enabled = 1;
            g_ui[i].pmax = 100;
            g_ui[i].pstep = 10;
            return &g_ui[i];
        }
    return NULL;
}

static uint32_t wlen(uint32_t w)
{
    uint32_t n = 0;
    while (w && rd16(w + 2 * n))
        n++;
    return n;
}

static int text_len(const UiWnd* w)
{
    int n = 0;
    while (n < UI_TEXT && w->text[n])
        n++;
    return n;
}

static void set_text(UiWnd* w, uint32_t gw)
{
    uint32_t n = gw ? wlen(gw) : 0;
    if (n >= UI_TEXT)
        n = UI_TEXT - 1;
    for (uint32_t i = 0; i < n; ++i)
        w->text[i] = rd16(gw + 2 * i);
    w->text[n] = 0;
}

/* --- the main thread ------------------------------------------------------------------------------ */
static void run_block(void* b) { ((void (^)(void))b)(); }

static void on_main(void (^b)(void))
{
    if (SDL_IsMainThread())
        b();
    else
        SDL_RunOnMainThread(run_block, (void*)b, true);
}

/* A control's caption: its text without the '&' mnemonic markers (SS_NOPREFIX keeps them). */
static NSString* caption(const UiWnd* w, int keep_amp)
{
    int n = text_len(w);
    unichar buf[UI_TEXT];
    int k = 0;
    for (int i = 0; i < n; ++i)
    {
        if (!keep_amp && w->text[i] == '&')
        {
            if (i + 1 < n && w->text[i + 1] == '&')
                buf[k++] = '&', ++i;
            continue;
        }
        buf[k++] = w->text[i];
    }
    return [NSString stringWithCharacters:buf length:(NSUInteger)k];
}

@interface UiFlipped : NSView
@end
@implementation UiFlipped
- (BOOL)isFlipped { return YES; }
- (void)drawRect:(NSRect)r
{
    if ([self window] && [[self window] contentView] == self) /* a dialog's face, as Win32 paints it */
    {
        [[NSColor windowBackgroundColor] setFill];
        NSRectFill(r);
    }
}
@end

@interface UiTarget : NSObject <NSWindowDelegate>
@end

static UiTarget* g_target;

@implementation UiTarget
- (void)click:(id)sender
{
    UiWnd* w = ui((uint32_t)[sender tag]);
    UiWnd* p = w ? ui(w->parent) : NULL;
    if (p) /* BN_CLICKED */
        user32_post(p->tid, p->hwnd, WM_COMMAND, w->id & 0xFFFFu, w->hwnd);
}
- (BOOL)windowShouldClose:(NSWindow*)sender
{
    for (int i = 0; i < UI_MAX; ++i)
        if (g_ui[i].used && g_ui[i].view == (void*)sender)
            user32_post(g_ui[i].tid, g_ui[i].hwnd, WM_CLOSE, 0, 0);
    return NO; /* the program decides: DestroyWindow closes it */
}
@end

static double screen_h(void) { return NSMaxY([[NSScreen screens][0] frame]); }

/* A label wider than its Win32 box (AppKit's font is wider than the dialog's) grows from the
 * edge its alignment keeps: right-aligned text grows left, centred both ways. */
static NSRect label_frame(NSTextField* t, NSRect r, uint32_t ss)
{
    CGFloat need = ceil([[t cell] cellSize].width);
    if (need <= r.size.width)
        return r;
    CGFloat grow = need - r.size.width;
    r.origin.x -= ss == 2 ? grow : ss == 1 ? grow / 2 : 0;
    r.size.width = need;
    return r;
}

/* the AppKit view for a new window (main thread) */
static void make_view(UiWnd* w)
{
    if (!g_target)
        g_target = [[UiTarget alloc] init];
    if (w->kind == UK_DIALOG)
    {
        NSRect r = NSMakeRect(w->x, screen_h() - w->y - w->h, w->w, w->h);
        NSWindow* win = [[NSWindow alloc] initWithContentRect:r
                                                    styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                                               | NSWindowStyleMaskMiniaturizable
                                                      backing:NSBackingStoreBuffered
                                                        defer:NO];
        [win setReleasedWhenClosed:NO];
        [win setDelegate:g_target];
        [win setTitle:caption(w, 1)];
        UiFlipped* content = [[UiFlipped alloc] initWithFrame:NSMakeRect(0, 0, w->w, w->h)];
        [win setContentView:content];
        [content release];
        w->view = win;
        return;
    }
    UiWnd* p = ui(w->parent);
    NSView* pv = !p ? nil : p->kind == UK_DIALOG ? [(NSWindow*)p->view contentView] : (NSView*)p->view;
    NSRect r = NSMakeRect(w->x, w->y, w->w, w->h);
    NSView* v;
    if (w->kind == UK_STATIC)
    {
        NSTextField* t = [NSTextField labelWithString:caption(w, (w->style & SS_NOPREFIX) != 0)];
        [t setFont:[NSFont systemFontOfSize:g_font_pt]];
        uint32_t ss = w->style & SS_TYPEMASK;
        [t setAlignment:ss == 1 ? NSTextAlignmentCenter : ss == 2 ? NSTextAlignmentRight : NSTextAlignmentLeft];
        [t setLineBreakMode:NSLineBreakByTruncatingTail];
        [t setFrame:label_frame(t, r, ss)];
        v = [t retain];
    }
    else if (w->kind == UK_BUTTON)
    {
        NSButton* b = [NSButton buttonWithTitle:caption(w, 0) target:g_target action:@selector(click:)];
        [b setFrame:NSInsetRect(r, -4, -3)]; /* AppKit's push button draws inside its frame */
        [b setFont:[NSFont systemFontOfSize:g_font_pt]];
        [b setTag:(NSInteger)w->hwnd];
        [b setEnabled:w->enabled];
        v = [b retain];
    }
    else if (w->kind == UK_PROGRESS)
    {
        NSProgressIndicator* pi = [[NSProgressIndicator alloc] initWithFrame:r];
        [pi setIndeterminate:NO];
        [pi setMinValue:w->pmin];
        [pi setMaxValue:w->pmax];
        [pi setDoubleValue:w->ppos];
        v = pi;
    }
    else if (w->kind == UK_ANIMATE)
    {
        NSProgressIndicator* pi = [[NSProgressIndicator alloc] initWithFrame:r];
        [pi setStyle:NSProgressIndicatorStyleSpinning];
        [pi setControlSize:NSControlSizeSmall];
        [pi setDisplayedWhenStopped:NO];
        v = pi;
    }
    else
        v = [[UiFlipped alloc] initWithFrame:r];
    [v setHidden:!w->visible];
    [pv addSubview:v];
    w->view = v;
    if (getenv("XIV_UI_LOG"))
        rt_log("[ui] %05x kind %d id %u (%d,%d %dx%d) visible %d parent view %p: \"%s\"\n", w->hwnd, w->kind, w->id, w->x,
            w->y, w->w, w->h, w->visible, (void*)pv, [caption(w, 1) UTF8String]);
}

/* XIV_UI_SHOT=<dir>: each dialog's content, drawn by AppKit into <dir>/dialog-<hwnd>-<n>.png at
 * most twice a second while it changes (main thread). Only our own windows, never the screen. */
static void shot(NSWindow* win, uint32_t hwnd)
{
    static const char* dir;
    static int checked;
    static uint64_t last_ns;
    static unsigned n;
    if (!checked)
        checked = 1, dir = getenv("XIV_UI_SHOT");
    uint64_t now = rt_monotonic_ns();
    if (!dir || ![win isVisible] || now - last_ns < 500000000u)
        return;
    last_ns = now;
    NSView* v = [win contentView];
    NSBitmapImageRep* rep = [v bitmapImageRepForCachingDisplayInRect:[v bounds]];
    [v cacheDisplayInRect:[v bounds] toBitmapImageRep:rep];
    NSData* png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    char path[1024];
    snprintf(path, sizeof path, "%s/dialog-%05x-%03u.png", dir, hwnd, n++);
    [png writeToFile:[NSString stringWithUTF8String:path] atomically:NO];
}

/* the view follows the window's state (main thread) */
static void sync_view(UiWnd* w)
{
    if (!w->view)
        return;
    if (w->kind != UK_DIALOG)
    {
        UiWnd* d = w;
        while (d && d->kind != UK_DIALOG)
            d = ui(d->parent);
        if (d && d->view)
        {
            NSWindow* dw = (NSWindow*)d->view;
            uint32_t dh = d->hwnd;
            dispatch_async(dispatch_get_main_queue(), ^{ shot(dw, dh); }); /* after this change is drawn */
        }
    }
    if (w->kind == UK_DIALOG)
    {
        NSWindow* win = (NSWindow*)w->view;
        [win setTitle:caption(w, 1)];
        [win setFrame:[win frameRectForContentRect:NSMakeRect(w->x, screen_h() - w->y - w->h, w->w, w->h)] display:YES];
        if (w->visible && ![win isVisible])
        {
            [NSApp activateIgnoringOtherApps:YES];
            [win makeKeyAndOrderFront:nil];
        }
        else if (!w->visible && [win isVisible])
            [win orderOut:nil];
        return;
    }
    NSView* v = (NSView*)w->view;
    NSRect r = NSMakeRect(w->x, w->y, w->w, w->h);
    [v setHidden:!w->visible];
    if (w->kind == UK_STATIC)
    {
        [(NSTextField*)v setStringValue:caption(w, (w->style & SS_NOPREFIX) != 0)];
        [v setFrame:label_frame((NSTextField*)v, r, w->style & SS_TYPEMASK)];
    }
    else if (w->kind == UK_BUTTON)
    {
        [(NSButton*)v setTitle:caption(w, 0)];
        [(NSButton*)v setEnabled:w->enabled];
        [v setFrame:NSInsetRect(r, -4, -3)];
    }
    else if (w->kind == UK_PROGRESS)
    {
        NSProgressIndicator* pi = (NSProgressIndicator*)v;
        [pi setMinValue:w->pmin];
        [pi setMaxValue:w->pmax];
        [pi setDoubleValue:w->ppos];
        [v setFrame:r];
    }
    else
        [v setFrame:r];
}

static void ui_sync(UiWnd* w)
{
    UiWnd copy = *w; /* the main thread reads a snapshot: guest threads keep changing the original */
    on_main(^{
        UiWnd c = copy;
        sync_view(&c);
    });
}

/* --- window procedures ---------------------------------------------------------------------------- */
static uint32_t ui_send(UiWnd* w, uint32_t msg, uint32_t wp, uint32_t lp)
{
    static int log = -1;
    if (log < 0)
        log = getenv("XIV_UI_LOG") != NULL;
    if (log && msg != WM_TIMER)
        rt_log("[ui] send %05x msg %#x wp %x lp %x\n", w->hwnd, msg, wp, lp);
    uint32_t a[4] = { w->hwnd, msg, wp, lp };
    return guest_call(w->wndproc, 4, a);
}

static void destroy(UiWnd* w);

/* DefWindowProc for our windows */
static uint32_t def_window(UiWnd* w, uint32_t msg, uint32_t wp, uint32_t lp)
{
    switch (msg)
    {
    case WM_SETTEXT:
        set_text(w, lp);
        ui_sync(w);
        return 1;
    case WM_GETTEXT:
    {
        int n = text_len(w);
        if (!wp)
            return 0;
        if ((uint32_t)n >= wp)
            n = (int)wp - 1;
        for (int i = 0; i < n; ++i)
            wr16(lp + 2 * (uint32_t)i, w->text[i]);
        wr16(lp + 2 * (uint32_t)n, 0);
        return (uint32_t)n;
    }
    case WM_GETTEXTLENGTH:
        return (uint32_t)text_len(w);
    case WM_SETFONT:
        w->font = wp;
        return 0;
    case WM_GETFONT:
        return w->font;
    case WM_CLOSE:
        destroy(w);
        return 0;
    }
    return 0;
}

/* DefDlgProc: the dialog procedure first; its TRUE means handled, with the result in DWL_MSGRESULT
 * except for the messages that return their value directly */
static uint32_t def_dialog(UiWnd* w, uint32_t msg, uint32_t wp, uint32_t lp)
{
    if (w->dlgproc)
    {
        uint32_t a[4] = { w->hwnd, msg, wp, lp };
        uint32_t r = guest_call(w->dlgproc, 4, a);
        w = ui(a[0]); /* the procedure may have destroyed it */
        if (r)
        {
            if (msg == WM_INITDIALOG || (msg >= 0x132 && msg <= 0x138) || msg == 0x2E || msg == 0x2F || msg == 0x37
                || msg == 0x39)
                return r;
            return w ? w->msgresult : 0;
        }
        if (!w)
            return 0;
    }
    switch (msg)
    {
    case WM_INITDIALOG:
        return 1;
    case WM_CLOSE: /* IDCANCEL, from the Cancel button if there is one */
    {
        uint32_t cancel = 0;
        for (int i = 0; i < UI_MAX; ++i)
            if (g_ui[i].used && g_ui[i].parent == w->hwnd && g_ui[i].id == 2)
                cancel = g_ui[i].hwnd;
        ui_send(w, WM_COMMAND, 2, cancel);
        return 0;
    }
    }
    return def_window(w, msg, wp, lp);
}

static uint32_t progress_proc(UiWnd* w, uint32_t msg, uint32_t wp, uint32_t lp)
{
    int old = w->ppos;
    switch (msg)
    {
    case PBM_SETRANGE:
    {
        uint32_t prev = ((uint32_t)(uint16_t)w->pmax << 16) | (uint16_t)w->pmin;
        w->pmin = (int)(lp & 0xFFFF), w->pmax = (int)(lp >> 16);
        ui_sync(w);
        return prev;
    }
    case PBM_SETRANGE32:
    {
        uint32_t prev = ((uint32_t)(uint16_t)w->pmax << 16) | (uint16_t)w->pmin;
        w->pmin = (int)wp, w->pmax = (int)lp;
        ui_sync(w);
        return prev;
    }
    case PBM_GETRANGE:
        if (lp)
            wr32(lp, (uint32_t)w->pmin), wr32(lp + 4, (uint32_t)w->pmax);
        return (uint32_t)(wp ? w->pmin : w->pmax);
    case PBM_SETPOS:
        w->ppos = (int)wp;
        break;
    case PBM_DELTAPOS:
        w->ppos += (int)wp;
        break;
    case PBM_SETSTEP:
    {
        int prev = w->pstep;
        w->pstep = (int)wp;
        return (uint32_t)prev;
    }
    case PBM_STEPIT:
        w->ppos += w->pstep;
        if (w->ppos > w->pmax)
            w->ppos = w->pmin + (w->ppos - w->pmax);
        break;
    case PBM_GETPOS:
        return (uint32_t)w->ppos;
    default:
        return def_window(w, msg, wp, lp);
    }
    if (w->ppos < w->pmin)
        w->ppos = w->pmin;
    if (w->ppos > w->pmax)
        w->ppos = w->pmax;
    if (w->ppos != old)
        ui_sync(w);
    return (uint32_t)old;
}

static uint32_t animate_proc(UiWnd* w, uint32_t msg, uint32_t wp, uint32_t lp)
{
    switch (msg)
    {
    case 0x0464: /* ACM_OPENW: the AVI is the spinner here */
    case 0x0464 + 3: /* ACM_OPENA */
        return 1;
    case 0x0465: /* ACM_PLAY */
    case 0x0466: /* ACM_STOP */
    {
        int play = msg == 0x0465;
        NSProgressIndicator* pi = (NSProgressIndicator*)w->view;
        on_main(^{
            if (play)
                [pi startAnimation:nil];
            else
                [pi stopAnimation:nil];
        });
        return 1;
    }
    }
    return def_window(w, msg, wp, lp);
}

#define WNDPROC_SHIM(name, body)                                                                                \
    static void sh_##name(Guest* g)                                                                             \
    {                                                                                                           \
        UiWnd* w = ui(ARG(0));                                                                                  \
        RET(w ? body(w, ARG(1), ARG(2), ARG(3)) : 0, 4);                                                        \
    }
WNDPROC_SHIM(ui_dialog_proc, def_dialog)
WNDPROC_SHIM(ui_control_proc, def_window)
WNDPROC_SHIM(ui_progress_proc, progress_proc)
WNDPROC_SHIM(ui_animate_proc, animate_proc)

static uint32_t proc_for(UiKind k)
{
    const char* n = k == UK_DIALOG ? "ui_dialog_proc" : k == UK_PROGRESS ? "ui_progress_proc"
                  : k == UK_ANIMATE ? "ui_animate_proc" : "ui_control_proc";
    return thunk_for("user32.dll", n);
}

/* --- creation and destruction -------------------------------------------------------------------- */
static void destroy(UiWnd* w)
{
    uint32_t h = w->hwnd;
    ui_send(w, WM_DESTROY, 0, 0);
    for (int i = 0; i < UI_MAX; ++i)
        if (g_ui[i].used && (g_ui[i].parent == h || g_ui[i].owner == h))
            destroy(&g_ui[i]);
    if (!(w = ui(h)))
        return;
    ui_send(w, WM_NCDESTROY, 0, 0);
    if (!(w = ui(h)))
        return;
    for (int i = 0; i < 32; ++i)
        if (g_timers[i].tid && g_timers[i].hwnd == h)
            memset(&g_timers[i], 0, sizeof g_timers[i]);
    void* v = w->view;
    int dialog = w->kind == UK_DIALOG;
    memset(w, 0, sizeof *w);
    if (g_focus == h)
        g_focus = 0;
    if (v)
        on_main(^{
            if (dialog)
            {
                [(NSWindow*)v orderOut:nil];
                [(NSWindow*)v close];
            }
            else
                [(NSView*)v removeFromSuperview];
            [(id)v release];
        });
}

static UiKind kind_of(const char* cls)
{
    if (!strcasecmp(cls, "STATIC"))
        return UK_STATIC;
    if (!strcasecmp(cls, "BUTTON"))
        return UK_BUTTON;
    if (!strcasecmp(cls, "msctls_progress32"))
        return UK_PROGRESS;
    if (!strcasecmp(cls, "SysAnimate32"))
        return UK_ANIMATE;
    return UK_OTHER;
}

/* A string or ordinal field of a dialog template: an atom id (0xFFFF, n), empty, or UTF-16 text
 * (narrowed into out). Returns the address after it. */
static uint32_t sz_or_ord(uint32_t p, char* out, size_t cap, uint32_t* ord)
{
    *ord = 0;
    out[0] = 0;
    uint16_t c = rd16(p);
    if (c == 0xFFFF)
    {
        *ord = rd16(p + 2);
        return p + 4;
    }
    size_t k = 0;
    while ((c = rd16(p)))
    {
        if (k + 1 < cap)
            out[k++] = c > 0x7F ? '?' : (char)c;
        p += 2;
    }
    out[k] = 0;
    return p + 2;
}

/* CreateDialogIndirectParam's work: the dialog and its controls from the template at tpl (guest),
 * in dialog units, then WM_SETFONT and WM_INITDIALOG. Returns the HWND or 0. */
static uint32_t create_dialog(uint32_t hinst, uint32_t tpl, uint32_t parent, uint32_t dlgproc, uint32_t param)
{
    int ex = rd16(tpl) == 1 && rd16(tpl + 2) == 0xFFFF;
    uint32_t style, exstyle, n, p;
    int16_t x, y, cx, cy;
    if (ex)
    {
        exstyle = rd32(tpl + 8), style = rd32(tpl + 12), n = rd16(tpl + 16);
        x = (int16_t)rd16(tpl + 18), y = (int16_t)rd16(tpl + 20), cx = (int16_t)rd16(tpl + 22), cy = (int16_t)rd16(tpl + 24);
        p = tpl + 26;
    }
    else
    {
        style = rd32(tpl), exstyle = rd32(tpl + 4), n = rd16(tpl + 8);
        x = (int16_t)rd16(tpl + 10), y = (int16_t)rd16(tpl + 12), cx = (int16_t)rd16(tpl + 14), cy = (int16_t)rd16(tpl + 16);
        p = tpl + 18;
    }
    char cls[128];
    uint32_t ord;
    p = sz_or_ord(p, cls, sizeof cls, &ord); /* menu */
    p = sz_or_ord(p, cls, sizeof cls, &ord); /* class: the dialog class here whatever it names */
    uint32_t title = p;
    p += 2 * (wlen(p) + 1);
    int pt = 8;
    if (style & DS_SETFONT)
    {
        pt = rd16(p);
        p += ex ? 6 : 2;
        p += 2 * (wlen(p) + 1); /* face: AppKit's system font stands in */
    }
    /* dialog base units of the template's font at 96 dpi (MS Shell Dlg 8: 6 x 13) */
    int bx = pt >= 10 ? 7 : 6, by = (pt * 4 + 2) / 3 + 2;
    g_font_pt = pt >= 9 ? 12 : 11;
#define DX(v) ((int)(v) * bx / 4)
#define DY(v) ((int)(v) * by / 8)

    UiWnd* d = ui_new();
    if (!d)
        return 0;
    d->kind = UK_DIALOG;
    d->style = style & ~WS_VISIBLE;
    d->exstyle = exstyle;
    d->owner = parent;
    d->hinst = hinst;
    d->tid = gt_self()->tid;
    d->wndproc = proc_for(UK_DIALOG);
    d->dlgproc = dlgproc;
    d->w = DX(cx), d->h = DY(cy);
    UiWnd* o = ui(parent);
    d->x = (o ? o->x : 0) + DX(x), d->y = (o ? o->y : 0) + DY(y) + CAPTION_H;
    set_text(d, title);
    uint32_t dh = d->hwnd;
    { UiWnd c = *d; on_main(^{ UiWnd cc = c; make_view(&cc); g_ui[(dh - UI_BASE) / UI_STRIDE].view = cc.view; }); }

    uint32_t first = 0;
    for (uint32_t i = 0; i < n; ++i)
    {
        p = (p + 3) & ~3u;
        uint32_t ist, iex, iid;
        int16_t ix, iy, icx, icy;
        if (ex)
        {
            iex = rd32(p + 4), ist = rd32(p + 8);
            ix = (int16_t)rd16(p + 12), iy = (int16_t)rd16(p + 14), icx = (int16_t)rd16(p + 16), icy = (int16_t)rd16(p + 18);
            iid = rd32(p + 20);
            p += 24;
        }
        else
        {
            ist = rd32(p), iex = rd32(p + 4);
            ix = (int16_t)rd16(p + 8), iy = (int16_t)rd16(p + 10), icx = (int16_t)rd16(p + 12), icy = (int16_t)rd16(p + 14);
            iid = rd16(p + 16);
            p += 18;
        }
        char icls[128];
        uint32_t cord;
        p = sz_or_ord(p, icls, sizeof icls, &cord);
        static const char* atoms[] = { "BUTTON", "EDIT", "STATIC", "LISTBOX", "SCROLLBAR", "COMBOBOX" };
        if (cord >= 0x80 && cord <= 0x85)
            strcpy(icls, atoms[cord - 0x80]);
        uint32_t ititle = p, tord;
        char tbuf[8];
        if (rd16(p) == 0xFFFF)
            p = sz_or_ord(p, tbuf, sizeof tbuf, &tord), ititle = 0;
        else
            p += 2 * (wlen(p) + 1);
        p += 2 + rd16(p); /* creation data */
        UiWnd* c = ui_new();
        if (!c)
            break;
        c->kind = kind_of(icls);
        c->parent = dh;
        c->id = iid;
        c->style = ist;
        c->exstyle = iex;
        c->hinst = hinst;
        c->tid = d->tid;
        c->wndproc = proc_for(c->kind);
        c->visible = (ist & WS_VISIBLE) != 0;
        c->enabled = !(ist & WS_DISABLED);
        c->x = DX(ix), c->y = DY(iy), c->w = DX(icx), c->h = DY(icy);
        if (ititle)
            set_text(c, ititle);
        if (c->kind == UK_OTHER)
            rt_log("[ui] dialog control class %s (id %u) has no view of its own\n", icls, iid);
        if (!first && (ist & 0x00010000u)) /* WS_TABSTOP */
            first = c->hwnd;
        uint32_t chw = c->hwnd;
        UiWnd snap = *c;
        on_main(^{ UiWnd cc = snap; make_view(&cc); g_ui[(chw - UI_BASE) / UI_STRIDE].view = cc.view; });
    }
#undef DX
#undef DY
    if (!(d = ui(dh)))
        return 0;
    if (style & DS_SETFONT)
        ui_send(d, WM_SETFONT, 0, 0);
    if (!(d = ui(dh)))
        return 0;
    if (ui_send(d, WM_INITDIALOG, first, param) && first)
        g_focus = first;
    if ((d = ui(dh)) && (style & WS_VISIBLE))
    {
        d->visible = 1;
        ui_sync(d);
    }
    return dh;
}

/* --- shims --------------------------------------------------------------------------------------------- */
/* the shim each override replaces, for windows that are not ours */
typedef struct { const char* name; Shim fn; } Prev;
static Prev g_prev[64];
static int g_nprev;

/* A call for a window that is not ours goes to the shim below; one of ours that is gone fails as
 * on Windows (the program may still call on it after DestroyWindow, inside its own handler). */
static void forward_n(Guest* g, const char* name, unsigned nargs)
{
    uint32_t h = ARG(0);
    Shim fn = NULL;
    for (int i = 0; i < g_nprev; ++i)
        if (!strcmp(g_prev[i].name, name))
            fn = g_prev[i].fn;
    if (fn && !(h >= UI_BASE && h < UI_BASE + UI_MAX * UI_STRIDE))
    {
        fn(g);
        return;
    }
    RET(0, nargs);
}

/* CreateDialogParamW(hinst, template, parent, dlgproc, param) */
static void sh_CreateDialogParamW(Guest* g)
{
    uint32_t hinst = ARG(0) ? ARG(0) : rt_image_base;
    uint32_t res = pe_find_resource(hinst, 5, ARG(1), 1);
    uint32_t tpl = res ? hinst + rd32(res) : 0;
    if (!tpl)
    {
        rt_log("[ui] CreateDialogParamW: no DIALOG %#x\n", ARG(1));
        RET(0, 5);
    }
    RET(create_dialog(hinst, tpl, ARG(2), ARG(3), ARG(4)), 5);
}

/* CreateDialogIndirectParamW(hinst, template, parent, dlgproc, param) */
static void sh_CreateDialogIndirectParamW(Guest* g)
{
    RET(create_dialog(ARG(0) ? ARG(0) : rt_image_base, ARG(1), ARG(2), ARG(3), ARG(4)), 5);
}

static void sh_GetDlgItem(Guest* g)
{
    for (int i = 0; i < UI_MAX; ++i)
        if (g_ui[i].used && g_ui[i].parent == ARG(0) && g_ui[i].id == ARG(1))
            RET(g_ui[i].hwnd, 2);
    RET(0, 2);
}

static void sh_GetParent(Guest* g)
{
    UiWnd* w = ui(ARG(0));
    RET(w ? (w->parent ? w->parent : w->owner) : 0, 1);
}

/* GetWindow(hwnd, cmd): siblings in creation order, the first child, the owner */
static void sh_GetWindow(Guest* g)
{
    UiWnd* w = ui(ARG(0));
    uint32_t cmd = ARG(1);
    if (!w)
        RET(0, 2);
    if (cmd == 4) /* GW_OWNER */
        RET(w->owner, 2);
    uint32_t parent = cmd == 5 ? w->hwnd : w->parent;
    UiWnd* best = NULL;
    for (int i = 0; i < UI_MAX; ++i)
    {
        UiWnd* c = &g_ui[i];
        if (!c->used || c->parent != parent || (cmd != 5 && !parent && c->kind != UK_DIALOG))
            continue;
        int better;
        switch (cmd)
        {
        case 0: /* GW_HWNDFIRST */
        case 5: /* GW_CHILD */
            better = !best || c->order < best->order;
            break;
        case 1: /* GW_HWNDLAST */
            better = !best || c->order > best->order;
            break;
        case 2: /* GW_HWNDNEXT */
            better = c->order > w->order && (!best || c->order < best->order);
            break;
        case 3: /* GW_HWNDPREV */
            better = c->order < w->order && (!best || c->order > best->order);
            break;
        default:
            better = 0;
        }
        if (better)
            best = c;
    }
    RET(best ? best->hwnd : 0, 2);
}

static uint32_t get_long(UiWnd* w, int32_t idx)
{
    switch (idx)
    {
    case -4: return w->wndproc;
    case -6: return w->hinst;
    case -8: return w->owner;
    case -12: return w->id;
    case -16: return w->style | (w->visible ? WS_VISIBLE : 0) | (w->enabled ? 0 : WS_DISABLED);
    case -20: return w->exstyle;
    case -21: return w->userdata;
    }
    if (w->kind == UK_DIALOG)
        switch (idx)
        {
        case 0: return w->msgresult;
        case 4: return w->dlgproc;
        case 8: return w->dlguser;
        }
    return 0;
}

static void sh_GetWindowLongW(Guest* g)
{
    UiWnd* w = ui(ARG(0));
    if (!w)
    {
        forward_n(g, "GetWindowLongW", 2);
        return;
    }
    RET(get_long(w, (int32_t)ARG(1)), 2);
}

static void sh_SetWindowLongW(Guest* g)
{
    UiWnd* w = ui(ARG(0));
    if (!w)
    {
        forward_n(g, "SetWindowLongW", 3);
        return;
    }
    int32_t idx = (int32_t)ARG(1);
    uint32_t v = ARG(2), old = get_long(w, idx);
    switch (idx)
    {
    case -4: w->wndproc = v; break;
    case -6: w->hinst = v; break;
    case -8: w->owner = v; break;
    case -12: w->id = v; break;
    case -16: w->style = v & ~WS_VISIBLE; break;
    case -20: w->exstyle = v; break;
    case -21: w->userdata = v; break;
    default:
        if (w->kind == UK_DIALOG && idx == 0)
            w->msgresult = v;
        else if (w->kind == UK_DIALOG && idx == 4)
            w->dlgproc = v;
        else if (w->kind == UK_DIALOG && idx == 8)
            w->dlguser = v;
    }
    RET(old, 3);
}

static void sh_SendMessageW(Guest* g)
{
    UiWnd* w = ui(ARG(0));
    if (!w)
    {
        forward_n(g, "SendMessageW", 4);
        return;
    }
    RET(ui_send(w, ARG(1), ARG(2), ARG(3)), 4);
}

static void sh_SendDlgItemMessageW(Guest* g)
{
    for (int i = 0; i < UI_MAX; ++i)
        if (g_ui[i].used && g_ui[i].parent == ARG(0) && g_ui[i].id == ARG(1))
            RET(ui_send(&g_ui[i], ARG(2), ARG(3), ARG(4)), 5);
    RET(0, 5);
}

static void sh_PostMessageW(Guest* g)
{
    UiWnd* w = ui(ARG(0));
    if (!w)
    {
        forward_n(g, "PostMessageW", 4);
        return;
    }
    RET(user32_post(w->tid, w->hwnd, ARG(1), ARG(2), ARG(3)), 4);
}

/* --- timers: WM_TIMER for our windows (and thread timers with a procedure) ---------------------- */

static int timers_ours(uint32_t hwnd) { return ui(hwnd) != NULL; }

static void fire_timers(uint32_t tid)
{
    uint64_t now = rt_monotonic_ns();
    for (int i = 0; i < 32; ++i)
    {
        Timer* t = &g_timers[i];
        if (!t->tid || t->tid != tid || now < t->due_ns)
            continue;
        t->due_ns = now + t->period_ns;
        /* one WM_TIMER at a time in the queue, as Windows coalesces them */
        uint32_t probe = gheap_alloc(28, 1);
        int pending = user32_take(tid, t->hwnd, WM_TIMER, WM_TIMER, 0, probe) && rd32(probe + 8) == t->id;
        gheap_free(probe);
        if (!pending)
            user32_post(tid, t->hwnd, WM_TIMER, t->id, t->proc);
    }
}

/* SetTimer(hwnd, id, ms, proc) */
static void sh_SetTimer(Guest* g)
{
    uint32_t hwnd = ARG(0), id = ARG(1), ms = ARG(2), proc = ARG(3);
    if (hwnd && !timers_ours(hwnd))
    {
        forward_n(g, "SetTimer", 4);
        return;
    }
    Timer* free_slot = NULL;
    for (int i = 0; i < 32; ++i)
    {
        Timer* t = &g_timers[i];
        if (t->tid && t->hwnd == hwnd && hwnd && t->id == id)
        {
            free_slot = t;
            break;
        }
        if (!t->tid && !free_slot)
            free_slot = t;
    }
    if (!free_slot)
        RET(0, 4);
    if (!hwnd)
        id = 0x7000u + (uint32_t)(free_slot - g_timers);
    free_slot->tid = hwnd ? ui(hwnd)->tid : gt_self()->tid;
    free_slot->hwnd = hwnd;
    free_slot->id = id;
    free_slot->proc = proc;
    free_slot->period_ns = (uint64_t)(ms < 10 ? 10 : ms) * 1000000u;
    free_slot->due_ns = rt_monotonic_ns() + free_slot->period_ns;
    RET(hwnd ? 1 : id, 4);
}

static void sh_KillTimer(Guest* g)
{
    for (int i = 0; i < 32; ++i)
        if (g_timers[i].tid && g_timers[i].hwnd == ARG(0) && g_timers[i].id == ARG(1))
        {
            memset(&g_timers[i], 0, sizeof g_timers[i]);
            RET(1, 2);
        }
    if (ARG(0) && !timers_ours(ARG(0)))
    {
        forward_n(g, "KillTimer", 2);
        return;
    }
    RET(0, 2);
}

/* --- the message loop ---------------------------------------------------------------------------- */
/* GetMessageW(msg, hwnd, min, max): waits; 0 for WM_QUIT */
static void sh_GetMessageW(Guest* g)
{
    uint32_t tid = gt_self()->tid, out = ARG(0);
    for (;;)
    {
        fire_timers(tid);
        user32_pump();
        if (user32_take(tid, ARG(1), ARG(2), ARG(3), 1, out))
            break;
        gt_unlock();
        plat_sleep_ms(2);
        gt_lock();
    }
    RET(rd32(out + 4) != WM_QUIT, 4);
}

/* PeekMessageW(msg, hwnd, min, max, remove) */
static void sh_PeekMessageW(Guest* g)
{
    uint32_t tid = gt_self()->tid;
    fire_timers(tid);
    user32_pump();
    RET(user32_take(tid, ARG(1), ARG(2), ARG(3), (ARG(4) & 1) != 0, ARG(0)), 5);
}

static void sh_DispatchMessageW(Guest* g)
{
    uint32_t p = ARG(0), hwnd = rd32(p), msg = rd32(p + 4), wp = rd32(p + 8), lp = rd32(p + 12);
    if (msg == WM_TIMER && lp) /* the timer's procedure: (hwnd, WM_TIMER, id, time) */
    {
        uint32_t a[4] = { hwnd, WM_TIMER, wp, rd32(p + 16) };
        guest_call(lp, 4, a);
        RET(0, 1);
    }
    UiWnd* w = ui(hwnd);
    if (!w)
    {
        if (hwnd >= UI_BASE && hwnd < UI_BASE + UI_MAX * UI_STRIDE)
            RET(0, 1);
        for (int i = 0; i < g_nprev; ++i)
            if (!strcmp(g_prev[i].name, "DispatchMessageW") && g_prev[i].fn)
            {
                g_prev[i].fn(g);
                return;
            }
        RET(0, 1);
    }
    RET(ui_send(w, msg, wp, lp), 1);
}

/* keyboard navigation inside the dialog is AppKit's: nothing for the program to do */
static void sh_IsDialogMessageW(Guest* g) { RET(0, 2); }

static void sh_DefWindowProcW(Guest* g)
{
    UiWnd* w = ui(ARG(0));
    if (!w)
    {
        forward_n(g, "DefWindowProcW", 4);
        return;
    }
    RET(def_window(w, ARG(1), ARG(2), ARG(3)), 4);
}

static void sh_DefDlgProcW(Guest* g)
{
    UiWnd* w = ui(ARG(0));
    RET(w ? def_dialog(w, ARG(1), ARG(2), ARG(3)) : 0, 4);
}

static void sh_EndDialog(Guest* g)
{
    UiWnd* w = ui(ARG(0));
    if (w)
        destroy(w);
    RET(w != NULL, 2);
}

/* --- geometry, visibility, text ------------------------------------------------------------------- */
/* the screen position of hwnd's client area */
static void client_origin(UiWnd* w, int* x, int* y)
{
    *x = *y = 0;
    while (w)
    {
        *x += w->x, *y += w->y;
        w = w->kind == UK_DIALOG ? NULL : ui(w->parent);
    }
}

static void write_rect(uint32_t p, int l, int t, int r, int b)
{
    wr32(p, (uint32_t)l), wr32(p + 4, (uint32_t)t), wr32(p + 8, (uint32_t)r), wr32(p + 12, (uint32_t)b);
}

static void sh_GetWindowRect(Guest* g)
{
    UiWnd* w = ui(ARG(0));
    if (!w)
    {
        if (!ARG(0)) /* the desktop */
        {
            NSRect f = [[NSScreen screens][0] frame];
            write_rect(ARG(1), 0, 0, (int)f.size.width, (int)f.size.height);
            RET(1, 2);
        }
        forward_n(g, "GetWindowRect", 2);
        return;
    }
    int x, y;
    client_origin(w, &x, &y);
    int top = w->kind == UK_DIALOG ? CAPTION_H : 0;
    write_rect(ARG(1), x, y - top, x + w->w, y + w->h);
    RET(1, 2);
}

static void sh_GetClientRect(Guest* g)
{
    UiWnd* w = ui(ARG(0));
    if (!w)
    {
        forward_n(g, "GetClientRect", 2);
        return;
    }
    write_rect(ARG(1), 0, 0, w->w, w->h);
    RET(1, 2);
}

/* MapWindowPoints(from, to, points, n): the low word of the result is the x shift, the high y */
static void sh_MapWindowPoints(Guest* g)
{
    int fx = 0, fy = 0, tx = 0, ty = 0;
    UiWnd* f = ui(ARG(0));
    UiWnd* t = ui(ARG(1));
    if (f)
        client_origin(f, &fx, &fy);
    if (t)
        client_origin(t, &tx, &ty);
    int dx = fx - tx, dy = fy - ty;
    for (uint32_t i = 0; i < ARG(3); ++i)
    {
        uint32_t p = ARG(2) + 8 * i;
        wr32(p, rd32(p) + (uint32_t)dx);
        wr32(p + 4, rd32(p + 4) + (uint32_t)dy);
    }
    RET(((uint32_t)(uint16_t)dy << 16) | (uint16_t)dx, 4);
}

static void sh_ClientToScreen(Guest* g)
{
    UiWnd* w = ui(ARG(0));
    if (!w)
    {
        forward_n(g, "ClientToScreen", 2);
        return;
    }
    int x, y;
    client_origin(w, &x, &y);
    wr32(ARG(1), rd32(ARG(1)) + (uint32_t)x);
    wr32(ARG(1) + 4, rd32(ARG(1) + 4) + (uint32_t)y);
    RET(1, 2);
}

static void sh_ScreenToClient(Guest* g)
{
    UiWnd* w = ui(ARG(0));
    if (!w)
    {
        forward_n(g, "ScreenToClient", 2);
        return;
    }
    int x, y;
    client_origin(w, &x, &y);
    wr32(ARG(1), rd32(ARG(1)) - (uint32_t)x);
    wr32(ARG(1) + 4, rd32(ARG(1) + 4) - (uint32_t)y);
    RET(1, 2);
}

/* SetWindowPos(hwnd, after, x, y, cx, cy, flags): a dialog's x, y, cx, cy are its window rect */
static void sh_SetWindowPos(Guest* g)
{
    UiWnd* w = ui(ARG(0));
    if (!w)
    {
        forward_n(g, "SetWindowPos", 7);
        return;
    }
    uint32_t flags = ARG(6);
    int top = w->kind == UK_DIALOG ? CAPTION_H : 0;
    if (!(flags & 0x0002u)) /* SWP_NOMOVE */
        w->x = (int32_t)ARG(2), w->y = (int32_t)ARG(3) + top;
    if (!(flags & 0x0001u)) /* SWP_NOSIZE */
        w->w = (int32_t)ARG(4), w->h = (int32_t)ARG(5) - top;
    if (flags & 0x0040u) /* SWP_SHOWWINDOW */
        w->visible = 1;
    if (flags & 0x0080u) /* SWP_HIDEWINDOW */
        w->visible = 0;
    ui_sync(w);
    RET(1, 7);
}

static void sh_MoveWindow(Guest* g)
{
    UiWnd* w = ui(ARG(0));
    if (!w)
    {
        forward_n(g, "MoveWindow", 6);
        return;
    }
    int top = w->kind == UK_DIALOG ? CAPTION_H : 0;
    w->x = (int32_t)ARG(1), w->y = (int32_t)ARG(2) + top, w->w = (int32_t)ARG(3), w->h = (int32_t)ARG(4) - top;
    ui_sync(w);
    RET(1, 6);
}

static void sh_ShowWindow(Guest* g)
{
    UiWnd* w = ui(ARG(0));
    if (!w)
    {
        forward_n(g, "ShowWindow", 2);
        return;
    }
    int was = w->visible;
    w->visible = ARG(1) != 0; /* SW_HIDE */
    if (w->visible != was)
    {
        ui_sync(w);
        ui_send(w, WM_SHOWWINDOW, (uint32_t)w->visible, 0);
    }
    RET(was, 2);
}

static void sh_DestroyWindow(Guest* g)
{
    UiWnd* w = ui(ARG(0));
    if (!w)
    {
        forward_n(g, "DestroyWindow", 1);
        return;
    }
    destroy(w);
    RET(1, 1);
}

static void sh_IsWindow(Guest* g)
{
    if (ui(ARG(0)))
        RET(1, 1);
    forward_n(g, "IsWindow", 1);
}

static void sh_IsWindowVisible(Guest* g)
{
    UiWnd* w = ui(ARG(0));
    if (!w)
    {
        forward_n(g, "IsWindowVisible", 1);
        return;
    }
    RET(w->visible, 1);
}

static void sh_EnableWindow(Guest* g)
{
    UiWnd* w = ui(ARG(0));
    if (!w)
        RET(0, 2);
    int was_disabled = !w->enabled;
    w->enabled = ARG(1) != 0;
    ui_sync(w);
    ui_send(w, WM_ENABLE, (uint32_t)w->enabled, 0);
    RET(was_disabled, 2);
}

static void sh_IsWindowEnabled(Guest* g)
{
    UiWnd* w = ui(ARG(0));
    RET(w ? w->enabled : 0, 1);
}

static void sh_SetFocus(Guest* g)
{
    if (!ui(ARG(0)))
    {
        forward_n(g, "SetFocus", 1);
        return;
    }
    uint32_t old = g_focus;
    g_focus = ARG(0);
    RET(old, 1);
}

static void sh_SetWindowTextW(Guest* g)
{
    UiWnd* w = ui(ARG(0));
    if (!w)
    {
        forward_n(g, "SetWindowTextW", 2);
        return;
    }
    RET(ui_send(w, WM_SETTEXT, 0, ARG(1)), 2);
}

static void sh_GetWindowTextW(Guest* g)
{
    UiWnd* w = ui(ARG(0));
    RET(w ? ui_send(w, WM_GETTEXT, ARG(2), ARG(1)) : 0, 3);
}

static void sh_GetWindowTextLengthW(Guest* g)
{
    UiWnd* w = ui(ARG(0));
    RET(w ? ui_send(w, WM_GETTEXTLENGTH, 0, 0) : 0, 1);
}

static void sh_SetDlgItemTextW(Guest* g)
{
    for (int i = 0; i < UI_MAX; ++i)
        if (g_ui[i].used && g_ui[i].parent == ARG(0) && g_ui[i].id == ARG(1))
            RET(ui_send(&g_ui[i], WM_SETTEXT, 0, ARG(2)), 3);
    RET(0, 3);
}

/* --- message boxes ------------------------------------------------------------------------------- */
static NSString* nsstring_w(uint32_t w)
{
    uint32_t n = wlen(w);
    unichar* b = malloc(2 * (n + 1));
    for (uint32_t i = 0; i < n; ++i)
        b[i] = rd16(w + 2 * i);
    NSString* s = [NSString stringWithCharacters:b length:n];
    free(b);
    return s;
}

/* MessageBoxW(hwnd, text, caption, type): an NSAlert, modal on the main thread while the guest
 * waits without the lock */
static void sh_MessageBoxW(Guest* g)
{
    NSString* text = [nsstring_w(ARG(1)) retain];
    NSString* title = [nsstring_w(ARG(2)) retain];
    uint32_t type = ARG(3) & 0xF;
    rt_log("[ui] MessageBox \"%s\": %s\n", [title UTF8String], [text UTF8String]);
    /* the buttons and the IDs they return, by MB_ type */
    static const char* labels[][3] = { { "OK" }, { "OK", "Cancel" }, { "Abort", "Retry", "Ignore" },
        { "Yes", "No", "Cancel" }, { "Yes", "No" }, { "Retry", "Cancel" } };
    static const uint32_t ids[][3] = { { 1 }, { 1, 2 }, { 3, 4, 5 }, { 6, 7, 2 }, { 6, 7 }, { 4, 2 } };
    if (type > 5)
        type = 0;
    __block NSInteger which = 0;
    gt_unlock();
    on_main(^{
        NSAlert* a = [[NSAlert alloc] init];
        [a setMessageText:title];
        [a setInformativeText:text];
        for (int i = 0; i < 3 && labels[type][i]; ++i)
            [a addButtonWithTitle:[NSString stringWithUTF8String:labels[type][i]]];
        [NSApp activateIgnoringOtherApps:YES];
        which = [a runModal] - NSAlertFirstButtonReturn;
        [a release];
    });
    gt_lock();
    [text release];
    [title release];
    RET(which >= 0 && which < 3 ? ids[type][which] : 1, 4);
}

/* --- resources and odds ------------------------------------------------------------------------- */
/* LoadStringW(hinst, id, buffer, n): string tables hold 16 counted UTF-16 strings per block; n 0
 * returns a pointer to the resource's own text */
static void sh_LoadStringW(Guest* g)
{
    uint32_t hinst = ARG(0) ? ARG(0) : rt_image_base, id = ARG(1), buf = ARG(2), n = ARG(3);
    uint32_t size = 0, p = pe_resource(hinst, 6, (id >> 4) + 1, &size);
    if (!p)
        RET(0, 4);
    for (uint32_t i = 0; i < (id & 15); ++i)
        p += 2 + 2 * rd16(p);
    uint32_t len = rd16(p);
    if (!n)
    {
        wr32(buf, p + 2);
        RET(len, 4);
    }
    if (len >= n)
        len = n - 1;
    for (uint32_t i = 0; i < len; ++i)
        wr16(buf + 2 * i, rd16(p + 2 + 2 * i));
    wr16(buf + 2 * len, 0);
    RET(len, 4);
}

static void sh_LoadImageW(Guest* g) { RET(0x00020001u, 6); } /* icons: the app's own stands in */

static void sh_CharNextW(Guest* g) { RET(rd16(ARG(0)) ? ARG(0) + 2 : ARG(0), 1); }

/* SystemParametersInfoW(action, param, pv, winini): the work area; the rest as the A call */
static void sh_SystemParametersInfoW(Guest* g)
{
    if (ARG(0) == 0x30) /* SPI_GETWORKAREA, in top-left screen coordinates */
    {
        NSScreen* s = [NSScreen screens][0];
        NSRect v = [s visibleFrame], f = [s frame];
        int top = (int)(NSMaxY(f) - NSMaxY(v));
        write_rect(ARG(2), (int)v.origin.x, top, (int)NSMaxX(v), top + (int)v.size.height);
        RET(1, 4);
    }
    for (int i = 0; i < g_nprev; ++i)
        if (!strcmp(g_prev[i].name, "SystemParametersInfoA") && g_prev[i].fn)
        {
            g_prev[i].fn(g);
            return;
        }
    RET(0, 4);
}

static void sh_GetDesktopWindow(Guest* g) { RET(0, 0); }

const ShimDef ui_shims[] = {
    { "user32.dll", "CreateDialogParamW", sh_CreateDialogParamW },
    { "user32.dll", "CreateDialogIndirectParamW", sh_CreateDialogIndirectParamW },
    { "user32.dll", "GetDlgItem", sh_GetDlgItem },
    { "user32.dll", "GetParent", sh_GetParent },
    { "user32.dll", "GetWindow", sh_GetWindow },
    { "user32.dll", "GetWindowLongW", sh_GetWindowLongW },
    { "user32.dll", "SetWindowLongW", sh_SetWindowLongW },
    { "user32.dll", "SendMessageW", sh_SendMessageW },
    { "user32.dll", "SendDlgItemMessageW", sh_SendDlgItemMessageW },
    { "user32.dll", "PostMessageW", sh_PostMessageW },
    { "user32.dll", "SetTimer", sh_SetTimer },
    { "user32.dll", "KillTimer", sh_KillTimer },
    { "user32.dll", "GetMessageW", sh_GetMessageW },
    { "user32.dll", "PeekMessageW", sh_PeekMessageW },
    { "user32.dll", "DispatchMessageW", sh_DispatchMessageW },
    { "user32.dll", "IsDialogMessageW", sh_IsDialogMessageW },
    { "user32.dll", "IsDialogMessage", sh_IsDialogMessageW },
    { "user32.dll", "DefWindowProcW", sh_DefWindowProcW },
    { "user32.dll", "DefDlgProcW", sh_DefDlgProcW },
    { "user32.dll", "EndDialog", sh_EndDialog },
    { "user32.dll", "GetWindowRect", sh_GetWindowRect },
    { "user32.dll", "GetClientRect", sh_GetClientRect },
    { "user32.dll", "MapWindowPoints", sh_MapWindowPoints },
    { "user32.dll", "ClientToScreen", sh_ClientToScreen },
    { "user32.dll", "ScreenToClient", sh_ScreenToClient },
    { "user32.dll", "SetWindowPos", sh_SetWindowPos },
    { "user32.dll", "MoveWindow", sh_MoveWindow },
    { "user32.dll", "ShowWindow", sh_ShowWindow },
    { "user32.dll", "DestroyWindow", sh_DestroyWindow },
    { "user32.dll", "IsWindow", sh_IsWindow },
    { "user32.dll", "IsWindowVisible", sh_IsWindowVisible },
    { "user32.dll", "EnableWindow", sh_EnableWindow },
    { "user32.dll", "IsWindowEnabled", sh_IsWindowEnabled },
    { "user32.dll", "SetFocus", sh_SetFocus },
    { "user32.dll", "SetWindowTextW", sh_SetWindowTextW },
    { "user32.dll", "GetWindowTextW", sh_GetWindowTextW },
    { "user32.dll", "GetWindowTextLengthW", sh_GetWindowTextLengthW },
    { "user32.dll", "SetDlgItemTextW", sh_SetDlgItemTextW },
    { "user32.dll", "MessageBoxW", sh_MessageBoxW },
    { "user32.dll", "LoadStringW", sh_LoadStringW },
    { "user32.dll", "LoadImageW", sh_LoadImageW },
    { "user32.dll", "CharNextW", sh_CharNextW },
    { "user32.dll", "SystemParametersInfoW", sh_SystemParametersInfoW },
    { "user32.dll", "GetDesktopWindow", sh_GetDesktopWindow },
    /* the built-in window procedures, as guest-callable addresses */
    { "user32.dll", "ui_dialog_proc", sh_ui_dialog_proc },
    { "user32.dll", "ui_control_proc", sh_ui_control_proc },
    { "user32.dll", "ui_progress_proc", sh_ui_progress_proc },
    { "user32.dll", "ui_animate_proc", sh_ui_animate_proc },
    { NULL, NULL, NULL },
};

void ui_appkit_init(void)
{
    /* the shims ours replace, for windows that are not ours (registered before this runs) */
    for (const ShimDef* d = ui_shims; d->name && g_nprev < 64; ++d)
    {
        g_prev[g_nprev].name = d->name;
        g_prev[g_nprev].fn = thunk_shim(d->dll, d->name);
        g_nprev++;
    }
    g_prev[g_nprev].name = "SystemParametersInfoA";
    g_prev[g_nprev++].fn = thunk_shim("user32.dll", "SystemParametersInfoA");
    thunk_override(ui_shims);
}

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
#define WM_CREATE 0x0001
#define WM_NCCREATE 0x0081
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
    int suppressed; /* a control the host leaves out (ui_suppress_controls): never shown */
    uint16_t text[UI_TEXT];
    char cls[48];      /* the class name GetClassName reports */
    int bx, by;        /* a dialog's base units (MapDialogRect) */
    struct { uint32_t atom, value; } props[8]; /* SetProp, by atom */
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
/* An Edit menu: AppKit sends Cmd-X/C/V/A/Z to text fields (the login page's) through the main
 * menu's items, and SDL's menu has none (main thread, once) */
static void add_edit_menu(void)
{
    NSMenu* bar = [NSApp mainMenu];
    if (!bar)
    {
        bar = [[[NSMenu alloc] init] autorelease];
        [NSApp setMainMenu:bar];
    }
    if ([bar itemWithTitle:@"Edit"])
        return;
    NSMenu* edit = [[[NSMenu alloc] initWithTitle:@"Edit"] autorelease];
    [edit addItemWithTitle:@"Undo" action:@selector(undo:) keyEquivalent:@"z"];
    NSMenuItem* redo = [edit addItemWithTitle:@"Redo" action:@selector(redo:) keyEquivalent:@"z"];
    [redo setKeyEquivalentModifierMask:NSEventModifierFlagCommand | NSEventModifierFlagShift];
    [edit addItem:[NSMenuItem separatorItem]];
    [edit addItemWithTitle:@"Cut" action:@selector(cut:) keyEquivalent:@"x"];
    [edit addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"];
    [edit addItemWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@"v"];
    [edit addItemWithTitle:@"Select All" action:@selector(selectAll:) keyEquivalent:@"a"];
    NSMenuItem* item = [[[NSMenuItem alloc] initWithTitle:@"Edit" action:NULL keyEquivalent:@""] autorelease];
    [item setSubmenu:edit];
    [bar addItem:item];
}

static void make_view(UiWnd* w)
{
    if (!g_target)
    {
        g_target = [[UiTarget alloc] init];
        add_edit_menu();
    }
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
    [v setHidden:!w->visible || w->suppressed];
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
            if (getenv("XIV_UI_LOG")) /* for screencapture -l: this window alone */
                rt_log("[ui] %05x is window number %ld\n", w->hwnd, (long)[win windowNumber]);
        }
        else if (!w->visible && [win isVisible])
            [win orderOut:nil];
        return;
    }
    NSView* v = (NSView*)w->view;
    NSRect r = NSMakeRect(w->x, w->y, w->w, w->h);
    [v setHidden:!w->visible || w->suppressed];
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
    case WM_NCCREATE:
        return 1;
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
    case 0x0464: /* ACM_OPENA: the AVI is the spinner here */
    case 0x0467: /* ACM_OPENW */
        return 1;
    case 0x0465: /* ACM_PLAY(repeat, from | to << 16): -1 repeats loop; anything else shows a frame */
    case 0x0466: /* ACM_STOP */
    {
        int play = msg == 0x0465 && wp == 0xFFFFFFFFu;
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

/* controls the host leaves out of every dialog, by id (tests/xivexe.c: ffxivlogin's Cancel and
 * Update Page buttons; its window's close button still cancels) */
static uint32_t g_suppressed[8];
static int g_nsuppressed;

void ui_suppress_controls(const uint32_t* ids, int n)
{
    for (int i = 0; i < n && g_nsuppressed < 8; ++i)
        g_suppressed[g_nsuppressed++] = ids[i];
}

/* --- creation hooks (SetWindowsHookEx WH_CBT): MFC attaches its CWnd objects to new windows from
 * HCBT_CREATEWND, subclassing them before their first message ------------------------------------ */
typedef struct { uint32_t id, type, proc, tid; } Hook;
static Hook g_hooks[16];
static uint32_t g_next_hook = 0x00070001u;

/* the window exists; hooks see it, then WM_NCCREATE and WM_CREATE with its CREATESTRUCTW */
static void created(UiWnd* w, uint32_t params)
{
    uint32_t h = w->hwnd;
    uint32_t cls = gheap_alloc(2 * (uint32_t)(strlen(w->cls) + 1), 1);
    for (size_t i = 0; w->cls[i]; ++i)
        wr16(cls + 2 * (uint32_t)i, (uint8_t)w->cls[i]);
    uint32_t cs = gheap_alloc(48 + 8, 1), cbt = cs + 48;
    wr32(cs, params);
    wr32(cs + 4, w->hinst);
    wr32(cs + 8, w->parent ? w->id : 0);
    wr32(cs + 12, w->parent ? w->parent : w->owner);
    wr32(cs + 16, (uint32_t)w->h), wr32(cs + 20, (uint32_t)w->w), wr32(cs + 24, (uint32_t)w->y), wr32(cs + 28, (uint32_t)w->x);
    wr32(cs + 32, w->style | (w->visible ? WS_VISIBLE : 0));
    wr32(cs + 36, 0);
    wr32(cs + 40, cls);
    wr32(cs + 44, w->exstyle);
    wr32(cbt, cs);
    wr32(cbt + 4, 0);
    uint32_t tid = gt_self()->tid;
    for (int i = 0; i < 16; ++i)
        if (g_hooks[i].id && g_hooks[i].type == 5 && g_hooks[i].tid == tid) /* WH_CBT */
        {
            uint32_t a[3] = { 3 /* HCBT_CREATEWND */, h, cbt };
            guest_call(g_hooks[i].proc, 3, a);
        }
    if ((w = ui(h)))
        ui_send(w, WM_NCCREATE, 0, cs);
    if ((w = ui(h)))
        ui_send(w, WM_CREATE, 0, cs);
    gheap_free(cs);
    gheap_free(cls);
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
    strcpy(d->cls, "#32770");
    d->bx = bx, d->by = by;
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
    created(d, param);
    if (!(d = ui(dh)))
        return 0;

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
        snprintf(c->cls, sizeof c->cls, "%s", icls);
        c->parent = dh;
        c->id = iid;
        c->style = ist;
        c->exstyle = iex;
        c->hinst = hinst;
        c->tid = d->tid;
        c->wndproc = proc_for(c->kind);
        c->visible = (ist & WS_VISIBLE) != 0;
        for (int k = 0; k < g_nsuppressed; ++k)
            if (g_suppressed[k] == iid)
                c->suppressed = 1;
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
        created(c, 0);
        if (!(d = ui(dh)))
            return 0;
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
static Prev g_prev[256];
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
    RET(w->visible && !w->suppressed, 1);
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
    if (getenv("XIV_UI_NOMODAL")) /* unattended runs: the first button, without waiting */
    {
        [text release];
        [title release];
        RET(ids[type][0], 4);
    }
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

/* one monitor: the main screen */
static void sh_MonitorFromWindow(Guest* g) { RET(0x00100001u, 2); }
static void sh_MonitorFromRect(Guest* g) { RET(0x00100001u, 2); }
static void sh_MonitorFromPoint(Guest* g) { RET(0x00100001u, 3); }

/* GetMonitorInfoW/A(monitor, MONITORINFO*): rcMonitor at 4, rcWork at 20, dwFlags (primary) at 36 */
static void sh_GetMonitorInfoW(Guest* g)
{
    uint32_t p = ARG(1);
    NSScreen* s = [NSScreen screens][0];
    NSRect v = [s visibleFrame], f = [s frame];
    int top = (int)(NSMaxY(f) - NSMaxY(v));
    write_rect(p + 4, 0, 0, (int)f.size.width, (int)f.size.height);
    write_rect(p + 20, (int)v.origin.x, top, (int)NSMaxX(v), top + (int)v.size.height);
    wr32(p + 36, 1);
    RET(1, 2);
}


/* --- classes and generic windows (MFC registers its own classes and checks the system ones) ------ */
typedef struct { char name[48]; uint32_t proc, style, extra; } UiClass;
static UiClass g_classes[32];

static void guest_name(uint32_t p, char* out, size_t cap)
{
    if (p < 0x10000u) /* an atom */
        snprintf(out, cap, "#%u", p);
    else
    {
        size_t k = 0;
        for (uint16_t c; (c = rd16(p)) && k + 1 < cap; p += 2)
            out[k++] = c > 0x7F ? '?' : (char)c;
        out[k] = 0;
    }
}

static UiClass* find_class(const char* name)
{
    for (int i = 0; i < 32; ++i)
        if (g_classes[i].name[0] && !strcasecmp(g_classes[i].name, name))
            return &g_classes[i];
    return NULL;
}

static uint32_t register_class(uint32_t namep, uint32_t proc, uint32_t style, uint32_t extra)
{
    char name[48];
    guest_name(namep, name, sizeof name);
    UiClass* c = find_class(name);
    for (int i = 0; !c && i < 32; ++i)
        if (!g_classes[i].name[0])
            c = &g_classes[i];
    if (!c)
        return 0;
    snprintf(c->name, sizeof c->name, "%s", name);
    c->proc = proc, c->style = style, c->extra = extra;
    /* and with user32.c, whose windows the top-level ones are (ui_create_window) */
    uint32_t wc = gheap_alloc(40, 1), gname = gheap_strdup(name);
    wr32(wc, style), wr32(wc + 4, proc), wr32(wc + 36, gname);
    guest_call(thunk_for("user32.dll", "RegisterClassA"), 1, &wc);
    gheap_free(wc);
    return 0xC100u + (uint32_t)(c - g_classes);
}

/* RegisterClassW(WNDCLASSW*) / RegisterClassExW(WNDCLASSEXW*) */
static void sh_RegisterClassW(Guest* g) { uint32_t w = ARG(0); RET(register_class(rd32(w + 36), rd32(w + 4), rd32(w), rd32(w + 12)), 1); }
static void sh_RegisterClassExW(Guest* g) { uint32_t w = ARG(0); RET(register_class(rd32(w + 40), rd32(w + 8), rd32(w + 4), rd32(w + 16)), 1); }

static void sh_UnregisterClassW(Guest* g)
{
    char name[48];
    guest_name(ARG(0), name, sizeof name);
    UiClass* c = find_class(name);
    if (c)
        c->name[0] = 0;
    RET(c != NULL, 2);
}

/* the window procedure a class name stands for: a registered class, or a system one */
static uint32_t class_proc(const char* name, UiKind* kind)
{
    UiClass* c = find_class(name);
    *kind = kind_of(name);
    if (c)
        return c->proc;
    if (!strcmp(name, "#32770"))
        return *kind = UK_DIALOG, proc_for(UK_DIALOG);
    return *kind != UK_OTHER ? proc_for(*kind) : 0;
}

/* GetClassInfoW(hinst, name, WNDCLASSW*) / GetClassInfoExW(hinst, name, WNDCLASSEXW*) */
static int class_info(uint32_t namep, uint32_t* proc, uint32_t* style)
{
    char name[48];
    guest_name(namep, name, sizeof name);
    UiKind k;
    *proc = class_proc(name, &k);
    UiClass* c = find_class(name);
    *style = c ? c->style : 0;
    return *proc != 0;
}

static void sh_GetClassInfoW(Guest* g)
{
    uint32_t proc, style, w = ARG(2);
    if (!class_info(ARG(1), &proc, &style))
        RET(0, 3);
    memset(GUEST_PTR(w), 0, 40);
    wr32(w, style), wr32(w + 4, proc), wr32(w + 16, ARG(0)), wr32(w + 36, ARG(1));
    RET(1, 3);
}

static void sh_GetClassInfoExW(Guest* g)
{
    uint32_t proc, style, w = ARG(2);
    if (!class_info(ARG(1), &proc, &style))
        RET(0, 3);
    memset(GUEST_PTR(w + 4), 0, 44);
    wr32(w + 4, style), wr32(w + 8, proc), wr32(w + 20, ARG(0)), wr32(w + 40, ARG(1));
    RET(1, 3);
}

static void sh_GetClassLongW(Guest* g) { RET(0, 2); }

static void sh_GetClassNameW(Guest* g)
{
    UiWnd* w = ui(ARG(0));
    uint32_t buf = ARG(1), n = ARG(2);
    if (!w || !n)
        RET(0, 3);
    uint32_t k = 0;
    for (; w->cls[k] && k + 1 < n; ++k)
        wr16(buf + 2 * k, (uint8_t)w->cls[k]);
    wr16(buf + 2 * k, 0);
    RET(k, 3);
}

/* CreateWindowExW's work: child windows get a plain view; a top-level one that is not a dialog
 * gets none (MFC's hidden helpers). Returns the HWND or 0. */
uint32_t ui_create_window(uint32_t exstyle, const char* cls, uint32_t name, uint32_t style, int32_t x, int32_t y,
    int32_t cx, int32_t cy, uint32_t parent, uint32_t menu, uint32_t hinst, uint32_t params)
{
    UiKind kind;
    uint32_t proc = class_proc(cls, &kind);
    if (!proc)
    {
        rt_log("[ui] CreateWindowExW: class %s is not registered\n", cls);
        return 0;
    }
    int child = (style & WS_CHILD) != 0;
    if (!child && kind == UK_OTHER)
    {
        /* a program's own top-level window (ffxivboot's, which Direct3D draws in): user32.c's SDL
         * window, as the game's is */
        uint32_t gcls = gheap_strdup(cls), gname = 0;
        if (name >= 0x10000u)
        {
            char n[256];
            guest_name(name, n, sizeof n);
            gname = gheap_strdup(n);
        }
        uint32_t a[12] = { exstyle, gcls, gname, style, (uint32_t)x, (uint32_t)y, (uint32_t)cx, (uint32_t)cy, parent, menu,
            hinst, params };
        uint32_t h = guest_call(thunk_for("user32.dll", "CreateWindowExA"), 12, a);
        gheap_free(gcls);
        if (gname)
            gheap_free(gname);
        return h;
    }
    UiWnd* w = ui_new();
    if (!w)
        return 0;
    w->kind = kind == UK_DIALOG ? UK_OTHER : kind; /* a #32770 made by hand has no template */
    snprintf(w->cls, sizeof w->cls, "%s", cls);
    w->style = style & ~WS_VISIBLE;
    w->exstyle = exstyle;
    w->parent = child ? parent : 0;
    w->owner = child ? 0 : parent;
    w->id = child ? menu : 0;
    w->hinst = hinst;
    w->tid = gt_self()->tid;
    w->wndproc = proc;
    w->visible = (style & WS_VISIBLE) != 0;
    w->enabled = !(style & WS_DISABLED);
    w->x = x == (int32_t)0x80000000 ? 0 : x, w->y = y == (int32_t)0x80000000 ? 0 : y;
    w->w = cx == (int32_t)0x80000000 ? 0 : cx, w->h = cy == (int32_t)0x80000000 ? 0 : cy;
    if (name >= 0x10000u)
        set_text(w, name);
    uint32_t h = w->hwnd;
    if (child && ui(parent))
    {
        UiWnd snap = *w;
        on_main(^{ UiWnd cc = snap; make_view(&cc); g_ui[(h - UI_BASE) / UI_STRIDE].view = cc.view; });
    }
    created(w, params);
    return ui(h) ? h : 0;
}

static void sh_CreateWindowExW(Guest* g)
{
    char cls[48];
    guest_name(ARG(1), cls, sizeof cls);
    RET(ui_create_window(ARG(0), cls, ARG(2), ARG(3), (int32_t)ARG(4), (int32_t)ARG(5), (int32_t)ARG(6),
            (int32_t)ARG(7), ARG(8), ARG(9), ARG(10), ARG(11)), 12);
}

/* for other host modules (webbrowser.m): a class served by a host window procedure, a window's
 * AppKit view (use it on the main thread), the default window procedure */
void ui_register_class(const char* name, uint32_t proc)
{
    UiClass* c = find_class(name);
    for (int i = 0; !c && i < 32; ++i)
        if (!g_classes[i].name[0])
            c = &g_classes[i];
    if (c)
    {
        snprintf(c->name, sizeof c->name, "%s", name);
        c->proc = proc;
    }
}

void* ui_view(uint32_t hwnd)
{
    UiWnd* w = ui(hwnd);
    return w ? w->view : NULL;
}

uint32_t ui_def_window(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp)
{
    UiWnd* w = ui(hwnd);
    return w ? def_window(w, msg, wp, lp) : 0;
}

uint32_t ui_window_tid(uint32_t hwnd)
{
    UiWnd* w = ui(hwnd);
    return w ? w->tid : 0;
}

/* SetWindowsHookExA/W(type, proc, module, tid): WH_CBT runs at window creation; others are kept */
static void sh_SetWindowsHookExW(Guest* g)
{
    for (int i = 0; i < 16; ++i)
        if (!g_hooks[i].id)
        {
            g_hooks[i].id = g_next_hook++;
            g_hooks[i].type = ARG(0);
            g_hooks[i].proc = ARG(1);
            g_hooks[i].tid = ARG(3) ? ARG(3) : gt_self()->tid;
            RET(g_hooks[i].id, 4);
        }
    RET(0, 4);
}

static void sh_UnhookWindowsHookEx(Guest* g)
{
    for (int i = 0; i < 16; ++i)
        if (g_hooks[i].id == ARG(0))
        {
            memset(&g_hooks[i], 0, sizeof g_hooks[i]);
            RET(1, 1);
        }
    RET(0, 1);
}

static void sh_CallNextHookEx(Guest* g) { RET(0, 4); }

/* CallWindowProcW(proc, hwnd, msg, wp, lp) */
static void sh_CallWindowProcW(Guest* g)
{
    if (!ARG(0))
        RET(0, 5);
    uint32_t a[4] = { ARG(1), ARG(2), ARG(3), ARG(4) };
    RET(guest_call(ARG(0), 4, a), 5);
}

/* --- window relations ------------------------------------------------------------------------------- */
static void sh_GetDlgCtrlID(Guest* g) { UiWnd* w = ui(ARG(0)); RET(w ? w->id : 0, 1); }

static void sh_IsChild(Guest* g)
{
    UiWnd* w = ui(ARG(1));
    while (w && w->parent)
    {
        if (w->parent == ARG(0))
            RET(1, 2);
        w = ui(w->parent);
    }
    RET(0, 2);
}

static void sh_GetTopWindow(Guest* g)
{
    UiWnd* best = NULL;
    for (int i = 0; i < UI_MAX; ++i)
        if (g_ui[i].used && g_ui[i].parent == ARG(0) && (!best || g_ui[i].order < best->order))
            best = &g_ui[i];
    RET(best ? best->hwnd : 0, 1);
}

static void sh_GetLastActivePopup(Guest* g) { RET(ARG(0), 1); }

/* GetNextDlgTabItem / GetNextDlgGroupItem(dlg, ctl, previous): the next sibling, wrapping */
static void sh_GetNextDlgTabItem(Guest* g)
{
    UiWnd* c = ui(ARG(1));
    int prev = ARG(2) != 0;
    UiWnd *best = NULL, *wrap = NULL;
    for (int i = 0; i < UI_MAX; ++i)
    {
        UiWnd* x = &g_ui[i];
        if (!x->used || x->parent != ARG(0) || !x->visible || !x->enabled)
            continue;
        uint32_t o = c ? c->order : 0;
        if (prev ? x->order < o && (!best || x->order > best->order) : x->order > o && (!best || x->order < best->order))
            best = x;
        if (!wrap || (prev ? x->order > wrap->order : x->order < wrap->order))
            wrap = x;
    }
    RET(best ? best->hwnd : wrap ? wrap->hwnd : 0, 3);
}

/* window properties: a few per window, by atom or string (narrowed to an atom-like hash) */
static uint32_t prop_key(uint32_t p)
{
    if (p < 0x10000u)
        return p;
    uint32_t h = 2166136261u;
    for (uint16_t c; (c = rd16(p)); p += 2)
        h = (h ^ c) * 16777619u;
    return h | 0x80000000u;
}

static void sh_SetPropW(Guest* g)
{
    UiWnd* w = ui(ARG(0));
    uint32_t k = prop_key(ARG(1));
    for (int i = 0; w && i < 8; ++i)
        if (w->props[i].atom == k || !w->props[i].atom)
        {
            w->props[i].atom = k, w->props[i].value = ARG(2);
            RET(1, 3);
        }
    RET(0, 3);
}

static void sh_GetPropW(Guest* g)
{
    UiWnd* w = ui(ARG(0));
    uint32_t k = prop_key(ARG(1));
    for (int i = 0; w && i < 8; ++i)
        if (w->props[i].atom == k)
            RET(w->props[i].value, 2);
    RET(0, 2);
}

static void sh_RemovePropW(Guest* g)
{
    UiWnd* w = ui(ARG(0));
    uint32_t k = prop_key(ARG(1));
    for (int i = 0; w && i < 8; ++i)
        if (w->props[i].atom == k)
        {
            uint32_t v = w->props[i].value;
            w->props[i].atom = w->props[i].value = 0;
            RET(v, 2);
        }
    RET(0, 2);
}

/* MapDialogRect(dlg, RECT*): dialog units to pixels with the dialog's base units */
static void sh_MapDialogRect(Guest* g)
{
    UiWnd* w = ui(ARG(0));
    int bx = w && w->bx ? w->bx : 6, by = w && w->by ? w->by : 13;
    uint32_t r = ARG(1);
    wr32(r, (uint32_t)((int32_t)rd32(r) * bx / 4));
    wr32(r + 4, (uint32_t)((int32_t)rd32(r + 4) * by / 8));
    wr32(r + 8, (uint32_t)((int32_t)rd32(r + 8) * bx / 4));
    wr32(r + 12, (uint32_t)((int32_t)rd32(r + 12) * by / 8));
    RET(1, 2);
}

static void sh_PostThreadMessageW(Guest* g) { RET(user32_post(ARG(0), 0, ARG(1), ARG(2), ARG(3)), 4); }

static void sh_RegisterWindowMessageW(Guest* g)
{
    static char names[32][64];
    char n[64];
    guest_name(ARG(0), n, sizeof n);
    for (int i = 0; i < 32; ++i)
    {
        if (!names[i][0])
            snprintf(names[i], sizeof names[i], "%s", n);
        if (!strcmp(names[i], n))
            RET(0xC200u + (uint32_t)i, 1);
    }
    RET(0, 1);
}

/* --- rectangles ----------------------------------------------------------------------------------------- */
#define RL(r) ((int32_t)rd32(r))
#define RT(r) ((int32_t)rd32((r) + 4))
#define RR(r) ((int32_t)rd32((r) + 8))
#define RB(r) ((int32_t)rd32((r) + 12))
static void sh_CopyRect(Guest* g) { memcpy(GUEST_PTR(ARG(0)), GUEST_PTR(ARG(1)), 16); RET(1, 2); }
static void sh_EqualRect(Guest* g) { RET(!memcmp(GUEST_PTR(ARG(0)), GUEST_PTR(ARG(1)), 16), 2); }
static void sh_IsRectEmpty(Guest* g) { RET(RR(ARG(0)) <= RL(ARG(0)) || RB(ARG(0)) <= RT(ARG(0)), 1); }
static void sh_PtInRect(Guest* g)
{
    int32_t x = (int32_t)ARG(1), y = (int32_t)ARG(2);
    RET(x >= RL(ARG(0)) && x < RR(ARG(0)) && y >= RT(ARG(0)) && y < RB(ARG(0)), 3);
}
static void sh_OffsetRect(Guest* g)
{
    uint32_t r = ARG(0);
    write_rect(r, RL(r) + (int32_t)ARG(1), RT(r) + (int32_t)ARG(2), RR(r) + (int32_t)ARG(1), RB(r) + (int32_t)ARG(2));
    RET(1, 3);
}
static void sh_IntersectRect(Guest* g)
{
    uint32_t a = ARG(1), b = ARG(2);
    int l = RL(a) > RL(b) ? RL(a) : RL(b), t = RT(a) > RT(b) ? RT(a) : RT(b);
    int r = RR(a) < RR(b) ? RR(a) : RR(b), bo = RB(a) < RB(b) ? RB(a) : RB(b);
    if (r <= l || bo <= t)
    {
        write_rect(ARG(0), 0, 0, 0, 0);
        RET(0, 3);
    }
    write_rect(ARG(0), l, t, r, bo);
    RET(1, 3);
}

/* --- colours, painting, menus: AppKit draws; the program's own drawing is not shown --------------- */
static void sh_GetSysColor(Guest* g)
{
    switch (ARG(0))
    {
    case 5: case 14: RET(0xFFFFFFu, 1);     /* COLOR_WINDOW, COLOR_HIGHLIGHTTEXT */
    case 8: case 9: case 18: RET(0, 1);     /* window, caption and button text */
    case 13: RET(0xD77800u, 1);             /* COLOR_HIGHLIGHT */
    case 16: RET(0xA0A0A0u, 1);             /* COLOR_BTNSHADOW */
    case 20: RET(0xFFFFFFu, 1);             /* COLOR_BTNHIGHLIGHT */
    }
    RET(0xF0F0F0u, 1); /* COLOR_BTNFACE and the rest */
}
static void sh_GetSysColorBrush(Guest* g) { RET(0x00090000u | (ARG(0) & 0xFF), 1); }
static void sh_InvalidateRect(Guest* g) { RET(1, 3); }
static void sh_InvalidateRgn(Guest* g) { RET(1, 3); }
static void sh_ValidateRect(Guest* g) { RET(1, 2); }
static void sh_GetCapture(Guest* g) { RET(0, 0); }
static void sh_GetMessagePos(Guest* g) { RET(0, 0); }
static void sh_GetMessageTime(Guest* g) { RET((uint32_t)(rt_monotonic_ns() / 1000000u), 0); }
static void sh_MessageBeep(Guest* g) { RET(1, 1); }
static void sh_WinHelpW(Guest* g) { RET(0, 4); }
static void sh_SetWindowContextHelpId(Guest* g) { RET(1, 2); }
static void sh_CopyAcceleratorTableW(Guest* g) { RET(0, 3); }
static void sh_CharUpperW(Guest* g)
{
    uint32_t p = ARG(0);
    if (p < 0x10000u)
        RET(p >= 'a' && p <= 'z' ? p - 32 : p, 1);
    for (uint32_t q = p; rd16(q); q += 2)
        if (rd16(q) >= 'a' && rd16(q) <= 'z')
            wr16(q, rd16(q) - 32);
    RET(p, 1);
}
static void sh_LoadIconW(Guest* g) { RET(0x00020001u, 2); }
static void sh_LoadCursorW(Guest* g) { RET(0x00030001u, 2); }
static void sh_LoadBitmapW(Guest* g) { RET(0x000A0001u, 2); }
/* BeginPaint(hwnd, PAINTSTRUCT*): a DC that draws nothing, the whole client invalid */
static void sh_BeginPaint(Guest* g)
{
    UiWnd* w = ui(ARG(0));
    uint32_t ps = ARG(1);
    memset(GUEST_PTR(ps), 0, 64);
    wr32(ps, 0x000B0001u);
    if (w)
        write_rect(ps + 8, 0, 0, w->w, w->h);
    RET(0x000B0001u, 2);
}
static void sh_EndPaint(Guest* g) { RET(1, 2); }
static void sh_GetWindowDC(Guest* g) { RET(0x000B0001u, 1); }
static void sh_ret1_2(Guest* g) { RET(1, 2); }
static void sh_ret1_3(Guest* g) { RET(1, 3); }
static void sh_ret1_4(Guest* g) { RET(1, 4); }
static void sh_ret1_5(Guest* g) { RET(1, 5); }
static void sh_ret1_6(Guest* g) { RET(1, 6); }
static void sh_ret1_9(Guest* g) { RET(1, 9); }
static void sh_ret0_1(Guest* g) { RET(0, 1); }
static void sh_ret1_1(Guest* g) { RET(1, 1); }
static void sh_ret1_8(Guest* g) { RET(1, 8); }
static void sh_retm1_1(Guest* g) { RET((uint32_t)-1, 1); }
static void sh_ret0_2(Guest* g) { RET(0, 2); }
static void sh_retm1_2(Guest* g) { RET((uint32_t)-1, 2); }
static void sh_retm1_3(Guest* g) { RET((uint32_t)-1, 3); }
static void sh_GetMenuCheckMarkDimensions(Guest* g) { RET((13u << 16) | 13u, 0); }

/* --- gdi32: device contexts that draw nothing --------------------------------------------------------- */
static void sh_GetDeviceCaps(Guest* g)
{
    switch (ARG(1))
    {
    case 88: case 90: RET(96, 2);     /* LOGPIXELSX, LOGPIXELSY */
    case 12: RET(32, 2);              /* BITSPIXEL */
    case 14: RET(1, 2);               /* PLANES */
    case 8: RET(1920, 2);             /* HORZRES */
    case 10: RET(1080, 2);            /* VERTRES */
    }
    RET(0, 2);
}
static void sh_CreateGdiObject(Guest* g) { RET(0x000C0001u, 1); }
static void sh_CreateBitmap(Guest* g) { RET(0x000C0002u, 5); }
static void sh_GetBkColor(Guest* g) { RET(0xFFFFFFu, 1); }
static void sh_GetTextColor(Guest* g) { RET(0, 1); }
static void sh_GetMapMode(Guest* g) { RET(1, 1); } /* MM_TEXT */
static void sh_SetMapMode(Guest* g) { RET(1, 2); }
static void sh_SaveDC(Guest* g) { RET(1, 1); }
static void sh_GetClipBox(Guest* g) { write_rect(ARG(1), 0, 0, 1920, 1080); RET(2, 2); } /* SIMPLEREGION */
static void sh_GetRgnBox(Guest* g) { write_rect(ARG(1), 0, 0, 0, 0); RET(1, 2); }
static void sh_ExtSelectClipRgn(Guest* g) { RET(2, 3); }
static void sh_Escape(Guest* g) { RET(0, 5); }
static void sh_PtVisible(Guest* g) { RET(1, 3); }
static void sh_RectVisible(Guest* g) { RET(1, 2); }
/* the extent and origin calls write their old SIZE/POINT when asked */
static void sh_extent4(Guest* g) { if (ARG(3)) wr32(ARG(3), 1), wr32(ARG(3) + 4, 1); RET(1, 4); }
static void sh_extent6(Guest* g) { if (ARG(5)) wr32(ARG(5), 1), wr32(ARG(5) + 4, 1); RET(1, 6); }
static void sh_extent2(Guest* g) { if (ARG(1)) wr32(ARG(1), 1), wr32(ARG(1) + 4, 1); RET(1, 2); }
static void sh_origin4(Guest* g) { if (ARG(3)) wr32(ARG(3), 0), wr32(ARG(3) + 4, 0); RET(1, 4); }

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
    { "user32.dll", "MonitorFromWindow", sh_MonitorFromWindow },
    { "user32.dll", "MonitorFromRect", sh_MonitorFromRect },
    { "user32.dll", "MonitorFromPoint", sh_MonitorFromPoint },
    { "user32.dll", "GetMonitorInfoW", sh_GetMonitorInfoW },
    { "user32.dll", "GetMonitorInfoA", sh_GetMonitorInfoW },
    { "user32.dll", "RegisterClassW", sh_RegisterClassW },
    { "user32.dll", "RegisterClassExW", sh_RegisterClassExW },
    { "user32.dll", "UnregisterClassW", sh_UnregisterClassW },
    { "user32.dll", "GetClassInfoW", sh_GetClassInfoW },
    { "user32.dll", "GetClassInfoExW", sh_GetClassInfoExW },
    { "user32.dll", "GetClassLongW", sh_GetClassLongW },
    { "user32.dll", "GetClassNameW", sh_GetClassNameW },
    { "user32.dll", "CreateWindowExW", sh_CreateWindowExW },
    { "user32.dll", "SetWindowsHookExW", sh_SetWindowsHookExW },
    { "user32.dll", "SetWindowsHookExA", sh_SetWindowsHookExW },
    { "user32.dll", "UnhookWindowsHookEx", sh_UnhookWindowsHookEx },
    { "user32.dll", "CallNextHookEx", sh_CallNextHookEx },
    { "user32.dll", "CallWindowProcW", sh_CallWindowProcW },
    { "user32.dll", "CallWindowProcA", sh_CallWindowProcW },
    { "user32.dll", "GetDlgCtrlID", sh_GetDlgCtrlID },
    { "user32.dll", "IsChild", sh_IsChild },
    { "user32.dll", "GetTopWindow", sh_GetTopWindow },
    { "user32.dll", "GetLastActivePopup", sh_GetLastActivePopup },
    { "user32.dll", "GetNextDlgTabItem", sh_GetNextDlgTabItem },
    { "user32.dll", "GetNextDlgGroupItem", sh_GetNextDlgTabItem },
    { "user32.dll", "SetPropW", sh_SetPropW },
    { "user32.dll", "GetPropW", sh_GetPropW },
    { "user32.dll", "RemovePropW", sh_RemovePropW },
    { "user32.dll", "MapDialogRect", sh_MapDialogRect },
    { "user32.dll", "PostThreadMessageW", sh_PostThreadMessageW },
    { "user32.dll", "RegisterWindowMessageW", sh_RegisterWindowMessageW },
    { "user32.dll", "RegisterClipboardFormatW", sh_RegisterWindowMessageW },
    { "user32.dll", "SendDlgItemMessageA", sh_SendDlgItemMessageW },
    { "user32.dll", "CopyRect", sh_CopyRect },
    { "user32.dll", "EqualRect", sh_EqualRect },
    { "user32.dll", "IsRectEmpty", sh_IsRectEmpty },
    { "user32.dll", "PtInRect", sh_PtInRect },
    { "user32.dll", "OffsetRect", sh_OffsetRect },
    { "user32.dll", "IntersectRect", sh_IntersectRect },
    { "user32.dll", "GetSysColor", sh_GetSysColor },
    { "user32.dll", "GetSysColorBrush", sh_GetSysColorBrush },
    { "user32.dll", "InvalidateRect", sh_InvalidateRect },
    { "user32.dll", "InvalidateRgn", sh_InvalidateRgn },
    { "user32.dll", "ValidateRect", sh_ValidateRect },
    { "user32.dll", "GetCapture", sh_GetCapture },
    { "user32.dll", "GetMessagePos", sh_GetMessagePos },
    { "user32.dll", "GetMessageTime", sh_GetMessageTime },
    { "user32.dll", "MessageBeep", sh_MessageBeep },
    { "user32.dll", "WinHelpW", sh_WinHelpW },
    { "user32.dll", "SetWindowContextHelpId", sh_SetWindowContextHelpId },
    { "user32.dll", "CopyAcceleratorTableW", sh_CopyAcceleratorTableW },
    { "user32.dll", "CharUpperW", sh_CharUpperW },
    { "user32.dll", "LoadIconW", sh_LoadIconW },
    { "user32.dll", "LoadCursorW", sh_LoadCursorW },
    { "user32.dll", "LoadBitmapW", sh_LoadBitmapW },
    { "user32.dll", "BeginPaint", sh_BeginPaint },
    { "user32.dll", "EndPaint", sh_EndPaint },
    { "user32.dll", "GetWindowDC", sh_GetWindowDC },
    { "user32.dll", "DrawEdge", sh_ret1_4 },
    { "user32.dll", "DrawFocusRect", sh_ret1_2 },
    { "user32.dll", "DrawIcon", sh_ret1_4 },
    { "user32.dll", "DrawTextExW", sh_ret1_6 },
    { "user32.dll", "FillRect", sh_ret1_3 },
    { "user32.dll", "GrayStringW", sh_ret1_9 },
    { "user32.dll", "TabbedTextOutW", sh_ret1_8 },
    { "user32.dll", "CheckMenuItem", sh_retm1_3 },
    { "user32.dll", "EnableMenuItem", sh_retm1_3 },
    { "user32.dll", "DestroyMenu", sh_ret1_1 },
    { "user32.dll", "GetMenuItemCount", sh_retm1_1 },
    { "user32.dll", "GetMenuItemID", sh_retm1_2 },
    { "user32.dll", "GetMenuState", sh_retm1_3 },
    { "user32.dll", "GetSubMenu", sh_ret0_2 },
    { "user32.dll", "ModifyMenuW", sh_ret1_5 },
    { "user32.dll", "SetMenuItemBitmaps", sh_ret1_5 },
    { "user32.dll", "GetMenuCheckMarkDimensions", sh_GetMenuCheckMarkDimensions },
    { "gdi32.dll", "GetDeviceCaps", sh_GetDeviceCaps },
    { "gdi32.dll", "CreateSolidBrush", sh_CreateGdiObject },
    { "gdi32.dll", "CreateRectRgnIndirect", sh_CreateGdiObject },
    { "gdi32.dll", "CreateBitmap", sh_CreateBitmap },
    { "gdi32.dll", "GetBkColor", sh_GetBkColor },
    { "gdi32.dll", "GetTextColor", sh_GetTextColor },
    { "gdi32.dll", "GetMapMode", sh_GetMapMode },
    { "gdi32.dll", "SetMapMode", sh_SetMapMode },
    { "gdi32.dll", "SaveDC", sh_SaveDC },
    { "gdi32.dll", "RestoreDC", sh_ret1_2 },
    { "gdi32.dll", "GetClipBox", sh_GetClipBox },
    { "gdi32.dll", "GetRgnBox", sh_GetRgnBox },
    { "gdi32.dll", "ExtSelectClipRgn", sh_ExtSelectClipRgn },
    { "gdi32.dll", "Escape", sh_Escape },
    { "gdi32.dll", "PtVisible", sh_PtVisible },
    { "gdi32.dll", "RectVisible", sh_RectVisible },
    { "gdi32.dll", "TextOutW", sh_ret1_5 },
    { "gdi32.dll", "ExtTextOutW", sh_ret1_8 },
    { "gdi32.dll", "SetViewportExtEx", sh_extent4 },
    { "gdi32.dll", "SetWindowExtEx", sh_extent4 },
    { "gdi32.dll", "SetViewportOrgEx", sh_origin4 },
    { "gdi32.dll", "OffsetViewportOrgEx", sh_origin4 },
    { "gdi32.dll", "ScaleViewportExtEx", sh_extent6 },
    { "gdi32.dll", "ScaleWindowExtEx", sh_extent6 },
    { "gdi32.dll", "GetViewportExtEx", sh_extent2 },
    { "gdi32.dll", "GetWindowExtEx", sh_extent2 },
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
    for (const ShimDef* d = ui_shims; d->name && g_nprev < 255; ++d)
    {
        g_prev[g_nprev].name = d->name;
        g_prev[g_nprev].fn = thunk_shim(d->dll, d->name);
        /* user32.c serves its windows with the A calls only: the W one without strings is the same */
        size_t n = strlen(d->name);
        if (!g_prev[g_nprev].fn && n > 1 && d->name[n - 1] == 'W')
        {
            char a[64];
            snprintf(a, sizeof a, "%.*sA", (int)(n - 1), d->name);
            g_prev[g_nprev].fn = thunk_shim(d->dll, a);
        }
        g_nprev++;
    }
    g_prev[g_nprev].name = "SystemParametersInfoA";
    g_prev[g_nprev++].fn = thunk_shim("user32.dll", "SystemParametersInfoA");
    thunk_override(ui_shims);
}

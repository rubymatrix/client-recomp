/* The WebBrowser ActiveX control (Internet Explorer's, CLSID 8856F961-340A-11D0-A96B-00C04FD705A2)
 * on WKWebView, for ffxivlogin.exe.
 *
 * ffxivlogin is an MFC dialog that hosts the control through MFC's OLE control container. What it
 * needs from it, and so what is here:
 *   - creation: CoGetClassObject / CoCreateInstance, IOleObject (client site, DoVerb in-place),
 *     IOleInPlaceObject (its window, its rectangle), IPersistStreamInit (no saved state),
 *     IConnectionPointContainer / IConnectionPoint for DWebBrowserEvents2, IProvideClassInfo2 (the
 *     event interface), IOleControl;
 *   - IDispatch on the control: Navigate2 (MFC's InvokeHelper), Refresh2, Stop, Busy, ...;
 *   - the events: BeforeNavigate2, DocumentComplete, NewWindow3, fired into the sink MFC advises;
 *   - from DocumentComplete: IWebBrowser2::get_Document, IHTMLDocument2::get_all,
 *     IHTMLElementCollection::tags / get_length / item, IHTMLElement::getAttribute. The page's
 *     <x-sqexauth> element is read with JavaScript when the load finishes and served from there.
 *
 * Objects live in guest memory with one vtable pointer per interface; every vtable slot is a
 * thunk to a shim here ("IOleObject::DoVerb"), so a slot without one traps with its name. The
 * WKWebView sits in a child window of the dialog (class "Shell Embedding"); its delegate runs on the
 * main thread and only posts a message to that window, whose procedure (on the guest thread)
 * fires the events.
 *
 * The page the control is sent to, http://account.square-enix.com/..., goes to XIV_ACCOUNT_URL
 * (default http://127.0.0.1: the server's web login) with the same path.
 */
#import <AppKit/AppKit.h>
#import <WebKit/WebKit.h>
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gthread.h"
#include "gwin.h"
#include "k32.h"
#include "plat.h"
#include "thunk.h"
#include "user32.h"

uint32_t ui_create_window(uint32_t exstyle, const char* cls, uint32_t name, uint32_t style, int32_t x, int32_t y,
    int32_t cx, int32_t cy, uint32_t parent, uint32_t menu, uint32_t hinst, uint32_t params);
void ui_register_class(const char* name, uint32_t proc);
void* ui_view(uint32_t hwnd);
uint32_t ui_def_window(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);
uint32_t ui_window_tid(uint32_t hwnd);
extern char k32_user_agent[256];
extern int (*ole_class_hook)(uint32_t clsid, uint32_t riid, uint32_t* obj);

#define S_OK 0u
#define S_FALSE 1u
#define E_NOTIMPL 0x80004001u
#define E_NOINTERFACE 0x80004002u
#define E_FAIL 0x80004005u
#define CONNECT_E_NOCONNECTION 0x80040200u
#define DISP_E_MEMBERNOTFOUND 0x80020003u
#define REGDB_E_CLASSNOTREG 0x80040154u

#define VT_EMPTY 0
#define VT_I4 3
#define VT_BSTR 8
#define VT_DISPATCH 9
#define VT_BOOL 11
#define VT_VARIANT 12
#define VT_BYREF 0x4000

#define WM_WEB_EVENT 0x7FF0 /* to the control's window: events are waiting */

/* --- GUIDs ------------------------------------------------------------------------------------- */
typedef struct { uint8_t b[16]; } Guid;

static Guid guid(const char* s) /* "8856F961-340A-11D0-A96B-00C04FD705A2" */
{
    Guid g;
    unsigned d1, d2, d3, d4[8];
    sscanf(s, "%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x", &d1, &d2, &d3, &d4[0], &d4[1], &d4[2], &d4[3], &d4[4], &d4[5],
        &d4[6], &d4[7]);
    memcpy(g.b, &d1, 4);
    uint16_t a = (uint16_t)d2, b = (uint16_t)d3;
    memcpy(g.b + 4, &a, 2);
    memcpy(g.b + 6, &b, 2);
    for (int i = 0; i < 8; ++i)
        g.b[8 + i] = (uint8_t)d4[i];
    return g;
}

static int is(uint32_t p, const Guid* gd) { return p && !memcmp(GUEST_PTR(p), gd->b, 16); }

static Guid CLSID_WebBrowser, IID_IUnknown, IID_IDispatch, IID_IWebBrowser, IID_IWebBrowserApp, IID_IWebBrowser2,
    IID_IOleObject, IID_IOleWindow, IID_IOleInPlaceObject, IID_IOleInPlaceActiveObject, IID_IPersist,
    IID_IPersistStreamInit, IID_IConnectionPointContainer, IID_IConnectionPoint, IID_IProvideClassInfo,
    IID_IProvideClassInfo2, IID_IOleControl, IID_IClassFactory, IID_IOleInPlaceSite, DIID_DWebBrowserEvents2,
    IID_IHTMLDocument, IID_IHTMLDocument2, IID_IHTMLElementCollection, IID_IHTMLElement;

static void guids_init(void)
{
    CLSID_WebBrowser = guid("8856F961-340A-11D0-A96B-00C04FD705A2");
    IID_IUnknown = guid("00000000-0000-0000-C000-000000000046");
    IID_IDispatch = guid("00020400-0000-0000-C000-000000000046");
    IID_IWebBrowser = guid("EAB22AC1-30C1-11CF-A7EB-0000C05BAE0B");
    IID_IWebBrowserApp = guid("0002DF05-0000-0000-C000-000000000046");
    IID_IWebBrowser2 = guid("D30C1661-CDAF-11D0-8A3E-00C04FC9E26E");
    IID_IOleObject = guid("00000112-0000-0000-C000-000000000046");
    IID_IOleWindow = guid("00000114-0000-0000-C000-000000000046");
    IID_IOleInPlaceObject = guid("00000113-0000-0000-C000-000000000046");
    IID_IOleInPlaceActiveObject = guid("00000117-0000-0000-C000-000000000046");
    IID_IPersist = guid("0000010C-0000-0000-C000-000000000046");
    IID_IPersistStreamInit = guid("7FD52380-4E07-101B-AE2D-08002B2EC713");
    IID_IConnectionPointContainer = guid("B196B284-BAB4-101A-B69C-00AA00341D07");
    IID_IConnectionPoint = guid("B196B286-BAB4-101A-B69C-00AA00341D07");
    IID_IProvideClassInfo = guid("B196B283-BAB4-101A-B69C-00AA00341D07");
    IID_IProvideClassInfo2 = guid("A6BC3AC0-DBAA-11CE-9DE3-00AA004BB851");
    IID_IOleControl = guid("B196B288-BAB4-101A-B69C-00AA00341D07");
    IID_IClassFactory = guid("00000001-0000-0000-C000-000000000046");
    IID_IOleInPlaceSite = guid("00000119-0000-0000-C000-000000000046");
    DIID_DWebBrowserEvents2 = guid("34A715A0-6587-11D0-924A-0020AFC7AC4D");
    IID_IHTMLDocument = guid("626FC520-A41E-11CF-A731-00A0C9082637");
    IID_IHTMLDocument2 = guid("332C4425-26CB-11D0-B483-00C04FD90119");
    IID_IHTMLElementCollection = guid("3050F21F-98B5-11CF-BB82-00AA00BDCE0B");
    IID_IHTMLElement = guid("3050F1FF-98B5-11CF-BB82-00AA00BDCE0B");
}

static void guid_log(const char* what, uint32_t p)
{
    const uint8_t* b = GUEST_PTR(p);
    uint32_t d1;
    uint16_t d2, d3;
    memcpy(&d1, b, 4), memcpy(&d2, b + 4, 2), memcpy(&d3, b + 6, 2);
    rt_log("[web] %s {%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}\n", what, d1, d2, d3, b[8], b[9], b[10], b[11],
        b[12], b[13], b[14], b[15]);
}

/* --- vtables: one thunk per slot, named after the interface and method ------------------------- */
static uint32_t vtable_n(const char* iface, unsigned n, const char* const* names, unsigned nnames)
{
    uint32_t v = gheap_alloc(4 * n, 1);
    for (unsigned i = 0; i < n; ++i)
    {
        char full[96];
        if (i < 3)
            snprintf(full, sizeof full, "%s::%s", iface, i == 0 ? "QueryInterface" : i == 1 ? "AddRef" : "Release");
        else if (names && i - 3 < nnames && names[i - 3])
            snprintf(full, sizeof full, "%s::%s", iface, names[i - 3]);
        else
            snprintf(full, sizeof full, "%s::#%u", iface, i);
        wr32(v + 4 * i, thunk_for("webbrowser.dll", full));
    }
    return v;
}

/* a vtable of n slots: IUnknown's three, then the named methods, then unnamed ones */
#define vtable(iface, n, names) vtable_n(iface, n, names, sizeof names / sizeof *names)

/* IDispatch's own four, then the interface's methods in vtable order (NULL: not served) */
#define DISPATCH4 "GetTypeInfoCount", "GetTypeInfo", "GetIDsOfNames", "Invoke"

/* --- the objects --------------------------------------------------------------------------------- */
/* The browser: one guest block, a vtable pointer per interface at these offsets */
enum
{
    B_WB = 0,    /* IUnknown, IDispatch, IWebBrowser, IWebBrowserApp, IWebBrowser2 */
    B_OO = 4,    /* IOleObject */
    B_IPO = 8,   /* IOleWindow, IOleInPlaceObject */
    B_IPAO = 12, /* IOleInPlaceActiveObject */
    B_PSI = 16,  /* IPersist, IPersistStreamInit */
    B_CPC = 20,  /* IConnectionPointContainer */
    B_CP = 24,   /* IConnectionPoint (DWebBrowserEvents2) */
    B_PCI = 28,  /* IProvideClassInfo, IProvideClassInfo2 */
    B_OC = 32,   /* IOleControl */
    B_SIZE = 36
};

/* what the page's <x-sqexauth> said, read when its load finished */
typedef struct
{
    int found;
    int has[4];
    char val[4][160]; /* sid, lang, region, utc */
} Auth;
static const char* const AUTH_NAMES[4] = { "sid", "lang", "region", "utc" };

typedef struct
{
    uint32_t obj;         /* guest address of the browser block */
    uint32_t site;        /* IOleClientSite */
    uint32_t sink;        /* the advised DWebBrowserEvents2 IDispatch */
    uint32_t hwnd;        /* the "Shell Embedding" child window */
    uint32_t doc, coll, elem, factory; /* the document side's objects (one each) */
    int busy;
    char url[2048];       /* the location, as the page reported it */
    Auth auth;
    WKWebView* view;
} Web;

static Web g_web;

/* events from the main thread, for the guest thread to fire */
typedef struct
{
    int kind; /* 1 BeforeNavigate2, 2 DocumentComplete, 3 NewWindow3 */
    char url[2048];
    Auth auth;
} WebEvent;
#define EVQ 32
static WebEvent g_evq[EVQ];
static unsigned g_evhead, g_evcount;
static volatile uint32_t g_evlock;
static void evlock(void) { while (plat_atomic_cas32(&g_evlock, 0, 1) != 0) plat_yield(); }
static void evunlock(void) { plat_atomic_cas32(&g_evlock, 1, 0); }

static void push_event(const WebEvent* e)
{
    evlock();
    if (g_evcount < EVQ)
        g_evq[(g_evhead + g_evcount++) % EVQ] = *e;
    evunlock();
    if (g_web.hwnd)
        user32_post(ui_window_tid(g_web.hwnd), g_web.hwnd, WM_WEB_EVENT, 0, 0);
}

static int pop_event(WebEvent* e)
{
    evlock();
    int got = g_evcount > 0;
    if (got)
    {
        *e = g_evq[g_evhead];
        g_evhead = (g_evhead + 1) % EVQ;
        g_evcount--;
    }
    evunlock();
    return got;
}

/* --- strings ------------------------------------------------------------------------------------ */
static uint32_t bstr(const char* s) /* UTF-8 to a guest BSTR */
{
    NSString* n = [NSString stringWithUTF8String:s ? s : ""];
    NSUInteger len = [n length];
    uint32_t p = gheap_alloc(4 + 2 * (uint32_t)len + 2, 0);
    wr32(p, 2 * (uint32_t)len);
    for (NSUInteger i = 0; i < len; ++i)
        wr16(p + 4 + 2 * (uint32_t)i, [n characterAtIndex:i]);
    wr16(p + 4 + 2 * (uint32_t)len, 0);
    return p + 4;
}

static NSString* nsstr_w(uint32_t w)
{
    if (!w)
        return @"";
    uint32_t n = 0;
    while (rd16(w + 2 * n))
        n++;
    unichar* b = malloc(2 * (n + 1));
    for (uint32_t i = 0; i < n; ++i)
        b[i] = rd16(w + 2 * i);
    NSString* s = [NSString stringWithCharacters:b length:n];
    free(b);
    return s;
}

/* a VARIANT's string, following VT_BYREF|VT_VARIANT */
static NSString* variant_string(uint32_t v)
{
    if (!v)
        return nil;
    uint16_t vt = rd16(v);
    if (vt == (VT_BYREF | VT_VARIANT))
        return variant_string(rd32(v + 8));
    if (vt == VT_BSTR)
        return nsstr_w(rd32(v + 8));
    if (vt == (VT_BYREF | VT_BSTR))
        return nsstr_w(rd32(rd32(v + 8)));
    return nil;
}

static int32_t variant_int(uint32_t v, int32_t def)
{
    if (!v)
        return def;
    uint16_t vt = rd16(v);
    if (vt == (VT_BYREF | VT_VARIANT))
        return variant_int(rd32(v + 8), def);
    if (vt == VT_I4 || vt == 2 /* VT_I2 */ || vt == 19 /* VT_UI4 */ || vt == 22 /* VT_INT */)
        return (int32_t)rd32(v + 8);
    if (vt == (VT_BYREF | VT_I4))
        return (int32_t)rd32(rd32(v + 8));
    return def;
}

/* --- the main thread: the WKWebView -------------------------------------------------------------- */
static void run_block(void* b) { ((void (^)(void))b)(); }

static void on_main(void (^b)(void))
{
    if (SDL_IsMainThread())
        b();
    else
        SDL_RunOnMainThread(run_block, (void*)b, true);
}

/* where http://account.square-enix.com/<path> really goes */
static NSURL* redirect(NSURL* u)
{
    if (![[u host] isEqualToString:@"account.square-enix.com"] && ![[u host] isEqualToString:@"secure.square-enix.com"])
        return nil;
    const char* base = getenv("XIV_ACCOUNT_URL") ? getenv("XIV_ACCOUNT_URL") : "http://127.0.0.1";
    NSString* rest = [u query] ? [NSString stringWithFormat:@"%@?%@", [u path], [u query]] : [u path];
    return [NSURL URLWithString:[NSString stringWithFormat:@"%s%@", base, rest]];
}

/* <x-sqexauth> in the document or any frame it can reach */
static NSString* const AUTH_JS =
    @"(function(){function f(d){var e=d.getElementsByTagName('x-sqexauth');"
     "if(e.length){var x=e[0],r={};['sid','lang','region','utc'].forEach(function(n){r[n]=x.getAttribute(n);});return r;}"
     "for(var i=0;i<d.defaultView.frames.length;i++){try{var q=f(d.defaultView.frames[i].document);if(q)return q;}catch(_){}}"
     "return null;}return JSON.stringify(f(document));})()";

@interface WebDelegate : NSObject <WKNavigationDelegate, WKUIDelegate>
@end

@implementation WebDelegate
- (void)webView:(WKWebView*)wv
    decidePolicyForNavigationAction:(WKNavigationAction*)a
                    decisionHandler:(void (^)(WKNavigationActionPolicy))handler
{
    NSURL* to = redirect([[a request] URL]);
    if (to)
    {
        handler(WKNavigationActionPolicyCancel);
        NSMutableURLRequest* r = [[[a request] mutableCopy] autorelease];
        [r setURL:to];
        [wv loadRequest:r];
        return;
    }
    if ([a targetFrame] && [[a targetFrame] isMainFrame])
    {
        WebEvent e = { .kind = 1 };
        snprintf(e.url, sizeof e.url, "%s", [[[[a request] URL] absoluteString] UTF8String]);
        g_web.busy = 1;
        push_event(&e);
    }
    handler(WKNavigationActionPolicyAllow);
}

- (void)webView:(WKWebView*)wv didFinishNavigation:(WKNavigation*)nav
{
    /* XIV_WEB_SCRIPT=<file.js>: run in every page once it loads (unattended tests sign in so) */
    const char* script = getenv("XIV_WEB_SCRIPT");
    NSString* js = script ? [NSString stringWithContentsOfFile:[NSString stringWithUTF8String:script]
                                                      encoding:NSUTF8StringEncoding
                                                         error:nil]
                          : nil;
    if (js)
        [wv evaluateJavaScript:js completionHandler:nil];
    NSString* url = [[[wv URL] absoluteString] copy];
    [wv evaluateJavaScript:AUTH_JS
         completionHandler:^(id result, NSError* err) {
             WebEvent e = { .kind = 2 };
             snprintf(e.url, sizeof e.url, "%s", [url UTF8String]);
             [url release];
             if ([result isKindOfClass:[NSString class]])
             {
                 NSData* d = [(NSString*)result dataUsingEncoding:NSUTF8StringEncoding];
                 id o = [NSJSONSerialization JSONObjectWithData:d options:0 error:nil];
                 if ([o isKindOfClass:[NSDictionary class]])
                 {
                     e.auth.found = 1;
                     for (int i = 0; i < 4; ++i)
                     {
                         id v = [(NSDictionary*)o objectForKey:[NSString stringWithUTF8String:AUTH_NAMES[i]]];
                         if ([v isKindOfClass:[NSString class]])
                         {
                             e.auth.has[i] = 1;
                             snprintf(e.auth.val[i], sizeof e.auth.val[i], "%s", [(NSString*)v UTF8String]);
                         }
                     }
                 }
             }
             g_web.busy = 0;
             push_event(&e);
         }];
}

- (void)webView:(WKWebView*)wv didFailProvisionalNavigation:(WKNavigation*)nav withError:(NSError*)err
{
    rt_log("[web] load failed: %s\n", [[err localizedDescription] UTF8String]);
    g_web.busy = 0;
}

/* target=_blank and window.open: NewWindow3, which ffxivlogin answers by opening the system browser */
- (WKWebView*)webView:(WKWebView*)wv
    createWebViewWithConfiguration:(WKWebViewConfiguration*)c
               forNavigationAction:(WKNavigationAction*)a
                    windowFeatures:(WKWindowFeatures*)f
{
    WebEvent e = { .kind = 3 };
    snprintf(e.url, sizeof e.url, "%s", [[[[a request] URL] absoluteString] UTF8String]);
    push_event(&e);
    return nil;
}
@end

static WebDelegate* g_delegate;

static void make_webview(uint32_t hwnd)
{
    on_main(^{
        NSView* host = (NSView*)ui_view(hwnd);
        if (!host)
            return;
        if (!g_delegate)
            g_delegate = [[WebDelegate alloc] init];
        WKWebViewConfiguration* c = [[WKWebViewConfiguration alloc] init];
        WKWebView* wv = [[WKWebView alloc] initWithFrame:[host bounds] configuration:c];
        [c release];
        [wv setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
        [wv setNavigationDelegate:g_delegate];
        [wv setUIDelegate:g_delegate];
        if (k32_user_agent[0])
            [wv setCustomUserAgent:[NSString stringWithUTF8String:k32_user_agent]];
        [host addSubview:wv];
        g_web.view = wv;
    });
}

static void navigate(NSString* url)
{
    rt_log("[web] Navigate %s\n", [url UTF8String]);
    snprintf(g_web.url, sizeof g_web.url, "%s", [url UTF8String]);
    if (k32_user_agent[0] && g_web.view)
    {
        NSString* ua = [NSString stringWithUTF8String:k32_user_agent];
        WKWebView* wv = g_web.view;
        on_main(^{ [wv setCustomUserAgent:ua]; });
    }
    NSURL* u = [NSURL URLWithString:url];
    WKWebView* wv = g_web.view;
    if (!u || !wv)
        return;
    g_web.busy = 1;
    on_main(^{
        NSURLRequest* r = [NSURLRequest requestWithURL:u cachePolicy:NSURLRequestReloadIgnoringLocalCacheData timeoutInterval:30];
        [wv loadRequest:r];
    });
}

/* --- firing events into the sink ---------------------------------------------------------------- */
/* sink->Invoke(dispid, IID_NULL, lcid, DISPATCH_METHOD, params, NULL, NULL, NULL); the arguments
 * are in reverse order in rgvarg, as COM passes them */
static void fire(uint32_t dispid, uint32_t args, uint32_t nargs)
{
    if (!g_web.sink)
        return;
    uint32_t dp = gheap_alloc(16, 1), iid = gheap_alloc(16, 1);
    wr32(dp, args);
    wr32(dp + 8, nargs);
    uint32_t a[9] = { g_web.sink, dispid, iid, 0x0409, 1, dp, 0, 0, 0 };
    guest_call(rd32(rd32(g_web.sink) + 24), 9, a); /* IDispatch::Invoke */
    gheap_free(dp);
    gheap_free(iid);
}

static void set_var(uint32_t v, uint16_t vt, uint32_t val)
{
    memset(GUEST_PTR(v), 0, 16);
    wr16(v, vt);
    wr32(v + 8, val);
}

static void fire_event(const WebEvent* e)
{
    if (e->kind == 2)
    {
        g_web.auth = e->auth;
        snprintf(g_web.url, sizeof g_web.url, "%s", e->url);
    }
    uint32_t url = bstr(e->url);
    if (e->kind == 1) /* BeforeNavigate2(pDisp, URL*, Flags*, TargetFrameName*, PostData*, Headers*, Cancel*) */
    {
        uint32_t v = gheap_alloc(16 * 7 + 16 * 6 + 4, 1), inner = v + 16 * 7, cancel = inner + 16 * 6;
        set_var(inner, VT_BSTR, url);
        set_var(inner + 16, VT_I4, 0);
        set_var(inner + 32, VT_BSTR, 0);
        set_var(inner + 48, VT_EMPTY, 0);
        set_var(inner + 64, VT_BSTR, 0);
        set_var(v + 0, VT_BYREF | VT_BOOL, cancel);
        set_var(v + 16, VT_BYREF | VT_VARIANT, inner + 64);
        set_var(v + 32, VT_BYREF | VT_VARIANT, inner + 48);
        set_var(v + 48, VT_BYREF | VT_VARIANT, inner + 32);
        set_var(v + 64, VT_BYREF | VT_VARIANT, inner + 16);
        set_var(v + 80, VT_BYREF | VT_VARIANT, inner);
        set_var(v + 96, VT_DISPATCH, g_web.obj + B_WB);
        fire(250, v, 7);
        gheap_free(v);
    }
    else if (e->kind == 2) /* DocumentComplete(pDisp, URL*) */
    {
        uint32_t v = gheap_alloc(16 * 3, 1), inner = v + 32;
        set_var(inner, VT_BSTR, url);
        set_var(v, VT_BYREF | VT_VARIANT, inner);
        set_var(v + 16, VT_DISPATCH, g_web.obj + B_WB);
        rt_log("[web] DocumentComplete %s%s\n", e->url, e->auth.found ? " (x-sqexauth)" : "");
        fire(259, v, 2);
        gheap_free(v);
    }
    else if (e->kind == 3) /* NewWindow3(ppDisp*, Cancel*, dwFlags, bstrUrlContext, bstrUrl) */
    {
        uint32_t v = gheap_alloc(16 * 5 + 8, 1), ppdisp = v + 80, cancel = ppdisp + 4;
        set_var(v, VT_BSTR, url);
        set_var(v + 16, VT_BSTR, bstr(g_web.url));
        set_var(v + 32, VT_I4, 0);
        set_var(v + 48, VT_BYREF | VT_BOOL, cancel);
        set_var(v + 64, VT_BYREF | VT_DISPATCH, ppdisp);
        rt_log("[web] NewWindow3 %s\n", e->url);
        fire(273, v, 5);
        gheap_free(v);
    }
    gheap_free(url - 4);
}

/* the control's window procedure: events posted from the main thread fire here */
static void sh_ShellEmbeddingProc(Guest* g)
{
    if (ARG(1) == WM_WEB_EVENT)
    {
        WebEvent e;
        while (pop_event(&e))
            fire_event(&e);
        RET(0, 4);
    }
    RET(ui_def_window(ARG(0), ARG(1), ARG(2), ARG(3)), 4);
}

/* --- IUnknown for every interface of every object ----------------------------------------------- */
static uint32_t g_doc_base, g_coll_base, g_elem_base, g_factory_base;

static uint32_t query(uint32_t self, uint32_t riid)
{
    uint32_t b = g_web.obj;
    if (self >= b && self < b + B_SIZE)
    {
        if (is(riid, &IID_IUnknown) || is(riid, &IID_IDispatch) || is(riid, &IID_IWebBrowser)
            || is(riid, &IID_IWebBrowserApp) || is(riid, &IID_IWebBrowser2))
            return b + B_WB;
        if (is(riid, &IID_IOleObject))
            return b + B_OO;
        if (is(riid, &IID_IOleWindow) || is(riid, &IID_IOleInPlaceObject))
            return b + B_IPO;
        if (is(riid, &IID_IOleInPlaceActiveObject))
            return b + B_IPAO;
        if (is(riid, &IID_IPersist) || is(riid, &IID_IPersistStreamInit))
            return b + B_PSI;
        if (is(riid, &IID_IConnectionPointContainer))
            return b + B_CPC;
        if (is(riid, &IID_IConnectionPoint))
            return b + B_CP;
        if (is(riid, &IID_IProvideClassInfo) || is(riid, &IID_IProvideClassInfo2))
            return b + B_PCI;
        if (is(riid, &IID_IOleControl))
            return b + B_OC;
        return 0;
    }
    if (self == g_doc_base)
        return is(riid, &IID_IUnknown) || is(riid, &IID_IDispatch) || is(riid, &IID_IHTMLDocument)
                   || is(riid, &IID_IHTMLDocument2) ? self : 0;
    if (self == g_coll_base)
        return is(riid, &IID_IUnknown) || is(riid, &IID_IDispatch) || is(riid, &IID_IHTMLElementCollection) ? self : 0;
    if (self == g_elem_base)
        return is(riid, &IID_IUnknown) || is(riid, &IID_IDispatch) || is(riid, &IID_IHTMLElement) ? self : 0;
    if (self == g_factory_base)
        return is(riid, &IID_IUnknown) || is(riid, &IID_IClassFactory) ? self : 0;
    return 0;
}

static void sh_QueryInterface(Guest* g)
{
    uint32_t r = query(ARG(0), ARG(1));
    if (ARG(2))
        wr32(ARG(2), r);
    if (!r)
        guid_log("QueryInterface: no", ARG(1));
    RET(r ? S_OK : E_NOINTERFACE, 3);
}
static void sh_AddRef(Guest* g) { RET(2, 1); }
static void sh_Release(Guest* g) { RET(1, 1); }

/* IDispatch's type information: none */
static void sh_GetTypeInfoCount(Guest* g) { if (ARG(1)) wr32(ARG(1), 0); RET(S_OK, 2); }
static void sh_GetTypeInfo(Guest* g) { if (ARG(3)) wr32(ARG(3), 0); RET(E_NOTIMPL, 4); }
static void sh_GetIDsOfNames(Guest* g) { RET(DISP_E_MEMBERNOTFOUND, 6); }

/* --- the class factory -------------------------------------------------------------------------- */
static uint32_t create_browser(void);

/* IClassFactory::CreateInstance(this, outer, riid, ppv) */
static void sh_CF_CreateInstance(Guest* g)
{
    uint32_t b = create_browser(), r = query(b, ARG(2));
    if (ARG(3))
        wr32(ARG(3), r);
    RET(r ? S_OK : E_NOINTERFACE, 4);
}
static void sh_CF_LockServer(Guest* g) { RET(S_OK, 2); }

/* CoGetClassObject(clsid, context, server, riid, ppv) */
static Shim g_prev_get_class_object;
static void sh_CoGetClassObject(Guest* g)
{
    if (!is(ARG(0), &CLSID_WebBrowser))
    {
        if (g_prev_get_class_object)
        {
            g_prev_get_class_object(g);
            return;
        }
        guid_log("CoGetClassObject: not registered", ARG(0));
        if (ARG(4))
            wr32(ARG(4), 0);
        RET(REGDB_E_CLASSNOTREG, 5);
    }
    uint32_t r = query(g_factory_base, ARG(3));
    if (ARG(4))
        wr32(ARG(4), r);
    RET(r ? S_OK : E_NOINTERFACE, 5);
}

static int (*g_prev_class_hook)(uint32_t clsid, uint32_t riid, uint32_t* obj);
static int class_hook(uint32_t clsid, uint32_t riid, uint32_t* obj)
{
    if (is(clsid, &CLSID_WebBrowser))
    {
        *obj = query(create_browser(), riid);
        return 1;
    }
    return g_prev_class_hook ? g_prev_class_hook(clsid, riid, obj) : 0;
}

/* --- IOleObject ----------------------------------------------------------------------------------- */
static void sh_OO_SetClientSite(Guest* g)
{
    g_web.site = ARG(1);
    if (ARG(1))
    {
        uint32_t a[1] = { ARG(1) };
        guest_call(rd32(rd32(ARG(1)) + 4), 1, a); /* AddRef */
    }
    RET(S_OK, 2);
}
static void sh_OO_GetClientSite(Guest* g) { if (ARG(1)) wr32(ARG(1), g_web.site); RET(g_web.site ? S_OK : E_FAIL, 2); }
static void sh_OO_SetHostNames(Guest* g) { RET(S_OK, 3); }
static void sh_OO_Close(Guest* g) { RET(S_OK, 2); }
static void sh_OO_GetUserClassID(Guest* g) { memcpy(GUEST_PTR(ARG(1)), CLSID_WebBrowser.b, 16); RET(S_OK, 2); }
/* the extent in HIMETRIC (0.01 mm), as the container sets it; until then, none */
static uint32_t g_extent[2];
static void sh_OO_SetExtent(Guest* g)
{
    g_extent[0] = rd32(ARG(2)), g_extent[1] = rd32(ARG(2) + 4);
    rt_log("[web] SetExtent %u x %u\n", g_extent[0], g_extent[1]);
    RET(S_OK, 3);
}
static void sh_OO_GetExtent(Guest* g)
{
    wr32(ARG(2), g_extent[0]), wr32(ARG(2) + 4, g_extent[1]);
    rt_log("[web] GetExtent: %u x %u\n", g_extent[0], g_extent[1]);
    RET(g_extent[0] ? S_OK : E_FAIL, 3);
}
static void sh_OO_Advise(Guest* g) { if (ARG(2)) wr32(ARG(2), 1); RET(S_OK, 3); }
static void sh_OO_Unadvise(Guest* g) { RET(S_OK, 2); }
/* OLEMISC: SETCLIENTSITEFIRST, ACTIVATEWHENVISIBLE, INSIDEOUT, CANTLINKINSIDE, RECOMPOSEONRESIZE */
static void sh_OO_GetMiscStatus(Guest* g) { if (ARG(2)) wr32(ARG(2), 0x00020000u | 0x100u | 0x80u | 0x10u | 0x1u); RET(S_OK, 3); }
static void sh_OO_SetColorScheme(Guest* g) { RET(S_OK, 2); }

/* the site's IOleInPlaceSite, called as COM would: method `slot` with the arguments after this */
static uint32_t site_call(unsigned slot, unsigned n, const uint32_t* args)
{
    if (!g_web.site)
        return E_FAIL;
    uint32_t iid = gheap_alloc(16, 0), out = gheap_alloc(4, 1);
    memcpy(GUEST_PTR(iid), IID_IOleInPlaceSite.b, 16);
    uint32_t q[3] = { g_web.site, iid, out };
    uint32_t hr = guest_call(rd32(rd32(g_web.site)), 3, q);
    uint32_t ips = rd32(out);
    gheap_free(iid);
    gheap_free(out);
    if (hr || !ips)
        return E_NOINTERFACE;
    uint32_t a[8] = { ips };
    for (unsigned i = 0; i < n && i < 7; ++i)
        a[1 + i] = args[i];
    hr = guest_call(rd32(rd32(ips) + 4 * slot), 1 + n, a);
    uint32_t r[1] = { ips };
    guest_call(rd32(rd32(ips) + 8), 1, r); /* Release */
    return hr;
}

/* DoVerb(this, verb, msg, site, lindex, hwndParent, rect): in place, in a child window of the site's
 * window over rect, with the WKWebView in it */
static void sh_OO_DoVerb(Guest* g)
{
    int32_t verb = (int32_t)ARG(1);
    uint32_t parent = ARG(5), rect = ARG(6);
    if (ARG(3) && !g_web.site)
        g_web.site = ARG(3);
    if (!g_web.hwnd && verb <= 0 && verb >= -5) /* PRIMARY, SHOW, OPEN, HIDE, UIACTIVATE, INPLACEACTIVATE */
    {
        if (!parent)
        {
            uint32_t out = gheap_alloc(4, 1);
            uint32_t a[1] = { out };
            site_call(3, 1, a); /* IOleWindow::GetWindow */
            parent = rd32(out);
            gheap_free(out);
        }
        int32_t x = 0, y = 0, w = 640, h = 400;
        if (rect)
            x = (int32_t)rd32(rect), y = (int32_t)rd32(rect + 4), w = (int32_t)rd32(rect + 8) - x, h = (int32_t)rd32(rect + 12) - y;
        g_web.hwnd = ui_create_window(0, "Shell Embedding", 0, 0x40000000u | 0x10000000u | 0x04000000u, x, y, w, h, parent,
            0, rt_image_base, 0);
        rt_log("[web] DoVerb %d: window %05x in %05x at (%d,%d %dx%d)\n", verb, g_web.hwnd, parent, x, y, w, h);
        make_webview(g_web.hwnd);
        site_call(5, 0, NULL); /* CanInPlaceActivate */
        site_call(6, 0, NULL); /* OnInPlaceActivate */
    }
    RET(S_OK, 7);
}

/* --- IOleInPlaceObject, IOleInPlaceActiveObject --------------------------------------------------- */
static void sh_IPO_GetWindow(Guest* g) { if (ARG(1)) wr32(ARG(1), g_web.hwnd); RET(g_web.hwnd ? S_OK : E_FAIL, 2); }
static void sh_IPO_ContextSensitiveHelp(Guest* g) { RET(E_NOTIMPL, 2); }
static void sh_IPO_InPlaceDeactivate(Guest* g) { RET(S_OK, 1); }
static void sh_IPO_UIDeactivate(Guest* g) { RET(S_OK, 1); }
/* SetObjectRects(this, pos, clip): the window follows */
static void sh_IPO_SetObjectRects(Guest* g)
{
    uint32_t r = ARG(1);
    if (r)
        rt_log("[web] SetObjectRects (%d,%d)-(%d,%d)\n", rd32(r), rd32(r + 4), rd32(r + 8), rd32(r + 12));
    if (g_web.hwnd && r)
    {
        int32_t x = (int32_t)rd32(r), y = (int32_t)rd32(r + 4);
        uint32_t a[7] = { g_web.hwnd, 0, (uint32_t)x, (uint32_t)y, rd32(r + 8) - (uint32_t)x, rd32(r + 12) - (uint32_t)y, 0x14 };
        guest_call(thunk_for("user32.dll", "SetWindowPos"), 7, a); /* SWP_NOZORDER | SWP_NOACTIVATE */
    }
    RET(S_OK, 3);
}
static void sh_IPO_ReactivateAndUndo(Guest* g) { RET(E_NOTIMPL, 1); }
static void sh_IPAO_TranslateAccelerator(Guest* g) { RET(S_FALSE, 2); }
static void sh_IPAO_OnFrameWindowActivate(Guest* g) { RET(S_OK, 2); }
static void sh_IPAO_OnDocWindowActivate(Guest* g) { RET(S_OK, 2); }
static void sh_IPAO_ResizeBorder(Guest* g) { RET(S_OK, 4); }
static void sh_IPAO_EnableModeless(Guest* g) { RET(S_OK, 2); }

/* --- IPersistStreamInit: no saved state ---------------------------------------------------------- */
static void sh_PSI_GetClassID(Guest* g) { memcpy(GUEST_PTR(ARG(1)), CLSID_WebBrowser.b, 16); RET(S_OK, 2); }
static void sh_PSI_IsDirty(Guest* g) { RET(S_FALSE, 1); }
static void sh_PSI_Load(Guest* g) { RET(S_OK, 2); }
static void sh_PSI_Save(Guest* g) { RET(S_OK, 3); }
static void sh_PSI_GetSizeMax(Guest* g) { if (ARG(1)) wr32(ARG(1), 0), wr32(ARG(1) + 4, 0); RET(S_OK, 2); }
static void sh_PSI_InitNew(Guest* g) { RET(S_OK, 1); }

/* --- events: IConnectionPointContainer, IConnectionPoint, IProvideClassInfo2 ------------------------ */
static void sh_CPC_EnumConnectionPoints(Guest* g) { if (ARG(1)) wr32(ARG(1), 0); RET(E_NOTIMPL, 2); }
static void sh_CPC_FindConnectionPoint(Guest* g)
{
    int ok = is(ARG(1), &DIID_DWebBrowserEvents2);
    if (!ok)
        guid_log("FindConnectionPoint: no", ARG(1));
    if (ARG(2))
        wr32(ARG(2), ok ? g_web.obj + B_CP : 0);
    RET(ok ? S_OK : CONNECT_E_NOCONNECTION, 3);
}
static void sh_CP_GetConnectionInterface(Guest* g) { memcpy(GUEST_PTR(ARG(1)), DIID_DWebBrowserEvents2.b, 16); RET(S_OK, 2); }
static void sh_CP_GetConnectionPointContainer(Guest* g) { if (ARG(1)) wr32(ARG(1), g_web.obj + B_CPC); RET(S_OK, 2); }
/* Advise(this, sink, cookie*): the sink's IDispatch */
static void sh_CP_Advise(Guest* g)
{
    uint32_t iid = gheap_alloc(16, 0), out = gheap_alloc(4, 1);
    memcpy(GUEST_PTR(iid), IID_IDispatch.b, 16);
    uint32_t q[3] = { ARG(1), iid, out };
    guest_call(rd32(rd32(ARG(1))), 3, q);
    g_web.sink = rd32(out) ? rd32(out) : ARG(1);
    gheap_free(iid);
    gheap_free(out);
    if (ARG(2))
        wr32(ARG(2), 1);
    rt_log("[web] events go to %08x\n", g_web.sink);
    RET(S_OK, 3);
}
static void sh_CP_Unadvise(Guest* g) { g_web.sink = 0; RET(S_OK, 2); }
static void sh_CP_EnumConnections(Guest* g) { if (ARG(1)) wr32(ARG(1), 0); RET(E_NOTIMPL, 2); }
static void sh_PCI_GetClassInfo(Guest* g) { if (ARG(1)) wr32(ARG(1), 0); RET(E_NOTIMPL, 2); }
/* GetGUID(this, GUIDKIND_DEFAULT_SOURCE_DISP_IID, guid*) */
static void sh_PCI_GetGUID(Guest* g)
{
    if (ARG(1) != 1)
        RET(0x80070057u, 3); /* E_INVALIDARG */
    memcpy(GUEST_PTR(ARG(2)), DIID_DWebBrowserEvents2.b, 16);
    RET(S_OK, 3);
}

/* --- IOleControl ------------------------------------------------------------------------------------ */
static void sh_OC_GetControlInfo(Guest* g) { if (ARG(1)) wr32(ARG(1) + 4, 0), wr32(ARG(1) + 8, 0), wr32(ARG(1) + 12, 0); RET(S_OK, 2); }
static void sh_OC_OnMnemonic(Guest* g) { RET(S_OK, 2); }
static void sh_OC_OnAmbientPropertyChange(Guest* g) { RET(S_OK, 2); }
static void sh_OC_FreezeEvents(Guest* g) { RET(S_OK, 2); }

/* --- IWebBrowser2 ------------------------------------------------------------------------------------ */
static void sh_WB_get_Document(Guest* g)
{
    if (ARG(1))
        wr32(ARG(1), g_doc_base);
    RET(S_OK, 2);
}
static void sh_WB_get_Busy(Guest* g) { if (ARG(1)) wr16(ARG(1), g_web.busy ? 0xFFFF : 0); RET(S_OK, 2); }
static void sh_WB_get_LocationURL(Guest* g) { if (ARG(1)) wr32(ARG(1), bstr(g_web.url)); RET(S_OK, 2); }
static void sh_WB_get_ReadyState(Guest* g) { if (ARG(1)) wr32(ARG(1), g_web.busy ? 1 : 4); RET(S_OK, 2); } /* LOADING, COMPLETE */
static void sh_WB_get_HWND(Guest* g) { if (ARG(1)) wr32(ARG(1), g_web.hwnd); RET(S_OK, 2); }
static void sh_WB_Stop(Guest* g)
{
    WKWebView* wv = g_web.view;
    if (wv)
        on_main(^{ [wv stopLoading]; });
    RET(S_OK, 1);
}
static void sh_WB_Refresh(Guest* g)
{
    WKWebView* wv = g_web.view;
    if (wv)
        on_main(^{ [wv reloadFromOrigin]; });
    RET(S_OK, 1);
}
static void sh_WB_Refresh2(Guest* g) { sh_WB_Refresh(g); g->esp += 4; }
/* Navigate(this, BSTR url, flags*, target*, post*, headers*) */
static void sh_WB_Navigate(Guest* g) { navigate(nsstr_w(ARG(1))); RET(S_OK, 6); }
/* Navigate2(this, VARIANT* url, flags*, target*, post*, headers*) */
static void sh_WB_Navigate2(Guest* g)
{
    NSString* u = variant_string(ARG(1));
    if (u)
        navigate(u);
    RET(u ? S_OK : 0x80070057u, 6);
}
static void sh_WB_put_Silent(Guest* g) { RET(S_OK, 2); }

/* IDispatch::Invoke(this, dispid, riid, lcid, flags, DISPPARAMS*, result*, excep*, argerr*): the
 * calls MFC's InvokeHelper makes; arguments are in reverse order */
static void sh_WB_Invoke(Guest* g)
{
    uint32_t dispid = ARG(1), dp = ARG(5), result = ARG(6);
    uint32_t args = dp ? rd32(dp) : 0, n = dp ? rd32(dp + 8) : 0;
#define ARGV(i) (args + 16u * (n - 1u - (i))) /* the i-th argument, first = 0 */
    if (result)
        memset(GUEST_PTR(result), 0, 16);
    switch (dispid)
    {
    case 500: /* Navigate2(URL, Flags, TargetFrameName, PostData, Headers) */
    case 0x68: /* Navigate */
    {
        NSString* u = n ? variant_string(ARGV(0)) : nil;
        if (u)
            navigate(u);
        RET(S_OK, 9);
    }
    case 0x69: /* Refresh2(Level) */
    case 0xFFFFFDDA: /* DISPID_REFRESH */
    {
        WKWebView* wv = g_web.view;
        if (wv)
            on_main(^{ [wv reloadFromOrigin]; });
        RET(S_OK, 9);
    }
    case 0x6A: /* Stop */
    {
        WKWebView* wv = g_web.view;
        if (wv)
            on_main(^{ [wv stopLoading]; });
        RET(S_OK, 9);
    }
    case 0xCB: /* Document */
        if (result)
            set_var(result, VT_DISPATCH, g_doc_base);
        RET(S_OK, 9);
    case 0xD3: /* LocationURL */
        if (result)
            set_var(result, VT_BSTR, bstr(g_web.url));
        RET(S_OK, 9);
    case 0xD4: /* Busy */
        if (result)
            set_var(result, VT_BOOL, g_web.busy ? 0xFFFF : 0);
        RET(S_OK, 9);
    case 0xFFFFFDF8: /* DISPID_READYSTATE */
    case 0xFFFFFDF3:
        if (result)
            set_var(result, VT_I4, g_web.busy ? 1 : 4);
        RET(S_OK, 9);
    case 0x22B: /* Silent */
    case 0x227: /* RegisterAsBrowser */
    case 0x228: /* RegisterAsDropTarget */
        RET(S_OK, 9);
    }
    rt_log("[web] Invoke: dispid %#x (flags %u, %u arguments) not served\n", dispid, ARG(4), n);
    RET(DISP_E_MEMBERNOTFOUND, 9);
#undef ARGV
}

/* --- the document side --------------------------------------------------------------------------- */
/* IHTMLDocument2::get_all(this, IHTMLElementCollection**) */
static void sh_DOC_get_all(Guest* g) { if (ARG(1)) wr32(ARG(1), g_coll_base); RET(S_OK, 2); }
static void sh_DOC_get_URL(Guest* g) { if (ARG(1)) wr32(ARG(1), bstr(g_web.url)); RET(S_OK, 2); }

/* IHTMLElementCollection::tags(this, VARIANT tagName (16 bytes by value), IDispatch**): this
 * collection again; get_length and item serve <x-sqexauth> as its only member when the page had one */
static int g_tags_auth;
static void sh_COLL_tags(Guest* g)
{
    NSString* t = variant_string(g->esp + 8);
    g_tags_auth = t && [t caseInsensitiveCompare:@"x-sqexauth"] == NSOrderedSame;
    if (ARG(5))
        wr32(ARG(5), g_coll_base);
    RET(S_OK, 6);
}
static void sh_COLL_get_length(Guest* g) { if (ARG(1)) wr32(ARG(1), g_tags_auth && g_web.auth.found ? 1 : 0); RET(S_OK, 2); }
/* item(this, VARIANT name, VARIANT index, IDispatch**) */
static void sh_COLL_item(Guest* g)
{
    int32_t i = variant_int(g->esp + 8, -1);
    int ok = g_tags_auth && g_web.auth.found && i == 0;
    if (ARG(9))
        wr32(ARG(9), ok ? g_elem_base : 0);
    RET(ok ? S_OK : E_FAIL, 10);
}

/* IHTMLElement::getAttribute(this, BSTR name, LONG flags, VARIANT* value): a string when present,
 * VT_NULL when not */
static void sh_ELEM_getAttribute(Guest* g)
{
    NSString* n = nsstr_w(ARG(1));
    uint32_t v = ARG(3);
    memset(GUEST_PTR(v), 0, 16);
    wr16(v, 1); /* VT_NULL */
    for (int i = 0; i < 4; ++i)
        if ([n caseInsensitiveCompare:[NSString stringWithUTF8String:AUTH_NAMES[i]]] == NSOrderedSame && g_web.auth.has[i])
            set_var(v, VT_BSTR, bstr(g_web.auth.val[i]));
    RET(S_OK, 4);
}

/* --- building the objects ------------------------------------------------------------------------- */
static uint32_t g_vt[16];

static uint32_t create_browser(void)
{
    if (g_web.obj)
        return g_web.obj;
    uint32_t b = gheap_alloc(B_SIZE, 1);
    wr32(b + B_WB, g_vt[0]);
    wr32(b + B_OO, g_vt[1]);
    wr32(b + B_IPO, g_vt[2]);
    wr32(b + B_IPAO, g_vt[3]);
    wr32(b + B_PSI, g_vt[4]);
    wr32(b + B_CPC, g_vt[5]);
    wr32(b + B_CP, g_vt[6]);
    wr32(b + B_PCI, g_vt[7]);
    wr32(b + B_OC, g_vt[8]);
    g_web.obj = b;
    rt_log("[web] WebBrowser control at %08x\n", b);
    return b;
}

static void build_vtables(void)
{
    static const char* const wb[] = { DISPATCH4,
        /* IWebBrowser */ "GoBack", "GoForward", "GoHome", "GoSearch", "Navigate", "Refresh", "Refresh2", "Stop",
        "get_Application", "get_Parent", "get_Container", "get_Document", "get_TopLevelContainer", "get_Type",
        "get_Left", "put_Left", "get_Top", "put_Top", "get_Width", "put_Width", "get_Height", "put_Height",
        "get_LocationName", "get_LocationURL", "get_Busy",
        /* IWebBrowserApp */ "Quit", "ClientToWindow", "PutProperty", "GetProperty", "get_Name", "get_HWND",
        "get_FullName", "get_Path", "get_Visible", "put_Visible", "get_StatusBar", "put_StatusBar", "get_StatusText",
        "put_StatusText", "get_ToolBar", "put_ToolBar", "get_MenuBar", "put_MenuBar", "get_FullScreen", "put_FullScreen",
        /* IWebBrowser2 */ "Navigate2", "QueryStatusWB", "ExecWB", "ShowBrowserBar", "get_ReadyState", "get_Offline",
        "put_Offline", "get_Silent", "put_Silent", "get_RegisterAsBrowser", "put_RegisterAsBrowser",
        "get_RegisterAsDropTarget", "put_RegisterAsDropTarget", "get_TheaterMode", "put_TheaterMode",
        "get_AddressBar", "put_AddressBar", "get_Resizable", "put_Resizable" };
    static const char* const oo[] = { "SetClientSite", "GetClientSite", "SetHostNames", "Close", "SetMoniker",
        "GetMoniker", "InitFromData", "GetClipboardData", "DoVerb", "EnumVerbs", "Update", "IsUpToDate",
        "GetUserClassID", "GetUserType", "SetExtent", "GetExtent", "Advise", "Unadvise", "EnumAdvise",
        "GetMiscStatus", "SetColorScheme" };
    static const char* const ipo[] = { "GetWindow", "ContextSensitiveHelp", "InPlaceDeactivate", "UIDeactivate",
        "SetObjectRects", "ReactivateAndUndo" };
    static const char* const ipao[] = { "GetWindow", "ContextSensitiveHelp", "TranslateAccelerator",
        "OnFrameWindowActivate", "OnDocWindowActivate", "ResizeBorder", "EnableModeless" };
    static const char* const psi[] = { "GetClassID", "IsDirty", "Load", "Save", "GetSizeMax", "InitNew" };
    static const char* const cpc[] = { "EnumConnectionPoints", "FindConnectionPoint" };
    static const char* const cp[] = { "GetConnectionInterface", "GetConnectionPointContainer", "Advise", "Unadvise",
        "EnumConnections" };
    static const char* const pci[] = { "GetClassInfo", "GetGUID" };
    static const char* const oc[] = { "GetControlInfo", "OnMnemonic", "OnAmbientPropertyChange", "FreezeEvents" };
    static const char* const cf[] = { "CreateInstance", "LockServer" };
    static const char* const doc[] = { DISPATCH4, "get_Script", "get_all", "get_body", "get_activeElement",
        "get_images", "get_applets", "get_links", "get_forms", "get_anchors", "put_title", "get_title", "get_scripts",
        "put_designMode", "get_designMode", "get_selection", "get_readyState", "get_frames", "get_embeds", "get_plugins",
        "put_alinkColor", "get_alinkColor", "put_bgColor", "get_bgColor", "put_fgColor", "get_fgColor", "put_linkColor",
        "get_linkColor", "put_vlinkColor", "get_vlinkColor", "get_referrer", "get_location", "get_lastModified",
        "put_URL", "get_URL" };
    static const char* const coll[] = { DISPATCH4, "toString", "put_length", "get_length", "get__newEnum", "item", "tags" };
    static const char* const elem[] = { DISPATCH4, "setAttribute", "getAttribute", "removeAttribute" };
    g_vt[0] = vtable("IWebBrowser2", 7 + 64, wb);
    g_vt[1] = vtable("IOleObject", 3 + 21, oo);
    g_vt[2] = vtable("IOleInPlaceObject", 3 + 6, ipo);
    g_vt[3] = vtable("IOleInPlaceActiveObject", 3 + 7, ipao);
    g_vt[4] = vtable("IPersistStreamInit", 3 + 6, psi);
    g_vt[5] = vtable("IConnectionPointContainer", 3 + 2, cpc);
    g_vt[6] = vtable("IConnectionPoint", 3 + 5, cp);
    g_vt[7] = vtable("IProvideClassInfo2", 3 + 2, pci);
    g_vt[8] = vtable("IOleControl", 3 + 4, oc);
    g_vt[9] = vtable("IClassFactory", 3 + 2, cf);
    g_vt[10] = vtable("IHTMLDocument2", 3 + 4 + 120, doc);
    g_vt[11] = vtable("IHTMLElementCollection", 3 + 4 + 6, coll);
    g_vt[12] = vtable("IHTMLElement", 3 + 4 + 90, elem);
    g_factory_base = gheap_alloc(4, 1);
    wr32(g_factory_base, g_vt[9]);
    g_doc_base = gheap_alloc(4, 1);
    wr32(g_doc_base, g_vt[10]);
    g_coll_base = gheap_alloc(4, 1);
    wr32(g_coll_base, g_vt[11]);
    g_elem_base = gheap_alloc(4, 1);
    wr32(g_elem_base, g_vt[12]);
}

#define IU(iface) { "webbrowser.dll", iface "::QueryInterface", sh_QueryInterface }, \
    { "webbrowser.dll", iface "::AddRef", sh_AddRef }, { "webbrowser.dll", iface "::Release", sh_Release }
#define DISP(iface) { "webbrowser.dll", iface "::GetTypeInfoCount", sh_GetTypeInfoCount }, \
    { "webbrowser.dll", iface "::GetTypeInfo", sh_GetTypeInfo }, { "webbrowser.dll", iface "::GetIDsOfNames", sh_GetIDsOfNames }
#define M(iface, name, fn) { "webbrowser.dll", iface "::" name, fn }

static const ShimDef k_shims[] = {
    IU("IWebBrowser2"), DISP("IWebBrowser2"),
    M("IWebBrowser2", "Invoke", sh_WB_Invoke),
    M("IWebBrowser2", "Navigate", sh_WB_Navigate),
    M("IWebBrowser2", "Navigate2", sh_WB_Navigate2),
    M("IWebBrowser2", "Refresh", sh_WB_Refresh),
    M("IWebBrowser2", "Refresh2", sh_WB_Refresh2),
    M("IWebBrowser2", "Stop", sh_WB_Stop),
    M("IWebBrowser2", "get_Document", sh_WB_get_Document),
    M("IWebBrowser2", "get_Busy", sh_WB_get_Busy),
    M("IWebBrowser2", "get_LocationURL", sh_WB_get_LocationURL),
    M("IWebBrowser2", "get_ReadyState", sh_WB_get_ReadyState),
    M("IWebBrowser2", "get_HWND", sh_WB_get_HWND),
    M("IWebBrowser2", "put_Silent", sh_WB_put_Silent),
    IU("IOleObject"),
    M("IOleObject", "SetClientSite", sh_OO_SetClientSite),
    M("IOleObject", "GetClientSite", sh_OO_GetClientSite),
    M("IOleObject", "SetHostNames", sh_OO_SetHostNames),
    M("IOleObject", "Close", sh_OO_Close),
    M("IOleObject", "DoVerb", sh_OO_DoVerb),
    M("IOleObject", "GetUserClassID", sh_OO_GetUserClassID),
    M("IOleObject", "SetExtent", sh_OO_SetExtent),
    M("IOleObject", "GetExtent", sh_OO_GetExtent),
    M("IOleObject", "Advise", sh_OO_Advise),
    M("IOleObject", "Unadvise", sh_OO_Unadvise),
    M("IOleObject", "GetMiscStatus", sh_OO_GetMiscStatus),
    M("IOleObject", "SetColorScheme", sh_OO_SetColorScheme),
    IU("IOleInPlaceObject"),
    M("IOleInPlaceObject", "GetWindow", sh_IPO_GetWindow),
    M("IOleInPlaceObject", "ContextSensitiveHelp", sh_IPO_ContextSensitiveHelp),
    M("IOleInPlaceObject", "InPlaceDeactivate", sh_IPO_InPlaceDeactivate),
    M("IOleInPlaceObject", "UIDeactivate", sh_IPO_UIDeactivate),
    M("IOleInPlaceObject", "SetObjectRects", sh_IPO_SetObjectRects),
    M("IOleInPlaceObject", "ReactivateAndUndo", sh_IPO_ReactivateAndUndo),
    IU("IOleInPlaceActiveObject"),
    M("IOleInPlaceActiveObject", "GetWindow", sh_IPO_GetWindow),
    M("IOleInPlaceActiveObject", "ContextSensitiveHelp", sh_IPO_ContextSensitiveHelp),
    M("IOleInPlaceActiveObject", "TranslateAccelerator", sh_IPAO_TranslateAccelerator),
    M("IOleInPlaceActiveObject", "OnFrameWindowActivate", sh_IPAO_OnFrameWindowActivate),
    M("IOleInPlaceActiveObject", "OnDocWindowActivate", sh_IPAO_OnDocWindowActivate),
    M("IOleInPlaceActiveObject", "ResizeBorder", sh_IPAO_ResizeBorder),
    M("IOleInPlaceActiveObject", "EnableModeless", sh_IPAO_EnableModeless),
    IU("IPersistStreamInit"),
    M("IPersistStreamInit", "GetClassID", sh_PSI_GetClassID),
    M("IPersistStreamInit", "IsDirty", sh_PSI_IsDirty),
    M("IPersistStreamInit", "Load", sh_PSI_Load),
    M("IPersistStreamInit", "Save", sh_PSI_Save),
    M("IPersistStreamInit", "GetSizeMax", sh_PSI_GetSizeMax),
    M("IPersistStreamInit", "InitNew", sh_PSI_InitNew),
    IU("IConnectionPointContainer"),
    M("IConnectionPointContainer", "EnumConnectionPoints", sh_CPC_EnumConnectionPoints),
    M("IConnectionPointContainer", "FindConnectionPoint", sh_CPC_FindConnectionPoint),
    IU("IConnectionPoint"),
    M("IConnectionPoint", "GetConnectionInterface", sh_CP_GetConnectionInterface),
    M("IConnectionPoint", "GetConnectionPointContainer", sh_CP_GetConnectionPointContainer),
    M("IConnectionPoint", "Advise", sh_CP_Advise),
    M("IConnectionPoint", "Unadvise", sh_CP_Unadvise),
    M("IConnectionPoint", "EnumConnections", sh_CP_EnumConnections),
    IU("IProvideClassInfo2"),
    M("IProvideClassInfo2", "GetClassInfo", sh_PCI_GetClassInfo),
    M("IProvideClassInfo2", "GetGUID", sh_PCI_GetGUID),
    IU("IOleControl"),
    M("IOleControl", "GetControlInfo", sh_OC_GetControlInfo),
    M("IOleControl", "OnMnemonic", sh_OC_OnMnemonic),
    M("IOleControl", "OnAmbientPropertyChange", sh_OC_OnAmbientPropertyChange),
    M("IOleControl", "FreezeEvents", sh_OC_FreezeEvents),
    IU("IClassFactory"),
    M("IClassFactory", "CreateInstance", sh_CF_CreateInstance),
    M("IClassFactory", "LockServer", sh_CF_LockServer),
    IU("IHTMLDocument2"), DISP("IHTMLDocument2"),
    M("IHTMLDocument2", "get_all", sh_DOC_get_all),
    M("IHTMLDocument2", "get_URL", sh_DOC_get_URL),
    IU("IHTMLElementCollection"), DISP("IHTMLElementCollection"),
    M("IHTMLElementCollection", "get_length", sh_COLL_get_length),
    M("IHTMLElementCollection", "item", sh_COLL_item),
    M("IHTMLElementCollection", "tags", sh_COLL_tags),
    IU("IHTMLElement"), DISP("IHTMLElement"),
    M("IHTMLElement", "getAttribute", sh_ELEM_getAttribute),
    { "webbrowser.dll", "ShellEmbedding::WndProc", sh_ShellEmbeddingProc },
    { NULL, NULL, NULL },
};

static const ShimDef k_overrides[] = {
    { "ole32.dll", "CoGetClassObject", sh_CoGetClassObject },
    { NULL, NULL, NULL },
};

/* after ole_init and ui_appkit_init */
void webbrowser_init(void)
{
    guids_init();
    g_prev_get_class_object = thunk_shim("ole32.dll", "CoGetClassObject");
    thunk_register(k_shims);
    thunk_override(k_overrides);
    g_prev_class_hook = ole_class_hook;
    ole_class_hook = class_hook;
}

/* once the guest heap is up (the vtables live there) */
void webbrowser_setup(void)
{
    build_vtables();
    ui_register_class("Shell Embedding", thunk_for("webbrowser.dll", "ShellEmbedding::WndProc"));
}

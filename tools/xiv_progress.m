/* A progress window for the builds tools/xiv_run.py runs (translating, compiling and linking a
 * program can take minutes). It reads lines on stdin and quits when they end:
 *   title <text>                         what is being prepared ("the game")
 *   @progress <step> <done> <total>      translate, compile or link (tools/build_xiv.py, XIV_PROGRESS)
 * Built by xiv_run.py with clang: clang -fobjc-arc -framework AppKit tools/xiv_progress.m */
#import <AppKit/AppKit.h>

static NSTextField* g_title;
static NSTextField* g_step;
static NSTextField* g_detail;
static NSProgressIndicator* g_bar;
static NSDate* g_start;

static NSTextField* label(NSRect r, CGFloat size, BOOL bold)
{
    NSTextField* t = [NSTextField labelWithString:@""];
    t.frame = r;
    t.font = bold ? [NSFont boldSystemFontOfSize:size] : [NSFont systemFontOfSize:size];
    t.lineBreakMode = NSLineBreakByTruncatingTail;
    return t;
}

/* the steps in order, as the player reads them */
static void show(NSString* step, long done, long total)
{
    NSArray* steps = @[ @"translate", @"compile", @"link" ];
    NSDictionary* names = @{ @"translate" : @"Translating the original code",
        @"compile" : @"Compiling for this Mac",
        @"link" : @"Linking" };
    NSDictionary* units = @{ @"translate" : @"functions", @"compile" : @"files", @"link" : @"modules" };
    NSUInteger k = [steps indexOfObject:step];
    g_step.stringValue = [NSString stringWithFormat:@"Step %lu of 3: %@", (unsigned long)(k == NSNotFound ? 1 : k + 1),
                                   names[step] ?: step];
    g_bar.indeterminate = total <= 1;
    if (total > 1)
    {
        g_bar.maxValue = total;
        g_bar.doubleValue = done;
    }
    else
        [g_bar startAnimation:nil];
    long secs = (long)-[g_start timeIntervalSinceNow];
    g_detail.stringValue = total > 1
        ? [NSString stringWithFormat:@"%ld of %ld %@  ·  %ld:%02ld", done, total, units[step] ?: @"", secs / 60, secs % 60]
        : [NSString stringWithFormat:@"%ld:%02ld", secs / 60, secs % 60];
}

int main(void)
{
    @autoreleasepool
    {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        g_start = [NSDate date];
        NSWindow* w = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 440, 132)
                                                  styleMask:NSWindowStyleMaskTitled
                                                    backing:NSBackingStoreBuffered
                                                      defer:NO];
        w.title = @"FINAL FANTASY XIV";
        w.level = NSFloatingWindowLevel;
        NSView* v = w.contentView;
        g_title = label(NSMakeRect(20, 92, 400, 22), 14, YES);
        g_title.stringValue = @"Preparing FINAL FANTASY XIV for this Mac";
        g_step = label(NSMakeRect(20, 66, 400, 18), 12, NO);
        g_step.stringValue = @"Starting…";
        g_bar = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(20, 40, 400, 20)];
        g_bar.indeterminate = YES;
        [g_bar startAnimation:nil];
        g_detail = label(NSMakeRect(20, 14, 400, 18), 11, NO);
        g_detail.textColor = NSColor.secondaryLabelColor;
        for (NSView* x in @[ g_title, g_step, g_bar, g_detail ])
            [v addSubview:x];
        [w center];
        [w makeKeyAndOrderFront:nil];

        /* stdin on a thread of its own; the window follows on the main thread */
        [NSThread detachNewThreadWithBlock:^{
            char line[1024];
            while (fgets(line, sizeof line, stdin))
            {
                NSString* s = [[NSString stringWithUTF8String:line]
                    stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
                dispatch_async(dispatch_get_main_queue(), ^{
                    if ([s hasPrefix:@"title "])
                        g_title.stringValue = [NSString stringWithFormat:@"Preparing %@ for this Mac",
                                                        [s substringFromIndex:6]];
                    else if ([s hasPrefix:@"@progress "])
                    {
                        NSArray* f = [s componentsSeparatedByString:@" "];
                        if (f.count >= 4)
                            show(f[1], [f[2] longLongValue], [f[3] longLongValue]);
                    }
                });
            }
            dispatch_async(dispatch_get_main_queue(), ^{ [NSApp terminate:nil]; });
        }];
        /* the elapsed time ticks on between progress lines */
        [NSTimer scheduledTimerWithTimeInterval:1
                                        repeats:YES
                                          block:^(NSTimer* t) {
                                              long secs = (long)-[g_start timeIntervalSinceNow];
                                              NSString* d = g_detail.stringValue;
                                              NSRange r = [d rangeOfString:@"·  " options:NSBackwardsSearch];
                                              NSString* clock = [NSString stringWithFormat:@"%ld:%02ld", secs / 60, secs % 60];
                                              g_detail.stringValue = r.location != NSNotFound
                                                  ? [[d substringToIndex:r.location + r.length] stringByAppendingString:clock]
                                                  : clock;
                                          }];
        [NSApp run];
    }
    return 0;
}

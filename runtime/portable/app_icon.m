/* The Dock icon of a running program: its exe's own icon, from the image's resources.
 *
 * The native programs are plain binaries (the game runs from build/FINAL FANTASY XIV.app, the
 * launcher programs from build/bin), so macOS would show a generic icon. A Windows exe carries its
 * icon as an RT_GROUP_ICON (a directory of sizes) and one RT_ICON per size; put back together they
 * are a .ico file, which NSImage reads. The icon comes from the player's own exe, at run time.
 */
#import <AppKit/AppKit.h>
#include <stdlib.h>
#include <string.h>

#include "gwin.h"
#include "runtime.h"

#define RT_ICON 3
#define RT_GROUP_ICON 14

/* the offset (from the resource root) an entry of a resource directory leads to, by id; the first
 * entry when id is 0; ~0 when there is none */
static uint32_t res_find(uint32_t root, uint32_t dir, uint32_t id)
{
    uint32_t n = (uint32_t)rd16(dir + 12) + rd16(dir + 14);
    for (uint32_t i = 0; i < n; ++i)
    {
        uint32_t e = dir + 16 + 8 * i, name = rd32(e);
        if (!id || (!(name & 0x80000000u) && name == id))
            return rd32(e + 4);
    }
    return ~0u;
}

/* a resource's data (any language): its guest address and size, or 0 */
static uint32_t res_data(uint32_t base, uint32_t root, uint32_t type, uint32_t name, uint32_t* size)
{
    uint32_t t = res_find(root, root, type);
    if (t == ~0u || !(t & 0x80000000u))
        return 0;
    uint32_t n = res_find(root, root + (t & 0x7FFFFFFFu), name);
    if (n == ~0u || !(n & 0x80000000u))
        return 0;
    uint32_t l = res_find(root, root + (n & 0x7FFFFFFFu), 0);
    if (l == ~0u || (l & 0x80000000u))
        return 0;
    *size = rd32(root + l + 4);
    return base + rd32(root + l);
}

void app_icon_from_image(uint32_t base)
{
    uint32_t rva = rd32(base + rd32(base + 0x3C) + 24 + 96 + 8 * 2);
    if (!rva)
        return;
    uint32_t root = base + rva, gsize = 0;
    uint32_t grp = res_data(base, root, RT_GROUP_ICON, 0, &gsize);
    if (!grp || gsize < 6)
        return;
    uint32_t count = rd16(grp + 4);
    /* .ico: the 6-byte header, a 16-byte entry per image, then the images */
    size_t total = 6 + 16 * (size_t)count;
    uint32_t data[64], sizes[64];
    if (count > 64)
        count = 64;
    for (uint32_t i = 0; i < count; ++i)
    {
        data[i] = res_data(base, root, RT_ICON, rd16(grp + 6 + 14 * i + 12), &sizes[i]);
        total += data[i] ? sizes[i] : 0;
    }
    uint8_t* ico = calloc(1, total);
    ico[2] = 1, ico[4] = (uint8_t)count;
    uint32_t off = 6 + 16 * count;
    for (uint32_t i = 0; i < count; ++i)
    {
        uint8_t* e = ico + 6 + 16 * i;
        memcpy(e, GUEST_PTR(grp + 6 + 14 * i), 8); /* width, height, colours, reserved, planes, bit count */
        uint32_t n = data[i] ? sizes[i] : 0;
        memcpy(e + 8, &n, 4);
        memcpy(e + 12, &off, 4);
        if (n)
            memcpy(ico + off, GUEST_PTR(data[i]), n);
        off += n;
    }
    NSData* d = [NSData dataWithBytesNoCopy:ico length:total freeWhenDone:YES];
    NSImage* image = [[NSImage alloc] initWithData:d];
    if (getenv("XIV_UI_LOG"))
        rt_log("[icon] %u images from the exe, %s\n", count, image ? "in the Dock" : "not readable");
    if (image)
    {
        [NSApp setApplicationIconImage:image];
        [image release];
    }
}

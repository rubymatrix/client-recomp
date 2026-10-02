/* Defaults from the app bundle. See appdefaults.h. */
#include <stdio.h>
#include <string.h>

#include "appdefaults.h"

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>

int app_default(const char* key, char* out, size_t n)
{
    CFBundleRef b = CFBundleGetMainBundle();
    if (!b || !n)
        return 0;
    CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
    CFTypeRef v = CFBundleGetValueForInfoDictionaryKey(b, k);
    CFRelease(k);
    if (v && CFGetTypeID(v) == CFStringGetTypeID())
        return CFStringGetCString((CFStringRef)v, out, (CFIndex)n, kCFStringEncodingUTF8) && out[0];
    if (v && CFGetTypeID(v) == CFNumberGetTypeID())
    {
        long long x = 0;
        CFNumberGetValue((CFNumberRef)v, kCFNumberLongLongType, &x);
        snprintf(out, n, "%lld", x);
        return 1;
    }
    return 0;
}

int app_resource(const char* name, char* out, size_t n)
{
    CFBundleRef b = CFBundleGetMainBundle();
    if (!b)
        return 0;
    CFStringRef s = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
    CFURLRef u = CFBundleCopyResourceURL(b, s, NULL, NULL);
    CFRelease(s);
    if (!u)
        return 0;
    int ok = CFURLGetFileSystemRepresentation(u, true, (UInt8*)out, (CFIndex)n);
    CFRelease(u);
    return ok;
}

#else

int app_default(const char* key, char* out, size_t n)
{
    (void)key, (void)out, (void)n;
    return 0;
}

int app_resource(const char* name, char* out, size_t n)
{
    (void)name, (void)out, (void)n;
    return 0;
}

#endif

int app_bundled(void)
{
    char v[1024]; /* CFStringGetCString fails outright on a short buffer */
    return app_default("FFXIGameFolder", v, sizeof v) || app_default("FFXIServer", v, sizeof v);
}

/* Saved sign-in passwords. See keychain.h. */
#include <string.h>

#include "keychain.h"

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

#define SERVICE "FINAL FANTASY XI sign-in"

/* A query for key's item: class, service and account; the caller adds the rest and releases it. */
static CFMutableDictionaryRef query(const char* key)
{
    CFMutableDictionaryRef q =
        CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFStringRef service = CFStringCreateWithCString(NULL, SERVICE, kCFStringEncodingUTF8);
    CFStringRef account = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
    CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(q, kSecAttrService, service);
    CFDictionarySetValue(q, kSecAttrAccount, account);
    CFRelease(service);
    CFRelease(account);
    return q;
}

int keychain_get(const char* key, char* out, size_t n)
{
    CFMutableDictionaryRef q = query(key);
    CFDictionarySetValue(q, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(q, kSecMatchLimit, kSecMatchLimitOne);
    CFTypeRef data = NULL;
    OSStatus st = SecItemCopyMatching(q, &data);
    CFRelease(q);
    if (st != errSecSuccess || !data)
        return 0;
    CFIndex len = CFDataGetLength(data);
    int ok = len >= 0 && (size_t)len < n;
    if (ok)
    {
        memcpy(out, CFDataGetBytePtr(data), (size_t)len);
        out[len] = 0;
    }
    CFRelease(data);
    return ok;
}

int keychain_set(const char* key, const char* password)
{
    CFDataRef data = CFDataCreate(NULL, (const UInt8*)password, (CFIndex)strlen(password));
    CFMutableDictionaryRef q = query(key);
    CFMutableDictionaryRef change =
        CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(change, kSecValueData, data);
    OSStatus st = SecItemUpdate(q, change);
    if (st == errSecItemNotFound)
    {
        CFDictionarySetValue(q, kSecValueData, data);
        st = SecItemAdd(q, NULL);
    }
    CFRelease(change);
    CFRelease(q);
    CFRelease(data);
    return st == errSecSuccess;
}

void keychain_delete(const char* key)
{
    CFMutableDictionaryRef q = query(key);
    SecItemDelete(q);
    CFRelease(q);
}

#else

int keychain_get(const char* key, char* out, size_t n)
{
    (void)key, (void)out, (void)n;
    return 0;
}

int keychain_set(const char* key, const char* password)
{
    (void)key, (void)password;
    return 0;
}

void keychain_delete(const char* key)
{
    (void)key;
}

#endif

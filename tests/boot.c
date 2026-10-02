/* Boot test for the stand-in FFXiMain.dll, outside the game.
 *
 * Does what FFXi.dll does to bring the game module up: load it, DllGetClassObject for the
 * GameMain class, CreateInstance, Release. That runs the loader, the game's own CRT
 * initialisation (heap, locale, static constructors) and its class factory, all translated,
 * with real Win32 underneath - the first end-to-end use of both bridges.
 *
 * usage: boot.exe <path to stand-in FFXiMain.dll> <path to retail FFXiMain.dll>
 */
#include <windows.h>

#include <stdio.h>
#include <string.h>

/* FFXI GameMain {1027DC46-750D-4B1F-8834-1D25B8BEBAB8} */
static const CLSID CLSID_GameMain = { 0x1027DC46, 0x750D, 0x4B1F, { 0x88, 0x34, 0x1D, 0x25, 0xB8, 0xBE, 0xBA, 0xB8 } };

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        printf("usage: boot.exe <stand-in FFXiMain.dll> <retail FFXiMain.dll>\n");
        return 2;
    }
    SetEnvironmentVariableA("FFXI_RECOMP_RETAIL", argv[2]);
    SetEnvironmentVariableA("FFXI_RECOMP_NO_DIALOG", "1");
    if (argc > 3 && !strcmp(argv[3], "relocate"))
    {
        /* Occupy the preferred base, as the retail launcher's DLLs do, so Windows must relocate the
         * game image and the translation has to follow it. */
        void* hold = VirtualAlloc((void*)0x10000000, 0x01000000, MEM_RESERVE, PAGE_NOACCESS);
        printf("holding 0x10000000: %s\n", hold ? "yes" : "no");
    }
    CoInitialize(NULL);
    HMODULE h = LoadLibraryA(argv[1]);
    if (!h)
    {
        printf("LoadLibrary(%s) failed: %lu\n", argv[1], GetLastError());
        return 1;
    }
    typedef HRESULT(WINAPI * GetClassObject)(REFCLSID, REFIID, void**);
    GetClassObject gco = (GetClassObject)GetProcAddress(h, "DllGetClassObject");
    IClassFactory* cf = NULL;
    HRESULT hr = gco(&CLSID_GameMain, &IID_IClassFactory, (void**)&cf);
    printf("DllGetClassObject(GameMain): %08lx %p\n", hr, (void*)cf);
    if (FAILED(hr) || !cf)
        return 1;
    IUnknown* obj = NULL;
    hr = cf->lpVtbl->CreateInstance(cf, NULL, &IID_IUnknown, (void**)&obj);
    printf("CreateInstance(IUnknown): %08lx %p\n", hr, (void*)obj);
    if (SUCCEEDED(hr) && obj)
        printf("Release -> %lu\n", obj->lpVtbl->Release(obj));
    printf("factory Release -> %lu\n", cf->lpVtbl->Release(cf));
    printf("BOOT %s\n", SUCCEEDED(hr) ? "OK" : "FAILED");
    return SUCCEEDED(hr) ? 0 : 1;
}

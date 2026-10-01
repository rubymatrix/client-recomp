/* The shader translator (runtime/portable/sm_msl.c) over a folder of D3D9 shaders
 * (tools/extract_shaders.py): each .bin becomes a .metal beside it, which tools/sm_test.sh
 * compiles with Apple's Metal compiler. Prints the translation failures.
 *
 *   build/sm_test <folder> [--opts]   (--opts: also each pixel shader with alpha test and shadow samplers)
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sm_msl.h"

int main(int argc, char** argv)
{
    if (argc < 2)
        return 2;
    DIR* d = opendir(argv[1]);
    if (!d)
        return perror(argv[1]), 2;
    static char out[1 << 20];
    struct dirent* e;
    unsigned ok = 0, bad = 0;
    while ((e = readdir(d)))
    {
        size_t n = strlen(e->d_name);
        if (n < 4 || strcmp(e->d_name + n - 4, ".bin"))
            continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", argv[1], e->d_name);
        FILE* f = fopen(path, "rb");
        if (!f)
            continue;
        static uint32_t tok[1 << 18];
        size_t nt = fread(tok, 4, sizeof tok / 4, f);
        fclose(f);
        SmInfo info;
        SmOpts opts = { 0 };
        if (argc > 2)
            opts.alpha_func = 5, opts.shadow_mask = 0;
        char err[256];
        size_t len = sm_translate(tok, nt, &opts, &info, out, sizeof out, err, sizeof err);
        if (!len)
        {
            printf("FAIL %s: %s\n", e->d_name, err);
            bad++;
            continue;
        }
        snprintf(path + strlen(path) - 4, 8, ".metal");
        f = fopen(path, "w");
        fwrite(out, 1, len, f);
        fclose(f);
        ok++;
    }
    closedir(d);
    printf("translated %u, failed %u\n", ok, bad);
    return bad ? 1 : 0;
}

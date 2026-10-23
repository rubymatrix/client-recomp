/* Unpack FINAL FANTASY XIV from its retail disc's InstallShield cabinets into an install folder, as
 * the disc's own setup would lay it out, without running any of the disc's code.
 *
 *   xiv_unpack <disc>/data/data1.hdr <install folder>
 *
 * The disc's setup (InstallShield 2009, data1.hdr + data1..3.cab) installs three file groups:
 *   the group holding ffxivboot.exe  -> the install folder (boot, updater, config, *.ver)
 *   NewComponent1                    -> client\  (scripts, character, cutscene, effect files)
 *   NewComponent2                    -> data\    (the hashed data folders, 00\0B\00\00.DAT ...)
 * Everything else on the disc is InstallShield's own support files and DirectX.
 *
 * Files already in place at their size are kept, so an interrupted unpack resumes. libunshield
 * checks each file's MD5 from the cabinet header as it writes it; a mismatch stops the unpack.
 * Progress goes to stdout as "progress <bytes done> <bytes total> <files done> <files total>"
 * lines, for the installer to show. Built by tools/xiv_install.py with third_party/unshield.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "libunshield.h"

typedef struct
{
    UnshieldFileGroup* group;
    const char* sub; /* folder under the install, "" for the install itself */
} Target;

static int mkdirs(const char* path)
{
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char* p = tmp + 1; *p; ++p)
        if (*p == '/')
        {
            *p = 0;
            if (mkdir(tmp, 0755) && errno != EEXIST)
                return 0;
            *p = '/';
        }
    return !mkdir(tmp, 0755) || errno == EEXIST;
}

/* the host path of file i of a group: install/sub/directory/name, with the cabinet's backslashes */
static void target_path(Unshield* u, int i, const char* install, const char* sub, char* out, size_t n)
{
    const char* dir = unshield_directory_name(u, unshield_file_directory(u, i));
    snprintf(out, n, "%s%s%s%s%s/%s", install, sub[0] ? "/" : "", sub, dir && dir[0] ? "/" : "", dir ? dir : "",
        unshield_file_name(u, i));
    for (char* p = out + strlen(install); *p; ++p)
        if (*p == '\\')
            *p = '/';
}

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        fprintf(stderr, "usage: xiv_unpack <disc>/data/data1.hdr <install folder>\n");
        return 2;
    }
    unshield_set_log_level(0);
    Unshield* u = unshield_open(argv[1]);
    if (!u)
    {
        fprintf(stderr, "xiv_unpack: %s is not an InstallShield cabinet header\n", argv[1]);
        return 1;
    }
    const char* install = argv[2];

    Target targets[3] = { { NULL, "" }, { unshield_file_group_find(u, "NewComponent1"), "client" },
        { unshield_file_group_find(u, "NewComponent2"), "data" } };
    for (int g = 0; g < unshield_file_group_count(u) && !targets[0].group; ++g)
    {
        UnshieldFileGroup* fg = unshield_file_group_get(u, g);
        for (unsigned i = fg->first_file; i <= fg->last_file; ++i)
            if (unshield_file_is_valid(u, (int)i) && !strcasecmp(unshield_file_name(u, (int)i), "ffxivboot.exe"))
                targets[0].group = fg;
    }
    if (!targets[0].group || !targets[1].group || !targets[2].group)
    {
        fprintf(stderr, "xiv_unpack: this disc does not have FINAL FANTASY XIV's file groups\n");
        unshield_close(u);
        return 1;
    }

    unsigned long long total = 0, done = 0;
    unsigned files = 0, files_done = 0;
    for (int t = 0; t < 3; ++t)
        for (unsigned i = targets[t].group->first_file; i <= targets[t].group->last_file; ++i)
            if (unshield_file_is_valid(u, (int)i))
                total += unshield_file_size(u, (int)i), files++;
    printf("progress 0 %llu 0 %u\n", total, files);
    fflush(stdout);

    char path[4096], dir[4096];
    unsigned long long last = 0;
    for (int t = 0; t < 3; ++t)
        for (unsigned i = targets[t].group->first_file; i <= targets[t].group->last_file; ++i)
        {
            if (!unshield_file_is_valid(u, (int)i))
                continue;
            size_t size = unshield_file_size(u, (int)i);
            target_path(u, (int)i, install, targets[t].sub, path, sizeof path);
            struct stat st;
            if (stat(path, &st) || (size_t)st.st_size != size)
            {
                snprintf(dir, sizeof dir, "%s", path);
                *strrchr(dir, '/') = 0;
                if (!mkdirs(dir) || !unshield_file_save(u, (int)i, path))
                {
                    fprintf(stderr, "xiv_unpack: could not unpack %s\n", path);
                    unlink(path);
                    unshield_close(u);
                    return 1;
                }
            }
            done += size, files_done++;
            if (done - last >= (64ull << 20) || files_done == files)
            {
                printf("progress %llu %llu %u %u\n", done, total, files_done, files);
                fflush(stdout);
                last = done;
            }
        }
    unshield_close(u);
    return 0;
}

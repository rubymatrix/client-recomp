/* host/datui.c against the install's DATs: the window themes and the lobby sheet parse to the
 * byte, their images decode, and the layouts render. A small software rasterizer draws the quads
 * into TGA files (build/datui/) to look at: every window theme, the lobby's title screen as the
 * sheet lays it out, and a sign-in mock-up of the two.
 *
 *   build/datui_test --game <FINAL FANTASY XI folder> [--out folder] */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "datui.h"

static int failures;

static void check(int ok, const char* what)
{
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        failures++;
}

/* ---- images by name, from the DATs loaded so far ---- */

typedef struct Named
{
    char name[9];
    DatImage img;
} Named;

static Named g_images[64];
static unsigned g_nimages;

static void load_images(const DatFile* f, const char* category, const char* const* names)
{
    for (; *names; ++names)
    {
        if (g_nimages == sizeof g_images / sizeof *g_images)
            return;
        Named* n = &g_images[g_nimages];
        if (dat_image(f, category, *names, &n->img))
        {
            snprintf(n->name, sizeof n->name, "%s", *names);
            g_nimages++;
        }
    }
}

static const DatImage* image(const char* name)
{
    /* the newest wins: a theme's pieces replace the last theme's */
    for (unsigned i = g_nimages; i-- > 0;)
        if (!strcmp(g_images[i].name, name))
            return &g_images[i].img;
    return NULL;
}

static void drop_images(unsigned keep)
{
    while (g_nimages > keep)
        dat_image_free(&g_images[--g_nimages].img);
}

/* ---- the rasterizer: axis-aligned quads, nearest texels (wrapping), vertex colours bilinear,
 * source-over blending ---- */

typedef struct Canvas
{
    int w, h;
    uint8_t* rgba;
} Canvas;

static Canvas canvas(int w, int h, uint8_t r, uint8_t g, uint8_t b)
{
    Canvas c = { w, h, malloc((size_t)w * h * 4) };
    for (int i = 0; i < w * h; ++i)
    {
        c.rgba[i * 4 + 0] = r;
        c.rgba[i * 4 + 1] = g;
        c.rgba[i * 4 + 2] = b;
        c.rgba[i * 4 + 3] = 255;
    }
    return c;
}

static void draw(Canvas* c, const UiQuad* q, unsigned n)
{
    for (unsigned k = 0; k < n; ++k, ++q)
    {
        const DatImage* img = image(q->image);
        if (!img || q->x1 <= q->x0 || q->y1 <= q->y0)
            continue;
        int x0 = (int)floorf(q->x0), x1 = (int)ceilf(q->x1), y0 = (int)floorf(q->y0), y1 = (int)ceilf(q->y1);
        for (int y = y0 < 0 ? 0 : y0; y < y1 && y < c->h; ++y)
            for (int x = x0 < 0 ? 0 : x0; x < x1 && x < c->w; ++x)
            {
                float s = (x + 0.5f - q->x0) / (q->x1 - q->x0), t = (y + 0.5f - q->y0) / (q->y1 - q->y0);
                if (s < 0 || s >= 1 || t < 0 || t >= 1)
                    continue;
                int u = (int)floorf(q->u0 + s * (q->u1 - q->u0)), v = (int)floorf(q->v0 + t * (q->v1 - q->v0));
                u = ((u % (int)img->w) + (int)img->w) % (int)img->w;
                v = ((v % (int)img->h) + (int)img->h) % (int)img->h;
                const uint8_t* tx = img->rgba + ((size_t)v * img->w + (size_t)u) * 4;
                float src[4];
                for (int i = 0; i < 4; ++i)
                {
                    float top = q->color[0][i] + s * (q->color[1][i] - q->color[0][i]);
                    float bot = q->color[2][i] + s * (q->color[3][i] - q->color[2][i]);
                    src[i] = tx[i] / 255.0f * ((top + t * (bot - top)) / 255.0f);
                }
                uint8_t* d = c->rgba + ((size_t)y * c->w + x) * 4;
                for (int i = 0; i < 3; ++i)
                    d[i] = (uint8_t)(src[i] * 255 * src[3] + d[i] * (1 - src[3]) + 0.5f);
            }
    }
}

static int write_tga(const Canvas* c, const char* path)
{
    FILE* fp = fopen(path, "wb");
    if (!fp)
        return 0;
    uint8_t hdr[18] = { 0, 0, 2 };
    hdr[12] = (uint8_t)c->w;
    hdr[13] = (uint8_t)(c->w >> 8);
    hdr[14] = (uint8_t)c->h;
    hdr[15] = (uint8_t)(c->h >> 8);
    hdr[16] = 32;
    hdr[17] = 0x28; /* top-down, 8 alpha bits */
    fwrite(hdr, 1, sizeof hdr, fp);
    for (int i = 0; i < c->w * c->h; ++i)
    {
        const uint8_t* p = c->rgba + i * 4;
        uint8_t bgra[4] = { p[2], p[1], p[0], p[3] };
        fwrite(bgra, 1, 4, fp);
    }
    return !fclose(fp);
}

/* ---- the checks ---- */

static const char* const THEME_IMAGES[] = { "newtex", "corner", "hfr1", "vfr1", NULL };

static int theme(const char* game, int n, DatFile* f, DatSheet* s)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/ROM/0/%d.DAT", game, 13 + n);
    if (!dat_read(path, f))
        return 0;
    if (!dat_sheet(f, "win00", s))
    {
        dat_free(f);
        return 0;
    }
    load_images(f, "menu", THEME_IMAGES);
    return 1;
}

int main(int argc, char** argv)
{
    const char *game = NULL, *outdir = "build/datui";
    for (int i = 1; i + 1 < argc; i += 2)
    {
        if (!strcmp(argv[i], "--game"))
            game = argv[i + 1];
        else if (!strcmp(argv[i], "--out"))
            outdir = argv[i + 1];
    }
    if (!game)
    {
        fprintf(stderr, "usage: datui_test --game <FINAL FANTASY XI folder> [--out folder]\n");
        return 2;
    }
    char path[1024];

    /* the lobby: its sheet and the images the title screen uses */
    DatFile lobby, fonts;
    snprintf(path, sizeof path, "%s/ROM/0/2.DAT", game);
    check(dat_read(path, &lobby), "ROM/0/2.DAT reads");
    snprintf(path, sizeof path, "%s/ROM/0/1.DAT", game);
    check(dat_read(path, &fonts), "ROM/0/1.DAT reads");
    DatSheet lob = { 0 };
    check(dat_sheet(&lobby, "lobbyps2", &lob) && lob.nsprites == 227, "lobbyps2: 227 sprites");
    check(lob.nsprites > 1 && lob.sprites[1].nparts == 27, "lobbyps2 sprite 1 (the title screen): 27 parts");
    static const char* const LOBBY_IMAGES[] = { "xilogo", "otp", "chmkfnt", "lbfontp", NULL };
    static const char* const MENU_IMAGES[] = { "buttonto", "menufont", "yubi", NULL };
    load_images(&lobby, "menu", LOBBY_IMAGES);
    load_images(&fonts, "menu", MENU_IMAGES);
    const DatImage* logo = image("xilogo");
    check(logo && logo->w == 256 && logo->h == 256, "xilogo decodes, 256x256 (DXT)");
    check(image("buttonto") != NULL, "buttonto decodes");
    check(image("menufont") != NULL, "menufont decodes");
    {
        DatImage moji;
        UiFont font;
        int ok = dat_image(&fonts, "font", "moji", &moji) && ui_font(&moji, &font);
        check(ok, "font/moji decodes (1024x2048) and measures");
        check(ok && font.advance['W' - ' '] > font.advance['i' - ' '] && font.advance[0] > 0, "  proportional: W wider than i");
        UiQuad t[16];
        check(ok && ui_text(&font, "Sign in", 0, 0, 1, (const uint8_t[4]){ 255, 255, 255, 255 }, t, 16) == 6,
            "  'Sign in': a quad a glyph, none for the space");
        if (ok)
            dat_image_free(&moji);
    }
    unsigned lobby_images = g_nimages;

    /* every window theme */
    Canvas all = canvas(4 * 300 + 20, 2 * 190 + 20, 40, 60, 40);
    UiQuad q[UI_WINDOW_QUADS + 64];
    for (int t = 1; t <= 8; ++t)
    {
        DatFile f;
        DatSheet s;
        char what[96];
        snprintf(what, sizeof what, "theme %d: win00 parses, 10 one-part sprites, its images decode", t);
        int ok = theme(game, t, &f, &s);
        const DatImage* bg = image("newtex");
        const DatImage* corner = image("corner");
        ok = ok && s.nsprites == 10 && bg && bg->w == 128 && corner && corner->w == 32 && image("hfr1") && image("vfr1");
        check(ok, what);
        if (ok)
        {
            unsigned n = ui_window(&s, 20 + (t - 1) % 4 * 300.0f, 20 + (t - 1) / 4 * 190.0f, 280, 170, 1, 0, q);
            check(n == UI_WINDOW_QUADS, "  lays out as a window");
            draw(&all, q, n);
            dat_sheet_free(&s);
            dat_free(&f);
        }
        drop_images(lobby_images);
    }

    /* the title screen, as the sheet lays it out (512x448 around the centre), at 2x */
    Canvas title = canvas(1024, 896, 0, 0, 0);
    if (lob.nsprites > 1)
    {
        unsigned n = ui_sprite(&lob.sprites[1], 512, 448, 2, q, 64);
        check(n >= 20, "title screen: its parts become quads");
        draw(&title, q, n);
    }

    /* a sign-in mock-up: the title's logo, then a theme 1 window with fields in theme 3 */
    Canvas mock = canvas(1280, 720, 0, 0, 0);
    if (lob.nsprites > 1)
    {
        unsigned n = ui_sprite(&lob.sprites[1], 640, 330, 1.25f, q, 64);
        /* the logo and copyright only: drop the menu buttons */
        unsigned keep = 0;
        for (unsigned i = 0; i < n; ++i)
            if (strcmp(q[i].image, "buttonto"))
                q[keep++] = q[i];
        draw(&mock, q, keep);
    }
    DatFile f1, f3;
    DatSheet s1, s3;
    if (theme(game, 1, &f1, &s1))
    {
        draw(&mock, q, ui_window(&s1, 700, 220, 480, 270, 1, 1, q));
        if (theme(game, 3, &f3, &s3))
        {
            for (int i = 0; i < 3; ++i)
                draw(&mock, q, ui_window(&s3, 860, 262.0f + i * 62, 290, 44, 0.5f, 0, q));
            dat_sheet_free(&s3);
            dat_free(&f3);
        }
        dat_sheet_free(&s1);
        dat_free(&f1);
    }

    char p1[1100], p2[1100], p3[1100];
    snprintf(p1, sizeof p1, "%s/windows.tga", outdir);
    snprintf(p2, sizeof p2, "%s/title.tga", outdir);
    snprintf(p3, sizeof p3, "%s/signin-mock.tga", outdir);
    check(write_tga(&all, p1) && write_tga(&title, p2) && write_tga(&mock, p3), "writes the renders");
    printf("renders: %s %s %s\n", p1, p2, p3);

    free(all.rgba);
    free(title.rgba);
    free(mock.rgba);
    drop_images(0);
    dat_sheet_free(&lob);
    dat_free(&lobby);
    dat_free(&fonts);
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures != 0;
}

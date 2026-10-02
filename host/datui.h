/* The game's own UI art, read straight from the install's DATs: images (window frames, fonts, the
 * lobby's logo and buttons), the sprite sheets that cut them up, and the window frame laid out at
 * any size. For screens host64 draws before the game runs (sign-in), in the game's look.
 *
 * A DAT is a list of chunks: name[4], u32 (type in bits 0-6, size in 16-byte units in bits 7-25),
 * 8 bytes of padding, then the payload.
 *   type 0x20, an image: u8 flag, category[8] ("menu    "), name[8], a BITMAPINFOHEADER, then
 *     DXT1/3/5 (the FourCC stored reversed, the blocks 12 bytes after it), 8-bit (a BGRA palette
 *     of 256, then indices) or 32-bit BGRA. Rows top-down. Alpha is PS2-style: 0x80 is opaque.
 *   type 0x31, a sprite sheet: name[16], u8 textures, that many category+name[16], u16 sprites;
 *     each sprite is u8 parts, and each part is 45 bytes then the category+name[16] of its image:
 *       i16 x, y per vertex (TL, TR, BL, BR), i16 uv width, height, u, v (texels, wrapping),
 *       u8 pad, RGBA per vertex (0x80 = 1.0: the lobby's buttons' 96,96,127 is the menus' blue),
 *       u8 mode[4]
 *   type 0x30, a menu layout (window positions, which sheet sprites they use): not read yet.
 *
 * Text is the game's "font/moji" (1.DAT): 16-pixel cells, 64 a row, in character-code order from
 * the space (ASCII, then more), each glyph at the left of its cell; its advance is measured here
 * from its alpha.
 *
 * Where things are (ROM/0): 1.DAT the fonts and menu icons, 2.DAT the lobby ("lobbyps2": 512x448
 * screen space, centred), 14.DAT..21.DAT window themes 1-8 ("win00"). */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct DatFile
{
    uint8_t* data;
    size_t size;
} DatFile;

/* The whole file into memory. 0 when it cannot be read. */
int dat_read(const char* host_path, DatFile* f);
void dat_free(DatFile* f);

typedef struct DatImage
{
    uint32_t w, h;
    uint8_t* rgba; /* w * h * 4, RGBA, alpha 0..255 (the file's 0..0x80, doubled) */
} DatImage;

/* The image called category/name (case-insensitive, without the space padding: "menu", "corner"),
 * decoded. 0 when there is none or its format is one not read here. */
int dat_image(const DatFile* f, const char* category, const char* name, DatImage* out);
void dat_image_free(DatImage* img);

typedef struct DatPart
{
    int16_t x[4], y[4];         /* TL, TR, BL, BR */
    int16_t uw, uh, u, v;       /* the texels: u, v to u + uw, v + uh */
    uint8_t color[4][4];        /* per vertex, R G B A, 0x80 = 1.0 */
    uint8_t mode[4];
    char category[9], name[9];  /* the image, lowercase, unpadded */
} DatPart;

typedef struct DatSprite
{
    uint32_t nparts;
    DatPart* parts;
} DatSprite;

typedef struct DatSheet
{
    char name[9];
    uint32_t nsprites;
    DatSprite* sprites;
    DatPart* all; /* every part, sprite by sprite */
} DatSheet;

/* The sprite sheet called name ("win00", "lobbyps2"). 0 when there is none or it does not parse. */
int dat_sheet(const DatFile* f, const char* name, DatSheet* out);
void dat_sheet_free(DatSheet* s);

/* One textured quad, axis-aligned: the unit the screens draw. */
typedef struct UiQuad
{
    float x0, y0, x1, y1;   /* pixels */
    float u0, v0, u1, v1;   /* texels of the image; past its size they wrap */
    uint8_t color[4][4];    /* RGBA per vertex (TL, TR, BL, BR), 255 = 1.0: multiplies the texel */
    const char* image;      /* the DatPart's name */
    float skew;             /* the top edge this many pixels right of the bottom's: italics */
    uint8_t shape;          /* 1: the image gives only the shape (its alpha); the colours are the colour */
    uint8_t blend;          /* UI_BLEND_*: how it lands on what is under it */
} UiQuad;

/* How a quad lands, as the sheets' modes ([1,0,1,1], [1,0,2,1], [1,2,0,1]) have the PS2's blend:
 * over what is there by its alpha, added to it, or taken from it */
enum
{
    UI_BLEND_ALPHA,
    UI_BLEND_ADD,
    UI_BLEND_SUBTRACT,
};

/* A sheet part's blend, from its mode */
int ui_part_blend(const DatPart* p);

enum
{
    UI_WINDOW_QUADS = 9,
};

/* A window of a theme's "win00" sheet at x, y, w by h pixels, with the frame's pieces scale pixels
 * per texel: the background (sprite 0 or, focused, 9: the theme's own shading) and the eight frame
 * pieces (sprites 1-8: corners 24 texels square, edges 5 thick), the edges and background tiling
 * rather than stretching. The frame is drawn untinted: the sheet's colours on it are not how the
 * game draws it. The number of quads written to out[UI_WINDOW_QUADS]; 0 if the sheet is not a
 * window theme. */
unsigned ui_window(const DatSheet* win00, float x, float y, float w, float h, float scale, int focused, UiQuad* out);

/* A sheet sprite's parts as quads, its coordinates scaled by scale and moved to origin (the
 * lobby's are relative to the screen's centre). Parts that are not axis-aligned rectangles are
 * skipped. The number written, at most max. */
unsigned ui_sprite(const DatSprite* s, float origin_x, float origin_y, float scale, UiQuad* out, unsigned max);

/* ASCII text in the game's font. */
typedef struct UiFont
{
    const char* image; /* the atlas's name in the quads: "moji" */
    uint8_t cell;      /* 16 */
    uint8_t advance[95]; /* ' '..'~': pixels to the next glyph at scale 1 */
} UiFont;

/* The metrics of a decoded "moji". 0 if it is not that atlas. */
int ui_font(const DatImage* moji, UiFont* f);
float ui_text_width(const UiFont* f, const char* s, float scale);
/* s at x, y (the top of its line, font->cell * scale tall) in rgba: one quad a glyph, at most max.
 * Characters outside ASCII are skipped. */
unsigned ui_text(const UiFont* f, const char* s, float x, float y, float scale, const uint8_t rgba[4], UiQuad* out,
    unsigned max);

/* The menus' own small bold italic letters, "font/font" (1.DAT): what the Config pages' captions and
 * buttons are made of. A glyph is at u, v, w by h in the texture, and sits v - top below the top of
 * its line; each advances its width less one, a space (and a character it lacks) 4. */
typedef struct UiGlyph
{
    char c;
    uint8_t u, v, w, h, top;
} UiGlyph;

const UiGlyph* ui_menu_glyph(char c);
int ui_menu_text_width(const char* s); /* texels */

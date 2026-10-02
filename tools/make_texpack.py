"""Texture pack entries for host64 --textures: a high-resolution replacement for one of the game's
own textures, which the renderer swaps in whenever the game uploads that texture.

  FFXI_TEXLOG=1 FFXI_TEXDUMP=<folder> build/host64 ...     the hash and size of each texture the game
                                                            uploads, and its bytes in <folder>
  python3 tools/make_texpack.py --hash 7d9fc1e8a2b894d2 --size 1024x2048 --png <moji_4x.png> --additive --out <folder>

The game does not upload its DAT images as they are stored (the font, font/moji in ROM/0/1.DAT, goes
up with its alpha widened to 0-255 and its colours brightened), so an entry is keyed by what the
renderer sees: the FNV-1a hash of the texture's first level as uploaded (d3d8.c texture_replacement),
and its size. The entry is <folder>/<hash>_<w>x<h>.dds, DXT5 with a full chain of mipmaps.

The replacement keeps the original's layout at any whole scale - the game's texture coordinates are
fractions of the texture, so they still land on the same art - and its colours and alpha are those of
the upload (a dump from FFXI_TEXDUMP shows them).

--pad N [--pad-min M] names the entry <hash>_<w>x<h>_padN[minM].dds: the renderer draws each glyph quad (a single quad
of transformed vertices) with it N texels wider on both sides. FFXI cuts its italic name font at
upright boxes, so the lean and the outline are lost at the ends of a name; the replacement's ink
must then stay within N texels of each glyph's box. With M, only quads at least M texels tall
(the name font, not the small menu font packed beside it).

--glyphs names a table of ink boxes (x0 x1 y0 y1 a line, in the original texture's texels), written
beside the entry as <entry>.glyphs. FFXI draws each glyph of its name and small menu fonts as a
fixed-width cell from where the glyph starts, which takes in the edge of the glyph beside it in the
sheet and cuts the glyph's own lean; with a table the renderer draws each such quad over the ink box
of the glyph that starts in its cell instead.

--additive is for a texture the game draws with ONE/ONE blending (its text, FFXI_DRAWLOG shows the
blend): only colour reaches the screen, so the colour is multiplied by alpha - a soft edge fades to
black, not to a hard edge - and every texel's colour counts in the mipmaps and the compression.

Needs numpy."""
import argparse
import os
import struct
import sys
import zlib

import numpy as np


# --- PNG in -------------------------------------------------------------------------------------------
def read_png(path):
    d = open(path, 'rb').read()
    if d[:8] != b'\x89PNG\r\n\x1a\n':
        sys.exit(f'{path}: not a PNG')
    off, idat = 8, b''
    while off < len(d):
        n = struct.unpack('>I', d[off:off + 4])[0]
        kind = d[off + 4:off + 8]
        if kind == b'IHDR':
            w, h, depth, ctype, _, _, interlace = struct.unpack('>IIBBBBB', d[off + 8:off + 21])
        elif kind == b'IDAT':
            idat += d[off + 8:off + 8 + n]
        off += 12 + n
    if depth != 8 or ctype not in (2, 6) or interlace:
        sys.exit(f'{path}: needs 8-bit RGB or RGBA, not interlaced')
    bpp = 4 if ctype == 6 else 3
    raw = np.frombuffer(zlib.decompress(idat), np.uint8).reshape(h, w * bpp + 1)
    out = np.zeros((h, w * bpp), np.uint8)
    prev = np.zeros(w * bpp, np.int32)
    for y in range(h):
        f, line = raw[y, 0], raw[y, 1:].astype(np.int32)
        if f == 0:
            cur = line
        elif f == 2:
            cur = (line + prev) & 255
        else:  # sub, average and Paeth depend on the pixel to the left: one pixel at a time
            cur = np.zeros_like(line)
            for x in range(0, w * bpp, bpp):
                left = cur[x - bpp:x] if x else np.zeros(bpp, np.int32)
                up = prev[x:x + bpp]
                ul = prev[x - bpp:x] if x else np.zeros(bpp, np.int32)
                if f == 1:
                    pr = left
                elif f == 3:
                    pr = (left + up) // 2
                else:
                    pa, pb, pc = abs(up - ul), abs(left - ul), abs(left + up - 2 * ul)
                    pr = np.where((pa <= pb) & (pa <= pc), left, np.where(pb <= pc, up, ul))
                cur[x:x + bpp] = (line[x:x + bpp] + pr) & 255
        out[y] = cur
        prev = cur
    img = out.reshape(h, w, bpp)
    if bpp == 3:
        img = np.concatenate([img, np.full((h, w, 1), 255, np.uint8)], 2)
    return img


# --- mipmaps and DXT5 ---------------------------------------------------------------------------------
def mip_chain(rgba, additive=False):
    """Each level half the last, averaged with alpha as the weight (so the clear texels' colour does
    not bleed into the edges) - or plainly, for additive, whose colour is already weighted."""
    levels = [rgba]
    cur = rgba.astype(np.float32)
    while cur.shape[0] > 1 or cur.shape[1] > 1:
        h, w = cur.shape[:2]
        h2, w2 = max(1, h // 2), max(1, w // 2)
        c = cur[:h2 * 2 if h > 1 else 1, :w2 * 2 if w > 1 else 1]
        fy, fx = (2 if h > 1 else 1), (2 if w > 1 else 1)
        c = c.reshape(h2, fy, w2, fx, 4)
        a = c[..., 3:4]
        if additive:
            rgb = c[..., :3].mean((1, 3))
        else:
            rgb = (c[..., :3] * a).sum((1, 3)) / np.maximum(a.sum((1, 3)), 1e-6)
        alpha = a.mean((1, 3))
        cur = np.concatenate([rgb, alpha], -1)
        levels.append(np.clip(cur + 0.5, 0, 255).astype(np.uint8))
    return levels


def to565(c):
    c = np.clip(c, 0, 255).astype(np.int32)
    return (c[..., 0] * 31 + 127) // 255 << 11 | (c[..., 1] * 63 + 127) // 255 << 5 | (c[..., 2] * 31 + 127) // 255


def from565(v):
    return np.stack([(v >> 11 & 31) * 255 // 31, (v >> 5 & 63) * 255 // 63, (v & 31) * 255 // 31], -1).astype(np.float32)


def dxt5(rgba, additive=False):
    """Blocks for one level: alpha from its block's extremes (8 steps), colour along the line between
    the extremes of the block's opaque texels (the clear ones' colour is never seen - except additive,
    where every texel's is)."""
    h, w = rgba.shape[:2]
    H, W = (h + 3) // 4 * 4, (w + 3) // 4 * 4
    img = np.zeros((H, W, 4), np.float32)
    img[:h, :w] = rgba
    if H > h:
        img[h:, :w] = img[h - 1:h, :w]
    if W > w:
        img[:, w:] = img[:, w - 1:w]
    b = img.reshape(H // 4, 4, W // 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(-1, 16, 4)
    n = len(b)
    # alpha
    a = b[:, :, 3]
    a0, a1 = a.max(1).round(), a.min(1).round()
    same = a0 == a1
    a0 = np.where(same, np.minimum(a0 + 1, 255), a0)
    a1 = np.where(same & (a0 == 255), 254, a1)
    steps = np.stack([a0, a1] + [((7 - i) * a0 + i * a1) / 7 for i in range(1, 7)], 1)  # N,8
    ai = np.abs(a[:, :, None] - steps[:, None, :]).argmin(2).astype(np.uint64)  # N,16
    abits = np.zeros(n, np.uint64)
    for i in range(16):
        abits |= ai[:, i] << np.uint64(3 * i)
    # colour
    rgb = b[:, :, :3]
    seen = a > (-1 if additive else 8)
    anyseen = seen.any(1)
    wgt = np.where(seen | ~anyseen[:, None], 1.0, 0.0)[:, :, None]
    lo = np.where(wgt > 0, rgb, 1e9).min(1)
    hi = np.where(wgt > 0, rgb, -1e9).max(1)
    inset = (hi - lo) / 16
    c0, c1 = to565(hi - inset), to565(lo + inset)
    swap = c0 < c1
    c0, c1 = np.where(swap, c1, c0), np.where(swap, c0, c1)
    eq = c0 == c1  # one colour: 4-colour mode needs c0 > c1
    c0 = np.where(eq & (c0 < 0xFFFF), c0 + 1, c0)
    c1 = np.where(eq & (c0 == 0xFFFF) & (c1 == 0xFFFF), 0xFFFE, c1)
    p0, p1 = from565(c0), from565(c1)
    pal = np.stack([p0, p1, (2 * p0 + p1) / 3, (p0 + 2 * p1) / 3], 1)  # N,4,3
    ci = ((rgb[:, :, None, :] - pal[:, None, :, :]) ** 2).sum(-1).argmin(2).astype(np.uint32)
    cbits = np.zeros(n, np.uint32)
    for i in range(16):
        cbits |= ci[:, i] << np.uint32(2 * i)
    out = np.zeros((n, 16), np.uint8)
    out[:, 0] = a0.astype(np.uint8)
    out[:, 1] = a1.astype(np.uint8)
    for k in range(6):
        out[:, 2 + k] = ((abits >> np.uint64(8 * k)) & np.uint64(255)).astype(np.uint8)
    out[:, 8:10] = c0.astype('<u2').view(np.uint8).reshape(n, 2)
    out[:, 10:12] = c1.astype('<u2').view(np.uint8).reshape(n, 2)
    out[:, 12:16] = cbits.astype('<u4').view(np.uint8).reshape(n, 4)
    return out.tobytes()


def write_dds(path, levels, additive=False):
    h, w = levels[0].shape[:2]
    data = b''.join(dxt5(lv, additive) for lv in levels)
    DDSD = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000  # caps height width pixelformat mipmapcount linearsize
    hdr = struct.pack('<4sIIIIIII44x', b'DDS ', 124, DDSD, h, w, max(1, w // 4) * max(1, h // 4) * 16, 0, len(levels))
    hdr += struct.pack('<II4s20x', 32, 0x4, b'DXT5')  # pixel format: FOURCC
    hdr += struct.pack('<IIII4x', 0x1000 | 0x8 | 0x400000, 0, 0, 0)  # texture, complex, mipmap
    assert len(hdr) == 128
    with open(path, 'wb') as f:
        f.write(hdr)
        f.write(data)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--hash', required=True, help='the texture\'s hash, as FFXI_TEXLOG=1 logs it')
    ap.add_argument('--size', required=True, help='its size as the game creates it: WxH')
    ap.add_argument('--png', required=True, help='the replacement: the same layout at a whole scale')
    ap.add_argument('--out', required=True, help='the texture pack folder')
    ap.add_argument('--additive', action='store_true', help='the game draws it with ONE/ONE blending (text)')
    ap.add_argument('--pad', type=int, default=0,
                    help='texels to widen each glyph quad drawn with it (italic fonts the game cuts at upright boxes)')
    ap.add_argument('--pad-min', type=int, default=0, help='... only glyph quads at least this many texels tall')
    ap.add_argument('--glyphs', help='a glyph table (lines of x0 x1 y0 y1, the texture\'s texels): each glyph quad is '
                    'drawn over the ink box of the glyph starting in its cell')
    a = ap.parse_args()
    digest = int(a.hash, 16)
    w, h = (int(v) for v in a.size.lower().split('x'))
    img = read_png(a.png)
    sy, sx = img.shape[0] / h, img.shape[1] / w
    if sx != sy or sx != int(sx) or sx < 1:
        sys.exit(f'{a.png} is {img.shape[1]}x{img.shape[0]}: not {w}x{h} at a whole scale')
    if a.additive:
        img = img.copy()
        img[..., :3] = (img[..., :3].astype(np.uint32) * img[..., 3:4] + 127) // 255
    os.makedirs(a.out, exist_ok=True)
    path = os.path.join(a.out, f'{digest:016x}_{w}x{h}' + (f'_pad{a.pad}' + (f'min{a.pad_min}' if a.pad_min else '') if a.pad else '') + '.dds')
    write_dds(path, mip_chain(img, a.additive), a.additive)
    if a.glyphs:
        import shutil
        shutil.copyfile(a.glyphs, path[:-4] + '.glyphs')
    print(f'{digest:016x} ({w}x{h}) -> {path} ({img.shape[1]}x{img.shape[0]} DXT5)')


if __name__ == '__main__':
    main()

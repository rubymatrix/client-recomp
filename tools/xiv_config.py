#!/usr/bin/env python3
"""Create, show or edit FINAL FANTASY XIV 1.23b's config.sys (what ffxivconfig.exe writes).

The file is a flat little-endian struct of 0x2AC bytes (format and every field: decomp/docs/config_sys.md):

    +0x00  u32[24]   magic 0x20120419, then 23 settings
    +0x60  wchar[32] Windows font name (used only when use_system_font)
    +0xA0  wchar[262] screenshot directory ("" = default <My Games>\\FINAL FANTASY XIV\\screenshots)

ffxivgame.exe reads it once at start-up (FUN_00405c90, called from FUN_00402b30) from
"<My Documents>\\My Games\\FINAL FANTASY XIV\\config.sys"; the recompiled client maps My Documents
to ~/Library/Application Support/FFXIV1_0/Documents, which is the default --path here.

Examples:
    xiv_config.py --show
    xiv_config.py --resolution 3440x1440 --fullscreen --msaa 0
    xiv_config.py --set shadow=3 --set 17=0 --show

Edits keep every field it is not asked to change (and any bytes past 0x2AC); a file that would not
change is not rewritten. Without a file, the game's own built-in defaults are the starting point.
"""
import argparse
import os
import struct
import sys
import tempfile

DEFAULT_PATH = os.path.expanduser(
    "~/Library/Application Support/FFXIV1_0/Documents/My Games/FINAL FANTASY XIV/config.sys")

MAGIC = 0x20120419          # current format (ffxivconfig.exe always writes this)
OLD_MAGIC_A = 0x20100706    # oldest: 19 dwords, no font fields; ffxivgame converts it
OLD_MAGIC_B = 0x20101020    # 24 dwords + font name, no screenshot dir
SIZE = 0x2AC
FONT_OFF, FONT_CHARS = 0x60, 32
SHOT_OFF, SHOT_CHARS = 0xA0, (SIZE - 0xA0) // 2

# (index, name, meaning). Values are what ffxivgame.exe does with each dword (FUN_00402b30).
FIELDS = [
    (0, "magic", "format magic, 0x20120419"),
    (1, "upnp", "UPnP port mapping: 0 automatic, 1 use, 2 do not use"),
    (2, "port", "client port, 55296-55551 (0xD800-0xD8FF)"),
    (3, "adapter", "D3D adapter ordinal"),
    (4, "fullscreen", "display mode: 0 windowed, 1 full screen"),
    (5, "width", "window / back-buffer width (game raises it to >= 1024)"),
    (6, "height", "window / back-buffer height (game raises it to >= 720)"),
    (7, "general_quality", "General Drawing Quality 0-9 = 3D render scale 30,40,50,60,70,80,90,100,150,195 %"),
    (8, "background_quality", "Background Drawing Quality 0-4 = 20,40,60,80,100 %"),
    (9, "msaa_type", "multisampling: 0 none, 1 2x, 3 4x, 7 8x (D3DMULTISAMPLE_TYPE = value+1)"),
    (10, "msaa_quality", "multisample quality + 1 (D3D quality level = value-1); 1 for plain MSAA"),
    (11, "shadow", "Shadow Detail 0-4 lowest..highest = shadow map 256 << value"),
    (12, "ambient_occlusion", "Ambient Occlusion on/off"),
    (13, "depth_of_field", "Depth of Field on/off"),
    (14, "disable_cutscene_effects", "Disable cutscene effects on/off"),
    (15, "texture_quality", "Texture Quality: 0 high, 1 standard, 2 low"),
    (16, "texture_filter", "Texture Filtering: 0 highest (16x aniso), 1 high (8x), 2 standard (4x), 3 low (1x)"),
    (17, "fps_limit", "Frame Rate Limit: 0-4 = 60,30,20,15,10 fps"),
    (18, "disable_sound", "Disable Sound Device"),
    (19, "sound_in_background", "Play sound when application is in background"),
    (20, "hardware_cursor", "Enable hardware mouse cursor"),
    (21, "use_system_font", "Use Windows system font (then font_name/22/23 apply)"),
    (22, "font_quality", "system font: quality combo index"),
    (23, "font_thickness", "system font: thickness combo index (Fine/Normal/Thick/Thickest)"),
]
NAMES = {name: idx for idx, name, _ in FIELDS}

# ffxivgame.exe's own defaults when there is no file (FUN_00405c90 prologue).
GAME_DEFAULTS = [MAGIC, 0, 0xD800, 0, 0, 1280, 720, 7, 4, 0, 1, 2,
                 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 2, 1]

# ffxivconfig.exe's multisampling list (table at ffxivconfig 0x0044D8B0): option -> (field 9, field 10)
MSAA = {0: (0, 1), 2: (1, 1), 4: (3, 1), 8: (7, 1)}
FPS = {60: 0, 30: 1, 20: 2, 15: 3, 10: 4}


def wstr_get(buf, off, chars):
    raw = bytes(buf[off:off + chars * 2])
    text = raw.decode("utf-16-le", errors="replace")
    return text.split("\0", 1)[0]


def wstr_set(buf, off, chars, text):
    enc = text.encode("utf-16-le")
    if len(enc) // 2 >= chars:
        raise SystemExit(f"string too long ({len(text)} chars, max {chars - 1}): {text!r}")
    buf[off:off + chars * 2] = enc + b"\0" * (chars * 2 - len(enc))


def defaults():
    buf = bytearray(SIZE)
    struct.pack_into("<24I", buf, 0, *GAME_DEFAULTS)
    wstr_set(buf, FONT_OFF, FONT_CHARS, "MS UI Gothic")
    return buf


def load(path):
    """Returns (buffer, original bytes or None). Old formats are converted the way ffxivgame does."""
    if not os.path.exists(path):
        return defaults(), None
    data = open(path, "rb").read()
    if len(data) < 4:
        raise SystemExit(f"{path}: too short to be config.sys ({len(data)} bytes)")
    magic = struct.unpack_from("<I", data, 0)[0]
    if magic == MAGIC:
        buf = bytearray(data)
        if len(buf) < SIZE:
            buf += defaults()[len(buf):]
        return buf, data
    buf = defaults()
    d = list(struct.unpack_from("<%dI" % min(24, len(data) // 4), data, 0))
    d += [0] * (24 - len(d))
    v = list(GAME_DEFAULTS)
    if magic == OLD_MAGIC_A:
        # FUN_00405c90: [1..6] kept, [7] = {7,9,2,0,4,1}[old 9], [9],[10] = old 7,8, [11..16] = old 10..15,
        # [18..20] = old 16..18; [8] and [17] keep their defaults
        v[1:7] = d[1:7]
        v[7] = [7, 9, 2, 0, 4, 1][d[9]] if d[9] < 6 else 7
        v[9], v[10] = d[7], d[8]
        v[11:16] = d[10:15]
        v[16] = d[15]
        v[18], v[19], v[20] = d[16], d[17], d[18]
    elif magic == OLD_MAGIC_B:
        v[1:24] = d[1:24]
        buf[FONT_OFF:FONT_OFF + FONT_CHARS * 2] = data[FONT_OFF:FONT_OFF + FONT_CHARS * 2].ljust(FONT_CHARS * 2, b"\0")
    else:
        raise SystemExit(f"{path}: unknown magic 0x{magic:08X} (the game would ignore this file)")
    v[0] = MAGIC
    struct.pack_into("<24I", buf, 0, *v)
    print(f"note: converted format 0x{magic:08X} to 0x{MAGIC:08X}", file=sys.stderr)
    return buf, data


def get(buf, i):
    return struct.unpack_from("<I", buf, i * 4)[0]


def put(buf, i, value):
    struct.pack_into("<I", buf, i * 4, value & 0xFFFFFFFF)


def show(buf, path):
    print(f"{path}")
    for idx, name, meaning in FIELDS:
        val = get(buf, idx)
        shown = f"0x{val:08X}" if idx in (0,) else str(val if val < 0x80000000 else val - (1 << 32))
        print(f"  [{idx:2}] +0x{idx * 4:02X} {name:<25} {shown:<11} {meaning}")
    print(f"       +0x60 font_name                 {wstr_get(buf, FONT_OFF, FONT_CHARS)!r}")
    print(f"       +0xA0 screenshot_dir            {wstr_get(buf, SHOT_OFF, SHOT_CHARS)!r}")
    if len(buf) > SIZE:
        print(f"       ({len(buf) - SIZE} extra bytes past 0x2AC kept as is)")
    w, h = get(buf, 5), get(buf, 6)
    if w < 1024 or h < 720:
        print(f"  note: the game raises {w}x{h} to at least 1024x720")


def parse_int(text):
    return int(text, 0)


def apply_set(buf, spec):
    if "=" not in spec:
        raise SystemExit(f"--set wants id=value, got {spec!r}")
    key, value = spec.split("=", 1)
    key = key.strip()
    if key in ("font_name", "0x60"):
        wstr_set(buf, FONT_OFF, FONT_CHARS, value)
        return
    if key in ("screenshot_dir", "0xa0", "0xA0"):
        wstr_set(buf, SHOT_OFF, SHOT_CHARS, value)
        return
    if key in NAMES:
        idx = NAMES[key]
    else:
        try:
            idx = parse_int(key)
        except ValueError:
            raise SystemExit(f"unknown field {key!r} (names: {', '.join(NAMES)}, font_name, screenshot_dir)")
    if not 0 <= idx < 24:
        raise SystemExit(f"field index {idx} out of range 0-23")
    if idx == 0:
        raise SystemExit("refusing to change the magic")
    put(buf, idx, parse_int(value))


def write_atomic(path, data):
    d = os.path.dirname(path) or "."
    os.makedirs(d, exist_ok=True)
    fd, tmp = tempfile.mkstemp(prefix=".config.sys.", dir=d)
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
        os.chmod(tmp, 0o644)
        os.replace(tmp, path)
    except BaseException:
        if os.path.exists(tmp):
            os.unlink(tmp)
        raise


def main():
    ap = argparse.ArgumentParser(description="Create, show or edit FFXIV 1.23b config.sys",
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument("--path", default=DEFAULT_PATH, help="config.sys to read (default: %(default)s)")
    ap.add_argument("--output", help="write here instead of --path (e.g. a scratch file)")
    ap.add_argument("--show", action="store_true", help="print every field (after edits)")
    ap.add_argument("--dry-run", action="store_true", help="do not write")
    ap.add_argument("--resolution", metavar="WxH", help="e.g. 3440x1440")
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--fullscreen", action="store_true")
    mode.add_argument("--windowed", action="store_true")
    ap.add_argument("--msaa", type=int, choices=sorted(MSAA), help="multisampling samples (0 = off)")
    ap.add_argument("--quality", type=int, choices=range(1, 11), metavar="1-10",
                    help="General Drawing Quality as the config tool numbers it (8 = standard, 100%% scale)")
    ap.add_argument("--bg-quality", type=int, choices=range(1, 6), metavar="1-5",
                    help="Background Drawing Quality (5 = highest)")
    ap.add_argument("--shadow", type=int, choices=range(0, 5), metavar="0-4", help="Shadow Detail, 4 = highest")
    ap.add_argument("--fps", type=int, choices=sorted(FPS), help="frame rate limit")
    ap.add_argument("--set", action="append", default=[], metavar="ID=VALUE",
                    help="any field by index (0-23) or name; also font_name=, screenshot_dir=")
    args = ap.parse_args()

    buf, original = load(args.path)
    if args.resolution:
        try:
            w, h = (int(x) for x in args.resolution.lower().split("x"))
        except ValueError:
            raise SystemExit(f"--resolution wants WxH, got {args.resolution!r}")
        put(buf, 5, w)
        put(buf, 6, h)
    if args.fullscreen:
        put(buf, 4, 1)
    if args.windowed:
        put(buf, 4, 0)
    if args.msaa is not None:
        t, q = MSAA[args.msaa]
        put(buf, 9, t)
        put(buf, 10, q)
    if args.quality is not None:
        put(buf, 7, args.quality - 1)
    if args.bg_quality is not None:
        put(buf, 8, args.bg_quality - 1)
    if args.shadow is not None:
        put(buf, 11, args.shadow)
    if args.fps is not None:
        put(buf, 17, FPS[args.fps])
    for spec in args.set:
        apply_set(buf, spec)

    out = args.output or args.path
    data = bytes(buf)
    edited = bool(args.resolution or args.fullscreen or args.windowed or args.msaa is not None
                  or args.quality is not None or args.bg_quality is not None or args.shadow is not None
                  or args.fps is not None or args.set)
    if args.show or not edited:
        show(buf, out)
    if not edited:
        return
    current = open(out, "rb").read() if os.path.exists(out) else None
    if current == data:
        print(f"unchanged: {out}")
    elif args.dry_run:
        print(f"would write {len(data)} bytes to {out}")
    else:
        write_atomic(out, data)
        print(f"wrote {len(data)} bytes to {out}")


if __name__ == "__main__":
    main()

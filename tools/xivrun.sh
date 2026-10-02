#!/bin/sh
# Rebuild the boot test and run it against the local install; prints the last lines.
cd "$(dirname "$0")/.." || exit 1
EXE="$HOME/Projects/FFXIV1_0/decomp/orig/ffxivgame.exe"
GAME="$HOME/Projects/FFXIV1_0/installer/target/prefix/drive_c/Program Files (x86)/SquareEnix/FINAL FANTASY XIV"
.venv/bin/python tools/build_xiv.py xivboot --exe "$EXE" 2>&1 | grep -E "error|failed|undefined|Undefined" | head -20
# a placeholder session: enough for the game to accept its launch argument (real ones come from the web login)
: "${XIV_ARGS:=$(.venv/bin/python tools/xiv_args.py 0123456789abcdef0123456789abcdef0123456789abcdef01234567)}"
export XIV_ARGS
timeout "${XIV_TIMEOUT:-60}" build/xivboot "$GAME/ffxivgame.exe" "$GAME" 2>&1 | grep -v "no shim:" | tail -"${XIV_TAIL:-25}"

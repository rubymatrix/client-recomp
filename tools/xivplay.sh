#!/bin/sh
# Rebuild the boot test (translating again with --translate) and sign in with the local test
# account (build/test-account.txt, made by tools/xiv_login.py --signup) to run the game.
cd "$(dirname "$0")/.." || exit 1
EXE="${XIV_EXE:-$PWD/../retail/FINAL FANTASY XIV/ffxivgame.exe}"
[ "$1" = "--translate" ] && { .venv/bin/python tools/build_xiv.py translate --exe "$EXE" 2>&1 | grep -E "translated|Error"; shift; }
.venv/bin/python tools/build_xiv.py xivboot --exe "$EXE" 2>&1 | grep -E "error|Undefined" | head -20
U=$(sed -n 's/^user=//p' build/test-account.txt)
XIV_PASSWORD=$(sed -n 's/^password=//p' build/test-account.txt) timeout "${XIV_TIMEOUT:-60}" .venv/bin/python tools/xiv_login.py --user "$U" "$@" > build/run.log 2>&1
echo "exit=$?"
grep -v "no shim:" build/run.log | tail -"${XIV_TAIL:-12}"

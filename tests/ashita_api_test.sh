#!/bin/sh
# Regression test for the Ashita v4 layer (host/addons/lua/ashita*.lua): runs
# tests/ashita_api_test.txt in the addon harness with the test addon tests/addons/ashtest and
# compares the output with tests/ashita_api_test.expected. Ashita's own libs are not needed.
#
#   tests/ashita_api_test.sh --game <FFXI folder> [--update]
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
game=""
update=0
while [ $# -gt 0 ]; do
    case "$1" in
        --game) game="$2"; shift 2 ;;
        --update) update=1; shift ;;
        *) echo "usage: $0 --game <FFXI folder> [--update]"; exit 2 ;;
    esac
done
[ -n "$game" ] || { echo "usage: $0 --game <FFXI folder> [--update]"; exit 2; }

data=$(mktemp -d "${TMPDIR:-/tmp}/ashtest.XXXXXX")
trap 'rm -rf "$data"' EXIT
mkdir -p "$data/ashita/addons" "$data/ashita/config/boot" "$data/ashita/config/ashita"
cp -R "$here/addons/ashtest" "$data/ashita/addons/"
# a boot config, and offsets / pointers of our own (Ashita's files are fetched, not in the repo)
printf '[ffxi.registry]\n0003 = 4096 ; a comment\n[ashita.language]\nashita = 2\n' > "$data/ashita/config/boot/boot.ini"
printf '[test.section]\nvalue = 0x7E30 ; the value\n' > "$data/ashita/config/ashita/custom.offsets.ini"
printf '[test.pointer]\nmodule = FFXiMain.dll\npattern = A1????????85C05E74????80\noffset = 1\ncount = 0\n' \
    > "$data/ashita/config/ashita/custom.pointers.ini"

out="$data/out.txt"
FFXI_ADDONS_LUA="$root/host/addons/lua" "$root/build/host64" --game "$game" --data-dir "$data" \
    --addon-harness "$here/ashita_api_test.txt" > "$data/raw.txt" 2>&1 || true
grep -E '^\[chat |^\[game\]|^\[harness\]|unsupported|error' "$data/raw.txt" | grep -v 'patch.ver\|ImGui assertion' > "$out"

if [ $update -eq 1 ]; then
    cp "$out" "$here/ashita_api_test.expected"
    echo "updated ashita_api_test.expected"
    exit 0
fi
if diff -u "$here/ashita_api_test.expected" "$out"; then
    echo "ashita_api_test: PASS"
else
    echo "ashita_api_test: FAIL"
    exit 1
fi

#!/bin/sh
# Regression test for the Windower layer (host/addons/lua/windower*.lua, windower_native.c):
# runs tests/windower_layer_test.txt in the addon harness with the test addon tests/addons/wlayer
# (a Windower addon using the raw windower.* API) and compares the output with
# tests/windower_layer_test.expected. "@login" in the script stands for the login packets
# tools/windower_survey.py uses (a zone-in, stats and vitals for the player "Tester").
#
#   tests/windower_layer_test.sh --game <FFXI folder> [--update]
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

data=$(mktemp -d "${TMPDIR:-/tmp}/wlayer.XXXXXX")
trap 'rm -rf "$data"' EXIT
mkdir -p "$data/windower/addons"
cp -R "$here/addons/wlayer" "$data/windower/addons/"

login=$(cd "$root" && python3 -c "
import sys; sys.path.insert(0, 'tools'); import windower_survey as w
print('\n'.join('packet_in ' + p for p in w.LOGIN))")
script="$data/script.txt"
while IFS= read -r line; do
    if [ "$line" = "@login" ]; then printf '%s\n' "$login"; else printf '%s\n' "$line"; fi
done < "$here/windower_layer_test.txt" > "$script"

out="$data/out.txt"
FFXI_ADDONS_LUA="$root/host/addons/lua" "$root/build/host64" --game "$game" --data-dir "$data" \
    --addon-harness "$script" > "$data/raw.txt" 2>&1 || true
grep -E '^\[chat |^\[game\]|^\[harness\]|error' "$data/raw.txt" | grep -v 'patch.ver' | sed "s|$data|<data>|g" > "$out"

if [ $update -eq 1 ]; then
    cp "$out" "$here/windower_layer_test.expected"
    echo "updated windower_layer_test.expected"
    exit 0
fi
if diff -u "$here/windower_layer_test.expected" "$out"; then
    echo "windower_layer_test: PASS"
else
    echo "windower_layer_test: FAIL"
    exit 1
fi

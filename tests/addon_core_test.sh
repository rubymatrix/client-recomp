#!/bin/sh
# Regression test for the addon host's core (host/addons/): runs tests/addon_core_test.txt in the
# addon harness with the test addon tests/addons/coretest and compares the output with
# tests/addon_core_test.expected.
#
#   tests/addon_core_test.sh --game <FFXI folder> [--update]
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

data=$(mktemp -d "${TMPDIR:-/tmp}/coretest.XXXXXX")
trap 'rm -rf "$data"' EXIT
mkdir -p "$data/xi/addons"
cp -R "$here/addons/coretest" "$data/xi/addons/"

out="$data/out.txt"
FFXI_ADDONS_LUA="$root/host/addons/lua" "$root/build/host64" --game "$game" --data-dir "$data" \
    --addon-harness "$here/addon_core_test.txt" > "$data/raw.txt" 2>&1 || true
grep -E '^\[chat |^\[game\]|^\[harness\]|unbalanced|error' "$data/raw.txt" | grep -v 'patch.ver' > "$out"

if [ $update -eq 1 ]; then
    cp "$out" "$here/addon_core_test.expected"
    echo "updated addon_core_test.expected"
    exit 0
fi
if diff -u "$here/addon_core_test.expected" "$out"; then
    echo "addon_core_test: PASS"
else
    echo "addon_core_test: FAIL"
    exit 1
fi

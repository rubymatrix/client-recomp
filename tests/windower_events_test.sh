#!/bin/sh
# Regression test for host/addons/lua/windower_events.lua (Windower's packet-derived events and
# state): runs tests/windower_events_test.txt in the addon harness with the test addon
# tests/addons/wevtest and compares its output with tests/windower_events_test.expected.
#
#   tests/windower_events_test.sh --game <FFXI folder> [--update]
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

data=$(mktemp -d "${TMPDIR:-/tmp}/wevtest.XXXXXX")
trap 'rm -rf "$data"' EXIT
mkdir -p "$data/xi/addons"
cp -R "$here/addons/wevtest" "$data/xi/addons/"

out="$data/out.txt"
FFXI_ADDONS_LUA="$root/host/addons/lua" "$root/build/host64" --game "$game" --data-dir "$data" \
    --addon-harness "$here/windower_events_test.txt" > "$data/raw.txt" 2>&1 || true
# the lines that matter: events, getter results, section markers, packets out
grep -E '^EV |^\[harness\] (=|lua error|packet_out|packet_in: [0-9]+ bytes in, 28 out)|^--- |error' "$data/raw.txt" \
    | sed -E 's/^\[harness\] packet_in: ([0-9]+) bytes in, 28 out.*/[harness] packet_in blocked/' > "$out"

if [ $update -eq 1 ]; then
    cp "$out" "$here/windower_events_test.expected"
    echo "updated windower_events_test.expected"
    exit 0
fi
if diff -u "$here/windower_events_test.expected" "$out"; then
    echo "windower_events_test: PASS"
else
    echo "windower_events_test: FAIL"
    exit 1
fi

#!/bin/bash
# End-to-end test of the Mac install, from the retail disc to the lobby's character list, in folders
# of its own: a fresh clone of this checkout, a fresh install, Documents and app folder. Your own
# install, settings and app are not touched.
#
#   tools/xiv_e2e.sh [--dir <test folder>] [--keep] [--resume]
#
# --resume picks up a run in --dir that was stopped after its bootstrap (the install carries on by
# itself): it checks stages 3 to 6.
#
# Needs: the disc mounted; the server running on this Mac (meteor-web for the version check, patches
# and login page; Meteor's lobby); the commits to test committed (the clone is of HEAD).
# Stages, each PASS or FAIL with its time:
#   1 bootstrap   install.command from the clone: unpack, build boot and updater, open the app
#   2 unpack      every file of the disc's three file groups in place
#   3 boot patch  boot.ver 2010.09.18.0000 (ffxivboot patched and rebuilt itself)
#   4 game patch  game.ver 2012.09.19.0001, the four programs byte-identical to retail 1.23b
#   5 sign-in     ffxivlogin found <x-sqexauth> and started the game (signed in by script)
#   6 lobby       the lobby sent the character list; "Invalid session" fails
# Settings: XIV_E2E_SERVER (127.0.0.1), XIV_E2E_ACCOUNT_URL (http://127.0.0.1:8090),
# XIV_E2E_CERT (../patch-signing/patch_cert.der), XIV_E2E_ACCOUNT (../meteor-develop/.client-account:
# user= and password= lines), XIV_E2E_LOBBY_LOG (../meteor-develop/.run/logs/lobby.log).
set -u
HERE="$(cd "$(dirname "$0")/.." && pwd)"
WS="$(dirname "$HERE")"
DIR="" KEEP=0 RESUME=0
while [ $# -gt 0 ]; do
    case "$1" in
        --dir) DIR="$2"; shift 2 ;;
        --keep) KEEP=1; shift ;;
        --resume) RESUME=1; shift ;;
        -h|--help) sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
DIR="${DIR:-${TMPDIR:-/tmp}/xiv-e2e-$(date +%Y%m%d-%H%M%S)}"
SERVER="${XIV_E2E_SERVER:-127.0.0.1}"
ACCOUNT_URL="${XIV_E2E_ACCOUNT_URL:-http://127.0.0.1:8090}"
CERT="${XIV_E2E_CERT:-$WS/patch-signing/patch_cert.der}"
ACCOUNT="${XIV_E2E_ACCOUNT:-$WS/meteor-develop/.client-account}"
LOBBY_LOG="${XIV_E2E_LOBBY_LOG:-$WS/meteor-develop/.run/logs/lobby.log}"
# retail 1.23b's programs (SHA-256), what the patches must produce
WANT_BOOT=6a18533d4c3b296ccdedd84c81a3eb99ae5ddb47c3416de60e3414983783efef
WANT_GAME=9341f2b4567440b310a4d494f5cc5599ca334ba51c8042247317ff466492f2e9
WANT_LOGIN=5ebcda2e6b2ce3911dbff67f361a27720162dac15aff01cefce814ce4e46a67a
WANT_UPDATER=2e83ae594382e49ec0d41748b6a000e5cd06b8f750bd33fbd7542a7c44b5d836

INSTALL="$DIR/install" DOCS="$DIR/documents" APPS="$DIR/apps" SRC="$DIR/source" LOG="$DIR/e2e.log"
mkdir -p "$DIR" "$DOCS" "$APPS"
T0=$(date +%s) FAILED=0 COMPLETE=0
say()  { printf '%s\n' "$*" | tee -a "$LOG"; }
stage_t=$T0
result() { # result <PASS|FAIL> <stage> <detail>
    local now; now=$(date +%s)
    say "$(printf '%-4s %-11s %5ds  %s' "$1" "$2" $((now - stage_t)) "$3")"
    stage_t=$now
    [ "$1" = FAIL ] && FAILED=1
    return 0
}
# wait_for <timeout s> <command...>: runs the command every 10 s until it succeeds (give it a function,
# so what it checks is read again each time)
wait_for() {
    local limit=$(( $(date +%s) + $1 )); shift
    until "$@"; do
        [ "$(date +%s)" -ge "$limit" ] && return 1
        sleep 10
    done
}
finish() {
    pkill -f "$INSTALL/" 2>/dev/null
    [ $COMPLETE = 1 ] || FAILED=1  # stopped before the last stage
    rm -f "$DIR/signin.js"
    say "$( [ $FAILED = 0 ] && echo PASSED || echo FAILED ) in $(( $(date +%s) - T0 ))s; log $LOG"
    [ $KEEP = 1 ] || [ $FAILED = 1 ] || rm -rf "$INSTALL" "$SRC/build" "$SRC/generated"
    exit $FAILED
}
trap finish EXIT

say "end-to-end test in $DIR ($(git -C "$HERE" log --oneline -1))$( [ $RESUME = 1 ] && echo ', resumed')"
if [ $RESUME = 0 ]; then
DISC=$(for v in /Volumes/*; do [ -f "$v/ffxivsetup.exe" ] && [ -f "$v/data/data1.hdr" ] && echo "$v" && break; done)
[ -n "$DISC" ] || { result FAIL setup "the FINAL FANTASY XIV disc is not mounted"; exit; }
curl -sf -o /dev/null "http://$SERVER:54996/patch/vercheck/ffxiv/win32/release/boot/2010.09.18.0000" \
    || { result FAIL setup "no version check at $SERVER:54996 (meteor-web)"; exit; }
nc -z "$SERVER" 54994 2>/dev/null || { result FAIL setup "no lobby at $SERVER:54994"; exit; }
[ -f "$CERT" ] || { result FAIL setup "no patch certificate $CERT"; exit; }
[ -f "$ACCOUNT" ] || { result FAIL setup "no test account $ACCOUNT"; exit; }
[ -f "$LOBBY_LOG" ] || { result FAIL setup "no lobby log $LOBBY_LOG"; exit; }
# the sign-in: meteor-web's form filled from the test account, in the login window's page
/usr/bin/python3 - "$ACCOUNT" "$DIR/signin.js" <<'PY'
import json, sys
acct = dict(l.strip().split('=', 1) for l in open(sys.argv[1]) if '=' in l)
open(sys.argv[2], 'w').write("""(function(){var u=document.getElementById('username'),p=document.getElementById('password');
if(!u||!p||window.__signed)return;window.__signed=1;u.value=%s;p.value=%s;
var b=document.querySelector('button[name=login]');if(b)b.click();else u.form.submit();})();""" % (
    json.dumps(acct['user']), json.dumps(acct['password'])))
PY
chmod 600 "$DIR/signin.js"
wc -l < "$LOBBY_LOG" | tr -d ' ' > "$DIR/lobby_start"  # the lobby lines before this run
result PASS setup "disc $DISC, server $SERVER"

# 1: the bootstrap, as a player runs it, from a clone of this checkout
XIV_REPO="file://$HERE" XIV_BRANCH="$(git -C "$HERE" branch --show-current)" XIV_SOURCE="$SRC" XIV_CALLER_DIR="$DIR" \
    bash "$HERE/install.command" --disc "$DISC" --install-dir "$INSTALL" --app-dir "$APPS" --server "$SERVER" \
    --patch-cert "$CERT" --account-url "$ACCOUNT_URL" \
    --test-env "XIV_DOCUMENTS=$DOCS" --test-env "XIV_WEB_SCRIPT=$DIR/signin.js" >> "$LOG" 2>&1 \
    && result PASS bootstrap "app $APPS/FINAL FANTASY XIV.app opened" \
    || { result FAIL bootstrap "install.command failed (see the log)"; exit; }

# 2: the disc's files
n=$(find "$INSTALL" -type f | wc -l | tr -d ' ')
[ "$n" -ge 137765 ] && result PASS unpack "$n files" || result FAIL unpack "$n files, want 137765"
fi
LOBBY_START=$(( $(cat "$DIR/lobby_start") + 1 ))

# 3: the boot patch, and boot rebuilt for its new build
ver() { cat "$INSTALL/$1.ver" 2>/dev/null; }
boot_patched() { [ "$(ver boot)" = 2010.09.18.0000 ]; }
game_patched() { [ "$(ver game)" = 2012.09.19.0001 ]; }
wait_for 1800 boot_patched \
    && result PASS "boot patch" "boot.ver $(ver boot)" || { result FAIL "boot patch" "boot.ver $(ver boot)"; exit; }

# 4: every game patch, and the programs they bring as retail has them
wait_for 5400 game_patched \
    || { result FAIL "game patch" "game.ver $(ver game)"; exit; }
sha() { shasum -a 256 "$INSTALL/$1" 2>/dev/null | cut -d' ' -f1; }
bad=""
[ "$(sha ffxivboot.exe)" = $WANT_BOOT ] || bad="$bad ffxivboot"
[ "$(sha ffxivgame.exe)" = $WANT_GAME ] || bad="$bad ffxivgame"
[ "$(sha ffxivlogin.exe)" = $WANT_LOGIN ] || bad="$bad ffxivlogin"
[ "$(sha ffxivupdater.exe)" = $WANT_UPDATER ] || bad="$bad ffxivupdater"
[ -z "$bad" ] && result PASS "game patch" "game.ver $(ver game), programs match retail 1.23b" \
    || result FAIL "game patch" "differ from retail:$bad"

# 5: the sign-in started the game (the game is built while login is open)
RUNLOG="$SRC/build/run.log"
signed_in() { grep -q "CreateProcessW(.*ffxivgame.exe.*started" "$RUNLOG" 2>/dev/null; }
wait_for 2400 signed_in && result PASS sign-in "login started the game" \
    || { result FAIL sign-in "no game started (see $RUNLOG)"; exit; }

# 6: the lobby took the session and sent the characters
lobby() { tail -n +"$LOBBY_START" "$LOBBY_LOG" | grep -E "Get characters|Invalid session" | head -1; }
lobby_answered() { [ -n "$(lobby)" ]; }
wait_for 3600 lobby_answered
case "$(lobby)" in
    *"Get characters"*) result PASS lobby "character list sent" ;;
    *"Invalid session"*) result FAIL lobby "the lobby refused the session" ;;
    *) result FAIL lobby "the game never reached the lobby (see $RUNLOG)" ;;
esac
COMPLETE=1

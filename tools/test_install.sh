#!/bin/bash
# Run install.sh the way a player does (piped to bash, questions on the terminal) against this
# checkout, uncommitted changes included, with everything it touches in a sandbox folder:
#
#   tools/test_install.sh [--full] [--saved] [--sandbox <folder>] [-- <setup.py options>]
#
# The working tree (tracked and untracked files, not ignored ones) becomes a commit in a bare repo
# in the sandbox; install.sh clones it from there (FFXI_REPO, FFXI_BRANCH) into <sandbox>/source
# (FFXI_SOURCE). The sign-in screen's saved files are <sandbox>/data (FFXI_DATA_DIR), the app goes
# to <sandbox>/Applications, and it is not opened. Nothing in the player's own folders changes.
#
#   (default)       --settings-only: the questions and what the build would get, in seconds
#   --full          build and install the app into the sandbox (a whole build: several minutes)
#   --saved         start from copies of this Mac's saved files (~/Library/Application Support/
#                   FFXIRecompile/FFXI), as an update would
#   --sandbox dir   use (or reuse) this folder: a second run there is an update of the first
#
# Without a --game option, the FINAL FANTASY XI folder next to this checkout is the game when there
# is one (setup.py looks for it, or opens a Finder window, otherwise).
#
# e.g. tools/test_install.sh -- --server play.example.net --dats none
set -e
here="$(cd "$(dirname "$0")/.." && pwd)"
full= saved= sandbox=
while [ $# -gt 0 ]; do
    case "$1" in
        --full) full=1 ;;
        --saved) saved=1 ;;
        --sandbox) sandbox="$2"; shift ;;
        --) shift; break ;;
        -h|--help) sed -n '2,/^set -e/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option $1 (--help)" >&2; exit 2 ;;
    esac
    shift
done
sandbox="${sandbox:-$(mktemp -d "${TMPDIR:-/tmp}/ffxi-install-test.XXXXXX")}"
mkdir -p "$sandbox"
sandbox="$(cd "$sandbox" && pwd)"

# the working tree as a commit, without touching the index or the branch
index="$sandbox/index"
cp "$here/.git/index" "$index" 2>/dev/null || true
GIT_INDEX_FILE="$index" git -C "$here" add -A
tree=$(GIT_INDEX_FILE="$index" git -C "$here" write-tree)
rm -f "$index"
commit=$(git -C "$here" commit-tree "$tree" -p HEAD -m "install test snapshot")
[ -d "$sandbox/repo.git" ] || git init -q --bare "$sandbox/repo.git"
git -C "$here" push -q -f "$sandbox/repo.git" "$commit:refs/heads/install-test"

if [ -n "$saved" ]; then
    mkdir -p "$sandbox/data"
    real="$HOME/Library/Application Support/FFXIRecompile/FFXI"
    for f in signin.cfg settings.reg modern.cfg; do
        [ -f "$real/$f" ] && cp "$real/$f" "$sandbox/data/"
    done
fi

echo "sandbox: $sandbox"
args=(--install-dir "$sandbox/Applications" --no-open)
case " $* " in
    *" --game "*) ;;
    *) [ -d "$here/../FINAL FANTASY XI" ] && args+=(--game "$(cd "$here/../FINAL FANTASY XI" && pwd)") ;;
esac
[ -n "$full" ] || args+=(--settings-only)
# piped, as from curl: install.sh asks its questions on /dev/tty
status=0
FFXI_REPO="file://$sandbox/repo.git" FFXI_BRANCH=install-test FFXI_SOURCE="$sandbox/source" \
    FFXI_DATA_DIR="$sandbox/data" bash -s -- "${args[@]}" "$@" <"$here/install.sh" || status=$?

echo
app="$sandbox/Applications/Final Fantasy XI.app"
if [ -d "$app" ]; then
    echo "installed $app, defaults:"
    plutil -p "$app/Contents/Info.plist" | grep FFXI || true
fi
for f in signin.cfg settings.reg modern.cfg; do
    if [ -n "$full" ] && [ -f "$sandbox/data/$f" ]; then
        echo "--- data/$f"
        tr -d '\r' <"$sandbox/data/$f"
    fi
done
echo "sandbox kept: $sandbox (again with --sandbox to test an update; rm -rf it when done)"
exit $status

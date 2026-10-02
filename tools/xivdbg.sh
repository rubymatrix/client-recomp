#!/bin/sh
# Run the recompiled client under lldb with lldb's MCP server listening, so Claude Code (the
# "lldb" server in the workspace .mcp.json, Xcode's lldb-mcp) can drive the same session you see.
#
#   tools/xivdbg.sh                 sign in with build/test-account.txt, launch build/xivboot-dbg
#   tools/xivdbg.sh --release       debug the fast LTO build (build/xivboot): symbols only
#   tools/xivdbg.sh --attach        attach to the running xivboot instead of launching
#   tools/xivdbg.sh --stopped       load and sign in, but wait at the (lldb) prompt before `run`
#   tools/xivdbg.sh -- <args>       extra xivboot args
#
# XIV_* env vars (XIV_SERIAL=1 is handy: guest threads take turns) pass through to the game.
# MCP port: XIV_LLDB_MCP_PORT (default 59999). Build the debug binary first with
#   .venv/bin/python tools/build_xiv.py xivboot --exe ~/Projects/FFXIV1_0/decomp/orig/ffxivgame.exe --debug
cd "$(dirname "$0")/.." || exit 1
ROOT=$(pwd)
BIN=build/xivboot-dbg
MODE=launch
RUN=1
while [ $# -gt 0 ]; do
    case "$1" in
        --release) BIN=build/xivboot ;;
        --attach) MODE=attach ;;
        --stopped) RUN=0 ;;
        --) shift; break ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
    shift
done
PORT=${XIV_LLDB_MCP_PORT:-59999}
GAME=${XIV_GAME:-"$HOME/Projects/FFXIV1_0/installer/target/prefix/drive_c/Program Files (x86)/SquareEnix/FINAL FANTASY XIV"}
LLDB=$(xcrun -f lldb 2>/dev/null || echo lldb)

set -- -o "command script import $ROOT/tools/lldb/xiv_lldb.py" \
       -o "protocol-server start MCP listen://localhost:$PORT" "$@"

if [ "$MODE" = attach ]; then
    PID=$(pgrep -n xivboot) || { echo "xivboot is not running" >&2; exit 1; }
    echo "attaching to xivboot pid $PID (MCP on localhost:$PORT)"
    exec "$LLDB" -p "$PID" "$@"
fi

[ -x "$BIN" ] || { echo "$BIN missing: build it first (see the header of this script)" >&2; exit 1; }
U=$(sed -n 's/^user=//p' build/test-account.txt)
ARG=$(XIV_PASSWORD=$(sed -n 's/^password=//p' build/test-account.txt) \
      .venv/bin/python tools/xiv_login.py --user "$U" --print | tail -1) || exit 1
export XIV_ARGS="$ARG" XIV_TICK=${XIV_TICK:-0x10000} XIV_LOBBY=${XIV_LOBBY:-127.0.0.1}
echo "signed in as $U; launching $BIN under lldb (MCP on localhost:$PORT)"

set -- "$@" -o "target create $BIN" \
       -o "settings set -- target.run-args \"$GAME/ffxivgame.exe\" \"$GAME\"" \
       -o "process handle SIGPROF -n false -p true -s false"
[ "$RUN" = 1 ] && set -- "$@" -o "process launch"
exec "$LLDB" "$@"

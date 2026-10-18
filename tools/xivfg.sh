#!/usr/bin/env bash
# Launch the client in the foreground of this terminal with the normal login (it stops at
# character select for a click). Stops a client started by xivctl first.
set -euo pipefail
WS="$(cd "$(dirname "$0")/../.." && pwd)"
RECOMP="$WS/recomp"
"$WS/xivctl" stop >/dev/null 2>&1 || true
[[ -f "$WS/meteor-server/.public.env" ]] && source "$WS/meteor-server/.public.env"
WEB_LOCAL="${METEOR_WEB_HOST:-127.0.0.1}"; [[ "$WEB_LOCAL" == 0.0.0.0 ]] && WEB_LOCAL=127.0.0.1
ACCOUNT="$WS/meteor-server/.client-account"
USER_NAME="$(sed -n 's/^user=//p' "$ACCOUNT")"
export XIV_PASSWORD="$(sed -n 's/^password=//p' "$ACCOUNT")"
unset XIV_DIRECT_LOGIN
cd "$RECOMP"
exec .venv/bin/python tools/xiv_login.py --meteor --server "http://$WEB_LOCAL:${METEOR_WEB_PORT:-8000}" \
    --lobby 127.0.0.1 --user "$USER_NAME"

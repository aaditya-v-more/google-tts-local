#!/bin/sh
# Adds google-tts-local's voices to Earmark, the read-aloud book reader, by registering a
# plugin file in Earmark's plugins folder (Earmark's plugin contract: one JSON file per plugin).
set -eu
usage() {
    cat <<'HELP'
Usage: gtts-earmark install|uninstall|status [options]
  install    Register the voices with Earmark (they appear in its voice menu)
  uninstall  Remove them from Earmark again; gtts and its voices stay installed
  status     Say whether they are registered, and whether the server is running
Options: --app NAME  --port PORT
Defaults: app Earmark (a development build is "Earmark Dev"), port 5591.
On a Mac, Earmark starts the gtts-service LaunchAgent itself the first time one of the
voices is used; run gtts fetch first. On Linux, run the server yourself (see the README).
HELP
}
[ "$#" -gt 0 ] || { usage >&2; exit 2; }
MODE=$1; shift
case $MODE in install|uninstall|status) ;; -h|--help) usage; exit 0 ;; *) usage >&2; exit 2 ;; esac
: "${HOME:?HOME must be set}"
APP=Earmark
PORT=5591
while [ "$#" -gt 0 ]; do
    case $1 in -h|--help) usage; exit 0 ;; esac
    [ "$#" -ge 2 ] || { echo "Missing value for $1" >&2; exit 2; }
    case $1 in
        --app) APP=$2 ;;
        --port) PORT=$2 ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
    shift 2
done
case $PORT in ''|*[!0-9]*) echo 'Port must be an integer from 1 to 65535.' >&2; exit 2 ;; esac
[ "$PORT" -ge 1 ] && [ "$PORT" -le 65535 ] || { echo 'Port must be an integer from 1 to 65535.' >&2; exit 2; }
case $APP in ''|*/*) echo 'The app name cannot be empty or contain /.' >&2; exit 2 ;; esac

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
case $(uname -s) in
    Darwin) FOLDER="$HOME/Library/Application Support/$APP/Plugins" ;;
    *) FOLDER="${XDG_CONFIG_HOME:-$HOME/.config}/$APP/Plugins" ;;
esac
FILE="$FOLDER/google-tts-local.json"
URL="http://127.0.0.1:$PORT"

if [ "$MODE" = status ]; then
    if [ -f "$FILE" ]; then printf 'Registered with %s: %s\n' "$APP" "$FILE"; else printf 'Not registered with %s.\n' "$APP"; fi
    if curl -fsS -m 2 "$URL/health" >/dev/null 2>&1; then printf 'The server is running at %s.\n' "$URL"
    else printf 'The server is not running at %s.\n' "$URL"; fi
    exit 0
fi
if [ "$MODE" = uninstall ]; then
    rm -f "$FILE"
    rmdir "$FOLDER" 2>/dev/null || true
    printf 'Removed the voices from %s. gtts and its voices are still installed.\n' "$APP"
    exit 0
fi

json() { printf '"%s"' "$(printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g')"; }
# On a Mac, Earmark can start the server itself through the LaunchAgent the first time a
# voice is used. "start" must be an absolute path.
START=
SERVICE="$SCRIPT_DIR/gtts-service"
[ -x "$SERVICE" ] || SERVICE=$(command -v gtts-service 2>/dev/null || true)
if [ "$(uname -s)" = Darwin ] && case $SERVICE in /*) [ -x "$SERVICE" ] ;; *) false ;; esac; then
    START=$(printf ',\n  "start": [%s, "install", "--port", %s]' "$(json "$SERVICE")" "$(json "$PORT")")
fi
mkdir -p "$FOLDER"
TEMP=$(mktemp "$FOLDER/.google-tts-local.XXXXXX")
trap 'rm -f "$TEMP"' EXIT HUP INT TERM
cat > "$TEMP" <<EOF
{
  "name": "Google",
  "url": "$URL/speak",
  "voices": "$URL/voices",
  "speed": "engine"$START
}
EOF
chmod 644 "$TEMP"
mv -f "$TEMP" "$FILE"
trap - EXIT HUP INT TERM
printf 'Registered the voices with %s: %s\n' "$APP" "$FILE"
printf 'They appear under Google in its voice menu, for the book'"'"'s language, when %s next comes to the front.\n' "$APP"
if [ "$(uname -s)" = Darwin ] && [ -z "$START" ]; then
    printf 'Run gtts serve, or gtts-service install, so they can speak.\n'
fi

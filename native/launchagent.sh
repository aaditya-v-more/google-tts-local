#!/bin/sh
set -eu
LABEL=com.aadityavmore.google-tts-local
usage() {
    cat <<'HELP'
Usage: gtts-service generate|install|uninstall [options]
  generate   Write a LaunchAgent plist without loading it
  install    Write and load the per-user LaunchAgent
  uninstall  Unload and remove the per-user LaunchAgent
Options: --output FILE  --gtts FILE  --data-dir DIR  --host IPv4  --port PORT
Defaults: ~/Library/LaunchAgents/com.aadityavmore.google-tts-local.plist,
          sibling gtts executable, GTTS_DATA or ~/Library/Application Support/google-tts-local,
          host 127.0.0.1, port 5591. Run gtts fetch before installing the service.
HELP
}
[ "$#" -gt 0 ] || { usage >&2; exit 2; }
MODE=$1; shift
case $MODE in generate|install|uninstall) ;; -h|--help) usage; exit 0 ;; *) usage >&2; exit 2 ;; esac
: "${HOME:?HOME must be set}"
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
OUTPUT="$HOME/Library/LaunchAgents/$LABEL.plist"
GTTS="$SCRIPT_DIR/gtts"
DATA=${GTTS_DATA:-"$HOME/Library/Application Support/google-tts-local"}
PORT=5591
HOST=127.0.0.1
while [ "$#" -gt 0 ]; do
    case $1 in -h|--help) usage; exit 0 ;; esac
    [ "$#" -ge 2 ] || { echo "Missing value for $1" >&2; exit 2; }
    case $1 in
        --output) OUTPUT=$2 ;;
        --gtts) GTTS=$2 ;;
        --data-dir) DATA=$2 ;;
        --port) PORT=$2 ;;
        --host) HOST=$2 ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
    shift 2
done
case $PORT in ''|*[!0-9]*) echo 'Port must be an integer from 1 to 65535.' >&2; exit 2 ;; esac
[ "$PORT" -ge 1 ] && [ "$PORT" -le 65535 ] || { echo 'Port must be an integer from 1 to 65535.' >&2; exit 2; }
for value in "$OUTPUT" "$GTTS" "$DATA" "$HOST"; do [ -n "$value" ] || { echo 'Paths and host cannot be empty.' >&2; exit 2; }; done
case $OUTPUT in /*) ;; *) OUTPUT="$PWD/$OUTPUT" ;; esac
case $GTTS in /*) ;; *) GTTS="$PWD/$GTTS" ;; esac
case $DATA in /*) ;; *) DATA="$PWD/$DATA" ;; esac
DOMAIN="gui/$(id -u)"
if [ "$MODE" = uninstall ]; then
    if launchctl print "$DOMAIN/$LABEL" >/dev/null 2>&1; then
        launchctl bootout "$DOMAIN/$LABEL" || { echo 'Could not stop the LaunchAgent.' >&2; exit 1; }
    fi
    rm -f "$OUTPUT"
    printf 'Removed LaunchAgent %s. Voice data was retained.\n' "$LABEL"
    exit 0
fi
if [ "$MODE" = install ]; then
    [ -x "$GTTS" ] || { echo "gtts executable not found: $GTTS" >&2; exit 1; }
    [ -r "$DATA/libchrometts.so" ] || { echo 'Voice data is missing. Run gtts fetch first.' >&2; exit 1; }
fi
xml() { printf '%s' "$1" | sed 's/\&/\&amp;/g; s/</\&lt;/g; s/>/\&gt;/g; s/"/\&quot;/g; s/'"'"'/\&apos;/g'; }
LOG_DIR="$HOME/Library/Logs/google-tts-local"
mkdir -p "$(dirname -- "$OUTPUT")"
if [ "$MODE" = install ]; then mkdir -p "$LOG_DIR"; fi
umask 077
TEMP=$(mktemp "${OUTPUT}.XXXXXX")
trap 'rm -f "$TEMP"' EXIT HUP INT TERM
{
    printf '%s\n' '<?xml version="1.0" encoding="UTF-8"?>' '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' '<plist version="1.0"><dict>'
    printf '<key>Label</key><string>%s</string>\n' "$(xml "$LABEL")"
    printf '<key>ProgramArguments</key><array><string>%s</string><string>serve</string><string>--host</string><string>%s</string><string>--port</string><string>%s</string><string>--data-dir</string><string>%s</string></array>\n' "$(xml "$GTTS")" "$(xml "$HOST")" "$(xml "$PORT")" "$(xml "$DATA")"
    printf '%s\n' '<key>RunAtLoad</key><true/>' '<key>KeepAlive</key><true/>'
    printf '<key>StandardOutPath</key><string>%s</string>\n' "$(xml "$LOG_DIR/stdout.log")"
    printf '<key>StandardErrorPath</key><string>%s</string>\n' "$(xml "$LOG_DIR/stderr.log")"
    printf '%s\n' '</dict></plist>'
} > "$TEMP"
plutil -lint "$TEMP" >/dev/null
mv -f "$TEMP" "$OUTPUT"
trap - EXIT HUP INT TERM
if [ "$MODE" = install ]; then
    if launchctl print "$DOMAIN/$LABEL" >/dev/null 2>&1; then
        launchctl bootout "$DOMAIN/$LABEL" || { echo 'Could not stop the existing LaunchAgent.' >&2; exit 1; }
    fi
    launchctl bootstrap "$DOMAIN" "$OUTPUT"
    printf 'Installed and started %s.\n' "$LABEL"
else
    printf 'Wrote %s (not loaded).\n' "$OUTPUT"
fi

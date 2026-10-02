#!/bin/sh
# Starts the e-ink LAN relay if it is not already running.
# Safe to call repeatedly: DSM Task Scheduler runs it at boot and every few
# minutes as a watchdog.
DIR="$(cd "$(dirname "$0")" && pwd)"
PID_FILE="$DIR/relay.pid"
LOG_FILE="$DIR/relay.log"

if [ -f "$PID_FILE" ] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null; then
    exit 0
fi

# Keep the log small: rotate once it passes 1 MB.
if [ -f "$LOG_FILE" ] && [ "$(wc -c < "$LOG_FILE")" -gt 1048576 ]; then
    mv -f "$LOG_FILE" "$LOG_FILE.1"
fi

PYTHON="$(command -v python3 || echo /usr/bin/python3)"
nohup "$PYTHON" -u "$DIR/eink_nas_relay.py" --config "$DIR/relay.env" >> "$LOG_FILE" 2>&1 &
echo $! > "$PID_FILE"

#!/bin/sh
# Install and launch the henge app on an iOS Simulator, stream its console, and
# exit 0 only if the M1 summary line reports "PASS: m1 (0 failures)".
# Usage: scripts/app-run.sh <path/to/henge.app> [simulator name] [timeout seconds]
set -eu
APP=$1
SIM=${2:-iPhone 17}
TIMEOUT=${3:-60}

[ -d "$APP" ] || { echo "app-run: $APP not found (run 'make app' first)" >&2; exit 1; }
BUNDLE_ID=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$APP/Info.plist")

# Boot if needed ("Unable to boot device in current state: Booted" is fine), then wait.
xcrun simctl boot "$SIM" 2>/dev/null || true
xcrun simctl bootstatus "$SIM" -b >/dev/null
xcrun simctl install "$SIM" "$APP"

LOG=$(mktemp -t henge-app-run)
trap 'rm -f "$LOG"' EXIT
# --console-pty keeps the app's stdout line-buffered; simctl blocks until the app exits.
xcrun simctl launch --console-pty --terminate-running-process "$SIM" "$BUNDLE_ID" >"$LOG" 2>&1 &
LAUNCH_PID=$!

status=1
i=0
while [ "$i" -lt "$TIMEOUT" ]; do
  if grep -q '^PASS: m1 (0 failures)' "$LOG"; then status=0; break; fi
  if grep -q '^FAIL: m1 ' "$LOG"; then break; fi
  if ! kill -0 "$LAUNCH_PID" 2>/dev/null; then break; fi
  sleep 1
  i=$((i + 1))
done

xcrun simctl terminate "$SIM" "$BUNDLE_ID" 2>/dev/null || true
kill "$LAUNCH_PID" 2>/dev/null || true
wait "$LAUNCH_PID" 2>/dev/null || true
tr -d '\r' <"$LOG"
if [ "$status" -ne 0 ] && [ "$i" -ge "$TIMEOUT" ]; then
  echo "app-run: timed out after ${TIMEOUT}s waiting for the m1 summary line" >&2
fi
[ "$status" -eq 0 ] && echo "app-run: OK" || echo "app-run: FAILED" >&2
exit "$status"

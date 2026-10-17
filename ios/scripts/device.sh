#!/bin/bash
# Device helpers (iPhone connected by USB or on the same network, Developer Mode on).
#   ios/scripts/device.sh list                    # paired devices
#   ios/scripts/device.sh install                 # install build-ios/iphoneos/BotWRecomp.app
#   ios/scripts/device.sh copy-game [dir]         # copy the game folder (default $BOTW_GAME) into Documents/game
#                                                 # (incremental: unchanged files are skipped)
#   ios/scripts/device.sh run [args...]           # launch with console output on this terminal
#   ios/scripts/device.sh log                     # fetch Library/Application Support/BotWRecomp/runtime.log
# BOTW_DEVICE selects the device (UDID/name); default: the first paired iPhone.
set -euo pipefail
. "$(dirname "$0")/env.sh"
BID="$BOTW_BUNDLE_ID"
dev() {
  if [ -n "${BOTW_DEVICE:-}" ]; then echo "$BOTW_DEVICE"; return; fi
  local j; j=$(mktemp)
  xcrun devicectl list devices --json-output "$j" >/dev/null 2>&1
  uv run --no-project python -I -c "
import json,sys
d=json.load(open('$j'))['result']['devices']
ids=[x['hardwareProperties'].get('udid') for x in d
     if x.get('hardwareProperties',{}).get('deviceType')=='iPhone' and x['hardwareProperties'].get('reality')=='physical']
print(ids[0] if ids else '')"
  rm -f "$j"
}
cmd="${1:-}"; shift || true
need_dev() { local d; d=$(dev); [ -n "$d" ] || { echo "no physical iPhone found (connect it, unlock it, trust this Mac)" >&2; return 1; }; echo "$d"; }
case "$cmd" in install|copy-game|run|log) D=$(need_dev) || exit 1 ;; esac
case "$cmd" in
  list) xcrun devicectl list devices ;;
  install) xcrun devicectl device install app --device "$D" "$ROOT/build-ios/iphoneos/BotWRecomp.app" ;;
  copy-game)
    src="${1:-$BOTW_GAME}"
    time xcrun devicectl device copy to --device "$D" --domain-type appDataContainer --domain-identifier "$BID" \
      --source "$src" --destination Documents/game ;;
  run) xcrun devicectl device process launch --device "$D" --console --terminate-existing "$BID" "$@" ;;
  log)
    xcrun devicectl device copy from --device "$D" --domain-type appDataContainer --domain-identifier "$BID" \
      --source "Library/Application Support/BotWRecomp/runtime.log" --destination "$ROOT/build-ios/runtime-device.log"
    echo "$ROOT/build-ios/runtime-device.log" ;;
  *) sed -n 2,10p "$0"; exit 2 ;;
esac

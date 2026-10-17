#!/bin/bash
# Make Xcode create/renew the iOS development provisioning profile for BotWRecomp
# (bundle ID + entitlements) using the Apple account signed into Xcode > Settings > Accounts.
# Free Personal Team: the profile lasts 7 days and only covers devices registered to the team
# (a phone gets registered the first time it is connected and used from Xcode).
#
#   ios/scripts/provision.sh          # increased-memory-limit only (works on a free team)
#   ios/scripts/provision.sh --paid   # + extended-virtual-addressing (paid program only)
set -euo pipefail
. "$(dirname "$0")/env.sh"
TEAM="$(botw_team)"
[ -n "$TEAM" ] || { echo "no Apple Development certificate in the keychain: sign in under Xcode > Settings > Accounts (Manage Certificates > + Apple Development), or set BOTW_TEAM to your team ID" >&2; exit 1; }
BUNDLE_ID="$BOTW_BUNDLE_ID"
ENT="$ROOT/ios/BotWRecomp.entitlements"
[ "${1:-}" = "--paid" ] && ENT="$ROOT/ios/BotWRecomp-paid.entitlements"
B="$ROOT/build-ios/provision"
cmake -S "$ROOT/ios/provision" -B "$B" -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DBOTW_TEAM="$TEAM" -DBOTW_BUNDLE_ID="$BUNDLE_ID" -DBOTW_ENTITLEMENTS="$ENT" >/dev/null
# With the phone connected, provision for it explicitly so Xcode registers it and the (free-team)
# profile lists it; Xcode drops devices that are not part of the current request.
J=$(mktemp)
xcrun devicectl list devices --json-output "$J" >/dev/null 2>&1 || true
PHONE=$(uv run --no-project python -I -c "
import json
try: d=json.load(open('$J'))['result']['devices']
except Exception: d=[]
ids=[x['hardwareProperties'].get('udid') for x in d if x.get('hardwareProperties',{}).get('reality')=='physical'
     and x['hardwareProperties'].get('deviceType')=='iPhone']
print(ids[0] if ids else '')")
rm -f "$J"
DEST='generic/platform=iOS'
[ -n "${BOTW_DEVICE_UDID:-$PHONE}" ] && DEST="id=${BOTW_DEVICE_UDID:-$PHONE}"
echo "provisioning $BUNDLE_ID for $DEST"
xcodebuild -project "$B/BotWProvision.xcodeproj" -scheme BotWProvision -destination "$DEST" \
  -allowProvisioningUpdates -allowProvisioningDeviceRegistration build 2>&1 | grep -E "error:|BUILD (SUCCEEDED|FAILED)" || true

#!/bin/bash
# Sign build-ios/<sdk>/BotWRecomp.app.
#   iphonesimulator: ad-hoc.
#   iphoneos: embeds the newest unexpired Xcode-managed profile for the bundle ID and signs with
#             the "Apple Development" identity, using only entitlements the profile grants.
set -euo pipefail
. "$(dirname "$0")/env.sh"
SDK="${1:-iphoneos}"
APP="$ROOT/build-ios/$SDK/BotWRecomp.app"
BUNDLE_ID="$BOTW_BUNDLE_ID"
WANT="$ROOT/ios/BotWRecomp.entitlements"
[ "${BOTW_PAID:-0}" = "1" ] && WANT="$ROOT/ios/BotWRecomp-paid.entitlements"

# App icon (optional, not in the repo: put a 1024x1024 PNG at ios/assets/Assets.xcassets/AppIcon.appiconset/
# AppIcon-1024.png): compile the asset catalog into the bundle and merge its Info.plist keys
ICONS="$ROOT/ios/assets/Assets.xcassets"
if [ -f "$ICONS/AppIcon.appiconset/AppIcon-1024.png" ]; then
  xcrun actool "$ICONS" --compile "$APP" --platform "$SDK" --minimum-deployment-target 18.0 \
    --app-icon AppIcon --target-device iphone --target-device ipad \
    --output-partial-info-plist "$ROOT/build-ios/$SDK/icon-info.plist" --output-format human-readable-text >/dev/null
  /usr/libexec/PlistBuddy -c "Merge $ROOT/build-ios/$SDK/icon-info.plist" "$APP/Info.plist" >/dev/null 2>&1 || true
fi

if [ "$SDK" = "iphonesimulator" ]; then
  codesign --force --sign - --timestamp=none "$APP"
  exit 0
fi

OUT="$ROOT/build-ios/$SDK/sign"
mkdir -p "$OUT"
uv run --no-project python -I "$ROOT/ios/scripts/pick_profile.py" "$BUNDLE_ID" "$WANT" "$OUT"
cp "$OUT/profile.mobileprovision" "$APP/embedded.mobileprovision"
IDENTITY="${BOTW_IDENTITY:-$(cat "$OUT/identity.txt")}"
codesign --force --sign "$IDENTITY" --timestamp=none --entitlements "$OUT/entitlements.plist" "$APP"
codesign --verify --strict "$APP" && echo "signed $APP with $IDENTITY"
codesign -d --entitlements - "$APP" 2>/dev/null | grep -E "kernel|application-identifier" -A1 | grep -v "^--" || true

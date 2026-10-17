#!/bin/bash
# Configure + build + sign the iOS app into build-ios/<sdk>/BotWRecomp.app.
#   ios/scripts/build.sh iphoneos          # device (needs a provisioning profile, see provision.sh)
#   ios/scripts/build.sh iphonesimulator   # simulator
# Env: BOTW_IOS_JOBS (default 6), BOTW_PAID=1 (sign with extended-virtual-addressing too).
set -euo pipefail
. "$(dirname "$0")/env.sh"
SDK="${1:-iphoneos}"
B="$ROOT/build-ios/$SDK"
[ -d "$BOTW_IOS_DEPS/prefix-$SDK" ] || "$ROOT/ios/scripts/build_deps.sh" "$SDK"
cmake -S "$ROOT/ios" -B "$B" -G Ninja -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT="$SDK" \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=18.0 \
  -DBOTW_BUNDLE_ID="$BOTW_BUNDLE_ID" -DBOTW_IOS_DEPS="$BOTW_IOS_DEPS" >/dev/null
nice -n 10 cmake --build "$B" -j "${BOTW_IOS_JOBS:-6}"
"$ROOT/ios/scripts/sign.sh" "$SDK"

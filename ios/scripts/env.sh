# Shared settings for the iOS scripts (sourced, not run). Set any of these in the environment or in
# ios/local.env (not committed), e.g. BOTW_BUNDLE_ID=com.yourname.BotWRecomp
#   BOTW_BUNDLE_ID  app bundle ID, unique across Apple accounts (default com.<Mac user name>.BotWRecomp)
#   BOTW_TEAM       Apple team ID (default: the team of the first "Apple Development" certificate)
#   BOTW_IOS_DEPS   where build_deps.sh puts SDL3 / fmt / zstd (default ios-deps/ in the repo)
#   BOTW_GAME       game folder on this Mac with code/ content/ meta/ (default ../game next to the repo)
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
if [ -f "$ROOT/ios/local.env" ]; then
  . "$ROOT/ios/local.env"
fi
BOTW_BUNDLE_ID="${BOTW_BUNDLE_ID:-com.$(id -un | tr 'A-Z' 'a-z' | tr -cd 'a-z0-9-').BotWRecomp}"
BOTW_IOS_DEPS="${BOTW_IOS_DEPS:-$ROOT/ios-deps}"
BOTW_GAME="${BOTW_GAME:-$ROOT/../game}"

botw_team() {
  if [ -n "${BOTW_TEAM:-}" ]; then
    echo "$BOTW_TEAM"
    return
  fi
  security find-certificate -a -c "Apple Development" -p 2>/dev/null |
    openssl x509 -noout -subject -nameopt multiline 2>/dev/null | awk '/organizationalUnitName/ {print $3; exit}'
}

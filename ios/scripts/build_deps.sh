#!/bin/bash
# Fetches pinned SDL3 / fmt / zstd sources and builds them as static arm64 libraries for
# iphoneos and iphonesimulator. Output: ios-deps/prefix-<sdk>/{include,lib}.
#
#   ios/scripts/build_deps.sh            # both SDKs
#   ios/scripts/build_deps.sh iphoneos   # one SDK
#
# Every archive is checked against a pinned SHA-256 (taken from the Homebrew formulae) and
# extracted into its own directory; nothing from the downloads is executed except via CMake.
set -euo pipefail
. "$(dirname "$0")/env.sh"

DEPS="$BOTW_IOS_DEPS"
MIN_IOS="${BOTW_IOS_MIN:-18.0}"
JOBS="${BOTW_IOS_JOBS:-6}"
SDKS=("$@")
[ $# -eq 0 ] && SDKS=(iphoneos iphonesimulator)

# name version url sha256
PKGS=(
  "SDL3 3.4.18 https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz 9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3"
  "fmt 12.2.0 https://github.com/fmtlib/fmt/releases/download/12.2.0/fmt-12.2.0.zip a2f4a8d51178f954e4c339007f77edd76ba0cb2e36f87a48e5a5403d9be5878f"
  "zstd 1.5.7 https://github.com/facebook/zstd/archive/refs/tags/v1.5.7.tar.gz 37d7284556b20954e56e1ca85b80226768902e2edabd3b649e9e72c0c9012ee3"
)

fetch() {  # name ver url sha -> prints source dir
  local name=$1 ver=$2 url=$3 sha=$4
  local dl="$DEPS/downloads/$name-$ver" src="$DEPS/$name-$ver"
  local file="$dl/$(basename "$url")"
  if [ ! -d "$src" ]; then
    mkdir -p "$dl"
    [ -f "$file" ] || curl -fL --retry 3 -o "$file" "$url" >&2
    local got
    got=$(shasum -a 256 "$file" | cut -d' ' -f1)
    if [ "$got" != "$sha" ]; then
      echo "checksum mismatch for $file: $got" >&2
      exit 1
    fi
    local tmp="$DEPS/.extract-$name-$ver"
    rm -rf "$tmp" && mkdir -p "$tmp"
    case "$file" in
      *.zip) ditto -x -k "$file" "$tmp" ;;
      *) tar -xzf "$file" -C "$tmp" ;;
    esac
    # archives contain one top-level directory
    mv "$tmp"/* "$src"
    rmdir "$tmp"
  fi
  echo "$src"
}

build() {  # name srcdir sdk extra-cmake-args...
  local name=$1 src=$2 sdk=$3
  shift 3
  local bdir="$DEPS/build/$name-$sdk" prefix="$DEPS/prefix-$sdk"
  cmake -S "$src" -B "$bdir" -G Ninja \
    -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT="$sdk" -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN_IOS" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$prefix" -DBUILD_SHARED_LIBS=OFF "$@" >/dev/null
  nice -n 10 cmake --build "$bdir" -j "$JOBS"
  cmake --install "$bdir" >/dev/null
}

for sdk in "${SDKS[@]}"; do
  for p in "${PKGS[@]}"; do
    read -r name ver url sha <<<"$p"
    src=$(fetch "$name" "$ver" "$url" "$sha")
    echo "== $name $ver ($sdk)"
    case $name in
      SDL3) build SDL3 "$src" "$sdk" -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF ;;
      fmt) build fmt "$src" "$sdk" -DFMT_TEST=OFF -DFMT_DOC=OFF -DFMT_INSTALL=ON ;;
      zstd) build zstd "$src/build/cmake" "$sdk" -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_SHARED=OFF \
              -DZSTD_BUILD_STATIC=ON -DZSTD_BUILD_TESTS=OFF -DZSTD_MULTITHREAD_SUPPORT=ON ;;
    esac
  done
done
echo "deps installed under $DEPS/prefix-{${SDKS[*]}}"

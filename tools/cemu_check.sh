#!/bin/zsh
# Syntax-check the vendored Cemu sources (runtime/cemu) with the shims; print the first error of each
# failing file, then a summary of missing include files.
cd "$(dirname "$0")/.."
OUT=build/cemu_check; mkdir -p $OUT; rm -f $OUT/*.err
FLAGS=(-std=c++20 -fsyntax-only -w -Iruntime/cemu_shims -Iruntime/cemu -Iruntime/cemu/Cafe -Iruntime/cemu/dependencies/metal-cpp -Iruntime/cemu/dependencies/imgui -DEMULATOR_VERSION_MAJOR=2 -DEMULATOR_VERSION_MINOR=6 -DEMULATOR_VERSION_PATCH=0 -DEMULATOR_HASH=a53cf0e
       -I/opt/homebrew/include -DBOOST_NOWIDE_NO_LIB -DENABLE_METAL -DBOTWRECOMP -include Common/precompiled.h)
files=(${(f)"$( (find runtime/cemu -name '*.cpp' -o -name '*.mm' | grep -v '/dependencies/' | grep -v 'LatteOverlay.cpp\|EmitGLSL' ; ls runtime/cemu_shims/*.cpp) | sort)"})
[[ -n "$1" ]] && files=(${(f)"$(printf '%s\n' $files | grep -E "$1")"})
for f in $files; do
  ( lang=(); [[ $f == *.mm ]] && lang=(-x objective-c++)
    clang++ $FLAGS $lang $f > $OUT/$(echo $f | tr '/' '_').err 2>&1 || true ) &
  while (( $(jobs -r | wc -l) >= 10 )); do sleep 0.2; done
done
wait
ok=0; bad=0
for e in $OUT/*.err; do
  if grep -q 'error:' $e; then bad=$((bad+1)); else ok=$((ok+1)); fi
done
echo "ok $ok, failing $bad"
echo "--- missing includes:"
grep -h "fatal error: '.*' file not found" $OUT/*.err | sed -E "s/.*fatal error: '(.*)' file not found/\1/" | sort | uniq -c | sort -rn | head -40
echo "--- other first errors:"
for e in $OUT/*.err; do grep -m1 'error:' $e | grep -v 'file not found'; done | sed -E 's/^[^:]+:[0-9]+:[0-9]+: //' | sort | uniq -c | sort -rn | head -40

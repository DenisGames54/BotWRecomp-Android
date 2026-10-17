#!/bin/zsh
# Symbolize the last "host frames" line of build/run.log: guest addresses for recompiled frames.
cd "$(dirname "$0")/.."
line=$(grep 'host frames (base' ${1:-build/run.log} | tail -1)
base=$(echo "$line" | sed -E 's/.*base (0x[0-9a-f]+).*/\1/')
addrs=$(echo "$line" | sed -E 's/.*\): //')
atos -o build/BotWRecomp -l $base ${=addrs} | while read -r l; do
  if [[ "$l" == *botw.ppc:* ]]; then
    n=${l##*botw.ppc:}; n=${n%%)*}
    printf "%s   [guest %08x]\n" "$l" $n
  else
    echo "$l"
  fi
done

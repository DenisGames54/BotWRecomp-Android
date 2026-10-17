#!/bin/zsh
# usage: tools/run.sh <seconds> [runtime flags...]   (log: build/run.log)
cd "$(dirname "$0")/.."
secs=${1:-30}; shift
export BOTW_USER_ROOT="$HOME/Library/Application Support/BotWRecomp-test"  # never touches real saves
./build/BotWRecomp --game "${BOTW_GAME:-../game}" --headless --seconds $secs "$@" > build/run.log 2>&1
echo "exit $?"
grep -E "FATAL|unimplemented" build/run.log | head -5

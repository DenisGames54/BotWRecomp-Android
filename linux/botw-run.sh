#!/bin/bash
# Launcher for the Linux ARM64 build (see the README, section "Running").
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
cd "$ROOT" || exit 1
BIN=${BOTW_BIN:-}
if [ -z "$BIN" ]; then
  for b in build-pgo-use build; do [ -x "$ROOT/$b/BotWRecomp" ] && BIN="$ROOT/$b/BotWRecomp" && break; done
fi
[ -x "$BIN" ] || { echo "no binary found: build it first or set BOTW_BIN" >&2; exit 1; }
export BOTW_GAME=${BOTW_GAME:-$ROOT/game}
export BOTW_USER_ROOT=${BOTW_USER_ROOT:-$HOME/botw-user}
K=/sys/class/kgsl/kgsl-3d0
C=/sys/devices/system/cpu
CPUS=${BOTW_CPUS-4-7}

restore() {
  [ -n "$GPU_LOCKED" ] && { echo "$OLD_MIN" > $K/devfreq/min_freq; echo "$OLD_PWR" > $K/min_pwrlevel; }
  if [ -n "$IDLE_OFF" ]; then for c in 4 5 6 7; do for s in $C/cpu$c/cpuidle/state[1-9]*; do echo 0 > $s/disable; done; done; fi
}
trap restore EXIT INT TERM

if [ "${BOTW_GPU_LOCK:-1}" = 1 ] && [ -w $K/devfreq/min_freq ]; then
  OLD_MIN=$(cat $K/devfreq/min_freq); OLD_PWR=$(cat $K/min_pwrlevel)
  cat $K/devfreq/max_freq > $K/devfreq/min_freq
  echo 0 > $K/min_pwrlevel
  GPU_LOCKED=1
fi
if [ -n "$BOTW_NOIDLE" ]; then
  IDLE_OFF=1
  for c in 4 5 6 7; do for s in $C/cpu$c/cpuidle/state[1-9]*; do echo 1 > $s/disable; done; done
fi

(
  sleep 4
  P=$(pgrep -n BotWRecomp)
  if [ -n "$P" ]; then
    HEAVY_TID=$(ps -L -o tid,pcpu -p $P --sort=-pcpu | sed -n '2p' | awk '{print $1}')
    if [ -n "$HEAVY_TID" ]; then
      taskset -cp 7 "$HEAVY_TID" >/dev/null 2>&1
    fi
  fi
) &

${CPUS:+taskset -c $CPUS} "$BIN" "$@"

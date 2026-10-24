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

# Pin the busiest thread (the game's critical path) to the fastest core. The Android scheduler often leaves
# it on a slower core, which changes the frame rate by up to ~35% from run to run. BOTW_HEAVY_CPU= disables it.
fastest_cpu() {
  local b=-1 m=0 f d
  for d in $C/cpu[0-9]*; do
    f=$(cat $d/cpufreq/cpuinfo_max_freq 2>/dev/null) || continue
    if [ "$f" -gt "$m" ]; then m=$f; b=${d##*cpu}; fi
  done
  [ "$b" = -1 ] || echo "$b"
}
HEAVY=${BOTW_HEAVY_CPU-$(fastest_cpu)}
pin_heavy() {
  local p t
  p=$(pgrep -n -x BotWRecomp) || return
  t=$(top -H -b -n 2 -d 2 -p "$p" 2>/dev/null | awk '/^top/{n++} n==2 && $1 ~ /^[0-9]+$/ {print $1; exit}')
  [ -z "$t" ] || [ "$t" = "$PREV_HEAVY" ] && return
  [ -n "$PREV_HEAVY" ] && taskset -cp "${CPUS:-0-7}" "$PREV_HEAVY" >/dev/null 2>&1
  taskset -cp "$HEAVY" "$t" >/dev/null 2>&1 && echo "[botw-run] busiest thread $t -> cpu $HEAVY" >&2 && PREV_HEAVY=$t
}
if [ -n "$HEAVY" ]; then
  ( for d in 6 40 60; do sleep $d; pin_heavy; done ) &
fi

${CPUS:+taskset -c $CPUS} "$BIN" "$@"

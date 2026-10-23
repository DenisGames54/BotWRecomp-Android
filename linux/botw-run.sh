#!/bin/bash
K=/sys/class/kgsl/kgsl-3d0
C=/sys/devices/system/cpu
OLD_MIN=$(cat $K/devfreq/min_freq); OLD_PWR=$(cat $K/min_pwrlevel)
for c in 4 5 6 7; do OLD_GOV[$c]=$(cat $C/cpu$c/cpufreq/scaling_governor); done
restore() {
  if [ -n "$BOTW_NOIDLE" ]; then for c in 4 5 6 7; do for s in $C/cpu$c/cpuidle/state[1-9]*; do echo 0 > $s/disable; done; done; fi
  echo $OLD_MIN > $K/devfreq/min_freq; echo $OLD_PWR > $K/min_pwrlevel
  for c in 4 5 6 7; do echo ${OLD_GOV[$c]} > $C/cpu$c/cpufreq/scaling_governor; done
}
trap restore EXIT INT TERM
cat $K/devfreq/max_freq > $K/devfreq/min_freq
echo 0 > $K/min_pwrlevel

if [ -n "$BOTW_NOIDLE" ]; then for c in 4 5 6 7; do for s in $C/cpu$c/cpuidle/state[1-9]*; do echo 1 > $s/disable; done; done; fi
export BOTW_USER_ROOT=$HOME/botw-user BOTW_GAME=/sdcard/Download/game DISPLAY=:5
cd ~/BotWRecomp
${AFF-taskset -c 4-7} ./build-clang/BotWRecomp "$@"

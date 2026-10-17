#!/bin/zsh
# usage: [BOTW_GAME=<game folder>] tools/play.sh [runtime flags...]   windowed, with sound and controller (F11: fullscreen)
# defaults: 60 fps, 2x resolution; --fps 30 / --scale 1 for the original
cd "$(dirname "$0")/.."
exec ./build/BotWRecomp --game "${BOTW_GAME:-../game}" "$@"

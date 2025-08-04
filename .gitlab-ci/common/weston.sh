#!/usr/bin/env bash

CI_COMMON_DIR=$(dirname -- "${BASH_SOURCE[0]}")

# init-stage2.sh overwrites this when xorg is already started
WESTON_X11_SOCK=${WESTON_X11_SOCK:-/tmp/.X11-unix/X0}
mkdir -p /tmp/.X11-unix
export DISPLAY=:0

WAYLAND_DISPLAY=wayland-0
weston --config="$CI_COMMON_DIR/weston.ini" --socket="$WAYLAND_DISPLAY" "$@" &
export WAYLAND_DISPLAY

while [ ! -S "$WESTON_X11_SOCK" ]; do sleep 1; done

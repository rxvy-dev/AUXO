#!/usr/bin/env bash
killall -q polybar
while pgrep -u "$UID" -x polybar >/dev/null; do sleep 0.2; done
polybar auxo 2>&1 | tee -a /tmp/polybar-"$UID".log & disown

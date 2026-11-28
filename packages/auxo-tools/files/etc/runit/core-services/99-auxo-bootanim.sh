# shellcheck shell=sh
# Auxo boot animation (sourced by runit stage 1): drawn in the background on the console while
# the services start, then it clears itself. Off: sudo auxo-tweak bootanim off, or boot with auxo.noanim.
if [ -x /usr/libexec/auxo/auxo-bootanim ]; then
    /usr/libexec/auxo/auxo-bootanim --tty /dev/tty1 </dev/null >/dev/null 2>&1 &
fi

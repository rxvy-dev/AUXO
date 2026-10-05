# Auxo: close the boot splash once the system is up.
# The initramfs starts Plymouth ("auxo-tweak splash on") and it keeps running after the
# switch to the real root. systemd distros quit it with plymouth-quit.service; runit has no
# such hook, so without this the splash holds the screen forever and the login screen
# (SDDM, GDM, greetd...) or the text console never appears.
# Sourced by /etc/runit/1 after the other core services: plain sh, no exit.
if command -v plymouth >/dev/null 2>&1 && plymouth --ping >/dev/null 2>&1; then
    plymouth quit >/dev/null 2>&1 || :
fi

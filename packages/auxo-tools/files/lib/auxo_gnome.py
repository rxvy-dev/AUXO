"""Auxo and GNOME: plain, stock GNOME.

Auxo 3.0.4 betas shipped a GNOME "rice" (Dash to Dock, Blur my Shell and
system-wide dconf defaults). With it, the normal GNOME session failed to start
while GNOME Classic still worked, so it was removed. GNOME now gets only what
stock GNOME does, plus the wallpaper, dark mode and accent colour that
`auxo-tweak accent` sets through gsettings.

cleanup() undoes the old rice on systems that had it.
"""
import os
import shutil

from auxo_core import R, DRY, log, run, have

EXT_DIR = "/usr/share/gnome-shell/extensions"
DCONF_PROFILE = "/etc/dconf/profile/user"
DCONF_FILE = "/etc/dconf/db/local.d/00-auxo"
# extensions the old rice downloaded from extensions.gnome.org (not owned by any package)
FETCHED = ["dash-to-dock@micxgx.gmail.com", "blur-my-shell@aunetx"]


def _owned_by_package(path):
    if DRY or not have("xbps-query"):
        return False
    return run(["xbps-query", "-o", path], capture=True, check=False).strip() != ""


def cleanup():
    """Remove the old Auxo GNOME rice. Safe to run any number of times."""
    changed = False
    if os.path.exists(R(DCONF_FILE)):
        os.remove(R(DCONF_FILE))
        changed = True
    prof = R(DCONF_PROFILE)
    if os.path.exists(prof) and open(prof).read() == "user-db:user\nsystem-db:local\n":
        os.remove(prof)  # only ours; a profile someone else wrote is left alone
        changed = True
    for uuid in FETCHED:
        d = f"{EXT_DIR}/{uuid}"
        if os.path.isdir(R(d)) and not _owned_by_package(d):
            shutil.rmtree(R(d))
            changed = True
    if changed:
        if have("dconf") or DRY:
            run(["dconf", "update"], check=False)
        log("GNOME: removed the old Auxo GNOME extensions and defaults (stock GNOME from now on)")
    return changed

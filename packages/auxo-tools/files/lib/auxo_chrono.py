"""Chrono, rxvy's Wayland desktop (https://github.com/rxvy-dev/chrono), as an Auxo desktop.

Chrono isn't in Void's repositories, so `auxo-tweak desktop chrono` builds it from source:
Void's packages for the build (DESKTOPS["chrono"], the same list Chrono's own install-deps.sh
uses on Void, including Void's wlroots 0.20), then a pinned, tested commit of Chrono, built
with CMake and installed to /usr (where greetd and other login managers find chrono-session).
"""
import os

from auxo_core import R, DRY, AuxoError, log, run, have

REPO = "https://github.com/rxvy-dev/chrono"
REF = "48590c66e55bbe9f933374a1262048ba50246439"   # tested with Auxo; update with --chrono-ref
SRC = "/var/cache/auxo/chrono"


def installed():
    return os.path.exists(R("/usr/bin/chrono")) and os.path.exists(R("/usr/bin/chrono-session"))


def build(ref=REF, force=False):
    """Fetch Chrono at `ref`, build it and install it to /usr. Needs the build packages."""
    if installed() and not force and not DRY:
        stamp = R(f"{SRC}/.auxo-installed")
        if os.path.exists(stamp) and open(stamp).read().strip() == ref:
            log("Chrono is already built at this version")
            return
    if not DRY and not have("git"):
        raise AuxoError("git is missing (it comes with Chrono's build packages)")
    log(f"building Chrono from {REPO} ({ref[:7]}) — this takes a few minutes")
    src = R(SRC)
    if os.path.isdir(os.path.join(src, ".git")):
        run(["git", "-C", src, "fetch", "--quiet", "origin"])
    else:
        os.makedirs(os.path.dirname(src), exist_ok=True)
        run(["git", "clone", "--quiet", REPO, src])
    run(["git", "-C", src, "checkout", "--quiet", "--force", ref])
    gen = ["-G", "Ninja"] if (have("ninja") or have("samu") or DRY) else []
    run(["cmake", "-S", src, "-B", f"{src}/build", *gen, "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
         "-DCMAKE_INSTALL_PREFIX=/usr"])
    run(["cmake", "--build", f"{src}/build"])
    run(["cmake", "--install", f"{src}/build"])
    if not DRY:
        if not installed():
            raise AuxoError("Chrono built but /usr/bin/chrono-session is missing — see the output above")
        with open(R(f"{SRC}/.auxo-installed"), "w") as f:
            f.write(ref + "\n")
    log("Chrono installed: pick it at the login screen, or run chrono-session from a text console")

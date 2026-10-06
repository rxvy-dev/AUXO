#!/usr/bin/env python3
"""Plays the hidden Summit game (auxo-tweak summit) in a virtual terminal (needs pyte).

  python3 tests/test-summit.py [--shots DIR]
"""
import fcntl
import os
import pty
import select
import struct
import sys
import tempfile
import termios
import time

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PKG = os.path.join(HERE, "packages/auxo-tools/files")
passed = failed = 0


def check(label, ok):
    global passed, failed
    mark = "\033[32m✓" if ok else "\033[31m✗"
    print(f"  {mark}\033[0m {label}")
    passed, failed = passed + bool(ok), failed + (not ok)


def run(cols=100, rows=32, shots=None, extra_env=None, script=None):
    import pyte
    screen = pyte.Screen(cols, rows)
    stream = pyte.ByteStream(screen)
    state = tempfile.mkdtemp()
    env = dict(os.environ, TERM="screen-256color", LANG="C.UTF-8", LC_ALL="C.UTF-8", ESCDELAY="25",
               AUXO_LIB=os.path.join(PKG, "lib"), AUXO_SUMMIT_SEED="7", XDG_STATE_HOME=state, **(extra_env or {}))
    pid, fd = pty.fork()
    if pid == 0:
        fcntl.ioctl(sys.stdout.fileno(), termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
        os.execvpe(sys.executable, [sys.executable, os.path.join(PKG, "bin/auxo-tweak"), "summit"], env)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))

    def pump(t):
        end = time.time() + t
        while time.time() < end:
            r, _, _ = select.select([fd], [], [], 0.03)
            if r:
                try:
                    data = os.read(fd, 65536)
                except OSError:
                    return
                if not data:
                    return
                stream.feed(data)

    def text():
        return "\n".join(screen.display)

    def send(k, t=0.15):
        os.write(fd, k.encode()); pump(t)

    snaps = []

    def snap(name):
        snaps.append((name, [list(screen.buffer[y][x] for x in range(cols)) for y in range(rows)]))

    def alt():
        import re
        m = re.search(r"SUMMIT\s+([\d,]+) m", text())
        return int(m.group(1).replace(",", "")) if m else -1

    if script:
        pump(1.2)
        out = script(send, pump, text, snap)
        try:
            os.kill(pid, 9); os.waitpid(pid, 0)
        except OSError:
            pass
        if shots:
            ns = {"os": os}
            src = open(os.path.join(HERE, "tests/test-installer.py")).read()
            exec(src[src.index("def render(shots"):src.index('print("▲ TUI walkthrough')], ns)
            ns["render"](snaps, shots, cols, rows)
        return out, state
    res = {}
    pump(1.2)
    res["title"] = "S U M M I T" in text() and "press Enter to climb" in text()
    snap("summit-title")
    send("\r", 0.5)
    res["playing"] = alt() == 0 and "♥ ♥ ♥" in text()
    a0 = alt()
    # climb: go up; when blocked by an overhang, step sideways and try again
    for i in range(60):
        before = alt()
        send("w", 0.06)
        if alt() == before:
            send("a" if i % 4 < 2 else "d", 0.06)
    pump(0.3)
    res["climbs"] = alt() > a0
    res["rocks"] = "●" in text() or "✶" in text()
    snap("summit-climbing")
    send("p", 0.4)
    res["pause"] = "paused" in text()
    send("p", 0.2)
    send("q", 0.6)
    try:
        wpid, status = os.waitpid(pid, os.WNOHANG)
        if wpid == 0:
            time.sleep(0.5)
            wpid, status = os.waitpid(pid, os.WNOHANG)
        res["quits"] = wpid == pid and os.WEXITSTATUS(status) == 0
    except ChildProcessError:
        res["quits"] = False
    if not res["quits"]:
        try:
            os.kill(pid, 9)
        except OSError:
            pass
    res["altitude"] = alt()
    if shots:
        ns = {"os": os}
        src = open(os.path.join(HERE, "tests/test-installer.py")).read()
        exec(src[src.index("def render(shots"):src.index('print("▲ TUI walkthrough')], ns)
        ns["render"](snaps, shots, cols, rows)
    return res


print("▲ Summit (hidden game) in a virtual terminal")
try:
    import pyte  # noqa: F401
except ImportError:
    print("  (pyte not installed — skipping: pip install pyte)")
    sys.exit(0)
shots = sys.argv[sys.argv.index("--shots") + 1] if "--shots" in sys.argv else None
r = run(shots=shots)
check("title screen", r["title"])
check("game starts at 0 m with three lives", r["playing"])
check(f"climbing raises the altitude (reached {r['altitude']:,} m)", r["climbs"])
check("rocks fall", r["rocks"])
check("P pauses", r["pause"])
check("Q quits cleanly", r["quits"])
# the summit: a 4-row mountain, climbed straight up
def summit(send, pump, text, snap):
    send("\r", 0.4)
    for _ in range(4):
        send("w", 0.1)
    pump(0.4)
    snap("summit-won")
    return {"won": "YOU REACHED THE SUMMIT" in text() and "8,848 m" in text(), "best": "new best climb" in text()}
w, state = run(shots=shots, extra_env={"AUXO_SUMMIT_ROWS": "4"}, script=summit)
check("reaching the top shows the summit screen", w["won"])
check("a first summit is a new best climb", w["best"])
check("best climb is saved", open(os.path.join(state, "auxo", "summit")).read().strip() == "8848")

# the game logic directly: three rock hits end the climb
sys.path.insert(0, os.path.join(PKG, "lib"))
import random  # noqa: E402
import auxo_summit as S  # noqa: E402
g = S.Game(21, random.Random(1))
for _ in range(3):
    g.hurt = 0
    g.rocks = [[g.px, g.py + 1]]
    g.tick = 3                       # next step moves rocks (fall_every is 4 at the bottom)
    g.step(view_top=10**6)           # spawn rocks far away
check("three hits and the mountain wins", g.dead and g.lives == 0)
g = S.Game(21, random.Random(1)); g.rocks = [[g.px, g.py + 1]]; g.tick = 3; g.step(10**6); g.rocks = [[g.px, g.py + 1]]; g.tick = 7; g.step(10**6)
check("a hit gives a moment of safety", g.lives == 2)

import subprocess
hp = subprocess.run([sys.executable, os.path.join(PKG, "bin/auxo-tweak"), "--help"], capture_output=True, text=True,
                    env=dict(os.environ, AUXO_LIB=os.path.join(PKG, "lib")))
check("summit is not listed in --help", hp.returncode == 0 and "accent" in hp.stdout and "summit" not in hp.stdout)
print(f"\n{passed} passed, {failed} failed")
sys.exit(1 if failed else 0)

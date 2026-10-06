"""Summit — a small climbing game hidden in Auxo.

Climb the wall to 8,848 m. Rocks fall from above; overhangs (█) block your way but
also shatter the rocks, so hide under them. Three lives. Started with `auxo-tweak summit`.

Keys: ← → (or A D / H L) move · ↑ (W / K / Space) climb · P pause · Q quit
"""
import curses
import locale
import os
import random
import time

SUMMIT_M = 8848
SUMMIT_ROW = int(os.environ.get("AUXO_SUMMIT_ROWS", "160"))   # rows to climb (the tests use a short mountain)
TICK = 0.05                      # seconds per game tick
LIVES = 3
SAVE = os.path.join(os.environ.get("XDG_STATE_HOME", os.path.expanduser("~/.local/state")), "auxo", "summit")

ACCENT_COLORS = {"violet": curses.COLOR_MAGENTA, "cyan": curses.COLOR_CYAN, "emerald": curses.COLOR_GREEN,
                 "amber": curses.COLOR_YELLOW, "rose": curses.COLOR_RED, "blue": curses.COLOR_BLUE,
                 "mono": curses.COLOR_WHITE}

TITLE = [
    "        /\\        ",
    "       /  \\       ",
    "      / /\\ \\      ",
    "     / /  \\ \\     ",
    "    /_/    \\_\\    ",
]


def accent():
    try:
        for line in open("/etc/auxo/auxo.conf"):
            if line.startswith("ACCENT="):
                return line.split("=", 1)[1].strip().strip('"')
    except OSError:
        pass
    return "violet"


def load_best():
    try:
        return int(open(SAVE).read().split()[0])
    except (OSError, ValueError, IndexError):
        return 0


def save_best(m):
    try:
        os.makedirs(os.path.dirname(SAVE), exist_ok=True)
        with open(SAVE, "w") as f:
            f.write(f"{m}\n")
    except OSError:
        pass


def altitude(row):
    return min(SUMMIT_M, round(row * SUMMIT_M / SUMMIT_ROW))


class Game:
    def __init__(self, width, rng):
        self.w = width
        self.rng = rng
        self.rows = {}           # world row -> list of chars (" " free, "#" overhang)
        self.px, self.py = width // 2, 0
        self.lives = LIVES
        self.rocks = []          # [x, y] in world rows
        self.bursts = []         # [x, y, ticks_left] shattered rocks
        self.hurt = 0            # invulnerability ticks after a hit
        self.tick = 0
        self.start = time.time()
        self.won = False
        self.dead = False

    # world
    def row(self, y):
        if y not in self.rows:
            r = [" "] * self.w
            if 3 <= y < SUMMIT_ROW - 1:
                chance = 0.28 + 0.25 * y / SUMMIT_ROW
                for _ in range(2):
                    if self.rng.random() < chance:
                        n = self.rng.randint(2, max(2, self.w // 6))
                        x = self.rng.randrange(0, self.w - n + 1)
                        r[x:x + n] = ["#"] * n
            self.rows[y] = r
        return self.rows[y]

    def free(self, x, y):
        return 0 <= x < self.w and self.row(y)[x] != "#"

    # input
    def move(self, dx):
        if self.free(self.px + dx, self.py):
            self.px += dx

    def climb(self):
        if self.free(self.px, self.py + 1):
            self.py += 1
            if self.py >= SUMMIT_ROW:
                self.won = True

    # simulation
    def level(self):
        return self.py / SUMMIT_ROW

    def step(self, view_top):
        self.tick += 1
        if self.hurt:
            self.hurt -= 1
        lvl = self.level()
        if self.rng.random() < 0.06 + 0.22 * lvl:            # more rocks higher up
            self.rocks.append([self.rng.randrange(self.w), view_top + 1])
        fall_every = max(1, 4 - int(lvl * 4))                # and they fall faster
        if self.tick % fall_every == 0:
            keep = []
            for r in self.rocks:
                r[1] -= 1
                if not self.free(r[0], r[1]):                # shattered on an overhang
                    self.bursts.append([r[0], r[1] + 1, 6])
                elif r[1] < self.py - 30:
                    pass                                     # long gone below the screen
                else:
                    keep.append(r)
            self.rocks = keep
        self.bursts = [[x, y, t - 1] for x, y, t in self.bursts if t > 1]
        for r in self.rocks:
            if r[0] == self.px and r[1] == self.py and not self.hurt:
                self.lives -= 1
                self.hurt = 30
                self.rocks.remove(r)
                if self.lives <= 0:
                    self.dead = True
                break

    def elapsed(self):
        return time.time() - self.start


class UI:
    def __init__(self, scr):
        self.scr = scr
        self.utf8 = "UTF-8" in (locale.getpreferredencoding(False) or "").upper()
        self.ch = {"me": "▲" if self.utf8 else "^", "rock": "●" if self.utf8 else "o",
                   "wall": "█" if self.utf8 else "#", "burst": "✶" if self.utf8 else "*",
                   "heart": "♥" if self.utf8 else "<3", "flag": "⚑" if self.utf8 else "F"}
        curses.curs_set(0)
        scr.nodelay(True)
        scr.keypad(True)
        self.color = curses.has_colors()
        if self.color:
            curses.start_color()
            try:
                curses.use_default_colors()
                bg = -1
            except curses.error:
                bg = curses.COLOR_BLACK
            a = ACCENT_COLORS.get(accent(), curses.COLOR_MAGENTA)
            curses.init_pair(1, a, bg)                       # accent
            curses.init_pair(2, curses.COLOR_WHITE, bg)      # rocks
            curses.init_pair(3, curses.COLOR_BLUE, bg)       # wall
            curses.init_pair(4, curses.COLOR_RED, bg)        # hearts / danger
            curses.init_pair(5, curses.COLOR_YELLOW, bg)     # summit / bursts

    def attr(self, n, extra=0):
        return (curses.color_pair(n) if self.color else 0) | extra

    def put(self, y, x, s, a=0):
        h, w = self.scr.getmaxyx()
        if 0 <= y < h and 0 <= x < w:
            try:
                self.scr.addstr(y, x, s[:max(0, w - x - 1)], a)
            except curses.error:
                pass

    def center(self, y, s, a=0):
        self.put(y, max(0, (self.scr.getmaxyx()[1] - len(s)) // 2), s, a)

    def key(self):
        try:
            return self.scr.get_wch()
        except curses.error:
            return None

    def wait_key(self, keys):
        while True:
            k = self.key()
            if k is None:
                time.sleep(TICK)
                continue
            if isinstance(k, str):
                k = k.lower()
                if k in ("\n", "\r"):
                    k = "enter"
            elif k == curses.KEY_ENTER:
                k = "enter"
            if k in keys:
                return k

    # screens
    def title(self, best):
        s = self.scr
        s.erase()
        h, _ = s.getmaxyx()
        top = max(1, h // 2 - 9)
        for i, line in enumerate(TITLE):
            self.center(top + i, line, self.attr(1, curses.A_BOLD))
        self.center(top + 6, "S U M M I T", self.attr(1, curses.A_BOLD))
        self.center(top + 8, "You found the hidden route. Climb to 8,848 m.")
        self.center(top + 9, "Rocks fall from above. Overhangs break them: hide underneath.")
        self.center(top + 11, "← →  move     ↑  climb     P  pause     Q  quit" if self.utf8
                    else "<- ->  move    up  climb    P  pause    Q  quit", self.attr(0, curses.A_DIM))
        if best:
            self.center(top + 13, f"best climb: {best:,} m" + (f"  {self.ch['flag']} summit" if best >= SUMMIT_M else ""),
                        self.attr(5))
        self.center(top + 15, "press Enter to climb", self.attr(1, curses.A_BOLD))
        s.refresh()
        return self.wait_key({"enter", "q", " "}) != "q"

    def draw(self, g, best, paused=False):
        s = self.scr
        s.erase()
        h, w = s.getmaxyx()
        vh = h - 4                                    # wall rows on screen
        left = max(1, (w - g.w) // 2)
        me_row = vh - 4                               # player sits near the bottom
        bottom = g.py - (vh - 1 - me_row)             # world row at the bottom of the view
        alt = altitude(g.py)
        hearts = (self.ch["heart"] + " ") * g.lives
        mm, ss = divmod(int(g.elapsed()), 60)
        self.put(0, left, f"SUMMIT", self.attr(1, curses.A_BOLD))
        self.put(0, left + 8, f"{alt:>5,} m / {SUMMIT_M:,} m")
        self.put(0, left + g.w - 14, f"{hearts:<8}", self.attr(4, curses.A_BOLD))
        self.put(0, left + g.w - 5, f"{mm:02d}:{ss:02d}", self.attr(0, curses.A_DIM))
        # side rails + altitude gauge
        for i in range(vh):
            y = 1 + i
            self.put(y, left - 1, "│" if self.utf8 else "|", self.attr(1))
            self.put(y, left + g.w, "│" if self.utf8 else "|", self.attr(1))
        frac = g.py / SUMMIT_ROW
        mark = 1 + int((1 - frac) * (vh - 1))
        self.put(mark, left + g.w + 2, f"◀ {alt:,}" if self.utf8 else f"< {alt:,}", self.attr(1))
        for i in range(vh):
            wy = bottom + (vh - 1 - i)
            y = 1 + i
            if wy == SUMMIT_ROW:
                self.put(y, left, (" " + self.ch["flag"] + " SUMMIT " + self.ch["flag"] + " ").center(g.w, "~"),
                         self.attr(5, curses.A_BOLD))
                continue
            if wy > SUMMIT_ROW or wy < 0:
                continue
            row = g.row(wy)
            for x, c in enumerate(row):
                if c == "#":
                    self.put(y, left + x, self.ch["wall"], self.attr(3))
        for x, wy, _ in g.bursts:
            self.put(1 + (vh - 1 - (wy - bottom)), left + x, self.ch["burst"], self.attr(5))
        for x, wy in g.rocks:
            if bottom <= wy < bottom + vh:
                self.put(1 + (vh - 1 - (wy - bottom)), left + x, self.ch["rock"], self.attr(2, curses.A_BOLD))
        if not (g.hurt and (g.hurt // 3) % 2):        # blink while invulnerable
            self.put(1 + me_row, left + g.px, self.ch["me"], self.attr(1, curses.A_BOLD))
        foot = "paused · P to carry on · Q to quit" if paused else f"best {best:,} m · P pause · Q quit"
        self.put(h - 2, left, foot, self.attr(0, curses.A_DIM))
        s.refresh()

    def end(self, g, best, new_best):
        s = self.scr
        s.erase()
        h, _ = s.getmaxyx()
        top = max(1, h // 2 - 6)
        alt = altitude(g.py)
        mm, ss = divmod(int(g.elapsed()), 60)
        if g.won:
            for i, line in enumerate(TITLE):
                self.center(top - 6 + i, line, self.attr(5, curses.A_BOLD))
            self.center(top, f"{self.ch['flag']}  YOU REACHED THE SUMMIT  {self.ch['flag']}", self.attr(5, curses.A_BOLD))
            self.center(top + 2, f"8,848 m in {mm}:{ss:02d} with {g.lives} {'life' if g.lives == 1 else 'lives'} left")
            self.center(top + 3, "Climb your own setup, indeed.", self.attr(1))
        else:
            self.center(top, "THE MOUNTAIN WINS THIS TIME", self.attr(4, curses.A_BOLD))
            self.center(top + 2, f"you climbed {alt:,} m in {mm}:{ss:02d}")
        if new_best:
            self.center(top + 5, "new best climb!", self.attr(1, curses.A_BOLD))
        elif best:
            self.center(top + 5, f"best climb: {best:,} m", self.attr(0, curses.A_DIM))
        self.center(top + 7, "R  climb again     Q  quit", self.attr(1, curses.A_BOLD))
        s.refresh()
        return self.wait_key({"r", "q"}) == "r"


LEFT = {curses.KEY_LEFT, "a", "h"}
RIGHT = {curses.KEY_RIGHT, "d", "l"}
UP = {curses.KEY_UP, "w", "k", " "}


def play(scr):
    ui = UI(scr)
    seed = os.environ.get("AUXO_SUMMIT_SEED")
    rng = random.Random(seed) if seed is not None else random.Random()
    best = load_best()
    if not ui.title(best):
        return
    while True:
        h, w = scr.getmaxyx()
        if h < 16 or w < 30:
            scr.erase()
            ui.center(h // 2, "make the terminal bigger to climb (30×16)")
            scr.refresh()
            if ui.wait_key({"q", "enter"}) == "q":
                return
            continue
        g = Game(min(41, w - 16), rng)
        paused = False
        while not (g.won or g.dead):
            t0 = time.time()
            k = ui.key()
            while k is not None:
                kk = k.lower() if isinstance(k, str) else k
                if kk == "q":
                    return
                if kk == "p":
                    paused = not paused
                elif not paused:
                    if kk in LEFT:
                        g.move(-1)
                    elif kk in RIGHT:
                        g.move(1)
                    elif kk in UP:
                        g.climb()
                k = ui.key()
            vh = scr.getmaxyx()[0] - 4                 # wall rows on screen; the player sits 4 up
            if not paused:
                g.step(view_top=g.py + vh - 4)          # world row at the top edge of the screen
            ui.draw(g, best, paused)
            time.sleep(max(0, TICK - (time.time() - t0)))
        alt = altitude(g.py)
        new_best = alt > best
        if new_best:
            best = alt
            save_best(best)
        if not ui.end(g, best, new_best):
            return


def main():
    locale.setlocale(locale.LC_ALL, "")
    os.environ.setdefault("ESCDELAY", "25")
    try:
        curses.wrapper(play)
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

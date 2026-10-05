"""The Auxo installer's text interface (curses).

A classic full-screen installer: steps on the left, the current question on the
right, key hints at the bottom. Everything is keyboard-driven and works on the
Linux console (8 colours) as well as in a 256-colour terminal.
"""
import curses
import os
import subprocess
import sys
import textwrap

from .config import InstallConfig, KEYBOARDS, LOCALES, FILESYSTEMS, EXTRAS, validate_user, validate
from . import disks as disklib
from .plan import build_plan, describe, Env, ONLINE_DESKTOPS
from .runner import Context, execute, InstallError, LOG

sys.path.insert(0, os.environ.get("AUXO_LIB", "/usr/lib/auxo"))
try:
    from auxo_desktops import DESKTOPS, KERNELS, SHELLS
    from auxo_accents import ACCENTS
except ImportError:  # running from the source tree
    here = os.path.dirname(os.path.abspath(__file__))
    sys.path.insert(0, os.path.join(here, "..", "..", "packages", "auxo-tools", "files", "lib"))
    from auxo_desktops import DESKTOPS, KERNELS, SHELLS
    from auxo_accents import ACCENTS

VERSION = "4.0"
STEPS = ["Welcome", "Keyboard", "Language", "Time zone", "Disk", "Desktop", "Look", "System",
         "Account", "Extras", "Install"]
LOGO = [
    "        /\\        ",
    "       /  \\       ",
    "      / /\\ \\      ",
    "     / /  \\ \\     ",
    "    /_/    \\_\\    ",
]
# accent → (256-colour index, basic curses colour for the Linux console)
ACCENT_COLORS = {"violet": (141, curses.COLOR_MAGENTA), "cyan": (45, curses.COLOR_CYAN),
                 "emerald": (42, curses.COLOR_GREEN), "amber": (214, curses.COLOR_YELLOW),
                 "rose": (204, curses.COLOR_RED), "blue": (75, curses.COLOR_BLUE),
                 "mono": (252, curses.COLOR_WHITE)}
DESKTOP_INFO = {
    "plasma": "Modern and flexible, with the Auxo global theme. Installs offline.",
    "gnome": "Clean and simple, with its own login screen.",
    "xfce": "Light and traditional. Great on older PCs.",
    "cinnamon": "Familiar layout, like Windows.",
    "hyprland": "Animated tiling, fully riced by Auxo (from the hyprland-void repo).",
    "sway": "Tiling Wayland, riced by Auxo. Keyboard-driven.",
    "i3": "The classic tiling window manager (X11), riced by Auxo.",
    "none": "No desktop: log in to a text console.",
}
SHELL_INFO = {"zsh": "Auxo prompt, autosuggestions, syntax highlighting (recommended)",
              "fish": "Friendly, with great defaults",
              "bash": "The classic"}

C_NORMAL, C_ACCENT, C_SEL, C_DIM, C_BAR, C_ERR, C_OK, C_WARN, C_BOX = range(1, 10)


class Back(Exception):
    """Esc: go to the previous step."""


class TUI:
    def __init__(self, scr, cfg, env, dry_run=False, disks=None, timezones=None):
        self.scr, self.cfg, self.env, self.dry_run = scr, cfg, env, dry_run
        self.disks = disks
        self.timezones = timezones
        self.step = 0
        self.done = set()
        self.password2 = ""
        curses.curs_set(0)
        scr.keypad(True)
        self.setup_colors()

    # ── colours / frame ───────────────────────────────────────────────
    def setup_colors(self):
        if not curses.has_colors():
            return
        curses.start_color()
        try:
            curses.use_default_colors()
        except curses.error:
            pass
        rich = curses.COLORS >= 256
        idx, basic = ACCENT_COLORS.get(self.cfg.accent, ACCENT_COLORS["violet"])
        acc = idx if rich else basic
        bg = 233 if rich else curses.COLOR_BLACK
        dim = 244 if rich else curses.COLOR_WHITE
        box = 238 if rich else curses.COLOR_BLUE
        fg = 255 if rich else curses.COLOR_WHITE
        curses.init_pair(C_NORMAL, fg, bg)
        curses.init_pair(C_ACCENT, acc, bg)
        curses.init_pair(C_SEL, curses.COLOR_BLACK, acc)
        curses.init_pair(C_DIM, dim, bg)
        curses.init_pair(C_BAR, curses.COLOR_BLACK, acc)
        curses.init_pair(C_ERR, curses.COLOR_RED, bg)
        curses.init_pair(C_OK, curses.COLOR_GREEN, bg)
        curses.init_pair(C_WARN, curses.COLOR_YELLOW, bg)
        curses.init_pair(C_BOX, box, bg)
        self.scr.bkgd(" ", curses.color_pair(C_NORMAL))

    def put(self, y, x, text, attr=0):
        h, w = self.scr.getmaxyx()
        if 0 <= y < h and 0 <= x < w:
            try:
                self.scr.addnstr(y, x, text, max(0, w - x - 1), attr)
            except curses.error:
                pass

    def frame(self, title, hints="↑↓ move · Enter select · Esc back · F10 quit"):
        self.scr.erase()
        h, w = self.scr.getmaxyx()
        if h < 24 or w < 80:
            self.put(0, 0, f"Please make the terminal at least 80×24 (it's {w}×{h}).", curses.color_pair(C_WARN))
            return False
        box = curses.color_pair(C_BOX)
        # header bar
        self.put(0, 0, " " * (w - 1), curses.color_pair(C_BAR))
        self.put(0, 2, "▲ AUXO LINUX", curses.color_pair(C_BAR) | curses.A_BOLD)
        self.put(0, 16, "· installer", curses.color_pair(C_BAR))
        tag = f"v{VERSION}{'  DRY RUN' if self.dry_run else ''} "
        self.put(0, w - len(tag) - 2, tag, curses.color_pair(C_BAR))
        # outer box + sidebar divider
        self.put(1, 0, "┌" + "─" * 19 + "┬" + "─" * (w - 23) + "┐", box)
        for y in range(2, h - 3):
            self.put(y, 0, "│", box)
            self.put(y, 20, "│", box)
            self.put(y, w - 2, "│", box)
        self.put(h - 3, 0, "├" + "─" * 19 + "┴" + "─" * (w - 23) + "┤", box)
        self.put(h - 2, 0, "│", box)
        self.put(h - 2, w - 2, "│", box)
        self.put(h - 1, 0, "└" + "─" * (w - 3) + "┘", box)
        # sidebar steps
        for i, name in enumerate(STEPS):
            y = 3 + i * 2 if h >= 28 else 3 + i
            if i == self.step:
                self.put(y, 2, f" ▸ {name:<14}", curses.color_pair(C_SEL) | curses.A_BOLD)
            elif i in self.done:
                self.put(y, 2, " ✓ ", curses.color_pair(C_OK))
                self.put(y, 5, name, curses.color_pair(C_NORMAL))
            else:
                self.put(y, 5, name, curses.color_pair(C_DIM))
        # title + hints
        self.put(2, 23, title, curses.color_pair(C_ACCENT) | curses.A_BOLD)
        self.put(3, 23, "─" * min(len(title) + 4, w - 27), curses.color_pair(C_BOX))
        self.put(h - 2, 2, hints, curses.color_pair(C_DIM))
        return True

    @property
    def body(self):
        """(top, left, height, width) of the content area under the title."""
        h, w = self.scr.getmaxyx()
        return 5, 23, h - 9, w - 26

    def text(self, y, lines, attr=None):
        top, left, height, width = self.body
        attr = curses.color_pair(C_NORMAL) if attr is None else attr
        for para in lines:
            for line in (textwrap.wrap(para, width) or [""]):
                self.put(y, left, line, attr)
                y += 1
        return y

    def key(self):
        k = self.scr.get_wch()
        if k == curses.KEY_F10:
            if self.ask_yes_no("Quit the installer?", ["Nothing has been changed on your disks yet."
                                                      if self.step < len(STEPS) - 1 else "The install is running."]):
                raise SystemExit(0)
            return None
        if k == curses.KEY_RESIZE:
            return "resize"
        return k

    # ── widgets ───────────────────────────────────────────────────────
    def menu(self, title, items, current=None, intro=None, filterable=False, hints=None, describe=None):
        """items: list of (key, label, detail, enabled). Returns the chosen key; Esc → Back."""
        sel = next((i for i, it in enumerate(items) if it[0] == current), 0)
        query = ""
        while True:
            shown = [it for it in items if query.lower() in (it[1] + " " + it[0]).lower()] if query else items
            sel = min(sel, max(0, len(shown) - 1))
            if not self.frame(title, hints or ("↑↓ move · type to search · Enter select · Esc back" if filterable
                                               else "↑↓ move · Enter select · Esc back · F10 quit")):
                self.key()
                continue
            top, left, height, width = self.body
            y = top
            if intro:
                y = self.text(y, intro, curses.color_pair(C_DIM)) + 1
            if filterable:
                self.put(y, left, f"Search: {query}█", curses.color_pair(C_ACCENT))
                y += 2
            rows = max(3, top + height - y - (4 if describe else 0))
            start = max(0, min(sel - rows // 2, len(shown) - rows))
            for i, (k, label, detail, enabled) in enumerate(shown[start:start + rows]):
                n = start + i
                line = f" {label:<26} {detail}" if detail else f" {label}"
                line = line[:width].ljust(width)
                if n == sel:
                    self.put(y + i, left, line, curses.color_pair(C_SEL) | curses.A_BOLD)
                else:
                    self.put(y + i, left, line, curses.color_pair(C_NORMAL if enabled else C_DIM))
            if len(shown) > rows:
                self.put(top + height, left + width - 12, f"{sel + 1}/{len(shown)}", curses.color_pair(C_DIM))
            if not shown:
                self.put(y, left, "nothing matches", curses.color_pair(C_DIM))
            elif describe:
                dy = top + height - 3
                self.put(dy - 1, left, "─" * width, curses.color_pair(C_BOX))
                for i, line in enumerate(textwrap.wrap(describe(shown[sel][0]) or "", width)[:3]):
                    self.put(dy + i, left, line, curses.color_pair(C_ACCENT))
            self.scr.refresh()
            k = self.key()
            if k in (curses.KEY_UP, "k") and not filterable or k == curses.KEY_UP:
                sel = max(0, sel - 1)
            elif k in (curses.KEY_DOWN, "j") and not filterable or k == curses.KEY_DOWN:
                sel = min(len(shown) - 1, sel + 1)
            elif k == curses.KEY_PPAGE:
                sel = max(0, sel - rows)
            elif k == curses.KEY_NPAGE:
                sel = min(len(shown) - 1, sel + rows)
            elif k == curses.KEY_HOME:
                sel = 0
            elif k == curses.KEY_END:
                sel = len(shown) - 1
            elif k in ("\n", "\r", curses.KEY_ENTER):
                if shown and shown[sel][3]:
                    return shown[sel][0]
                curses.beep()
            elif k == "\x1b":
                if query:
                    query = ""
                else:
                    raise Back()
            elif filterable and k in (curses.KEY_BACKSPACE, "\x7f", "\b"):
                query = query[:-1]
            elif filterable and isinstance(k, str) and k.isprintable():
                query += k
                sel = 0

    def form(self, title, fields, intro=None, check=None):
        """fields: list of [name, label, kind('text'|'password'), value]. check(values) → list of errors."""
        i = 0
        errors = []
        curses.curs_set(1)
        try:
            while True:
                if not self.frame(title, "Tab/↓ next · Shift-Tab/↑ previous · Enter continue · Esc back"):
                    self.key()
                    continue
                top, left, height, width = self.body
                y = top
                if intro:
                    y = self.text(y, intro, curses.color_pair(C_DIM)) + 1
                cursor = None
                for n, (name, label, kind, value) in enumerate(fields):
                    shown = "•" * len(value) if kind == "password" else value
                    self.put(y, left, f"{label:<20}", curses.color_pair(C_ACCENT if n == i else C_NORMAL))
                    field_w = min(36, width - 22)
                    self.put(y, left + 21, "[" + shown[-field_w:].ljust(field_w) + "]",
                             curses.color_pair(C_SEL if n == i else C_DIM))
                    if n == i:
                        cursor = (y, left + 22 + min(len(shown), field_w))
                    y += 2
                for e in errors:
                    y = self.text(y, ["✗ " + e], curses.color_pair(C_ERR))
                if cursor:
                    self.scr.move(*cursor)
                self.scr.refresh()
                k = self.key()
                name, label, kind, value = fields[i]
                if k in ("\t", curses.KEY_DOWN):
                    i = (i + 1) % len(fields)
                elif k in (curses.KEY_BTAB, curses.KEY_UP):
                    i = (i - 1) % len(fields)
                elif k in ("\n", "\r", curses.KEY_ENTER):
                    if i < len(fields) - 1:
                        i += 1
                        continue
                    values = {f[0]: f[3] for f in fields}
                    errors = check(values) if check else []
                    if not errors:
                        return values
                elif k == "\x1b":
                    raise Back()
                elif k in (curses.KEY_BACKSPACE, "\x7f", "\b"):
                    fields[i][3] = value[:-1]
                elif isinstance(k, str) and k.isprintable():
                    fields[i][3] = value + k
        finally:
            curses.curs_set(0)

    def checklist(self, title, items, chosen, intro=None):
        chosen = set(chosen)
        sel = 0
        while True:
            if not self.frame(title, "↑↓ move · Space toggle · Enter continue · Esc back"):
                self.key()
                continue
            top, left, height, width = self.body
            y = top
            if intro:
                y = self.text(y, intro, curses.color_pair(C_DIM)) + 1
            for n, (k, label, enabled) in enumerate(items):
                box = "[x]" if k in chosen else "[ ]"
                line = f" {box} {label}"[:width].ljust(width)
                attr = curses.color_pair(C_SEL) | curses.A_BOLD if n == sel else curses.color_pair(C_NORMAL if enabled else C_DIM)
                self.put(y + n, left, line, attr)
            self.scr.refresh()
            k = self.key()
            if k == curses.KEY_UP:
                sel = max(0, sel - 1)
            elif k == curses.KEY_DOWN:
                sel = min(len(items) - 1, sel + 1)
            elif k == " ":
                key, _, enabled = items[sel]
                if enabled:
                    chosen ^= {key}
                else:
                    curses.beep()
            elif k in ("\n", "\r", curses.KEY_ENTER):
                return [k2 for k2, _, _ in items if k2 in chosen]
            elif k == "\x1b":
                raise Back()

    def ask_yes_no(self, title, lines, yes="Yes", no="No", danger=False):
        choice = 1
        while True:
            h, w = self.scr.getmaxyx()
            bw = min(64, w - 6)
            wrapped = [l for para in lines for l in (textwrap.wrap(para, bw - 4) or [""])]
            bh = len(wrapped) + 6
            y0, x0 = (h - bh) // 2, (w - bw) // 2
            frame = curses.color_pair(C_ERR if danger else C_ACCENT)
            for y in range(bh):
                self.put(y0 + y, x0, " " * bw, curses.color_pair(C_NORMAL))
            self.put(y0, x0, "┌" + "─" * (bw - 2) + "┐", frame)
            for y in range(1, bh - 1):
                self.put(y0 + y, x0, "│", frame)
                self.put(y0 + y, x0 + bw - 1, "│", frame)
            self.put(y0 + bh - 1, x0, "└" + "─" * (bw - 2) + "┘", frame)
            self.put(y0, x0 + 2, f" {title} ", frame | curses.A_BOLD)
            for i, l in enumerate(wrapped):
                self.put(y0 + 2 + i, x0 + 2, l, curses.color_pair(C_NORMAL))
            by = y0 + bh - 2
            ys, ns = f"[ {yes} ]", f"[ {no} ]"
            self.put(by, x0 + bw - len(ys) - len(ns) - 5, ys,
                     (curses.color_pair(C_SEL) | curses.A_BOLD) if choice == 0 else curses.color_pair(C_NORMAL))
            self.put(by, x0 + bw - len(ns) - 3, ns,
                     (curses.color_pair(C_SEL) | curses.A_BOLD) if choice == 1 else curses.color_pair(C_NORMAL))
            self.scr.refresh()
            k = self.scr.get_wch()
            if k in (curses.KEY_LEFT, curses.KEY_RIGHT, "\t"):
                choice ^= 1
            elif k in ("y", "Y"):
                return True
            elif k in ("n", "N", "\x1b"):
                return False
            elif k in ("\n", "\r", curses.KEY_ENTER):
                return choice == 0

    # ── pages ─────────────────────────────────────────────────────────
    def page_welcome(self):
        while True:
            if not self.frame("Welcome to Auxo Linux", "Enter start · N connect to Wi-Fi · F10 quit"):
                self.key()
                continue
            top, left, height, width = self.body
            for i, line in enumerate(LOGO):
                self.put(top + i, left + 2, line, curses.color_pair(C_ACCENT) | curses.A_BOLD)
            self.put(top + 1, left + 24, "Auxo Linux", curses.color_pair(C_NORMAL) | curses.A_BOLD)
            self.put(top + 2, left + 24, "climb your own setup", curses.color_pair(C_DIM))
            self.put(top + 3, left + 24, "Void base · runit · rolling", curses.color_pair(C_DIM))
            y = self.text(top + 7, [
                "This installer puts Auxo on your computer. It asks a few questions, shows you a summary, "
                "and changes nothing until you press Install.",
                "",
                "Everything you pick here can be changed later with auxo-tweak.",
            ])
            net = "connected" if self.env.online else "offline — Plasma and the stable kernel still install; press N for Wi-Fi"
            self.put(y + 1, left, "Internet: ", curses.color_pair(C_DIM))
            self.put(y + 1, left + 10, net, curses.color_pair(C_OK if self.env.online else C_WARN))
            self.put(y + 2, left, f"Boot mode: {'UEFI' if self.env.efi else 'BIOS (legacy)'}", curses.color_pair(C_DIM))
            self.scr.refresh()
            k = self.key()
            if k in ("\n", "\r", curses.KEY_ENTER):
                return
            if k in ("n", "N"):
                self.run_outside(["nmtui"])
                self.env.online = online()

    def run_outside(self, argv):
        curses.endwin()
        try:
            subprocess.run(argv)
        except OSError:
            pass
        self.scr.refresh()

    def page_keyboard(self):
        items = [(k, f"{name}", k, True) for k, name in KEYBOARDS]
        self.cfg.keyboard = self.menu("Keyboard layout", items, self.cfg.keyboard, filterable=True,
                                      intro=["Pick the layout printed on your keyboard."])
        if os.geteuid() == 0 and not self.dry_run:
            subprocess.run(["loadkeys", self.cfg.console_keymap], capture_output=True)

    def page_language(self):
        items = [(k, name, k, True) for k, name in LOCALES]
        self.cfg.locale = self.menu("Language", items, self.cfg.locale, filterable=True,
                                    intro=["The language of your system and apps."])

    def page_timezone(self):
        zones = self.timezones or list_timezones()
        regions = sorted({z.split("/", 1)[0] for z in zones if "/" in z}) + ["UTC"]
        cur = self.cfg.timezone.split("/", 1)[0]
        while True:
            region = self.menu("Time zone — region", [(r, r, "", True) for r in regions], cur)
            if region == "UTC":
                self.cfg.timezone = "UTC"
                return
            cities = [z for z in zones if z.startswith(region + "/")]
            try:
                self.cfg.timezone = self.menu(f"Time zone — {region}",
                                              [(z, z.split("/", 1)[1].replace("_", " "), "", True) for z in cities],
                                              self.cfg.timezone, filterable=True)
                return
            except Back:
                continue

    def page_disk(self):
        if self.disks is None:
            self.disks = disklib.probe()
        while True:
            items = []
            for d in self.disks:
                label = f"{d['path']}  {disklib.human(d['size'])}"
                detail = d["model"] + (f" ({d['tran']})" if d["tran"] else "")
                if not d["usable"]:
                    detail = f"— {d['reason']}"
                items.append((d["path"], label, detail, d["usable"]))
            items.append(("manual", "Use existing partitions…", "dual boot / keep data", True))
            if not any(d["usable"] for d in self.disks):
                intro = ["No disk can be erased for Auxo (see the reasons). You can still use existing partitions."]
            else:
                intro = ["Pick the disk to ERASE and install Auxo on, or use partitions you made yourself."]
            choice = self.menu("Where should Auxo go?", items,
                               self.cfg.disk if self.cfg.mode == "erase" else "manual", intro=intro)
            try:
                if choice == "manual":
                    self.cfg.mode = "manual"
                    self.pick_partitions()
                else:
                    self.cfg.mode, self.cfg.disk = "erase", choice
                self.cfg.filesystem = self.menu("Filesystem", [(k, k, v.split("—", 1)[1].strip(), True)
                                                               for k, v in FILESYSTEMS.items()], self.cfg.filesystem)
                return
            except Back:
                continue

    def pick_partitions(self):
        parts = [(p["path"], f"{p['path']}  {disklib.human(p['size'])}",
                  f"{p['fstype'] or 'empty'} {p['label']}".strip(), not p["mountpoint"])
                 for d in self.disks for p in d["parts"]]
        if not parts:
            self.ask_yes_no("No partitions", ["There are no partitions to use. Make them with cfdisk first, "
                                              "or go back and erase a disk."], yes="OK", no="Back")
            raise Back()
        self.cfg.root_part = self.menu("Partition for Auxo ( / )", parts, self.cfg.root_part,
                                       intro=["This partition is FORMATTED. Everything on it is deleted."])
        if self.env.efi:
            efi = [p for p in parts if p[0] != self.cfg.root_part]
            efi.sort(key=lambda p: "vfat" not in p[2])
            self.cfg.efi_part = self.menu("EFI system partition", efi, self.cfg.efi_part,
                                          intro=["The small FAT32 partition the computer boots from. "
                                                 "If another system already uses it, Auxo is added next to it."])
            fstype = next((p[2] for p in parts if p[0] == self.cfg.efi_part), "")
            self.cfg.format_efi = "vfat" not in fstype and self.ask_yes_no(
                "Format the EFI partition?", [f"{self.cfg.efi_part} isn't FAT32 yet. Format it as FAT32?"])

    def page_desktop(self):
        items = []
        for k, d in DESKTOPS.items():
            tag = "needs internet" if k in ONLINE_DESKTOPS else "installs offline"
            items.append((k, d["name"], tag, True))
        intro = None if self.env.online else ["You're offline: desktops that need internet are swapped for Plasma."]
        self.cfg.desktop = self.menu("Desktop", items, self.cfg.desktop, intro=intro,
                                     describe=lambda k: DESKTOP_INFO.get(k, ""))

    def page_look(self):
        items = [(k, k, v[0], True) for k, v in ACCENTS.items()]
        if True:
            choice = self.menu("Accent colour", items, self.cfg.accent,
                               intro=["One colour for the boot menu, login screen, terminal, bars and panels. "
                                      "The installer previews it as you go."],
                               hints="↑↓ move · Enter pick · Esc back")
            self.cfg.accent = choice
            self.setup_colors()
            return

    def page_system(self):
        while True:
            self.cfg.kernel = self.menu("Kernel", [(k, k, v, True) for k, v in KERNELS.items()], self.cfg.kernel,
                                        intro=["The kernel you pick becomes the default; you can switch any time."])
            try:
                self.cfg.shell = self.menu("Shell", [(k, k, SHELL_INFO[k], True) for k in SHELLS], self.cfg.shell)
                return
            except Back:
                continue

    def page_account(self):
        fields = [["fullname", "Your name", "text", self.cfg.fullname],
                  ["username", "Username", "text", self.cfg.username],
                  ["password", "Password", "password", self.cfg.password],
                  ["password2", "Password again", "password", self.password2],
                  ["hostname", "Computer name", "text", self.cfg.hostname],
                  ["root_password", "Root password", "password", self.cfg.root_password]]

        def check(v):
            c = InstallConfig(**{**self.cfg.__dict__, "fullname": v["fullname"], "username": v["username"],
                                 "password": v["password"], "hostname": v["hostname"]})
            return validate_user(c, v["password2"])
        v = self.form("Your account", fields, check=check,
                      intro=["You'll use sudo for admin tasks. Leave the root password empty to keep root locked "
                             "(recommended)."])
        self.cfg.fullname, self.cfg.username, self.cfg.password = v["fullname"], v["username"], v["password"]
        self.password2, self.cfg.hostname, self.cfg.root_password = v["password2"], v["hostname"], v["root_password"]

    def page_extras(self):
        items = []
        for k, label in EXTRAS.items():
            enabled = not (k == "snapshots" and self.cfg.filesystem != "btrfs")
            if k in ("gaming", "flatpak") and not self.env.online:
                label += " (needs internet)"
            if not enabled:
                label += " (needs btrfs)"
            items.append((k, label, enabled))
        chosen = [x for x in self.cfg.extras if not (x == "snapshots" and self.cfg.filesystem != "btrfs")]
        self.cfg.extras = self.checklist("Extras", items, chosen, intro=["All optional. Each one can be switched "
                                                                          "on or off later with auxo-tweak."])

    def page_review(self):
        errs = validate(self.cfg, self.env.efi)
        while True:
            if not self.frame("Ready to install", "Enter install · Esc back · F10 quit"):
                self.key()
                continue
            top, left, height, width = self.body
            y = top
            friendly = {"Desktop": DESKTOPS.get(self.cfg.desktop, {}).get("name", self.cfg.desktop),
                        "Language": dict(LOCALES).get(self.cfg.locale, self.cfg.locale),
                        "Keyboard": dict(KEYBOARDS).get(self.cfg.keyboard, self.cfg.keyboard)}
            for label, value in describe(self.cfg, self.env):
                value = friendly.get(label, value)
                danger = label == "Disk" and self.cfg.mode == "erase"
                self.put(y, left, f"{label:<12}", curses.color_pair(C_DIM))
                self.put(y, left + 13, value[:width - 13], curses.color_pair(C_ERR if danger else C_NORMAL) |
                         (curses.A_BOLD if danger else 0))
                y += 1
            _, notes = build_plan(self.cfg, self.env)
            y += 1
            for n in notes:
                y = self.text(y, ["• " + n], curses.color_pair(C_WARN))
            for e in errs:
                y = self.text(y, ["✗ " + e], curses.color_pair(C_ERR))
            self.put(top + height - 1, left, "[ Install ]" if not errs else "Fix the problems above first (Esc).",
                     curses.color_pair(C_SEL) | curses.A_BOLD if not errs else curses.color_pair(C_ERR))
            self.scr.refresh()
            k = self.key()
            if k == "\x1b":
                raise Back()
            if k in ("\n", "\r", curses.KEY_ENTER) and not errs:
                lines = (["ALL DATA on " + self.cfg.disk + " will be deleted.", "", "This can't be undone."]
                         if self.cfg.mode == "erase" else
                         [f"{self.cfg.root_part} will be formatted.", "", "This can't be undone."])
                if self.ask_yes_no("Install Auxo now?", lines, yes="Install", no="Go back", danger=True):
                    return

    def page_install(self):
        steps, notes = build_plan(self.cfg, self.env)
        state = {"step": 0, "pct": 0.0, "lines": []}

        def draw():
            if not self.frame("Installing Auxo", "Please wait — don't turn off the computer"):
                return
            top, left, height, width = self.body
            for i, s in enumerate(steps[:height - 8]):
                mark, attr = ("✓", C_OK) if i < state["step"] else (("▸", C_ACCENT) if i == state["step"] else (" ", C_DIM))
                self.put(top + i, left, f" {mark} {s.title}", curses.color_pair(attr))
            by = top + min(len(steps), height - 8) + 1
            bw = width - 8
            fill = int(bw * state["pct"])
            self.put(by, left, "█" * fill, curses.color_pair(C_ACCENT))
            self.put(by, left + fill, "░" * (bw - fill), curses.color_pair(C_DIM))
            self.put(by, left + bw + 1, f"{int(state['pct'] * 100):3d}%", curses.color_pair(C_NORMAL) | curses.A_BOLD)
            for i, line in enumerate(state["lines"][-(top + height - by - 2):]):
                self.put(by + 2 + i, left, line[:width], curses.color_pair(C_DIM))
            self.scr.refresh()

        def on_step(i, s):
            state["step"] = i
            draw()

        def on_progress(p):
            state["pct"] = p
            draw()

        def on_line(line):
            state["lines"].append(line.replace("\t", " "))
            state["lines"] = state["lines"][-50:]
            draw()

        ctx = Context(self.env.target, dry_run=self.dry_run, on_line=on_line)
        try:
            execute(steps, ctx, on_step, on_progress)
            state["step"] = len(steps)
            draw()
            return notes + ctx.notes, None
        except (InstallError, OSError) as e:
            return notes + ctx.notes, str(e)

    def page_done(self, notes, error):
        while True:
            ok = error is None
            if not self.frame("Auxo is installed" if ok else "The install stopped",
                              "R reboot · Enter exit to the live system" if ok else "Enter exit · L show the log path"):
                self.key()
                continue
            top, left, height, width = self.body
            if ok:
                y = self.text(top, ["Remove the USB stick and reboot to start Auxo.",
                                    "", "Change anything later with auxo-tweak, and update with auxo-update."])
            else:
                y = self.text(top, ["Something went wrong:", ""], curses.color_pair(C_ERR))
                y = self.text(y, [error], curses.color_pair(C_NORMAL))
                y = self.text(y + 1, [f"The full log is in {LOG}. Your disk may be partly written; "
                                      "fix the problem and run the installer again."], curses.color_pair(C_DIM))
            for n in notes:
                y = self.text(y + 1, ["• " + n], curses.color_pair(C_WARN))
            self.scr.refresh()
            k = self.key()
            if k in ("r", "R") and ok and not self.dry_run:
                subprocess.run(["reboot"])
            if k in ("\n", "\r", curses.KEY_ENTER, "q"):
                return ok

    # ── flow ──────────────────────────────────────────────────────────
    def run(self):
        pages = [self.page_welcome, self.page_keyboard, self.page_language, self.page_timezone, self.page_disk,
                 self.page_desktop, self.page_look, self.page_system, self.page_account, self.page_extras,
                 self.page_review]
        while self.step < len(pages):
            try:
                pages[self.step]()
                self.done.add(self.step)
                self.step += 1
            except Back:
                self.step = max(0, self.step - 1)
        notes, error = self.page_install()
        if error is None:
            self.done.add(self.step)
        return self.page_done(notes, error)


def list_timezones(root="/usr/share/zoneinfo"):
    zones = []
    regions = ("Africa", "America", "Antarctica", "Arctic", "Asia", "Atlantic", "Australia", "Europe", "Indian", "Pacific")
    for r in regions:
        base = os.path.join(root, r)
        for dirpath, _dirs, files in os.walk(base):
            for f in files:
                zones.append(os.path.relpath(os.path.join(dirpath, f), root))
    return sorted(zones) or ["America/New_York", "America/Vancouver", "Europe/London", "Europe/Berlin", "Asia/Tokyo"]


def online(host="repo-default.voidlinux.org"):
    import socket
    try:
        socket.create_connection((host, 443), timeout=4).close()
        return True
    except OSError:
        return False


def main(cfg, env, dry_run=False, disks=None, timezones=None):
    os.environ.setdefault("ESCDELAY", "25")
    return curses.wrapper(lambda scr: TUI(scr, cfg, env, dry_run, disks, timezones).run())

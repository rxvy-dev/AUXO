#!/usr/bin/env python3
"""AuxoBSD Kconfig: a Kconfig-subset parser, resolver and curses menuconfig.

Supported: mainmenu, menu/endmenu, comment, source, config, bool/int/string
(with prompt), default [if], depends on, select [if], range, help.
Expressions: y n SYMBOL ! && || ( )

Commands:
  defconfig        reset to defaults, apply configs/defconfig, write outputs
  olddefconfig     keep .config values, fill in new symbols with defaults
  allyesconfig     enable every visible bool
  allnoconfig      disable every bool
  menuconfig       interactive curses editor
  genheader        regenerate build/config/autoconf.h and rust/src/autoconf.rs
  listconfig       print the resolved configuration
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KCONFIG = os.path.join(ROOT, "Kconfig")
DOTCONFIG = os.path.join(ROOT, ".config")
DEFCONFIG = os.path.join(ROOT, "configs", "defconfig")
OUT_H = os.path.join(ROOT, "build", "config", "autoconf.h")
OUT_RS = os.path.join(ROOT, "rust", "src", "autoconf.rs")


class Sym:
    def __init__(self, name):
        self.name = name
        self.type = "bool"
        self.prompt = None
        self.defaults = []      # (value, cond-or-None)
        self.depends = []       # expression strings (own + inherited from menus)
        self.selects = []       # (symbol name, cond-or-None)
        self.range = None
        self.help = ""
        self.value = "n"
        self.user = None        # value chosen by the user, None = use default
        self.forced = False     # turned on by another symbol's `select`


class Node:
    def __init__(self, kind, title=None, sym=None):
        self.kind = kind        # menu | sym | comment
        self.title = title
        self.sym = sym
        self.children = []
        self.depends = []


class Config:
    def __init__(self, path=KCONFIG):
        self.syms = {}
        self.order = []
        self.title = "Configuration"
        self.root = Node("menu", "root")
        self.base = os.path.dirname(path)
        self._parse(path, self.root, [])
        self.resolve()

    # ---------------------------------------------------------------- parse
    @staticmethod
    def _quoted(s):
        m = re.search(r'"((?:[^"\\]|\\.)*)"', s)
        return m.group(1).replace('\\"', '"') if m else None

    @staticmethod
    def _split_if(s):
        m = re.match(r"^(.*?)\s+if\s+(.*)$", s)
        return (m.group(1).strip(), m.group(2).strip()) if m else (s.strip(), None)

    def _parse(self, path, top, menu_deps):
        lines = open(path).read().split("\n")
        stack = [top]
        dep_stack = [list(menu_deps)]
        cur = None
        cur_menu = None
        i = 0
        while i < len(lines):
            raw = lines[i]
            i += 1
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            kw, _, rest = line.partition(" ")
            rest = rest.strip()

            if kw == "mainmenu":
                self.title = self._quoted(rest) or self.title
            elif kw == "source":
                sub = self._quoted(rest)
                self._parse(os.path.join(self.base, sub), stack[-1], dep_stack[-1])
            elif kw == "menu":
                n = Node("menu", self._quoted(rest))
                stack[-1].children.append(n)
                stack.append(n)
                dep_stack.append(list(dep_stack[-1]))
                cur, cur_menu = None, n
            elif kw == "endmenu":
                stack.pop()
                dep_stack.pop()
                cur, cur_menu = None, None
            elif kw == "comment":
                stack[-1].children.append(Node("comment", self._quoted(rest)))
                cur = cur_menu = None
            elif kw == "config":
                s = self.syms.get(rest) or Sym(rest)
                if rest not in self.syms:
                    self.syms[rest] = s
                    self.order.append(s)
                s.depends = list(dep_stack[-1])
                node = Node("sym", sym=s)
                stack[-1].children.append(node)
                cur, cur_menu = s, None
            elif kw in ("bool", "int", "string") and cur:
                cur.type = kw
                p = self._quoted(rest)
                if p:
                    cur.prompt = p
            elif kw == "prompt" and cur:
                cur.prompt = self._quoted(rest)
            elif kw == "default" and cur:
                val, cond = self._split_if(rest)
                q = self._quoted(val)
                cur.defaults.append((q if q is not None else val, cond))
            elif kw == "depends":
                expr = rest[3:].strip() if rest.startswith("on ") else rest
                if cur:
                    cur.depends.append(expr)
                elif cur_menu:
                    cur_menu.depends.append(expr)
                    for j in range(len(dep_stack) - 1, len(dep_stack)):
                        dep_stack[j].append(expr)
            elif kw == "select" and cur:
                name, cond = self._split_if(rest)
                cur.selects.append((name, cond))
            elif kw == "range" and cur:
                a, b = rest.split()[:2]
                cur.range = (int(a), int(b))
            elif kw == "help" and cur:
                indent = len(raw) - len(raw.lstrip())
                buf = []
                while i < len(lines):
                    l2 = lines[i]
                    if l2.strip() == "" or (len(l2) - len(l2.lstrip())) > indent:
                        buf.append(l2.strip())
                        i += 1
                    else:
                        break
                cur.help = "\n".join(buf).strip()
            else:
                raise SystemExit("%s: cannot parse line: %r" % (path, line))

    # ----------------------------------------------------------- expressions
    def _val_true(self, name):
        if name == "y":
            return True
        if name == "n":
            return False
        s = self.syms.get(name)
        if not s:
            return False
        if s.type == "bool":
            return s.value == "y"
        return s.value not in ("", "0")

    def eval(self, expr):
        if expr is None or expr.strip() == "":
            return True
        toks = re.findall(r"&&|\|\||!|\(|\)|[A-Za-z0-9_]+", expr)
        pos = [0]

        def peek():
            return toks[pos[0]] if pos[0] < len(toks) else None

        def take():
            t = peek()
            pos[0] += 1
            return t

        def p_or():
            v = p_and()
            while peek() == "||":
                take()
                r = p_and()
                v = v or r
            return v

        def p_and():
            v = p_not()
            while peek() == "&&":
                take()
                r = p_not()
                v = v and r
            return v

        def p_not():
            if peek() == "!":
                take()
                return not p_not()
            return p_atom()

        def p_atom():
            t = take()
            if t == "(":
                v = p_or()
                take()
                return v
            return self._val_true(t) if t else False

        return p_or()

    # --------------------------------------------------------------- resolve
    def visible(self, s):
        return s.prompt is not None and all(self.eval(d) for d in s.depends)

    def _clamp(self, s, v):
        try:
            n = int(v, 0)
        except (ValueError, TypeError):
            n = s.range[0] if s.range else 0
        if s.range:
            n = max(s.range[0], min(s.range[1], n))
        return str(n)

    def _default(self, s):
        for val, cond in s.defaults:
            if self.eval(cond):
                return val
        return {"bool": "n", "int": str(s.range[0]) if s.range else "0", "string": ""}[s.type]

    def resolve(self):
        for s in self.order:
            s.forced = False
        for _ in range(64):
            forced = set()
            for s in self.order:
                if s.type == "bool" and s.value == "y":
                    for name, cond in s.selects:
                        if self.eval(cond):
                            forced.add(name)
            changed = False
            for s in self.order:
                vis = self.visible(s)
                s.forced = s.name in forced
                if s.type == "bool":
                    if s.forced:
                        new = "y"
                    elif not vis:
                        new = "n"
                    elif s.user is not None:
                        new = s.user
                    else:
                        new = self._default(s)
                    new = "y" if new == "y" else "n"
                else:
                    if vis and s.user is not None:
                        new = s.user
                    else:
                        new = self._default(s)
                    if s.type == "int":
                        new = self._clamp(s, new)
                if new != s.value:
                    s.value = new
                    changed = True
            if not changed:
                return
        raise SystemExit("kconfig: dependency resolution did not converge")

    # -------------------------------------------------------------- file I/O
    def load(self, path):
        if not os.path.exists(path):
            return
        for line in open(path):
            line = line.rstrip("\n")
            m = re.match(r"^CONFIG_(\w+)=(.*)$", line)
            if m and m.group(1) in self.syms:
                s = self.syms[m.group(1)]
                v = m.group(2)
                if s.type == "string":
                    v = v[1:-1] if len(v) >= 2 and v[0] == '"' else v
                    v = v.replace('\\"', '"').replace("\\\\", "\\")
                s.user = v
                continue
            m = re.match(r"^# CONFIG_(\w+) is not set$", line)
            if m and m.group(1) in self.syms:
                self.syms[m.group(1)].user = "n"
        self.resolve()

    def dump_config(self):
        out = ["#", "# Automatically generated by tools/kconfig.py - edit with `make menuconfig`", "#"]

        def walk(node):
            for ch in node.children:
                if ch.kind == "menu":
                    out.extend(["", "#", "# " + ch.title, "#"])
                    walk(ch)
                elif ch.kind == "sym":
                    s = ch.sym
                    if s.type == "bool":
                        out.append("CONFIG_%s=y" % s.name if s.value == "y"
                                   else "# CONFIG_%s is not set" % s.name)
                    elif s.type == "int":
                        out.append("CONFIG_%s=%s" % (s.name, s.value))
                    else:
                        esc = s.value.replace("\\", "\\\\").replace('"', '\\"')
                        out.append('CONFIG_%s="%s"' % (s.name, esc))

        walk(self.root)
        return "\n".join(out) + "\n"

    def write_outputs(self, dotconfig=True):
        if dotconfig:
            with open(DOTCONFIG, "w") as f:
                f.write(self.dump_config())
        os.makedirs(os.path.dirname(OUT_H), exist_ok=True)
        h = ["/* generated by tools/kconfig.py - do not edit */",
             "#ifndef AUXO_AUTOCONF_H", "#define AUXO_AUTOCONF_H"]
        r = ["// generated by tools/kconfig.py - do not edit",
             "#![allow(dead_code)]"]
        for s in self.order:
            n = "CONFIG_" + s.name
            if s.type == "bool":
                if s.value == "y":
                    h.append("#define %s 1" % n)
                r.append("pub const %s: bool = %s;" % (n, "true" if s.value == "y" else "false"))
            elif s.type == "int":
                h.append("#define %s %s" % (n, s.value))
                r.append("pub const %s: usize = %s;" % (n, s.value))
            else:
                esc = s.value.replace("\\", "\\\\").replace('"', '\\"')
                h.append('#define %s "%s"' % (n, esc))
                r.append('pub const %s: &str = "%s";' % (n, esc))
        h.append("#endif")
        new_h = "\n".join(h) + "\n"
        new_r = "\n".join(r) + "\n"
        # only touch files when content changes so `make` does not rebuild needlessly
        for path, text in ((OUT_H, new_h), (OUT_RS, new_r)):
            try:
                if open(path).read() == text:
                    continue
            except OSError:
                pass
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w") as f:
                f.write(text)


# ------------------------------------------------------------------- menuconfig
def menuconfig(cfg):
    import curses

    state = {"dirty": False}

    def visible_items(menu):
        items = []
        for ch in menu.children:
            if ch.kind == "menu":
                if all(cfg.eval(d) for d in ch.depends):
                    items.append(ch)
            elif ch.kind == "sym":
                if cfg.visible(ch.sym):
                    items.append(ch)
            else:
                items.append(ch)
        return items

    def label(it):
        if it.kind == "menu":
            return "    %s  --->" % it.title
        if it.kind == "comment":
            return "    *** %s ***" % it.title
        s = it.sym
        if s.type == "bool":
            box = "-*-" if (s.forced and s.value == "y") else ("[*]" if s.value == "y" else "[ ]")
            return "%s %s" % (box, s.prompt)
        v = s.value if s.type == "int" else (s.value if len(s.value) < 30 else s.value[:27] + "...")
        return "(%s) %s" % (v, s.prompt)

    def popup(scr, title, lines, wait=True):
        h, w = scr.getmaxyx()
        bw = min(w - 4, max([len(title)] + [len(x) for x in lines]) + 4)
        bh = min(h - 2, len(lines) + 4)
        y0, x0 = (h - bh) // 2, (w - bw) // 2
        win = curses.newwin(bh, bw, y0, x0)
        win.box()
        win.addstr(0, 2, " %s " % title[: bw - 6])
        for k, l in enumerate(lines[: bh - 4]):
            win.addstr(k + 2, 2, l[: bw - 4])
        win.refresh()
        if wait:
            win.getch()

    def prompt_text(scr, title, initial):
        buf = list(initial)
        h, w = scr.getmaxyx()
        while True:
            popup(scr, title, ["".join(buf) + "_", "", "Enter = accept   Esc = cancel"], wait=False)
            ch = scr.getch()
            if ch in (10, 13, curses.KEY_ENTER):
                return "".join(buf)
            if ch == 27:
                return None
            if ch in (curses.KEY_BACKSPACE, 127, 8):
                if buf:
                    buf.pop()
            elif 32 <= ch < 127:
                buf.append(chr(ch))

    def edit(scr, it):
        s = it.sym
        if s.type == "bool":
            if s.forced:
                popup(scr, "Selected", ["%s is selected by another option and cannot be disabled." % s.name,
                                         "", "press any key"])
                return
            s.user = "n" if s.value == "y" else "y"
        else:
            hint = ("%s (range %d..%d)" % (s.prompt, *s.range)) if s.range else s.prompt
            v = prompt_text(scr, hint, s.value)
            if v is None:
                return
            if s.type == "int" and not re.match(r"^-?(0x[0-9a-fA-F]+|\d+)$", v):
                popup(scr, "Invalid", ["Not an integer: %s" % v, "", "press any key"])
                return
            s.user = v
        state["dirty"] = True
        cfg.resolve()

    def show_help(scr, it):
        if it.kind != "sym":
            return
        s = it.sym
        lines = ["CONFIG_%s  (%s)" % (s.name, s.type), "Current value: %s" % s.value, ""]
        lines += (s.help or "No help available.").split("\n")
        if s.depends:
            lines += ["", "Depends on: " + " && ".join(s.depends)]
        popup(scr, s.prompt or s.name, lines + ["", "press any key"])

    def save(scr):
        with open(DOTCONFIG, "w") as f:
            f.write(cfg.dump_config())
        cfg.write_outputs(dotconfig=False)
        state["dirty"] = False
        popup(scr, "Saved", ["Configuration written to .config", "", "press any key"])

    def ui(scr):
        curses.curs_set(0)
        scr.keypad(True)
        path = [cfg.root]
        pos = [0]
        top = [0]
        while True:
            menu = path[-1]
            items = visible_items(menu)
            pos[-1] = max(0, min(pos[-1], len(items) - 1)) if items else 0
            h, w = scr.getmaxyx()
            scr.erase()
            scr.addstr(0, 0, (" " + cfg.title)[: w - 1], curses.A_BOLD)
            crumb = " > ".join(n.title for n in path[1:]) or "(top)"
            scr.addstr(1, 0, (" " + crumb)[: w - 1])
            area = max(1, h - 5)
            if pos[-1] < top[-1]:
                top[-1] = pos[-1]
            if pos[-1] >= top[-1] + area:
                top[-1] = pos[-1] - area + 1
            for row in range(area):
                k = top[-1] + row
                if k >= len(items):
                    break
                attr = curses.A_REVERSE if k == pos[-1] else curses.A_NORMAL
                scr.addstr(3 + row, 1, label(items[k]).ljust(w - 3)[: w - 3], attr)
            foot = " Enter/Space: select   y/n: set   ?: help   Esc/Left: back   s: save   q: quit"
            scr.addstr(h - 1, 0, foot[: w - 1], curses.A_DIM)
            scr.refresh()

            ch = scr.getch()
            cur = items[pos[-1]] if items else None
            if ch in (curses.KEY_DOWN, ord("j")) and items:
                pos[-1] = (pos[-1] + 1) % len(items)
            elif ch in (curses.KEY_UP, ord("k")) and items:
                pos[-1] = (pos[-1] - 1) % len(items)
            elif ch == curses.KEY_NPAGE and items:
                pos[-1] = min(len(items) - 1, pos[-1] + area)
            elif ch == curses.KEY_PPAGE and items:
                pos[-1] = max(0, pos[-1] - area)
            elif ch == curses.KEY_HOME:
                pos[-1] = 0
            elif ch == curses.KEY_END and items:
                pos[-1] = len(items) - 1
            elif ch in (10, 13, curses.KEY_ENTER, curses.KEY_RIGHT, ord("l")) and cur:
                if cur.kind == "menu":
                    path.append(cur)
                    pos.append(0)
                    top.append(0)
                elif cur.kind == "sym":
                    edit(scr, cur)
            elif ch == ord(" ") and cur and cur.kind == "sym":
                edit(scr, cur)
            elif ch in (ord("y"), ord("n")) and cur and cur.kind == "sym" and cur.sym.type == "bool":
                if not cur.sym.forced:
                    cur.sym.user = chr(ch)
                    state["dirty"] = True
                    cfg.resolve()
            elif ch == ord("?") and cur:
                show_help(scr, cur)
            elif ch == ord("s"):
                save(scr)
            elif ch in (27, curses.KEY_LEFT, curses.KEY_BACKSPACE, 127, ord("h")) and len(path) > 1:
                path.pop(); pos.pop(); top.pop()
            elif ch in (ord("q"), 27, curses.KEY_LEFT, ord("h")):
                if state["dirty"]:
                    popup(scr, "Quit", ["You have unsaved changes.", "", "s = save and quit   d = discard   c = cancel"],
                          wait=False)
                    a = scr.getch()
                    if a == ord("s"):
                        save(scr)
                        return
                    if a == ord("d"):
                        return
                else:
                    return

    curses.wrapper(ui)


# ------------------------------------------------------------------------ main
def main(argv):
    cmd = argv[1] if len(argv) > 1 else "menuconfig"
    cfg = Config()
    if cmd == "defconfig":
        cfg.load(argv[2] if len(argv) > 2 else DEFCONFIG)
        cfg.write_outputs()
        print("configuration written to .config (defconfig)")
    elif cmd == "olddefconfig":
        cfg.load(DOTCONFIG)
        cfg.write_outputs()
    elif cmd == "allyesconfig":
        for s in cfg.order:
            if s.type == "bool":
                s.user = "y"
        cfg.resolve()
        cfg.write_outputs()
    elif cmd == "allnoconfig":
        for s in cfg.order:
            if s.type == "bool":
                s.user = "n"
        cfg.resolve()
        cfg.write_outputs()
    elif cmd == "menuconfig":
        if not os.path.exists(DOTCONFIG):
            cfg.load(DEFCONFIG)
        else:
            cfg.load(DOTCONFIG)
        menuconfig(cfg)
    elif cmd == "genheader":
        cfg.load(DOTCONFIG)
        cfg.write_outputs(dotconfig=False)
    elif cmd == "listconfig":
        cfg.load(DOTCONFIG)
        for s in cfg.order:
            print("CONFIG_%s=%s" % (s.name, s.value))
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main(sys.argv)

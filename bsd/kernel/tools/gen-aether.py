#!/usr/bin/env python3
"""Compile Aether's data (github.com/rxvy-dev/aether, copied into aether/) into
include/aether_data.h for the Aether app on AuxoBSD:

  aether/templates.json   the starter `main` file for each language
  aether/styles/*.xml     Aether's colour schemes (GtkSourceView style schemes)

    python3 tools/gen-aether.py [--check]
"""
import json
import os
import re
import sys
import xml.etree.ElementTree as ET

KDIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(KDIR, "aether")
OUT = os.path.join(KDIR, "include", "aether_data.h")
DEFAULT_THEME = "aether-crimson"           # Aether's default (aether/settings.py)

# language id → display name, file name for a new file, highlighter family
LANGS = {
    "c": ("C", "main.c", "c"), "cpp": ("C++", "main.cpp", "c"), "python": ("Python", "main.py", "py"),
    "javascript": ("JavaScript", "main.js", "js"), "typescript": ("TypeScript", "main.ts", "js"),
    "javascriptreact": ("JSX", "App.jsx", "js"), "typescriptreact": ("TSX", "App.tsx", "js"),
    "rust": ("Rust", "main.rs", "rust"), "go": ("Go", "main.go", "go"), "java": ("Java", "Main.java", "c"),
    "csharp": ("C#", "Program.cs", "c"), "kotlin": ("Kotlin", "Main.kt", "c"), "swift": ("Swift", "main.swift", "c"),
    "dart": ("Dart", "main.dart", "c"), "scala": ("Scala", "Main.scala", "c"), "shellscript": ("Shell", "main.sh", "sh"),
    "lua": ("Lua", "main.lua", "lua"), "perl": ("Perl", "main.pl", "sh"), "ruby": ("Ruby", "main.rb", "py"),
    "php": ("PHP", "index.php", "c"), "haskell": ("Haskell", "Main.hs", "hs"), "zig": ("Zig", "main.zig", "rust"),
    "elixir": ("Elixir", "main.ex", "py"), "r": ("R", "main.R", "py"), "julia": ("Julia", "main.jl", "py"),
    "html": ("HTML", "index.html", "html"), "css": ("CSS", "style.css", "css"), "sql": ("SQL", "query.sql", "sql"),
    "makefile": ("Makefile", "Makefile", "sh"), "dockerfile": ("Dockerfile", "Dockerfile", "sh"),
    "toml": ("TOML", "config.toml", "sh"), "yaml": ("YAML", "config.yaml", "sh"), "markdown": ("Markdown", "README.md", "md"),
    "json": ("JSON", "data.json", "js"),
}
STYLE_KEYS = [
    ("bg", "text", "background"), ("fg", "text", "foreground"), ("sel", "selection", "background"),
    ("cursor", "cursor", "foreground"), ("line", "current-line", "background"),
    ("gutter_fg", "line-numbers", "foreground"), ("gutter_bg", "line-numbers", "background"),
    ("keyword", "def:keyword", "foreground"), ("type", "def:type", "foreground"),
    ("string", "def:string", "foreground"), ("comment", "def:comment", "foreground"),
    ("number", "def:number", "foreground"), ("function", "def:function", "foreground"),
    ("preproc", "def:preprocessor", "foreground"), ("operator", "def:operator", "foreground"),
    ("heading", "def:heading", "foreground"),
]


def snippet_to_text(s):
    """Expand a VS Code-style snippet to plain text: ${1:x} → x, $0 / ${TM_…} → nothing."""
    out, i = [], 0
    while i < len(s):
        if s[i] == "\\" and i + 1 < len(s) and s[i + 1] in "$}\\":
            out.append(s[i + 1]); i += 2; continue
        if s[i] == "$":
            if i + 1 < len(s) and s[i + 1].isdigit():
                i += 1
                while i < len(s) and s[i].isdigit():
                    i += 1
                continue
            if i + 1 < len(s) and s[i + 1] == "{":
                depth, j = 1, i + 2
                while j < len(s) and depth:
                    depth += {"{": 1, "}": -1}.get(s[j], 0)
                    j += 1
                inner = s[i + 2:j - 1]
                m = re.match(r"\d+:(.*)$", inner, re.S)
                out.append(snippet_to_text(m.group(1)) if m else "")
                i = j
                continue
        out.append(s[i]); i += 1
    return "".join(out)


def cstr(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n").replace("\t", "\\t") + '"'


def hexcol(v, default):
    if not v or not re.fullmatch(r"#[0-9a-fA-F]{6}", v):
        return default
    return 0xFF000000 | int(v[1:], 16)


def themes():
    out = []
    for fn in sorted(os.listdir(os.path.join(SRC, "styles"))):
        if not fn.endswith(".xml"):
            continue
        root = ET.parse(os.path.join(SRC, "styles", fn)).getroot()
        st = {e.get("name"): e for e in root.iter("style")}
        vals = {}
        for key, name, attr in STYLE_KEYS:
            e = st.get(name)
            vals[key] = hexcol(e.get(attr) if e is not None else None, None)
        fg = vals["fg"] or 0xFFDDDDDD
        bg = vals["bg"] or 0xFF101010
        for k in vals:
            if vals[k] is None:
                vals[k] = {"gutter_bg": bg, "line": bg, "heading": vals["keyword"] or fg, "type": vals["keyword"] or fg,
                           "function": fg, "operator": fg, "preproc": vals["keyword"] or fg}.get(k, fg)
        if vals["sel"] == fg:
            vals["sel"] = 0xFF404040
        lum = ((bg >> 16 & 255) * 3 + (bg >> 8 & 255) * 6 + (bg & 255)) / 10
        out.append((root.get("id"), root.get("name"), lum > 128, vals))
    return out


def main(argv):
    check = "--check" in argv
    tpl = json.load(open(os.path.join(SRC, "templates.json")))
    th = themes()
    lines = ["/* Generated by tools/gen-aether.py from aether/ (github.com/rxvy-dev/aether, MIT).",
             " * Do not edit; run `make aether-data`. */",
             "#ifndef AUXO_AETHER_DATA_H", "#define AUXO_AETHER_DATA_H", "#include <stdint.h>", "",
             "enum { HL_PLAIN, HL_C, HL_PY, HL_JS, HL_RUST, HL_GO, HL_SH, HL_LUA, HL_HS, HL_HTML, HL_CSS, HL_SQL, HL_MD };",
             "struct aether_lang { const char *id, *name, *file; int hl; const char *main; };",
             "struct aether_theme { const char *id, *name; int light; uint32_t " +
             ", ".join(k for k, _, _ in STYLE_KEYS) + "; };", "",
             "static const struct aether_lang aether_langs[] __attribute__((unused)) = {"]
    for lid, (name, file, fam) in LANGS.items():
        if lid not in tpl:
            continue
        main = snippet_to_text(tpl[lid].get("main", ""))
        lines.append(f"    {{ {cstr(lid)}, {cstr(name)}, {cstr(file)}, HL_{fam.upper()},\n      {cstr(main)} }},")
    lines.append("};")
    lines.append(f"#define AETHER_LANG_COUNT {sum(1 for l in LANGS if l in tpl)}")
    lines.append("")
    lines.append("static const struct aether_theme aether_themes[] __attribute__((unused)) = {")
    default = 0
    for i, (tid, name, light, v) in enumerate(th):
        if tid == DEFAULT_THEME:
            default = i
        cols = ", ".join(f"0x{v[k]:08x}" for k, _, _ in STYLE_KEYS)
        lines.append(f"    {{ {cstr(tid)}, {cstr(name)}, {int(light)}, {cols} }},")
    lines.append("};")
    lines.append(f"#define AETHER_THEME_COUNT {len(th)}")
    lines.append(f"#define AETHER_THEME_DEFAULT {default}")
    lines += ["", "#endif", ""]
    text = "\n".join(lines)
    if check:
        same = os.path.exists(OUT) and open(OUT).read() == text
        print("aether_data.h is up to date" if same else "aether_data.h is stale: run make aether-data")
        return 0 if same else 1
    open(OUT, "w").write(text)
    print(f"{sum(1 for l in LANGS if l in tpl)} templates, {len(th)} themes → {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

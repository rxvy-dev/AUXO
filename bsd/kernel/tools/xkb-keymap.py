#!/usr/bin/env python3
"""Regenerate include/xkb_keymap.h: the US keymap compiled by libxkbcommon, so Wayland clients
on AuxoBSD get a complete keymap (no XKB include files needed).

    python3 tools/xkb-keymap.py build/rootfs     # a root filesystem built with ROOTFS_SET=full
"""
import ctypes
import os
import sys

ROOT = sys.argv[1] if len(sys.argv) > 1 else "build/rootfs"
OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "include", "xkb_keymap.h")
SOURCE = b"""xkb_keymap {
  xkb_keycodes { include "evdev+aliases(qwerty)" };
  xkb_types { include "complete" };
  xkb_compat { include "complete" };
  xkb_symbols { include "pc+us+inet(evdev)" };
  xkb_geometry { include "pc(pc105)" };
};"""

lib = ctypes.CDLL(os.path.join(ROOT, "usr/lib/x86_64-linux-gnu/libxkbcommon.so.0"))
lib.xkb_context_new.restype = ctypes.c_void_p
lib.xkb_context_include_path_append.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
lib.xkb_keymap_new_from_string.restype = ctypes.c_void_p
lib.xkb_keymap_new_from_string.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_int]
lib.xkb_keymap_get_as_string.restype = ctypes.c_char_p
lib.xkb_keymap_get_as_string.argtypes = [ctypes.c_void_p, ctypes.c_int]
ctx = lib.xkb_context_new(1)                       # XKB_CONTEXT_NO_DEFAULT_INCLUDES
lib.xkb_context_include_path_append(ctx, os.path.join(ROOT, "usr/share/X11/xkb").encode())
km = lib.xkb_keymap_new_from_string(ctx, SOURCE, 1, 0)
if not km:
    sys.exit("xkb-keymap: libxkbcommon couldn't compile the keymap")
text = lib.xkb_keymap_get_as_string(km, 1).decode()
lines = ['/* The US keyboard as a compiled XKB keymap (libxkbcommon, "evdev+aliases(qwerty)" / "pc+us+inet(evdev)").',
         ' * Sent to Wayland clients whole, so they need no XKB data files. Regenerate with',
         ' * tools/xkb-keymap.py. */', "#ifndef AUXO_XKB_KEYMAP_H", "#define AUXO_XKB_KEYMAP_H", "static const char XKB_KEYMAP[] ="]
lines += ['    "' + l.replace("\\", "\\\\").replace('"', '\\"') + '\\n"' for l in text.rstrip("\n").split("\n")]
lines[-1] += ";"
lines += ["#endif", ""]
open(OUT, "w").write("\n".join(lines))
print(f"{len(text)} bytes → {OUT}")

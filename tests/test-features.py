#!/usr/bin/env python3
"""AUXO 5.0 features: taskbar/dock pins, desktop widgets, VPNs, the boot animation, GRUB themes,
the new Hyprland styles and the live session's fallback.   python3 tests/test-features.py"""
import glob
import importlib.machinery
import importlib.util
import json
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PKG = os.path.join(HERE, "packages/auxo-tools/files")
sys.path.insert(0, os.path.join(PKG, "lib"))
HOME = tempfile.mkdtemp()
DATA = tempfile.mkdtemp()
os.environ.update(AUXO_HOME=HOME, XDG_DATA_DIRS=DATA, XDG_DATA_HOME=os.path.join(HOME, ".local/share"))
passed = failed = 0


def check(name, cond):
    global passed, failed
    passed += bool(cond)
    failed += not cond
    print(("  \033[32m✓\033[0m " if cond else "  \033[31m✗\033[0m ") + name)


def load_script(name, path):
    loader = importlib.machinery.SourceFileLoader(name, path)
    spec = importlib.util.spec_from_loader(name, loader)
    m = importlib.util.module_from_spec(spec)
    loader.exec_module(m)
    return m


def strip_jsonc(text):
    return json.loads(re.sub(r"^\s*//.*$", "", text, flags=re.M))


# fake installed apps
os.makedirs(f"{DATA}/applications")
os.makedirs(f"{DATA}/icons/Papirus/64x64/apps")
for app, name, extra in (("firefox", "Firefox", "StartupWMClass=firefox\n"), ("kitty", "kitty", ""),
                         ("thunar", "Thunar", ""), ("steam", "Steam", ""), ("auxo-settings", "Auxo Settings", ""),
                         ("com.discordapp.Discord", "Discord", "")):
    exe = "flatpak run com.discordapp.Discord" if "discord" in app else app
    open(f"{DATA}/applications/{app}.desktop", "w").write(
        f"[Desktop Entry]\nType=Application\nName={name}\nExec={exe} %U\nIcon={app}\n{extra}")
    open(f"{DATA}/icons/Papirus/64x64/apps/{app}.svg", "w").write("<svg/>")

print("▲ pinned apps (auxo-pins)")
import auxo_pins as P  # noqa: E402
import auxo_shell as S  # noqa: E402
apps = S.load_apps()
check("defaults: only installed apps, files/terminal once", P.load(apps) == [
    "thunar.desktop", "firefox.desktop", "kitty.desktop", "steam.desktop", "com.discordapp.Discord.desktop",
    "auxo-settings.desktop"])
P.add("Steam", apps, at=0)
check("pin by name, at a position", P.load(apps)[0] == "steam.desktop" and P.load(apps).count("steam.desktop") == 1)
P.remove("discord", apps)
check("unpin by short name", "com.discordapp.Discord.desktop" not in P.load(apps))
P.move("firefox", 1, apps)
check("move right", P.load(apps).index("firefox.desktop") == 3)
try:
    P.add("nope", apps)
    check("unknown app refused", False)
except ValueError:
    check("unknown app refused", True)
files = P.write_waybar(apps)
mods = json.loads(open(f"{HOME}/.config/waybar/pins-30.jsonc").read().split("\n", 1)[1])
check("a Waybar file per icon size", len(files) == 3 and all(os.path.exists(f) for f in files))
check("group/pins lists the pins in order", mods["group/pins"]["modules"][0] == "image#pin-steam")
img = mods["image#pin-firefox"]
check("pin: icon, click opens/focuses, right-click menu", img["path"].endswith("firefox.svg") and img["size"] == 30
      and img["on-click"] == "auxo-pins launch firefox.desktop" and img["on-click-right"] == "auxo-pins menu firefox.desktop")
check("pinned apps left out of the task list (no doubles)", "firefox" in mods["wlr/taskbar"]["ignore-list"]
      and "auxo-start" in mods["wlr/taskbar"]["ignore-list"])
for rice, size in (("hyprland-win", 30), ("hyprland", 22), ("hyprland-neon", 22), ("hyprland-zen", 22),
                   ("hyprland-candy", 22)):
    c = strip_jsonc(open(f"{PKG}/share/auxo/rices/{rice}/waybar/config.jsonc").read())
    check(f"{rice}: bar includes the {size}px pins", f"~/.config/waybar/pins-{size}.jsonc" in c["include"]
          and "group/pins" in c["modules-left"] + c["modules-center"] + c.get("modules-right", []))
dock = [b for b in strip_jsonc(open(f"{PKG}/share/auxo/rices/hyprland-mac/waybar/config.jsonc").read()) if b.get("name") == "dock"][0]
check("Mac dock: 48px pins", dock["include"] == ["~/.config/waybar/pins-48.jsonc"] and "group/pins" in dock["modules-center"])
check("no hard-coded app icons left in the bars", not any(re.search(r'"image#(files|firefox|steam|discord)"', open(p).read())
                                                         for p in glob.glob(f"{PKG}/share/auxo/rices/*/waybar/config.jsonc")))
check("Hyprland writes the pins before Waybar starts",
      "exec-once = auxo-pins refresh --no-reload; waybar" in open(f"{PKG}/share/auxo/rices/hyprland/hypr/hyprland.conf").read())
launcher = open(f"{PKG}/bin/auxo-launcher").read()
check("Start menu + Launchpad: pin to taskbar / keep in Dock", "Pin to taskbar" in launcher and "Keep in Dock" in launcher)
r = subprocess.run([sys.executable, f"{PKG}/bin/auxo-pins"], capture_output=True, text=True,
                   env={**os.environ, "AUXO_LIB": f"{PKG}/lib"})
check("auxo-pins lists the pins", r.returncode == 0 and "Steam" in r.stdout)

print("▲ desktop widgets (auxo-widgets)")
W = load_script("auxo_widgets", f"{PKG}/bin/auxo-widgets")
check("default widgets: clock, system, music", W.enabled() == ["clock", "system", "music"])
W.set_enabled(["calendar", "clock", "bogus"])
check("turn widgets on/off (unknown ignored)", W.enabled() == ["clock", "calendar"])
c = W.load()
c["PLACE"] = "bottom-left"
W.save(c)
check("place saved", W.load()["PLACE"] == "bottom-left")
j = json.dumps({"current_condition": [{"temp_C": "7", "temp_F": "45", "FeelsLikeC": "4", "FeelsLikeF": "39",
                                       "weatherDesc": [{"value": "Light rain"}], "weatherCode": "296"}],
                "nearest_area": [{"areaName": [{"value": "Vancouver"}]}]})
W.CACHE = os.path.join(HOME, "w.json")
w = W.weather("", fetch=lambda url: j)
check("weather from wttr.in: temperature, city, rain glyph", w["temp"] == "7" and w["city"] == "Vancouver"
      and W.weather_glyph(w["code"]) == "")
check("weather offline: None, no crash", W.weather("Nowhere", fetch=lambda url: 1 / 0) is None)
check("music: playerctl parsed", W.music_status(lambda a: "Playing\tSong\tArtist\n") == ("Playing", "Song", "Artist")
      and W.music_status(lambda a: "") is None)
check("widgets sit on the desktop layer, blurred on Hyprland",
      "Layer.BOTTOM" in open(f"{PKG}/bin/auxo-widgets").read()
      and "layerrule = blur, auxo-widgets" in open(f"{PKG}/share/auxo/rices/hyprland/hypr/hyprland.conf").read())

print("▲ VPN (Mullvad, Proton, any WireGuard file)")
import auxo_vpn as V  # noqa: E402
calls = []


def nm(argv, input=None, timeout=30):
    calls.append(argv)
    if argv[:2] == ["wg", "genkey"]:
        return 0, "PRIV\n"
    if argv[:2] == ["wg", "pubkey"]:
        return 0, "PUB\n"
    if argv[:3] == ["nmcli", "-t", "-f"]:
        return 0, "Home:802-11-wireless:yes\nmullvad-se-got:wireguard:yes\nwork\\:vpn:vpn:no\n"
    return 0, "Connection 'x' successfully added."


def net(url, data=None):
    if url == V.MULLVAD_KEY_API:
        return "10.64.1.2/32,fc00:bbbb::2/128"
    if "www/relays" in url:
        return json.dumps([{"hostname": "se-got-wg-001", "ipv4_addr_in": "1.2.3.4", "pubkey": "K", "country_code": "se",
                            "country_name": "Sweden", "city_name": "Gothenburg", "active": True},
                           {"hostname": "ch-zrh-wg-001", "ipv4_addr_in": "5.6.7.8", "pubkey": "K2", "country_code": "ch",
                            "country_name": "Switzerland", "city_name": "Zurich", "active": True}])
    return "You are connected to Mullvad"


v = V.VPN(runner=nm, fetch=net, tmpdir=tempfile.mkdtemp())
conns = v.connections()
check("lists WireGuard and OpenVPN connections only", [c["name"] for c in conns] == ["mullvad-se-got", "work:vpn"]
      and conns[0]["active"] and conns[0]["provider"] == "mullvad")
made = v.mullvad_account("1234 5678 1234 5678", country="ch")
check("Mullvad account: key made here, servers added", made == ["mullvad-ch-zrh"]
      and ["nmcli", "connection", "import", "type", "wireguard", "file", os.path.join(v.tmpdir, "mullvad-ch-zrh.conf")] in calls)
check("imported VPNs don't connect at every boot",
      ["nmcli", "connection", "modify", "mullvad-ch-zrh", "connection.autoconnect", "no"] in calls)
check("no private key file left behind", not os.listdir(v.tmpdir))
for bad, why in (("123", "16 digits"),):
    try:
        v.mullvad_account(bad)
    except V.VPNError as e:
        check("bad account number explained", why in str(e))
d = tempfile.mkdtemp()
pf = os.path.join(d, "wg-CH#22 FREE.conf")
open(pf, "w").write("[Interface]\nPrivateKey = a\nAddress = 10.2.0.2/32\nDNS = 10.2.0.1\n\n[Peer]\nPublicKey = b\n"
                    "AllowedIPs = 0.0.0.0/0\nEndpoint = 1.1.1.1:51820\n")
check("Proton file: recognised, valid interface name", v.import_file(pf) == "proton-wg-ch-22")
open(pf, "w").write("[Interface]\nAddress = 1\n")
try:
    v.import_file(pf)
    check("broken config refused", False)
except V.VPNError as e:
    check("broken config refused", "missing" in str(e))
check("interface names fit the 15-character limit", all(len(V.nm_name(n, "mullvad")) <= 15 for n in
                                                        ("a-very-long-server-name.conf", "x.conf", "SE GOT 001.conf")))
old = os.path.getmtime(pf) - 5
check("download watcher finds the new file", V.newest_download(old, d) == pf and V.newest_download(old + 60, d) is None)
tweak = open(f"{PKG}/bin/auxo-tweak").read()
check("auxo-tweak vpn: status, mullvad, proton, import, on, off", all(f'a in ("{x}"' in tweak or f'a == "{x}"' in tweak
                                                                     or f'"{x}")' in tweak for x in ("import", "check"))
      and 'sub.add_parser("vpn"' in tweak)

print("▲ boot: animation + GRUB themes")
B = load_script("auxo_bootanim", f"{PKG}/libexec/auxo-bootanim")
fr = B.frames(80, 25, "cyan", 4)
total = sum(d for d, _ in fr)
check("animation takes about 4 seconds", 3.5 < total < 5)
check("hides the cursor, then clears and gives it back", fr[0][1].startswith("\033[?25l") and fr[-1][1].endswith("\033[?25h"))
check("in the accent colour (console colours)", "\033[1;96m" in "".join(t for _, t in fr))
check("only console-safe characters", all(ord(ch) < 0x2600 for _, t in fr for ch in t))
core = open(f"{PKG}/etc/runit/core-services/99-auxo-bootanim.sh").read()
check("started in the background from runit stage 1 (never blocks boot)", "&" in core and "exit" not in core)
check("no Plymouth anywhere (it hangs runit boots)", "plymouth" not in open(f"{HERE}/mklive/packages.txt").read().split())
import auxo_theme as T  # noqa: E402
check("seven GRUB themes", list(T.GRUB_STYLES) == ["auxo", "windows", "mac", "neon", "zen", "candy", "minimal"])
txt = T.grub_theme_txt(T.grub_palette("violet", "neon"), True, "background.jpg")
check("GRUB theme: logo, jpg background, neon colours", 'file = "logo.png"' in txt and '"background.jpg"' in txt
      and 'item_color = "#bff6ff"' in txt)
check("minimal GRUB theme: no picture", "desktop-image:" not in T.grub_theme_txt(T.grub_palette("violet", "minimal")
                                                                                  , False, None))
for name, (_d, bg, _c) in T.GRUB_STYLES.items():
    if bg and "{accent}" not in bg:
        check(f"GRUB {name}: its wallpaper exists", os.path.exists(f"{PKG}/share/wallpapers/{bg}"))

print("▲ Hyprland styles")
import auxo_desktops as D  # noqa: E402
sys.path.insert(0, os.path.join(HERE, "installer"))
from auxo_install.config import STYLES  # noqa: E402
check("installer styles = auxo_desktops styles", {k: v[2] for k, v in STYLES.items()} == D.STYLE_RICE)
from PIL import Image  # noqa: E402
for style, rice, name, _desc in D.HYPR_STYLES:
    root = f"{PKG}/share/auxo/rices/{rice}"
    check(f"{name}: rice folder", os.path.isdir(root))
    if rice == "hyprland":
        continue
    check(f"{name}: style.conf, bar, lock screen", all(os.path.exists(f"{root}/{f}") for f in
                                                     ("hypr/style.conf", "waybar/config.jsonc", "waybar/style.css")))
for n in ("neon", "zen", "candy"):
    im = Image.open(f"{PKG}/share/wallpapers/auxo-{n}.jpg")
    check(f"{n} wallpaper: original, 2560×1440", im.size == (2560, 1440))
look = open(f"{PKG}/lib/auxo_look.py").read()
check("look presets for the new styles", all(f'"{n}": ("' in look for n in ("neon", "zen", "candy")))

print("▲ live session")
sess = open(f"{PKG}/bin/auxo-session").read()
check("auxo-session: shell syntax", subprocess.run(["sh", "-n", f"{PKG}/bin/auxo-session"]).returncode == 0)
check("falls back to Sway (software rendering) when Hyprland dies at once",
      "WLR_RENDERER=pixman" in sess and '-ge 20' in sess)
check("auxo-autostart: shell syntax + only auxo-* entries",
      subprocess.run(["sh", "-n", f"{PKG}/bin/auxo-autostart"]).returncode == 0
      and "auxo-*.desktop" in open(f"{PKG}/bin/auxo-autostart").read())
post = open(f"{HERE}/mklive/postsetup.sh").read()
check("live user gets the Windows style + Sway fallback bar", "rice hyprland-win" in post and "sway-waybar" in post)
check("ISO desktop entry has no konsole", "konsole" not in open(f"{HERE}/mklive/include/usr/share/applications/auxo-install.desktop").read())

print(f"\n{passed} passed, {failed} failed")
sys.exit(1 if failed else 0)

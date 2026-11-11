"""Facts from the live system: disks, keyboards, time zones, firmware, graphics.
Parsing is split from running commands so the tests can feed in sample output."""
import os
import re
import subprocess

ISO_LABEL = "AUXOBSD"

# (shown name, X11 layout, X11 variant, FreeBSD console keymap)
KEYBOARDS = [
    ("English (US)", "us", "", "us.kbd"),
    ("English (US, international)", "us", "intl", "us.acc.kbd"),
    ("English (UK)", "gb", "", "uk.kbd"),
    ("English (Dvorak)", "us", "dvorak", "us.dvorak.kbd"),
    ("English (Colemak)", "us", "colemak", "us.colemak.kbd"),
    ("Belgian", "be", "", "be.kbd"),
    ("Brazilian (ABNT2)", "br", "", "br.kbd"),
    ("Canadian French", "ca", "fr", "ca-fr.kbd"),
    ("Czech", "cz", "", "cz.kbd"),
    ("Danish", "dk", "", "dk.kbd"),
    ("Dutch", "nl", "", "nl.kbd"),
    ("Finnish", "fi", "", "fi.kbd"),
    ("French", "fr", "", "fr.kbd"),
    ("German", "de", "", "de.kbd"),
    ("Greek", "gr", "", "gr.kbd"),
    ("Hungarian", "hu", "", "hu.kbd"),
    ("Icelandic", "is", "", "is.kbd"),
    ("Italian", "it", "", "it.kbd"),
    ("Japanese", "jp", "", "jp.kbd"),
    ("Korean", "kr", "", "kr.kbd"),
    ("Latin American", "latam", "", "latinamerican.kbd"),
    ("Norwegian", "no", "", "no.kbd"),
    ("Polish", "pl", "", "pl.kbd"),
    ("Portuguese", "pt", "", "pt.kbd"),
    ("Russian", "ru", "", "ru.kbd"),
    ("Spanish", "es", "", "es.kbd"),
    ("Swedish", "se", "", "se.kbd"),
    ("Swiss German", "ch", "", "ch.kbd"),
    ("Swiss French", "ch", "fr", "ch-fr.kbd"),
    ("Turkish (Q)", "tr", "", "tr.kbd"),
    ("Ukrainian", "ua", "", "ua.kbd"),
]


def sh(cmd):
    try:
        return subprocess.run(cmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=10).stdout
    except (OSError, subprocess.SubprocessError):
        return ""


def parse_geom_disks(txt):
    """`geom disk list` → [{name, bytes, descr}]"""
    disks, cur = [], None
    for line in txt.splitlines():
        line = line.strip()
        if line.startswith("Geom name:"):
            cur = {"name": line.split(":", 1)[1].strip(), "bytes": 0, "descr": ""}
            disks.append(cur)
        elif cur and line.startswith("Mediasize:"):
            m = re.search(r"Mediasize:\s*(\d+)", line)
            cur["bytes"] = int(m.group(1)) if m else 0
        elif cur and line.startswith("descr:"):
            cur["descr"] = line.split(":", 1)[1].strip()
    return disks


def parse_boot_medium(glabel_txt):
    """`glabel status` → the disk holding the AuxoBSD ISO (cd0, da0, …) or ''."""
    for line in glabel_txt.splitlines():
        f = line.split()
        if len(f) >= 3 and f[0] == f"iso9660/{ISO_LABEL}":
            return re.sub(r"(p|s)\d+[a-h]?$", "", f[2])
    return ""


def installable(disks, boot_medium):
    return [d for d in disks if not re.match(r"^(cd|md|acd|zvol)\d*", d["name"]) and d["name"] != boot_medium
            and d["bytes"] > 0]


def disks():
    return installable(parse_geom_disks(sh(["geom", "disk", "list"])), parse_boot_medium(sh(["glabel", "status"])))


def human(n):
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if n < 1000 or unit == "TB":
            return f"{n:.0f} {unit}" if unit in ("B", "KB") else f"{n:.1f} {unit}"
        n /= 1000


def firmware():
    m = sh(["sysctl", "-n", "machdep.bootmethod"]).strip().upper()
    return "uefi" if m == "UEFI" else "bios" if m == "BIOS" else "uefi"


def timezones(path="/usr/share/zoneinfo/zone1970.tab"):
    """{region: [city, ...]} from zone1970.tab (or zone.tab)."""
    zones = {}
    for p in (path, path.replace("zone1970", "zone")):
        try:
            for line in open(p):
                if line.startswith("#") or not line.strip():
                    continue
                tz = line.split("\t")[2].strip()
                region, _, city = tz.partition("/")
                if city:
                    zones.setdefault(region, []).append(city)
            break
        except (OSError, IndexError):
            continue
    for r in zones:
        zones[r] = sorted(set(zones[r]))
    return dict(sorted(zones.items())) or {"Etc": ["UTC"]}


def current_timezone():
    try:
        link = os.readlink("/etc/localtime")
        return link.split("zoneinfo/", 1)[1]
    except (OSError, IndexError):
        pass
    try:
        return open("/var/db/zoneinfo").read().strip() or "UTC"
    except OSError:
        return "UTC"


def gpus():
    """(gpus, vm) using the same detection as auxo-tweak."""
    try:
        import auxo_bsd
        import auxo_gpu
        vm = auxo_gpu.vm_kind(auxo_bsd.kenv("smbios.system.product"), auxo_bsd.kenv("smbios.system.maker"))
        return auxo_gpu.detect(auxo_bsd.pci_devices()), vm
    except Exception:  # noqa: BLE001 — the installer must still open without it
        return [], ""

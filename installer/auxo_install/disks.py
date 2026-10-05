"""Disk discovery (lsblk) and partition naming. Parsing is pure; probing shells out."""
import json
import os
import subprocess

LSBLK_COLS = "NAME,PATH,SIZE,TYPE,FSTYPE,MOUNTPOINT,MODEL,TRAN,RM,RO,PTTYPE,LABEL,PARTLABEL"
MIN_DISK = 20 * 1000 ** 3  # 20 GB


def human(n):
    n = float(n or 0)
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if n < 1000 or unit == "TB":
            return f"{n:.0f} {unit}" if unit in ("B", "KB") else f"{n:.1f} {unit}"
        n /= 1000
    return f"{n:.1f} TB"


def part_path(disk, n):
    """/dev/sda + 1 → /dev/sda1 ; /dev/nvme0n1 + 1 → /dev/nvme0n1p1 ; /dev/mmcblk0 → p1."""
    return f"{disk}p{n}" if disk[-1].isdigit() else f"{disk}{n}"


def parse_lsblk(data, live_device=None):
    """lsblk -J output → (disks, partitions). Each disk: dict(path, size, model, tran, removable,
    usable, reason, parts=[...]). The live USB, read-only, tiny and virtual devices are unusable."""
    disks = []
    for d in data.get("blockdevices", []):
        if d.get("type") != "disk":
            continue
        name = d.get("name", "")
        if name.startswith(("zram", "loop", "ram", "sr", "fd")):
            continue
        children = d.get("children") or []
        parts = [{"path": c.get("path"), "size": int(c.get("size") or 0), "fstype": c.get("fstype") or "",
                  "label": c.get("label") or c.get("partlabel") or "", "mountpoint": c.get("mountpoint") or ""}
                 for c in children if c.get("type") == "part"]
        size = int(d.get("size") or 0)
        mounted = [p for p in parts if p["mountpoint"]] + ([d] if d.get("mountpoint") else [])
        reason = ""
        if d.get("ro"):
            reason = "read-only"
        elif live_device and (d.get("path") == live_device or any(p["path"] == live_device for p in parts)):
            reason = "this is the Auxo USB"
        elif size < MIN_DISK:
            reason = f"too small (needs {human(MIN_DISK)})"
        elif mounted:
            reason = "in use (mounted)"
        disks.append({
            "path": d.get("path"), "size": size, "model": (d.get("model") or "").strip() or "Disk",
            "tran": d.get("tran") or "", "removable": bool(d.get("rm")), "pttype": d.get("pttype") or "",
            "usable": not reason, "reason": reason, "parts": parts,
        })
    return disks


def live_device():
    """The block device the live system was booted from (to keep it out of the list)."""
    for mp in ("/run/initramfs/live", "/run/rootfsbase"):
        try:
            out = subprocess.run(["findmnt", "-no", "SOURCE", mp], capture_output=True, text=True).stdout.strip()
            if out:
                return out
        except OSError:
            pass
    return None


def probe():
    out = subprocess.run(["lsblk", "-J", "-b", "-o", LSBLK_COLS], capture_output=True, text=True).stdout
    try:
        return parse_lsblk(json.loads(out or "{}"), live_device())
    except json.JSONDecodeError:
        return []


def is_efi():
    return os.path.isdir("/sys/firmware/efi")


def efi_bits():
    try:
        return int(open("/sys/firmware/efi/fw_platform_size").read().strip())
    except (OSError, ValueError):
        return 64

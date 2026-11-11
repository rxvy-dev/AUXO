"""What the user picked in the installer."""
import re
from dataclasses import dataclass, field

USERNAME_RE = re.compile(r"^[a-z_][a-z0-9_-]{0,15}$")
HOSTNAME_RE = re.compile(r"^[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?$")
RESERVED = {"root", "toor", "daemon", "operator", "bin", "tty", "kmem", "games", "news", "man", "sshd", "smmsp",
            "mailnull", "bind", "unbound", "proxy", "_pflogd", "_dhcp", "uucp", "pop", "auditdistd", "www",
            "ntpd", "_ypldap", "hast", "nobody", "messagebus", "polkitd", "sddm", "auxo", "wheel", "video"}
MIN_DISK_GB = 24


@dataclass
class InstallConfig:
    disk: str = ""                 # e.g. "ada0", "nda0", "da1"
    disk_bytes: int = 0
    fs: str = "zfs"                # zfs | ufs
    swap_gb: int = 4
    firmware: str = "uefi"         # uefi | bios (what the live system booted with; both get installed)
    keyboard: str = "us"           # X11 layout
    keyboard_variant: str = ""
    keymap: str = "us.kbd"         # console (vt) keymap
    timezone: str = "UTC"
    hostname: str = "auxobsd"
    fullname: str = ""
    username: str = ""
    password: str = ""
    accent: str = "violet"
    shell: str = "zsh"
    nvidia_mode: str = ""          # "" automatic | primary | prime | off
    xbps: bool = False             # set up Void Linux packages (needs internet)
    extras: list = field(default_factory=list)

    def errors(self):
        """Problems that block installing, as readable sentences."""
        e = []
        if not self.disk:
            e.append("Pick a disk to install on.")
        elif self.disk_bytes and self.disk_bytes < MIN_DISK_GB * 1024 ** 3:
            e.append(f"AuxoBSD needs a disk of at least {MIN_DISK_GB} GB.")
        if self.fs not in ("zfs", "ufs"):
            e.append("Unknown file system.")
        if not USERNAME_RE.match(self.username or ""):
            e.append("Usernames are lowercase letters, digits, - and _, starting with a letter (up to 16).")
        elif self.username in RESERVED:
            e.append(f"'{self.username}' is reserved by the system; pick another username.")
        if not HOSTNAME_RE.match(self.hostname or ""):
            e.append("The computer name can use letters, digits and - (not at the start or end).")
        if not self.password:
            e.append("Set a password.")
        if any(c in (self.fullname or "") for c in ":,\n"):
            e.append("Your name can't contain ':' or ','.")
        return e

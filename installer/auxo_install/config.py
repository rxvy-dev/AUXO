"""What the user picked, plus validation. Pure Python, no system access."""
import re
from dataclasses import dataclass, field, asdict

USERNAME_RE = re.compile(r"^[a-z_][a-z0-9_-]{0,31}$")
HOSTNAME_RE = re.compile(r"^[a-zA-Z0-9]([a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?$")
RESERVED_USERS = {"root", "bin", "daemon", "nobody", "anon", "_greeter", "sys", "adm"}

FILESYSTEMS = {
    "btrfs": "btrfs — compressed, with bootable snapshots (recommended)",
    "ext4": "ext4 — simple and proven",
    "xfs": "xfs — fast with big files",
}

# keyboard layout (XKB name) → console keymap name where it differs
CONSOLE_KEYMAP = {"gb": "uk", "latam": "la-latin1", "br": "br-abnt2", "ch": "de_CH-latin1",
                  "pt": "pt-latin1", "cz": "cz-qwertz"}
KEYBOARDS = [
    ("us", "English (US)"), ("gb", "English (UK)"), ("ca", "Canadian (French)"), ("de", "German"),
    ("fr", "French"), ("es", "Spanish"), ("latam", "Spanish (Latin American)"), ("it", "Italian"),
    ("pt", "Portuguese"), ("br", "Portuguese (Brazil)"), ("nl", "Dutch"), ("be", "Belgian"),
    ("ch", "Swiss German"), ("se", "Swedish"), ("no", "Norwegian"), ("dk", "Danish"),
    ("fi", "Finnish"), ("pl", "Polish"), ("cz", "Czech"), ("hu", "Hungarian"), ("ro", "Romanian"),
    ("tr", "Turkish"), ("gr", "Greek"), ("ru", "Russian"), ("ua", "Ukrainian"), ("jp", "Japanese"),
    ("kr", "Korean"), ("il", "Hebrew"), ("ara", "Arabic"), ("in", "Indian"),
]
# English names only: the Linux console can't draw CJK, Cyrillic etc. in its default font
LOCALES = [
    ("en_US.UTF-8", "English (United States)"), ("en_GB.UTF-8", "English (United Kingdom)"),
    ("en_CA.UTF-8", "English (Canada)"), ("en_AU.UTF-8", "English (Australia)"),
    ("de_DE.UTF-8", "German"), ("fr_FR.UTF-8", "French"), ("fr_CA.UTF-8", "French (Canada)"),
    ("es_ES.UTF-8", "Spanish (Spain)"), ("es_MX.UTF-8", "Spanish (Mexico)"), ("it_IT.UTF-8", "Italian"),
    ("pt_PT.UTF-8", "Portuguese"), ("pt_BR.UTF-8", "Portuguese (Brazil)"), ("nl_NL.UTF-8", "Dutch"),
    ("sv_SE.UTF-8", "Swedish"), ("nb_NO.UTF-8", "Norwegian"), ("da_DK.UTF-8", "Danish"),
    ("fi_FI.UTF-8", "Finnish"), ("pl_PL.UTF-8", "Polish"), ("cs_CZ.UTF-8", "Czech"),
    ("hu_HU.UTF-8", "Hungarian"), ("ro_RO.UTF-8", "Romanian"), ("tr_TR.UTF-8", "Turkish"),
    ("el_GR.UTF-8", "Greek"), ("ru_RU.UTF-8", "Russian"), ("uk_UA.UTF-8", "Ukrainian"),
    ("ja_JP.UTF-8", "Japanese"), ("ko_KR.UTF-8", "Korean"), ("zh_CN.UTF-8", "Chinese (Simplified)"),
    ("zh_TW.UTF-8", "Chinese (Traditional)"), ("he_IL.UTF-8", "Hebrew"), ("ar_EG.UTF-8", "Arabic"),
    ("hi_IN.UTF-8", "Hindi"),
]
EXTRAS = {
    "snapshots": "Bootable btrfs snapshots before every update",
    "zram": "Compressed RAM swap (zram)",
    "drivers": "Install the right GPU driver (NVIDIA, AMD, Intel)",
    "firewall": "Firewall: block incoming connections",
    "flatpak": "Flatpak + Flathub app store",
    "gaming": "Gaming: Steam, GameMode, MangoHud, gamescope",
}
DEFAULT_EXTRAS = ["snapshots", "zram", "drivers"]


@dataclass
class InstallConfig:
    keyboard: str = "us"
    locale: str = "en_US.UTF-8"
    timezone: str = "UTC"
    disk: str = ""                 # e.g. /dev/nvme0n1 (erase mode)
    mode: str = "erase"            # erase | manual
    root_part: str = ""            # manual mode
    efi_part: str = ""             # manual mode, UEFI only
    format_efi: bool = False       # manual mode: format the EFI partition (only if it's new)
    filesystem: str = "btrfs"
    desktop: str = "plasma"
    accent: str = "violet"
    kernel: str = "linux"
    shell: str = "zsh"
    fullname: str = ""
    username: str = ""
    password: str = ""
    root_password: str = ""        # empty = root locked, use sudo
    hostname: str = "auxo"
    extras: list = field(default_factory=lambda: list(DEFAULT_EXTRAS))

    def to_dict(self, redact=True):
        d = asdict(self)
        if redact:
            for k in ("password", "root_password"):
                d[k] = "•" * 8 if d[k] else ""
        return d

    @property
    def console_keymap(self):
        return CONSOLE_KEYMAP.get(self.keyboard, self.keyboard)


def validate_user(cfg, password2=None):
    """Return a list of problems with the user page (empty list = fine)."""
    errs = []
    if not USERNAME_RE.match(cfg.username or ""):
        errs.append("Username: lowercase letters, digits, - and _, starting with a letter.")
    elif cfg.username in RESERVED_USERS:
        errs.append(f"Username '{cfg.username}' is reserved; pick another.")
    if not cfg.password:
        errs.append("Password can't be empty.")
    elif password2 is not None and cfg.password != password2:
        errs.append("The passwords don't match.")
    if not HOSTNAME_RE.match(cfg.hostname or ""):
        errs.append("Computer name: letters, digits and -, up to 63 characters.")
    if ":" in (cfg.fullname or "") or "\n" in (cfg.fullname or ""):
        errs.append("Full name can't contain ':'.")
    return errs


def validate(cfg, efi=True):
    """Everything needed before installing; returns a list of problems."""
    errs = validate_user(cfg)
    if cfg.mode == "erase" and not cfg.disk:
        errs.append("Pick a disk to install to.")
    if cfg.mode == "manual":
        if not cfg.root_part:
            errs.append("Pick the partition for Auxo (/).")
        if efi and not cfg.efi_part:
            errs.append("Pick the EFI system partition.")
        if cfg.root_part and cfg.root_part == cfg.efi_part:
            errs.append("The root and EFI partitions must be different.")
    if cfg.filesystem not in FILESYSTEMS:
        errs.append(f"Unknown filesystem {cfg.filesystem}.")
    return errs

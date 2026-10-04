"""Desktop / window-manager catalogue used by the installer and auxo-tweak (Void Linux).

Every package name here exists in Void's repos (checked against void-packages),
except the Hyprland packages, which come from the hyprland-void community repo
(Void doesn't package Hyprland). See HYPRLAND_REPO.
"""

# A graphical session on Void needs D-Bus + elogind (seat/session management) and polkit.
GUI_BASE = ["dbus", "elogind", "polkit", "NetworkManager", "pipewire", "wireplumber",
            "xdg-user-dirs", "xdg-utils", "mesa-dri", "vulkan-loader",
            "noto-fonts-ttf", "noto-fonts-emoji", "font-inter"]
GUI_SERVICES = ["dbus", "elogind", "NetworkManager"]

COMMON_WM = [
    "Thunar", "thunar-archive-plugin", "gvfs", "pavucontrol", "network-manager-applet",
    "blueman", "brightnessctl", "playerctl", "font-awesome6", "papirus-icon-theme",
    "breeze-gtk", "breeze-icons", "greetd", "tuigreet", "polkit-gnome",
]
WAYLAND_WM = ["Waybar", "wofi", "mako", "grim", "slurp", "wl-clipboard", "swappy",
              "qt6-wayland", "qt5-wayland", "xdg-desktop-portal-gtk"]
X11 = ["xorg-minimal", "xinit", "xorg-fonts", "xrandr", "xsetroot"]

# Hyprland is not in Void's official repos; this community repo builds it for Void.
HYPRLAND_REPO = "https://raw.githubusercontent.com/Makrennel/hyprland-void/repository-x86_64-glibc"

DESKTOPS = {
    "plasma": {
        "name": "KDE Plasma",
        "packages": GUI_BASE + ["kde-plasma", "dolphin", "konsole", "kate", "ark", "spectacle",
                                "xdg-desktop-portal-kde", "kde-gtk-config", "breeze-gtk", "greetd", "tuigreet"],
        "dm": "greetd",
        "session": "plasma",
        "cmd": "dbus-run-session startplasma-wayland",
    },
    "gnome": {
        "name": "GNOME",
        # GNOME is Wayland-only and has to be started by its own login screen (GDM).
        "packages": GUI_BASE + ["gnome-core", "gdm", "gnome-console", "gnome-tweaks",
                                "xdg-desktop-portal-gnome", "nautilus"],
        "dm": "gdm",
        "session": "gnome",
        "cmd": "gnome-session",
    },
    "xfce": {
        "name": "Xfce",
        "packages": GUI_BASE + X11 + ["xfce4", "xfce4-plugins", "network-manager-applet", "pavucontrol",
                                      "papirus-icon-theme", "greetd", "tuigreet"],
        "dm": "greetd",
        "session": "xfce",
        "cmd": "startx /usr/bin/startxfce4",
    },
    "cinnamon": {
        "name": "Cinnamon",
        "packages": GUI_BASE + X11 + ["cinnamon", "nemo", "gnome-terminal", "xed",
                                      "papirus-icon-theme", "greetd", "tuigreet"],
        "dm": "greetd",
        "session": "cinnamon",
        "cmd": "startx /usr/bin/cinnamon-session",
    },
    "hyprland": {
        "name": "Hyprland (Auxo rice)",
        "packages": GUI_BASE + ["hyprland", "hyprpaper", "hypridle", "hyprlock", "xdg-desktop-portal-hyprland",
                                "kitty", "polkit-kde-agent"] + WAYLAND_WM + COMMON_WM,
        "repo": HYPRLAND_REPO,
        "dm": "greetd",
        "session": "hyprland",
        "cmd": "dbus-run-session Hyprland",
        "rice": "hyprland",
    },
    "sway": {
        "name": "Sway (Auxo rice)",
        "packages": GUI_BASE + ["sway", "swaybg", "swayidle", "swaylock", "xdg-desktop-portal-wlr",
                                "foot"] + WAYLAND_WM + COMMON_WM,
        "dm": "greetd",
        "session": "sway",
        "cmd": "dbus-run-session sway",
        "rice": "sway",
    },
    "i3": {
        "name": "i3 (Auxo rice)",
        "packages": GUI_BASE + X11 + ["i3", "i3lock", "xss-lock", "polybar", "rofi", "picom", "dunst",
                                      "feh", "alacritty", "maim", "xclip"] + COMMON_WM,
        "dm": "greetd",
        "session": "i3",
        "cmd": "startx /usr/bin/i3",
        "rice": "i3",
    },
    "none": {
        "name": "No desktop (TTY)",
        "packages": [],
        "dm": None,
        "session": None,
    },
}

# What the live ISO's desktop ships; removed when the user installs a different desktop.
LIVE_PLASMA = ["kde-plasma", "dolphin", "konsole", "kate", "ark", "spectacle", "xdg-desktop-portal-kde"]

# Void kernel meta packages (each pulls in a versioned linuxX.Y package).
KERNELS = {
    "linux": "Stable — Void's current kernel",
    "linux-lts": "Long-term support — maximum stability",
    "linux-mainline": "Mainline — the newest kernel, for the newest hardware",
}

SHELLS = {
    "zsh": ("/usr/bin/zsh", ["zsh", "zsh-autosuggestions", "zsh-syntax-highlighting", "zsh-completions"]),
    "fish": ("/usr/bin/fish", ["fish-shell"]),
    "bash": ("/bin/bash", ["bash", "bash-completion"]),
}

# GNOME 47+ only accepts named accents; map ours to the nearest.
GNOME_ACCENT = {"violet": "purple", "cyan": "teal", "emerald": "green", "amber": "yellow",
                "rose": "pink", "blue": "blue", "mono": "slate"}

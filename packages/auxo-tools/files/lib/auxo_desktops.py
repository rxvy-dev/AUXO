"""Desktop / window-manager catalogue used by the installer and auxo-tweak."""

COMMON_WM = [
    "thunar", "thunar-archive-plugin", "gvfs", "pavucontrol", "network-manager-applet",
    "blueman", "brightnessctl", "playerctl", "ttf-font-awesome", "ttf-jetbrains-mono",
    "papirus-icon-theme", "xdg-user-dirs", "sddm", "xorg-server", "breeze", "breeze-gtk",
]
WAYLAND_WM = ["waybar", "wofi", "mako", "grim", "slurp", "wl-clipboard", "swappy", "qt6-wayland", "qt5-wayland"]

DESKTOPS = {
    "plasma": {
        "name": "KDE Plasma",
        "packages": ["plasma-desktop", "plasma-nm", "plasma-pa", "powerdevil", "kscreen", "bluedevil",
                     "breeze-gtk", "kde-gtk-config", "xdg-desktop-portal-kde", "konsole", "dolphin",
                     "kate", "ark", "spectacle", "sddm", "sddm-kcm"],
        "dm": "sddm",
        "session": "plasma",
    },
    "gnome": {
        "name": "GNOME",
        "packages": ["gnome", "gnome-tweaks", "gnome-console", "xdg-desktop-portal-gnome", "gdm"],
        "dm": "gdm",
        "session": "gnome",
    },
    "xfce": {
        "name": "Xfce",
        "packages": ["xfce4", "xfce4-goodies", "network-manager-applet", "pavucontrol", "papirus-icon-theme", "sddm", "xorg-server", "breeze"],
        "dm": "sddm",
        "session": "xfce",
    },
    "cinnamon": {
        "name": "Cinnamon",
        "packages": ["cinnamon", "nemo-fileroller", "gnome-terminal", "xed", "papirus-icon-theme", "sddm", "xorg-server", "breeze"],
        "dm": "sddm",
        "session": "cinnamon",
    },
    "hyprland": {
        "name": "Hyprland (Auxo rice)",
        "packages": ["hyprland", "hyprpaper", "hypridle", "hyprlock", "xdg-desktop-portal-hyprland",
                     "xdg-desktop-portal-gtk", "kitty", "polkit-kde-agent"] + WAYLAND_WM + COMMON_WM,
        "dm": "sddm",
        "session": "hyprland",
        "rice": "hyprland",
    },
    "sway": {
        "name": "Sway (Auxo rice)",
        "packages": ["sway", "swaybg", "swayidle", "swaylock", "xdg-desktop-portal-wlr", "xdg-desktop-portal-gtk",
                     "foot", "polkit-gnome"] + WAYLAND_WM + COMMON_WM,
        "dm": "sddm",
        "session": "sway",
        "rice": "sway",
    },
    "i3": {
        "name": "i3 (Auxo rice)",
        "packages": ["i3-wm", "i3lock", "xss-lock", "polybar", "rofi", "picom", "dunst", "feh", "alacritty",
                     "xorg-server", "xorg-xinit", "xorg-xrandr", "xorg-xsetroot", "maim", "xclip",
                     "polkit-gnome"] + COMMON_WM,
        "dm": "sddm",
        "session": "i3",
        "rice": "i3",
    },
    "none": {
        "name": "No desktop (TTY)",
        "packages": [],
        "dm": None,
        "session": None,
    },
}

# What the live ISO ships; removed when the user picks a different desktop.
LIVE_PLASMA = [
    "plasma-desktop", "plasma-nm", "plasma-pa", "plasma-systemmonitor", "powerdevil", "kscreen", "bluedevil",
    "kde-gtk-config", "breeze-gtk", "xdg-desktop-portal-kde", "konsole", "dolphin", "kate", "ark", "spectacle",
    "partitionmanager", "sddm-kcm",
]

KERNELS = {
    "linux": "Stable — the default Arch kernel",
    "linux-lts": "Long-term support — maximum stability",
    "linux-zen": "Zen — tuned for desktop responsiveness & gaming",
    "linux-hardened": "Hardened — security-focused patches",
}

SHELLS = {
    "zsh": ("/usr/bin/zsh", ["zsh", "zsh-autosuggestions", "zsh-syntax-highlighting", "zsh-completions"]),
    "fish": ("/usr/bin/fish", ["fish"]),
    "bash": ("/usr/bin/bash", ["bash", "bash-completion"]),
}

# GNOME 47+ only accepts named accents; map ours to the nearest.
GNOME_ACCENT = {"violet": "purple", "cyan": "teal", "emerald": "green", "amber": "yellow",
                "rose": "pink", "blue": "blue", "mono": "slate"}

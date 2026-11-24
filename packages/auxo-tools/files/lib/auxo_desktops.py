"""Desktop / window-manager catalogue used by the installer and auxo-tweak (Void Linux).

Every package name here exists in Void's repos (checked against void-packages),
except the Hyprland packages, which Auxo builds itself
See HYPRLAND_REPO.
"""

# A graphical session on Void needs D-Bus + elogind (seat/session management) and polkit.
GUI_BASE = ["dbus", "elogind", "polkit", "NetworkManager", "pipewire", "wireplumber", "alsa-pipewire",
            "xdg-user-dirs", "xdg-utils", "mesa-dri", "vulkan-loader",
            "noto-fonts-ttf", "noto-fonts-emoji", "font-inter", "font-firacode", "papirus-icon-theme",
            # Auxo Settings and the welcome app (python3-pyqt6 alone has no GUI modules on Void)
            "python3-pyqt6", "python3-pyqt6-gui", "python3-pyqt6-widgets"]
GUI_SERVICES = ["dbus", "elogind", "NetworkManager"]

# Plasma without the kde-plasma meta package (which adds a 254 MB wallpaper pack, Oxygen and
# extras). The live ISO ships exactly this list (mklive/packages.txt), so Plasma installs offline.
PLASMA_CORE = ["plasma-desktop", "plasma-workspace-x11", "plasma-nm", "plasma-pa", "powerdevil", "bluedevil",
               "kscreen", "kinfocenter", "kwallet-pam", "ksshaskpass", "kwrited", "sddm-kcm", "plasma-thunderbolt",
               "kf6-kded", "upower", "udisks2"]

COMMON_WM = [
    "Thunar", "thunar-archive-plugin", "gvfs", "pavucontrol", "network-manager-applet",
    "blueman", "brightnessctl", "playerctl", "power-profiles-daemon", "font-awesome6", "papirus-icon-theme",
    "breeze-gtk", "breeze-icons", "adwaita-icon-theme", "greetd", "tuigreet", "polkit-gnome",
]
WAYLAND_WM = ["Waybar", "wofi", "mako", "grim", "slurp", "wl-clipboard", "swappy", "cliphist", "rofimoji",
              "gammastep", "swww", "mpvpaper",
              "qt6-wayland", "qt5-wayland", "xdg-desktop-portal-gtk"]
X11 = ["xorg-minimal", "xinit", "xorg-fonts", "xrandr", "xsetroot"]

# Chrono (rxvy's own Wayland desktop) is built from source by auxo-tweak; these are the Void
# packages its build needs (the list Chrono's scripts/install-deps.sh uses on Void) plus what it
# uses at runtime. See auxo_chrono.py.
CHRONO_BUILD = ["base-devel", "git", "cmake", "meson", "ninja", "pkg-config",
                "wayland-devel", "wayland-protocols", "libinput-devel", "libxkbcommon-devel", "pixman-devel",
                "libdrm-devel", "cairo-devel", "pango-devel", "pam-devel", "libxcb-devel", "xcb-util-wm-devel",
                "xcb-util-renderutil-devel", "xcb-util-errors-devel", "xorg-server-xwayland", "libseat-devel",
                "libdisplay-info-devel", "libliftoff-devel", "MesaLib-devel", "libffi-devel", "expat-devel",
                "vulkan-loader-devel", "Vulkan-Headers", "glslang", "lcms2-devel", "eudev-libudev-devel",
                "qt6-base-devel", "qt6-declarative-devel", "qt6-wayland-devel", "qt6-svg", "layer-shell-qt-devel",
                "wlroots0.20-devel"]
CHRONO_RUNTIME = ["grim", "slurp", "wl-clipboard", "hwids", "foot", "xdg-desktop-portal-wlr",
                  "xdg-desktop-portal-gtk", "brightnessctl", "playerctl", "qt6-wayland", "greetd", "tuigreet"]

# Hyprland is not in Void's official repos; this community repo builds it for Void.
# Hyprland is built by Auxo (packages/hyprland, .github/workflows/build-hyprland.yml) because the
# community repo it used to come from stopped being rebuilt and no longer installs on current Void.
# The ISO carries the packages as a local repository; systems without it download the archive.
HYPRLAND_REPO = "/var/lib/auxo/hyprland-repo"
HYPRLAND_PACKAGES_URL = "https://github.com/rxvy-dev/AUXO/releases/latest/download/hyprland-x86_64.tar"
OLD_HYPRLAND_REPO = "https://raw.githubusercontent.com/Makrennel/hyprland-void/repository-x86_64-glibc"
# Auxo's own tools, published with every ISO build (auxo-update installs newer ones)
AUXO_PACKAGES_URL = "https://github.com/rxvy-dev/AUXO/releases/latest/download/auxo-packages-x86_64.tar"
AUXO_REPO = "/var/lib/auxo/auxo-repo"

DESKTOPS = {
    "plasma": {
        "name": "KDE Plasma",
        "packages": GUI_BASE + PLASMA_CORE + ["dolphin", "konsole", "kate", "ark", "spectacle",
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
        "rice": "gnome",              # applied through its own settings (auxo_theme.session_apply)
    },
    "xfce": {
        "name": "Xfce",
        "packages": GUI_BASE + X11 + ["xfce4", "xfce4-plugins", "network-manager-applet", "pavucontrol",
                                      "papirus-icon-theme", "greetd", "tuigreet"],
        "dm": "greetd",
        "session": "xfce",
        "cmd": "startx /usr/bin/startxfce4",
        "rice": "xfce",              # applied through its own settings (auxo_theme.session_apply)
    },
    "cinnamon": {
        "name": "Cinnamon",
        "packages": GUI_BASE + X11 + ["cinnamon", "nemo", "gnome-terminal", "xed",
                                      "papirus-icon-theme", "greetd", "tuigreet"],
        "dm": "greetd",
        "session": "cinnamon",
        "cmd": "startx /usr/bin/dbus-run-session /usr/bin/cinnamon-session",   # PipeWire needs a D-Bus session
        "rice": "cinnamon",              # applied through its own settings (auxo_theme.session_apply)
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
        "packages": GUI_BASE + X11 + ["i3", "i3lock", "xss-lock", "polybar", "rofi", "picom", "dunst", "clipmenu", "rofimoji",
                                      "feh", "alacritty", "maim", "xclip"] + COMMON_WM,
        "dm": "greetd",
        "session": "i3",
        "cmd": "startx /usr/bin/dbus-run-session /usr/bin/i3",                 # PipeWire needs a D-Bus session
        "rice": "i3",
    },
    "chrono": {
        "name": "Chrono (rxvy's desktop)",
        "packages": GUI_BASE + CHRONO_BUILD + CHRONO_RUNTIME,
        "build": "chrono",            # built from source by auxo_chrono
        "dm": "greetd",
        "session": "chrono",
        "cmd": "chrono-session",      # sets up its own D-Bus session
        "rice": "chrono",
    },
    "none": {
        "name": "No desktop (TTY)",
        "packages": [],
        "dm": None,
        "session": None,
    },
}

# What the live ISO's desktop ships; removed when the user installs a different desktop.
LIVE_PLASMA = [p for p in PLASMA_CORE if p not in ("upower", "udisks2")] + [
    "kde-plasma", "dolphin", "konsole", "kate", "ark", "spectacle", "xdg-desktop-portal-kde"]

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

"""App bundles: one click installs a set of well-liked apps from Void's repositories."""
APP_BUNDLES = {
    "browsers": ("Firefox and Chromium", ["firefox", "chromium"]),
    "media": ("Video and music players: mpv, VLC, Celluloid", ["mpv", "vlc", "celluloid"]),
    "creative": ("GIMP, Krita, Inkscape, Kdenlive, OBS Studio, Audacity, Blender, darktable",
                 ["gimp", "krita", "inkscape", "kdenlive", "obs", "audacity", "blender", "darktable"]),
    "office": ("LibreOffice, Thunderbird, Okular, KeePassXC", ["libreoffice", "thunderbird", "okular", "keepassxc"]),
    "dev": ("Compilers and tools: base-devel, git, CMake, Ninja, GDB, Clang, Python pip, Node.js, Rust, Go, VS Code (OSS), Neovim, lazygit",
            ["base-devel", "git", "cmake", "ninja", "gdb", "clang", "python3-pip", "nodejs", "rust", "cargo", "go",
             "vscode", "neovim", "lazygit"]),
    "containers": ("Podman, Docker and Docker Compose", ["podman", "docker", "docker-compose"]),
    "discord": ("Discord (from Flathub)", ["discord"]),
    "chat": ("Discord, Telegram, Element, HexChat (Discord comes from Flathub)",
             ["discord", "telegram-desktop", "element-desktop", "hexchat"]),
    "terminal": ("btop, htop, fastfetch, ncdu, tldr, bat, eza, fzf, ripgrep, zoxide",
                 ["btop", "htop", "fastfetch", "ncdu", "tldr", "bat", "eza", "fzf", "ripgrep", "zoxide"]),
    "fun": ("Terminal toys: cmatrix, asciiquarium, sl, cowsay, fortune, figlet, cava",
            ["cmatrix", "asciiquarium", "sl", "cowsay", "fortune-mod", "figlet", "cava"]),
    "tools": ("qBittorrent, Syncthing, GParted, virt-manager + QEMU", ["qbittorrent", "syncthing", "gparted", "virt-manager", "qemu"]),
}
NONFREE_APPS = set()     # apps that need Void's nonfree repo
# Apps Void doesn't ship as a ready package: Void's discord is restricted=yes (it may not be
# redistributed, so it's never in the binary repos and xbps says "not found in repository pool").
# These come from Flathub instead.
FLATPAK_APPS = {"discord": "com.discordapp.Discord"}

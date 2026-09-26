"""Auxo accent palette — shared by the installer, auxo-tweak, auxo-fetch and the asset generator."""

# name: (primary, secondary-for-gradients)
ACCENTS = {
    "violet": ("#a78bfa", "#22d3ee"),
    "cyan": ("#22d3ee", "#818cf8"),
    "emerald": ("#34d399", "#a3e635"),
    "amber": ("#fbbf24", "#fb7185"),
    "rose": ("#fb7185", "#c084fc"),
    "blue": ("#60a5fa", "#34d399"),
    "mono": ("#e5e7eb", "#9ca3af"),
}

DEFAULT = "violet"


def hex_to_rgb(h):
    h = h.lstrip("#")
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


def ansi(h):
    r, g, b = hex_to_rgb(h)
    return f"\033[38;2;{r};{g};{b}m"

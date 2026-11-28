"""The graphical Auxo installer (PyQt6).

It asks the same questions as the text installer and fills in the same InstallConfig, then runs
the install with the same engine: `auxo-installer --config FILE --no-tui --progress` as root,
reading its progress events (@@steps, @@step, @@pct, @@done, @@error) and log lines. The window
itself runs as you, never as root.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading

from PyQt6.QtCore import QObject, QPointF, QProcess, QRectF, Qt, QTimer, pyqtSignal
from PyQt6.QtGui import QColor, QFont, QIcon, QLinearGradient, QPainter, QPainterPath, QPen, QRadialGradient
from PyQt6.QtWidgets import (QApplication, QButtonGroup, QCheckBox, QComboBox, QCompleter, QFrame, QGridLayout,
                             QHBoxLayout, QLabel, QLineEdit, QListWidget, QListWidgetItem, QMessageBox,
                             QPlainTextEdit, QProgressBar, QPushButton, QRadioButton, QScrollArea, QStackedWidget, QVBoxLayout, QWidget)

from . import disks as disklib
from .config import EXTRAS, FILESYSTEMS, KEYBOARDS, LOCALES, STYLES, InstallConfig, validate, validate_user
from .plan import ONLINE_DESKTOPS, Env, build_plan, describe
from .runner import LOG
from .tui import ACCENTS, DESKTOP_INFO, DESKTOPS, KERNELS, SHELL_INFO, VERSION, list_timezones, online

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BG, PANEL, CARD, LINE = "#0b0b10", "#12121a", "#181822", "#2a2a38"
FG, MUTED, DANGER, WARN, OK = "#e8e8f0", "#8b8ba0", "#f87171", "#fbbf24", "#34d399"
FS_COLORS = {"ntfs": "#60a5fa", "vfat": "#fbbf24", "ext4": "#34d399", "btrfs": "#a78bfa", "xfs": "#22d3ee",
             "swap": "#6b7280", "crypto_LUKS": "#fb7185"}
TIPS = [
    "Auxo Settings changes almost everything (look, displays, apps, drivers) with no terminal needed.",
    "Press Super+/ on Hyprland, Sway or i3 to see every keyboard shortcut in plain words.",
    "auxo-tweak doctor --fix checks the login screen, sound, network and services, and repairs them.",
    "With btrfs, every update takes a snapshot first. Pick one from the boot menu to go back in time.",
    "Hyprland comes in three styles: Auxo, Mac (Liquid Glass) and Windows. Switch any time.",
    "auxo-tweak apps installs popular apps in bundles: browsers, media, creative, dev, chat and more.",
    "Auxo AI is a free assistant that runs on your PC: sudo auxo-tweak ai on.",
    "auxo-update updates everything: system packages, Flatpaks and firmware.",
]


def rgba(hex_color, a):
    c = QColor(hex_color)
    return f"rgba({c.red()},{c.green()},{c.blue()},{a})"


def stylesheet(accent):
    a = accent
    return f"""
    QWidget {{ color: {FG}; font-size: 14px; }}
    QLabel {{ background: transparent; }}
    QLabel#h1 {{ font-size: 30px; font-weight: 800; }}
    QLabel#h2 {{ font-size: 22px; font-weight: 700; }}
    QLabel#h3 {{ font-size: 15px; font-weight: 700; }}
    QLabel#muted, QLabel#hint {{ color: {MUTED}; }}
    QLabel#hint {{ font-size: 12px; }}
    QLabel#err {{ color: {DANGER}; }}
    QLabel#warn {{ color: {WARN}; }}
    QLabel#ok {{ color: {OK}; }}
    QLabel#chip {{ color: {MUTED}; background: {rgba(FG, 0.06)}; border-radius: 9px; padding: 2px 9px;
                   font-size: 11px; }}
    QLabel#chipon {{ color: {a}; background: {rgba(a, 0.14)}; border-radius: 9px; padding: 2px 9px; font-size: 11px; }}
    QFrame#side {{ background: {rgba(PANEL, 0.86)}; border-right: 1px solid {LINE}; }}
    QLabel#stepnow {{ color: {FG}; font-weight: 700; }}
    QLabel#stepdone {{ color: {MUTED}; }}
    QLabel#steplater {{ color: {rgba(MUTED, 0.6)}; }}
    QFrame#card {{ background: {rgba(CARD, 0.9)}; border: 1px solid {LINE}; border-radius: 14px; }}
    QFrame#card:hover {{ border-color: {rgba(a, 0.55)}; }}
    QFrame#card[checked="true"] {{ border: 2px solid {a}; background: {rgba(a, 0.08)}; }}
    QFrame#card:disabled {{ background: {rgba(CARD, 0.4)}; border-color: {rgba(LINE, 0.5)}; }}
    QFrame#box {{ background: {rgba(CARD, 0.9)}; border: 1px solid {LINE}; border-radius: 14px; }}
    QFrame#danger {{ background: {rgba(DANGER, 0.1)}; border: 1px solid {rgba(DANGER, 0.5)}; border-radius: 12px; }}
    QLineEdit, QComboBox, QPlainTextEdit, QListWidget {{
        background: {CARD}; border: 1px solid {LINE}; border-radius: 10px; padding: 8px 12px;
        selection-background-color: {a}; selection-color: {BG}; }}
    QLineEdit:focus, QComboBox:focus, QListWidget:focus {{ border-color: {a}; }}
    QLineEdit[bad="true"] {{ border-color: {DANGER}; }}
    QComboBox::drop-down {{ border: none; width: 24px; }}
    QComboBox QAbstractItemView {{ background: {CARD}; border: 1px solid {LINE}; selection-background-color: {rgba(a, 0.3)};
                                  selection-color: {FG}; outline: none; }}
    QListWidget {{ padding: 4px; outline: none; }}
    QListWidget::item {{ padding: 7px 10px; border-radius: 8px; }}
    QListWidget::item:selected {{ background: {rgba(a, 0.22)}; color: {FG}; }}
    QListWidget::item:hover {{ background: {rgba(FG, 0.05)}; }}
    QPlainTextEdit {{ font-family: "Fira Code", monospace; font-size: 12px; color: {MUTED}; }}
    QPushButton {{ background: {rgba(FG, 0.07)}; border: 1px solid {LINE}; border-radius: 10px; padding: 9px 18px; }}
    QPushButton:hover {{ background: {rgba(FG, 0.12)}; }}
    QPushButton:disabled {{ color: {rgba(MUTED, 0.5)}; }}
    QPushButton#primary {{ background: {a}; color: {BG}; border: none; font-weight: 700; padding: 10px 26px; }}
    QPushButton#primary:hover {{ background: {QColor(a).lighter(112).name()}; }}
    QPushButton#primary:disabled {{ background: {rgba(a, 0.3)}; color: {rgba(BG, 0.6)}; }}
    QPushButton#dangerbtn {{ background: {DANGER}; color: {BG}; border: none; font-weight: 700; padding: 10px 26px; }}
    QPushButton#link {{ background: transparent; border: none; color: {a}; padding: 4px 2px; text-align: left; }}
    QPushButton#link:hover {{ text-decoration: underline; }}
    QRadioButton, QCheckBox {{ spacing: 10px; background: transparent; }}
    QRadioButton::indicator, QCheckBox::indicator {{ width: 18px; height: 18px; border: 2px solid {LINE};
                                                     background: {CARD}; }}
    QRadioButton::indicator {{ border-radius: 11px; }}
    QCheckBox::indicator {{ border-radius: 6px; }}
    QRadioButton::indicator:checked {{ background: qradialgradient(cx:0.5, cy:0.5, radius:0.5, fx:0.5, fy:0.5,
                                       stop:0 {a}, stop:0.45 {a}, stop:0.55 {CARD}, stop:1 {CARD}); border-color: {a}; }}
    QCheckBox::indicator:checked {{ background: {a}; border-color: {a}; }}
    QCheckBox::indicator:disabled, QRadioButton::indicator:disabled {{ border-color: {rgba(LINE, 0.5)}; }}
    QProgressBar {{ background: {rgba(FG, 0.08)}; border: none; border-radius: 6px; height: 12px; }}
    QProgressBar::chunk {{ border-radius: 6px; background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                           stop:0 {a}, stop:1 {ACCENTS.get(accent_name(a), (a, a))[1]}); }}
    QScrollArea {{ background: transparent; border: none; }}
    QScrollArea > QWidget > QWidget {{ background: transparent; }}
    QScrollBar:vertical {{ background: transparent; width: 10px; margin: 2px; }}
    QScrollBar::handle:vertical {{ background: {rgba(FG, 0.15)}; border-radius: 4px; min-height: 30px; }}
    QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {{ height: 0; }}
    QToolTip {{ background: {CARD}; color: {FG}; border: 1px solid {LINE}; padding: 6px; }}
    """


def accent_name(hex_color):
    return next((k for k, v in ACCENTS.items() if v[0].lower() == hex_color.lower()), "violet")


def label(text, name=None, wrap=False):
    lab = QLabel(text)
    if name:
        lab.setObjectName(name)
    if wrap:
        lab.setWordWrap(True)
    return lab


def repolish(w):
    w.style().unpolish(w)
    w.style().polish(w)


class Card(QFrame):
    """A clickable, selectable card."""

    def __init__(self, on_click=None):
        super().__init__()
        self.setObjectName("card")
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.on_click = on_click
        self.setProperty("checked", False)

    def set_checked(self, on):
        self.setProperty("checked", bool(on))
        repolish(self)

    def mousePressEvent(self, e):
        if self.isEnabled() and self.on_click and e.button() == Qt.MouseButton.LeftButton:
            self.on_click()


class Logo(QWidget):
    """The Auxo mark: an open triangle with a dot, in the accent colours."""

    def __init__(self, size=56):
        super().__init__()
        self.setFixedSize(size, size)
        self.accent = ACCENTS["violet"]

    def paintEvent(self, _e):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        w, h = self.width(), self.height()
        g = QLinearGradient(0, 0, w, h)
        g.setColorAt(0, QColor(self.accent[0]))
        g.setColorAt(1, QColor(self.accent[1]))
        pen = QPen(g, max(3.0, w / 13))
        pen.setCapStyle(Qt.PenCapStyle.RoundCap)
        pen.setJoinStyle(Qt.PenJoinStyle.RoundJoin)
        p.setPen(pen)
        path = QPainterPath()
        path.moveTo(w * 0.1, h * 0.9)
        path.lineTo(w * 0.5, h * 0.2)
        path.lineTo(w * 0.9, h * 0.9)
        path.moveTo(w * 0.32, h * 0.9)
        path.lineTo(w * 0.5, h * 0.56)
        path.lineTo(w * 0.68, h * 0.9)
        p.drawPath(path)
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(QColor(self.accent[0]))
        p.drawEllipse(QPointF(w * 0.5, h * 0.07), w / 16, w / 16)
        p.end()


class DiskBar(QWidget):
    """A disk drawn as a bar of its partitions (or of what Auxo will make of it)."""

    def __init__(self):
        super().__init__()
        self.setFixedHeight(30)
        self.segments = []      # (size, colour, text)
        self.total = 1

    def set_disk(self, disk, after=None, efi=True, accent="#a78bfa"):
        self.total = max(1, disk["size"])
        if after:
            boot = 1000 ** 3 if efi else 1000 ** 2
            self.segments = [(boot, "#fbbf24", "EFI" if efi else ""), (self.total - boot, accent, "Auxo")]
        else:
            used = 0
            self.segments = []
            for p in disk["parts"]:
                self.segments.append((p["size"], FS_COLORS.get(p["fstype"], "#475569"),
                                      (p["label"] or p["fstype"] or "").strip()))
                used += p["size"]
            if self.total - used > self.total * 0.01:
                self.segments.append((self.total - used, None, "free" if disk["parts"] else "empty"))
        self.update()

    def paintEvent(self, _e):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        r = QRectF(0.5, 0.5, self.width() - 1, self.height() - 1)
        clip = QPainterPath()
        clip.addRoundedRect(r, 8, 8)
        p.setClipPath(clip)
        p.fillRect(r, QColor(FG).darker(700))
        x = r.left()
        for i, (size, colour, text) in enumerate(self.segments):
            w = max(4.0, r.width() * size / self.total)
            if i == len(self.segments) - 1:
                w = r.right() - x
            seg = QRectF(x, r.top(), w, r.height())
            if colour:
                p.fillRect(seg, QColor(colour))
            else:
                p.fillRect(seg, QColor(FG).darker(500))
            p.setPen(QColor(BG if colour else MUTED))
            f = p.font()
            f.setPointSizeF(8.5)
            f.setBold(True)
            p.setFont(f)
            if w > 54 and text:
                label_text = f"{text}  {disklib.human(size)}"
                if p.fontMetrics().horizontalAdvance(label_text) > w - 10:
                    label_text = text
                if p.fontMetrics().horizontalAdvance(label_text) <= w - 10:
                    p.drawText(seg.adjusted(8, 0, -4, 0), Qt.AlignmentFlag.AlignVCenter, label_text)
            if i:
                p.setPen(QPen(QColor(BG), 2))
                p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()))
            x += w
        p.end()


class StylePreview(QWidget):
    """A small drawing of a Hyprland style: auxo, mac or windows."""

    def __init__(self, style):
        super().__init__()
        self.style_key = style
        self.accent = ACCENTS["violet"]
        self.setFixedSize(176, 104)

    def paintEvent(self, _e):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        w, h = self.width(), self.height()
        clip = QPainterPath()
        clip.addRoundedRect(QRectF(0, 0, w, h), 10, 10)
        p.setClipPath(clip)
        g = QLinearGradient(0, 0, w, h)
        if self.style_key == "mac":
            g.setColorAt(0, QColor("#3d7be0"))
            g.setColorAt(1, QColor("#cfe8ff"))
        elif self.style_key == "windows":
            g.setColorAt(0, QColor("#e4e9ef"))
            g.setColorAt(1, QColor("#8fa6bf"))
        elif self.style_key == "neon":
            g.setColorAt(0, QColor("#090914"))
            g.setColorAt(0.65, QColor("#2a0b3d"))
            g.setColorAt(1, QColor("#ff2bd6"))
        elif self.style_key == "zen":
            g.setColorAt(0, QColor("#e9e4da"))
            g.setColorAt(1, QColor("#c9cfc2"))
        elif self.style_key == "candy":
            g.setColorAt(0, QColor("#ffd6e8"))
            g.setColorAt(0.5, QColor("#e3d5ff"))
            g.setColorAt(1, QColor("#c8f3e6"))
        else:
            g.setColorAt(0, QColor("#141420"))
            g.setColorAt(1, QColor(self.accent[0]).darker(220))
        p.fillRect(0, 0, w, h, g)
        p.setPen(Qt.PenStyle.NoPen)

        def win(x, y, ww, hh, light, radius=5):
            p.setBrush(QColor(255, 255, 255, 235) if light else QColor(20, 20, 30, 230))
            p.drawRoundedRect(QRectF(x, y, ww, hh), radius, radius)
            p.setBrush(QColor(0, 0, 0, 30) if light else QColor(255, 255, 255, 25))
            p.drawRoundedRect(QRectF(x, y, ww, 9), radius, radius)

        if self.style_key == "mac":
            p.setBrush(QColor(255, 255, 255, 150))
            p.drawRect(QRectF(0, 0, w, 9))
            win(26, 18, 110, 58, True, 7)
            for i, c in enumerate(("#ff5f57", "#febc2e", "#28c840")):
                p.setBrush(QColor(c))
                p.drawEllipse(QPointF(33 + i * 6, 22.5), 2, 2)
            p.setBrush(QColor(255, 255, 255, 140))
            p.drawRoundedRect(QRectF(44, 84, 88, 15), 6, 6)
            for i, c in enumerate(("#60a5fa", "#f97316", "#22c55e", "#a855f7", "#ef4444", "#14b8a6")):
                p.setBrush(QColor(c))
                p.drawRoundedRect(QRectF(49 + i * 13.5, 87, 9.5, 9.5), 3, 3)
        elif self.style_key == "windows":
            win(30, 14, 116, 62, True, 4)
            p.setBrush(QColor(243, 243, 243, 240))
            p.drawRect(QRectF(0, h - 13, w, 13))
            for i, c in enumerate(("#2563eb", "#9ca3af", "#f97316", "#374151", "#1e3a8a", "#5865f2")):
                p.setBrush(QColor(c))
                p.drawRoundedRect(QRectF(w / 2 - 40 + i * 14, h - 10, 8, 8), 2, 2)
        elif self.style_key == "neon":
            p.setBrush(QColor(9, 9, 20, 230))
            p.drawRect(QRectF(0, 0, w, 10))
            p.setBrush(QColor("#22e6ff"))
            for i in range(4):
                p.drawRect(QRectF(5 + i * 9, 3, 6, 4))
            win(8, 16, 84, h - 22, False, 2)
            win(98, 16, w - 106, h - 22, False, 2)
            p.setBrush(Qt.BrushStyle.NoBrush)
            p.setPen(QPen(QColor("#ff2bd6"), 1.6))
            p.drawRect(QRectF(8, 16, 84, h - 22))
            p.setPen(QPen(QColor("#22e6ff"), 1.0))
            p.drawRect(QRectF(98, 16, w - 106, h - 22))
        elif self.style_key == "zen":
            win(18, 14, w - 36, h - 36, True, 3)
            p.setBrush(QColor(40, 42, 38, 200))
            p.drawRoundedRect(QRectF(w / 2 - 36, h - 14, 72, 8), 4, 4)
            p.setBrush(QColor("#8fa889"))
            p.drawEllipse(QPointF(w / 2, h - 10), 2, 2)
        elif self.style_key == "candy":
            p.setBrush(QColor(255, 255, 255, 200))
            for x, ww in ((6, 34), (w / 2 - 22, 44), (w - 46, 40)):
                p.drawRoundedRect(QRectF(x, 5, ww, 11), 5.5, 5.5)
            win(14, 24, 90, 66, True, 12)
            win(110, 24, w - 124, 66, True, 12)
            for i, c in enumerate(("#ff8fc6", "#a98bff", "#6fdcbf")):
                p.setBrush(QColor(c))
                p.drawEllipse(QPointF(w / 2 - 12 + i * 12, 10.5), 3, 3)
        else:
            p.setBrush(QColor(20, 20, 30, 210))
            p.drawRoundedRect(QRectF(6, 5, w - 12, 10), 5, 5)
            p.setBrush(QColor(self.accent[0]))
            p.drawEllipse(QPointF(13, 10), 2.5, 2.5)
            win(6, 21, 80, h - 27, False)
            win(91, 21, w - 97, (h - 31) / 2, False)
            win(91, 21 + (h - 31) / 2 + 4, w - 97, (h - 31) / 2, False)
            p.setBrush(Qt.BrushStyle.NoBrush)
            p.setPen(QPen(QColor(self.accent[0]), 1.4))
            p.drawRoundedRect(QRectF(6, 21, 80, h - 27), 5, 5)
        p.end()


class Signals(QObject):
    online = pyqtSignal(bool)
    timezone = pyqtSignal(str)


# ── pages ─────────────────────────────────────────────────────────────
class Page(QWidget):
    title = ""
    heading = ""
    intro = ""

    def __init__(self, win):
        super().__init__()
        self.win, self.cfg, self.env = win, win.cfg, win.env
        outer = QVBoxLayout(self)
        outer.setContentsMargins(0, 0, 0, 0)
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        body = QWidget()
        scroll.setWidget(body)
        outer.addWidget(scroll)
        self.lay = QVBoxLayout(body)
        self.lay.setContentsMargins(48, 40, 48, 24)
        self.lay.setSpacing(14)
        if self.heading:
            self.lay.addWidget(label(self.heading, "h2"))
        if self.intro:
            self.lay.addWidget(label(self.intro, "muted", wrap=True))
            self.lay.addSpacing(6)
        self.build()
        self.lay.addStretch(1)

    def build(self):
        pass

    def enter(self):
        pass

    def problems(self):
        return []


class WelcomePage(Page):
    title = "Welcome"

    def build(self):
        top = QHBoxLayout()
        self.logo = Logo(84)
        top.addWidget(self.logo)
        top.addSpacing(18)
        col = QVBoxLayout()
        col.addWidget(label("Welcome to Auxo Linux", "h1"))
        col.addWidget(label(f"Version {VERSION} · Void base · runit · rolling", "muted"))
        top.addLayout(col, 1)
        self.lay.addLayout(top)
        self.lay.addSpacing(10)
        self.lay.addWidget(label("This puts Auxo on your computer. It asks a few questions, shows you a summary, and "
                                 "changes nothing until you press Install. Everything you pick can be changed later "
                                 "in Auxo Settings.", wrap=True))
        box = QFrame()
        box.setObjectName("box")
        g = QGridLayout(box)
        g.setContentsMargins(20, 16, 20, 16)
        g.setHorizontalSpacing(18)
        g.setVerticalSpacing(10)
        g.addWidget(label("Internet", "muted"), 0, 0)
        self.net = label("")
        g.addWidget(self.net, 0, 1)
        btns = QHBoxLayout()
        self.wifi = QPushButton("Connect to Wi-Fi…")
        self.wifi.clicked.connect(self.connect_wifi)
        again = QPushButton("Check again")
        again.clicked.connect(self.win.check_online)
        btns.addWidget(self.wifi)
        btns.addWidget(again)
        btns.addStretch(1)
        g.addLayout(btns, 1, 1)
        g.addWidget(label("Boot mode", "muted"), 2, 0)
        g.addWidget(label("UEFI" if self.env.efi else "BIOS (legacy)"), 2, 1)
        g.setColumnStretch(1, 1)
        self.lay.addWidget(box)
        feats = QHBoxLayout()
        feats.setSpacing(12)
        for head, text in (("Riced, every desktop", "Hyprland in six styles (Windows, Mac, Auxo, Neon, Zen, Candy), Sway, GNOME, "
                                                     "i3 and more, all themed in your colour."),
                           ("Undo button", "btrfs snapshots before every update. Boot into one if an update "
                                           "goes wrong."),
                           ("Ready to play", "Steam, Discord, GameMode and the right GPU driver, set up for you."),
                           ("No terminal needed", "Auxo Settings changes the look, displays, apps and drivers. "
                                                  "auxo-tweak does it all from a terminal too.")):
            f = QFrame()
            f.setObjectName("box")
            v = QVBoxLayout(f)
            v.setContentsMargins(16, 14, 16, 14)
            v.addWidget(label(head, "h3"))
            v.addWidget(label(text, "muted", wrap=True))
            v.addStretch(1)
            feats.addWidget(f, 1)
        self.lay.addLayout(feats)
        self.lay.addSpacing(8)
        heart = QHBoxLayout()
        heart.addWidget(label("♥", "err"))
        heart.addWidget(label("<b>Auxo supports cancer research.</b> Auxo is free. If it's useful to you, give to the "
                              "Terry Fox Foundation instead: terryfox.org", "muted", wrap=True), 1)
        self.lay.addLayout(heart)
        self.show_online()

    def show_online(self):
        if self.env.online:
            self.net.setText("Connected")
            self.net.setObjectName("ok")
        else:
            self.net.setText("Offline — Hyprland (every style), Sway and the stable kernel still install. Connect for every desktop, "
                             "Steam and Discord.")
            self.net.setObjectName("warn")
        self.net.setWordWrap(True)
        repolish(self.net)

    def connect_wifi(self):
        for argv in (["nm-connection-editor"], ["kcmshell6", "kcm_networkmanagement"],
                     ["konsole", "-e", "nmtui"], ["foot", "nmtui"], ["xterm", "-e", "nmtui"]):
            if shutil.which(argv[0]):
                subprocess.Popen(argv, start_new_session=True)
                QTimer.singleShot(15000, self.win.check_online)
                return
        QMessageBox.information(self, "Wi-Fi", "Use the network icon in the panel to connect, then press Check again.")


class RegionPage(Page):
    title = "Region"
    heading = "Language, keyboard and time"
    intro = "Pick your language, the layout printed on your keyboard, and where you are."

    def build(self):
        cols = QHBoxLayout()
        cols.setSpacing(28)
        left = QVBoxLayout()
        left.setSpacing(8)
        left.addWidget(label("Language", "h3"))
        self.lang = QComboBox()
        for code, name in LOCALES:
            self.lang.addItem(name, code)
        self.lang.setCurrentIndex(max(0, self.lang.findData(self.cfg.locale)))
        self.lang.currentIndexChanged.connect(lambda _i: setattr(self.cfg, "locale", self.lang.currentData()))
        left.addWidget(self.lang)
        left.addSpacing(12)
        left.addWidget(label("Time zone", "h3"))
        self.zones = self.win.timezones
        self.regions = sorted({z.split("/", 1)[0] for z in self.zones if "/" in z}) + ["UTC"]
        self.region = QComboBox()
        self.region.addItems(self.regions)
        self.city = QComboBox()
        self.city.setEditable(True)
        self.city.setInsertPolicy(QComboBox.InsertPolicy.NoInsert)
        self.city.completer().setFilterMode(Qt.MatchFlag.MatchContains)
        self.city.completer().setCompletionMode(QCompleter.CompletionMode.PopupCompletion)
        self.region.currentTextChanged.connect(self.fill_cities)
        self.city.currentIndexChanged.connect(self.pick_city)
        left.addWidget(self.region)
        left.addWidget(self.city)
        self.tz_hint = label("", "hint")
        left.addWidget(self.tz_hint)
        left.addStretch(1)
        cols.addLayout(left, 1)

        right = QVBoxLayout()
        right.setSpacing(8)
        right.addWidget(label("Keyboard", "h3"))
        self.kb_filter = QLineEdit()
        self.kb_filter.setPlaceholderText("Search layouts")
        self.kb_filter.textChanged.connect(self.filter_keyboards)
        right.addWidget(self.kb_filter)
        self.kb = QListWidget()
        self.kb.setMinimumHeight(260)
        for code, name in KEYBOARDS:
            it = QListWidgetItem(f"{name}   ·  {code}")
            it.setData(Qt.ItemDataRole.UserRole, code)
            self.kb.addItem(it)
            if code == self.cfg.keyboard:
                self.kb.setCurrentItem(it)
        self.kb.currentItemChanged.connect(self.pick_keyboard)
        right.addWidget(self.kb, 1)
        self.kb_test = QLineEdit()
        self.kb_test.setPlaceholderText("Type here to test your keyboard")
        right.addWidget(self.kb_test)
        cols.addLayout(right, 1)
        self.lay.addLayout(cols)
        self.set_timezone(self.cfg.timezone)
        self.win.signals.timezone.connect(self.guessed_timezone)
        self.region.activated.connect(lambda *_: setattr(self.win, "touched_timezone", True))
        self.city.activated.connect(lambda *_: setattr(self.win, "touched_timezone", True))

    def set_timezone(self, tz):
        region = tz.split("/", 1)[0] if "/" in tz else "UTC"
        self.region.setCurrentText(region)
        self.fill_cities(region)
        if "/" in tz:
            self.city.setCurrentIndex(max(0, self.city.findData(tz)))
        self.pick_city()

    def fill_cities(self, region):
        self.city.blockSignals(True)
        self.city.clear()
        if region == "UTC":
            self.city.addItem("Coordinated Universal Time", "UTC")
            self.city.setEnabled(False)
        else:
            self.city.setEnabled(True)
            for z in self.zones:
                if z.startswith(region + "/"):
                    self.city.addItem(z.split("/", 1)[1].replace("_", " ").replace("/", " / "), z)
        self.city.blockSignals(False)
        self.pick_city()

    def pick_city(self, *_a):
        tz = self.city.currentData()
        if tz:
            self.cfg.timezone = tz
            self.tz_hint.setText(f"Time zone: {tz}")

    def guessed_timezone(self, tz):
        if tz in self.zones and not self.win.touched_timezone:
            self.set_timezone(tz)
            self.tz_hint.setText(f"Time zone: {tz} (found from your connection)")

    def filter_keyboards(self, text):
        t = text.lower()
        for i in range(self.kb.count()):
            it = self.kb.item(i)
            it.setHidden(bool(t) and t not in it.text().lower())

    def pick_keyboard(self, it, _prev=None):
        if not it:
            return
        self.cfg.keyboard = it.data(Qt.ItemDataRole.UserRole)
        # let you test (and type your password with) the layout you picked
        if not self.win.dry_run and os.environ.get("DISPLAY") and shutil.which("setxkbmap"):
            subprocess.Popen(["setxkbmap", "-layout", self.cfg.keyboard], stderr=subprocess.DEVNULL)



class DiskPage(Page):
    title = "Disk"
    heading = "Where should Auxo go?"
    intro = "Pick a disk to erase and install Auxo on, or use partitions you made yourself (to keep Windows or your data)."

    def build(self):
        self.cards = {}
        self.disks = self.win.disks
        for d in self.disks:
            card = Card(lambda path=d["path"]: self.choose(path))
            v = QVBoxLayout(card)
            v.setContentsMargins(18, 14, 18, 14)
            v.setSpacing(8)
            row = QHBoxLayout()
            row.addWidget(label(f"<b>{d['model']}</b>"))
            row.addWidget(label(f"{d['path']} · {disklib.human(d['size'])}" + (f" · {d['tran'].upper()}" if d["tran"]
                                                                               else ""), "muted"))
            row.addStretch(1)
            if not d["usable"]:
                row.addWidget(label(d["reason"], "chip"))
                card.setEnabled(False)
            elif d["removable"]:
                row.addWidget(label("removable", "chip"))
            v.addLayout(row)
            bar = DiskBar()
            bar.set_disk(d)
            v.addWidget(bar)
            card.bar = bar
            self.cards[d["path"]] = card
            self.lay.addWidget(card)
        if not any(d["usable"] for d in self.disks):
            self.lay.addWidget(label("No disk can be erased for Auxo (see the reasons). You can still use existing "
                                     "partitions.", "warn", wrap=True))
        manual = Card(lambda: self.choose("manual"))
        mv = QVBoxLayout(manual)
        mv.setContentsMargins(18, 14, 18, 14)
        mv.addWidget(label("<b>Use existing partitions…</b>"))
        mv.addWidget(label("Dual boot, or keep a separate data partition. Make the partitions first (with KDE "
                           "Partition Manager or cfdisk).", "muted", wrap=True))
        self.cards["manual"] = manual
        self.lay.addWidget(manual)

        self.parts_box = QFrame()
        self.parts_box.setObjectName("box")
        g = QGridLayout(self.parts_box)
        g.setContentsMargins(18, 14, 18, 14)
        g.addWidget(label("Auxo ( / )", "muted"), 0, 0)
        self.root_part = QComboBox()
        g.addWidget(self.root_part, 0, 1)
        g.addWidget(label("Formatted: everything on it is deleted.", "hint"), 1, 1)
        self.efi_label = label("EFI system partition", "muted")
        g.addWidget(self.efi_label, 2, 0)
        self.efi_part = QComboBox()
        g.addWidget(self.efi_part, 2, 1)
        self.format_efi = QCheckBox("Format the EFI partition (only if it's new)")
        g.addWidget(self.format_efi, 3, 1)
        g.setColumnStretch(1, 1)
        parts = [p for d in self.disks for p in d["parts"]]
        for combo in (self.root_part, self.efi_part):
            combo.addItem("Pick a partition", "")
            for p in parts:
                text = f"{p['path']}  {disklib.human(p['size'])}  {p['fstype'] or 'empty'} {p['label']}".strip()
                combo.addItem(text + ("  (mounted)" if p["mountpoint"] else ""), p["path"])
                if p["mountpoint"]:
                    combo.model().item(combo.count() - 1).setEnabled(False)
        vfat = next((p["path"] for p in parts if p["fstype"] == "vfat"), "")
        if vfat:
            self.efi_part.setCurrentIndex(self.efi_part.findData(vfat))
        self.root_part.currentIndexChanged.connect(self.manual_changed)
        self.efi_part.currentIndexChanged.connect(self.manual_changed)
        self.format_efi.toggled.connect(lambda on: setattr(self.cfg, "format_efi", on))
        for w in (self.efi_label, self.efi_part, self.format_efi):
            w.setVisible(self.env.efi)
        self.lay.addWidget(self.parts_box)

        self.lay.addSpacing(6)
        self.lay.addWidget(label("Filesystem", "h3"))
        self.fs = QButtonGroup(self)
        for k, text in FILESYSTEMS.items():
            name, desc = text.split("—", 1)
            rb = QRadioButton(f"{name.strip()}  —  {desc.strip()}")
            rb.setProperty("key", k)
            rb.setChecked(k == self.cfg.filesystem)
            self.fs.addButton(rb)
            self.lay.addWidget(rb)
        self.fs.buttonToggled.connect(lambda b, on: on and setattr(self.cfg, "filesystem", b.property("key")))
        self.after = label("", "hint", wrap=True)
        self.lay.addWidget(self.after)
        first = next((d["path"] for d in self.disks if d["usable"]), None)
        self.choose(self.cfg.disk if self.cfg.mode == "erase" and self.cfg.disk else first or "manual")

    def choose(self, key):
        self.win.problem.setText("") if hasattr(self.win, "problem") else None
        for k, c in self.cards.items():
            c.set_checked(k == key)
        if key == "manual":
            self.cfg.mode = "manual"
            self.cfg.disk = ""
        else:
            self.cfg.mode, self.cfg.disk = "erase", key
        self.parts_box.setVisible(key == "manual")
        accent = ACCENTS.get(self.cfg.accent, ACCENTS["violet"])[0]
        for d in self.disks:
            self.cards[d["path"]].bar.set_disk(d, after=d["path"] == key, efi=self.env.efi, accent=accent)
        if key != "manual":
            d = next(d for d in self.disks if d["path"] == key)
            self.after.setText(f"After the install: {'a 1 GB EFI partition and ' if self.env.efi else ''}"
                               f"Auxo on the rest of {d['path']} ({disklib.human(d['size'])}). "
                               "Everything on this disk now is deleted.")
        else:
            self.after.setText("")
            self.manual_changed()

    def manual_changed(self, *_a):
        self.win.problem.setText("") if hasattr(self.win, "problem") else None
        self.cfg.root_part = self.root_part.currentData() or ""
        self.cfg.efi_part = (self.efi_part.currentData() or "") if self.env.efi else ""
        fstype = next((p["fstype"] for d in self.disks for p in d["parts"] if p["path"] == self.cfg.efi_part), "")
        self.format_efi.setChecked(bool(self.cfg.efi_part) and fstype != "vfat")

    def problems(self):
        errs = [e for e in validate(InstallConfig(**{**self.cfg.__dict__, "username": "x", "password": "x"}),
                                    self.env.efi)]
        return errs


class DesktopPage(Page):
    title = "Desktop"
    heading = "Pick your desktop"
    intro = "All of them come riced by Auxo. You can install another one later in Auxo Settings."

    def build(self):
        # the Hyprland styles are desktops of their own here: easy to find, one click
        entries = []
        for k, d in DESKTOPS.items():
            if k == "hyprland":
                for style, (sname, sdesc, _rice) in STYLES.items():
                    name = f"Hyprland — {sname} style" + (" (the live session)" if style == "windows" else "")
                    info = sdesc + "."
                    entries.append((f"hyprland:{style}", name, info, k, style))
            else:
                entries.append((k, d["name"], DESKTOP_INFO.get(k, ""), k, None))
        grid = QGridLayout()
        grid.setSpacing(12)
        self.cards, self.previews = {}, []
        for i, (key, name, info, desk, style) in enumerate(entries):
            card = Card(lambda key=key: self.choose_key(key))
            v = QVBoxLayout(card)
            v.setContentsMargins(16, 14, 16, 14)
            v.setSpacing(6)
            row = QHBoxLayout()
            row.addWidget(label(f"<b>{name}</b>"))
            row.addStretch(1)
            if desk in ONLINE_DESKTOPS:
                row.addWidget(label("needs internet" if self.env.online else "offline: Hyprland instead", "chip"))
            else:
                row.addWidget(label("installs offline", "chipon"))
            v.addLayout(row)
            if style:
                prev = StylePreview(style)
                self.previews.append(prev)
                v.addWidget(prev)
            v.addWidget(label(info, "muted", wrap=True))
            v.addStretch(1)
            self.cards[key] = card
            grid.addWidget(card, i // 2, i % 2)
        self.lay.addLayout(grid)
        self.choose(self.cfg.desktop, self.cfg.style)

    def choose_key(self, key):
        desk, _, style = key.partition(":")
        self.choose(desk, style or None)

    def choose(self, desk, style=None):
        self.cfg.desktop = desk
        if desk == "hyprland":
            self.cfg.style = style or self.cfg.style or "auxo"
        key = f"hyprland:{self.cfg.style}" if desk == "hyprland" else desk
        for k, c in self.cards.items():
            c.set_checked(k == key)

    def choose_style(self, style):
        self.choose("hyprland", style)

    def set_accent(self, accent):
        for p in self.previews:
            p.accent = accent
            p.update()


class LookPage(Page):
    title = "Look"
    heading = "Your colour"
    intro = ("One accent colour for the boot menu, login screen, terminal, bars and panels. The installer shows it "
             "as you pick.")

    def build(self):
        row = QHBoxLayout()
        row.setSpacing(14)
        self.swatches = {}
        for k, (c1, c2) in ACCENTS.items():
            card = Card(lambda k=k: self.choose(k))
            card.setFixedSize(112, 122)
            v = QVBoxLayout(card)
            v.setContentsMargins(10, 14, 10, 10)
            dot = QLabel()
            dot.setFixedSize(56, 56)
            dot.setStyleSheet(f"border-radius: 28px; background: qlineargradient(x1:0, y1:0, x2:1, y2:1, "
                              f"stop:0 {c1}, stop:1 {c2});")
            v.addWidget(dot, 0, Qt.AlignmentFlag.AlignHCenter)
            v.addWidget(label(k.capitalize()), 0, Qt.AlignmentFlag.AlignHCenter)
            self.swatches[k] = card
            row.addWidget(card)
        row.addStretch(1)
        self.lay.addLayout(row)
        self.lay.addSpacing(10)
        self.lay.addWidget(label("Light or dark mode, wallpaper, fonts, gaps and more are in Auxo Settings → Look "
                                 "after the install.", "muted", wrap=True))
        self.choose(self.cfg.accent)

    def choose(self, k):
        self.cfg.accent = k
        for key, c in self.swatches.items():
            c.set_checked(key == k)
        self.win.set_accent(k)


class SystemPage(Page):
    title = "System"
    heading = "Kernel and shell"
    intro = "The defaults are right for most people. Both can be switched any time."

    def build(self):
        self.lay.addWidget(label("Kernel", "h3"))
        self.kernel = QButtonGroup(self)
        for k, desc in KERNELS.items():
            rb = QRadioButton(f"{k}  —  {desc}" + ("" if k == "linux" or self.env.online else "  (needs internet)"))
            rb.setProperty("key", k)
            rb.setChecked(k == self.cfg.kernel)
            self.kernel.addButton(rb)
            self.lay.addWidget(rb)
        self.kernel.buttonToggled.connect(lambda b, on: on and setattr(self.cfg, "kernel", b.property("key")))
        self.lay.addSpacing(12)
        self.lay.addWidget(label("Shell", "h3"))
        self.shell = QButtonGroup(self)
        for k, desc in SHELL_INFO.items():
            rb = QRadioButton(f"{k}  —  {desc}")
            rb.setProperty("key", k)
            rb.setChecked(k == self.cfg.shell)
            self.shell.addButton(rb)
            self.lay.addWidget(rb)
        self.shell.buttonToggled.connect(lambda b, on: on and setattr(self.cfg, "shell", b.property("key")))


class AccountPage(Page):
    title = "Account"
    heading = "Who's using this computer?"
    intro = "You'll use your password to log in and for admin tasks (sudo)."

    def build(self):
        g = QGridLayout()
        g.setHorizontalSpacing(16)
        g.setVerticalSpacing(10)
        self.auto_user = self.auto_host = True
        self.fullname = QLineEdit(self.cfg.fullname)
        self.username = QLineEdit(self.cfg.username)
        self.hostname = QLineEdit(self.cfg.hostname)
        self.password = QLineEdit()
        self.password2 = QLineEdit()
        for w in (self.password, self.password2):
            w.setEchoMode(QLineEdit.EchoMode.Password)
        self.fullname.setPlaceholderText("Alex Smith")
        self.username.setPlaceholderText("alex")
        self.fullname.textEdited.connect(self.name_typed)
        self.username.textEdited.connect(lambda _t: (setattr(self, "auto_user", False), self.update_fields()))
        self.username.textChanged.connect(self.user_changed)
        self.hostname.textEdited.connect(lambda _t: (setattr(self, "auto_host", False), self.update_fields()))
        for w in (self.password, self.password2):
            w.textChanged.connect(self.update_fields)
        rows = [("Your name", self.fullname), ("Username", self.username), ("Password", self.password),
                ("Password again", self.password2), ("Computer name", self.hostname)]
        for i, (t, w) in enumerate(rows):
            g.addWidget(label(t, "muted"), i, 0)
            g.addWidget(w, i, 1)
        self.strength = QProgressBar()
        self.strength.setRange(0, 4)
        self.strength.setTextVisible(False)
        self.strength.setFixedHeight(6)
        g.addWidget(self.strength, 2, 2)
        self.strength_label = label("", "hint")
        g.addWidget(self.strength_label, 3, 2)
        g.setColumnStretch(1, 1)
        g.setColumnMinimumWidth(2, 150)
        self.lay.addLayout(g)
        self.root_check = QCheckBox("Set a separate root password (most people don't need one: root stays locked)")
        self.root_pw = QLineEdit()
        self.root_pw.setEchoMode(QLineEdit.EchoMode.Password)
        self.root_pw.setPlaceholderText("Root password")
        self.root_pw.setVisible(False)
        self.root_check.toggled.connect(self.root_pw.setVisible)
        self.root_pw.textChanged.connect(self.update_fields)
        self.root_check.toggled.connect(self.update_fields)
        self.lay.addSpacing(6)
        self.lay.addWidget(self.root_check)
        self.lay.addWidget(self.root_pw)
        self.errors = label("", "err", wrap=True)
        self.lay.addWidget(self.errors)
        self.shown_errors = False

    def name_typed(self, text):
        if self.auto_user:
            first = (text.strip().split() or [""])[0].lower()
            self.username.setText("".join(c for c in first if c.isalnum() or c in "-_")[:32])
        self.update_fields()

    def user_changed(self, text):
        if self.auto_host and text:
            self.hostname.setText(f"{text}-pc"[:63])
        self.update_fields()

    def update_fields(self, *_a):
        c = self.cfg
        c.fullname, c.username, c.hostname = self.fullname.text().strip(), self.username.text().strip(), \
            self.hostname.text().strip()
        c.password = self.password.text()
        c.root_password = self.root_pw.text() if self.root_check.isChecked() else ""
        pw = c.password
        score = sum((len(pw) >= 8, len(pw) >= 12, any(ch.isdigit() for ch in pw) and any(ch.isalpha() for ch in pw),
                     any(not ch.isalnum() for ch in pw))) if pw else 0
        self.strength.setValue(score)
        self.strength_label.setText(["", "Weak", "Okay", "Good", "Strong"][score])
        if self.shown_errors:
            self.problems()

    def problems(self):
        errs = validate_user(self.cfg, self.password2.text())
        if self.root_check.isChecked() and not self.root_pw.text():
            errs.append("Root password is empty: untick the box to keep root locked.")
        self.shown_errors = True
        self.errors.setText("\n".join(errs))
        for w, bad in ((self.username, any(e.startswith("Username") for e in errs)),
                       (self.password, any(e.startswith("Password") for e in errs)),
                       (self.password2, any("match" in e for e in errs)),
                       (self.hostname, any(e.startswith("Computer") for e in errs))):
            w.setProperty("bad", bad)
            repolish(w)
        return errs


class ExtrasPage(Page):
    title = "Extras"
    heading = "Extras"
    intro = "All optional. Each one can be switched on or off later in Auxo Settings."

    def build(self):
        self.boxes = {}
        for k, text in EXTRAS.items():
            cb = QCheckBox(text)
            cb.toggled.connect(self.sync)
            self.boxes[k] = cb
            self.lay.addWidget(cb)
        self.hint = label("", "hint", wrap=True)
        self.lay.addWidget(self.hint)

    def enter(self):
        btrfs = self.cfg.filesystem == "btrfs"
        for k, cb in self.boxes.items():
            text = EXTRAS[k]
            if k == "snapshots" and not btrfs:
                text += "  (needs btrfs)"
            if k in ("gaming", "flatpak", "discord") and not self.env.online:
                text += "  (needs internet)"
            cb.blockSignals(True)
            cb.setText(text)
            cb.setEnabled(not (k == "snapshots" and not btrfs))
            cb.setChecked(k in self.cfg.extras and cb.isEnabled())
            cb.blockSignals(False)
        self.sync()

    def sync(self, *_a):
        self.cfg.extras = [k for k, cb in self.boxes.items() if cb.isChecked() and cb.isEnabled()]
        self.hint.setText("" if self.env.online else "You're offline: the extras that need internet are skipped. "
                          "Add them later in Auxo Settings.")


class ReviewPage(Page):
    title = "Summary"
    heading = "Ready to install"
    intro = "Check everything. Nothing has been changed yet."

    def build(self):
        self.box = QFrame()
        self.box.setObjectName("box")
        self.grid = QGridLayout(self.box)
        self.grid.setContentsMargins(20, 16, 20, 16)
        self.grid.setHorizontalSpacing(24)
        self.grid.setVerticalSpacing(8)
        self.lay.addWidget(self.box)
        self.danger = QFrame()
        self.danger.setObjectName("danger")
        dl = QHBoxLayout(self.danger)
        dl.setContentsMargins(16, 12, 16, 12)
        self.danger_text = label("", wrap=True)
        dl.addWidget(self.danger_text)
        self.lay.addWidget(self.danger)
        self.notes = label("", "warn", wrap=True)
        self.errors = label("", "err", wrap=True)
        self.lay.addWidget(self.notes)
        self.lay.addWidget(self.errors)

    def enter(self):
        while self.grid.count():
            self.grid.takeAt(0).widget().deleteLater()
        friendly = {"Desktop": DESKTOPS.get(self.cfg.desktop, {}).get("name", self.cfg.desktop) +
                    (f" — {STYLES[self.cfg.style][0]} style" if self.cfg.desktop == "hyprland" else ""),
                    "Language": dict(LOCALES).get(self.cfg.locale, self.cfg.locale),
                    "Keyboard": dict(KEYBOARDS).get(self.cfg.keyboard, self.cfg.keyboard),
                    "Accent": self.cfg.accent.capitalize(),
                    "Extras": ", ".join(EXTRAS[x].split(" (")[0] for x in self.cfg.extras) or "none"}
        for i, (k, v) in enumerate(describe(self.cfg, self.env)):
            self.grid.addWidget(label(k, "muted"), i, 0, Qt.AlignmentFlag.AlignTop)
            self.grid.addWidget(label(friendly.get(k, v), wrap=True), i, 1)
        self.grid.setColumnStretch(1, 1)
        if self.cfg.mode == "erase":
            self.danger_text.setText(f"<b>Everything on {self.cfg.disk} will be deleted.</b> This can't be undone.")
        else:
            self.danger_text.setText(f"<b>{self.cfg.root_part} will be formatted.</b> Everything on it is deleted. "
                                     "Your other partitions are kept.")
        _steps, notes = build_plan(self.cfg, self.env)
        self.notes.setText("\n".join("• " + n for n in notes))
        errs = validate(self.cfg, self.env.efi)
        self.errors.setText("\n".join("✗ " + e for e in errs))

    def problems(self):
        return validate(self.cfg, self.env.efi)


class InstallPage(Page):
    title = "Install"

    def build(self):
        self.heading_label = label("Installing Auxo", "h2")
        self.lay.addWidget(self.heading_label)
        self.now = label("Starting…", "muted")
        self.lay.addWidget(self.now)
        row = QHBoxLayout()
        self.bar = QProgressBar()
        self.bar.setRange(0, 1000)
        self.bar.setTextVisible(False)
        self.bar.setFixedHeight(12)
        row.addWidget(self.bar, 1)
        self.pct = label("0%", "h3")
        self.pct.setMinimumWidth(48)
        self.pct.setAlignment(Qt.AlignmentFlag.AlignRight)
        row.addWidget(self.pct)
        self.lay.addLayout(row)
        self.steps_box = QVBoxLayout()
        self.steps_box.setSpacing(4)
        self.lay.addLayout(self.steps_box)
        self.tip_box = QFrame()
        self.tip_box.setObjectName("box")
        tl = QVBoxLayout(self.tip_box)
        tl.setContentsMargins(18, 14, 18, 14)
        tl.addWidget(label("Did you know?", "h3"))
        self.tip = label(TIPS[0], "muted", wrap=True)
        tl.addWidget(self.tip)
        self.lay.addWidget(self.tip_box)
        self.details = QPushButton("Show details")
        self.details.setObjectName("link")
        self.details.clicked.connect(self.toggle_log)
        self.lay.addWidget(self.details)
        self.log = QPlainTextEdit()
        self.log.setReadOnly(True)
        self.log.setMinimumHeight(180)
        self.log.setMaximumBlockCount(3000)
        self.log.setVisible(False)
        self.lay.addWidget(self.log)
        self.step_labels = []
        self.tip_i = 0
        self.tips = QTimer(self)
        self.tips.timeout.connect(self.next_tip)

    def toggle_log(self):
        self.log.setVisible(not self.log.isVisible())
        self.details.setText("Hide details" if self.log.isVisible() else "Show details")

    def next_tip(self):
        self.tip_i = (self.tip_i + 1) % len(TIPS)
        self.tip.setText(TIPS[self.tip_i])

    def set_steps(self, titles):
        for lab in self.step_labels:
            lab.deleteLater()
        self.step_labels = []
        for t in titles:
            lab = label("○  " + t, "steplater")
            self.steps_box.addWidget(lab)
            self.step_labels.append(lab)

    def set_step(self, i):
        for j, lab in enumerate(self.step_labels):
            title = lab.text()[3:]
            mark, name = ("✓", "stepdone") if j < i else (("●", "stepnow") if j == i else ("○", "steplater"))
            lab.setText(f"{mark}  {title}")
            lab.setObjectName(name)
            repolish(lab)
        if 0 <= i < len(self.step_labels):
            self.now.setText(self.step_labels[i].text()[3:] + "…")

    def set_pct(self, p):
        self.bar.setValue(int(p * 1000))
        self.pct.setText(f"{int(p * 100)}%")

    def add_line(self, line):
        self.log.appendPlainText(line)


class DonePage(Page):
    title = "Done"

    def build(self):
        self.big = label("", "h1")
        self.lay.addWidget(self.big)
        self.text = label("", wrap=True)
        self.lay.addWidget(self.text)
        self.notes = label("", "warn", wrap=True)
        self.lay.addWidget(self.notes)
        row = QHBoxLayout()
        self.reboot = QPushButton("Restart now")
        self.reboot.setObjectName("primary")
        self.reboot.clicked.connect(self.win.restart)
        self.close_btn = QPushButton("Keep exploring")
        self.close_btn.clicked.connect(self.win.close)
        self.log_btn = QPushButton("Show the log")
        self.log_btn.clicked.connect(lambda: self.win.show_log())
        row.addWidget(self.reboot)
        row.addWidget(self.close_btn)
        row.addWidget(self.log_btn)
        row.addStretch(1)
        self.lay.addSpacing(8)
        self.lay.addLayout(row)
        self.heart = label("♥  <b>Auxo supports cancer research.</b> If Auxo is useful to you, give to the Terry Fox "
                           "Foundation: terryfox.org", "muted", wrap=True)
        self.lay.addSpacing(16)
        self.lay.addWidget(self.heart)

    def show_result(self, ok, notes, error):
        if ok:
            self.big.setText("Auxo is installed")
            self.text.setText("Remove the USB stick and restart to start Auxo. Change anything later in Auxo "
                              "Settings, and update with auxo-update.")
        else:
            self.big.setText("The install stopped")
            self.text.setText(f"Something went wrong:\n\n{error}\n\nThe full log is in {LOG}. Your disk may be partly "
                              "written: fix the problem and run the installer again.")
        self.reboot.setVisible(ok)
        self.close_btn.setText("Keep exploring" if ok else "Close")
        self.heart.setVisible(ok)
        self.notes.setText("\n".join("• " + n for n in notes))


# ── the window ────────────────────────────────────────────────────────
class Installer(QWidget):
    PAGES = [WelcomePage, RegionPage, DiskPage, DesktopPage, LookPage, SystemPage, AccountPage, ExtrasPage,
             ReviewPage, InstallPage, DonePage]

    def __init__(self, cfg, env, dry_run=False, disks=None, timezones=None, installer=None, extra_args=()):
        super().__init__()
        self.cfg, self.env, self.dry_run = cfg, env, dry_run
        self.disks = disks if disks is not None else disklib.probe()
        self.timezones = timezones or list_timezones()
        self.installer = installer or os.path.join(HERE, "auxo-installer")
        self.extra_args = list(extra_args)
        self.touched_timezone = False
        self.signals = Signals()
        self.signals.online.connect(self.online_changed)
        self.proc = None
        self.result = None
        self.cfg_file = None
        self.buf = ""
        self.setWindowTitle("Install Auxo Linux" + (" (dry run)" if dry_run else ""))
        self.setWindowIcon(QIcon.fromTheme("auxo-logo"))
        self.setMinimumSize(1040, 700)
        self.accent = ACCENTS["violet"]

        root = QHBoxLayout(self)
        root.setContentsMargins(0, 0, 0, 0)
        root.setSpacing(0)
        side = QFrame()
        side.setObjectName("side")
        side.setFixedWidth(240)
        sl = QVBoxLayout(side)
        sl.setContentsMargins(26, 30, 20, 24)
        sl.setSpacing(12)
        top = QHBoxLayout()
        self.logo = Logo(34)
        top.addWidget(self.logo)
        top.addWidget(label("<b>Auxo</b> Linux", "h3"))
        top.addStretch(1)
        sl.addLayout(top)
        sl.addSpacing(22)
        self.step_labels = []
        for cls in self.PAGES[:-1]:
            lab = label(cls.title, "steplater")
            sl.addWidget(lab)
            self.step_labels.append(lab)
        sl.addStretch(1)
        if dry_run:
            sl.addWidget(label("Dry run: no disk is touched", "warn", wrap=True))
        root.addWidget(side)

        main = QVBoxLayout()
        main.setContentsMargins(0, 0, 0, 0)
        main.setSpacing(0)
        self.stack = QStackedWidget()
        self.pages = [cls(self) for cls in self.PAGES]
        for p in self.pages:
            self.stack.addWidget(p)
        main.addWidget(self.stack, 1)
        foot = QHBoxLayout()
        foot.setContentsMargins(48, 12, 48, 24)
        self.problem = label("", "err", wrap=True)
        foot.addWidget(self.problem, 1)
        self.back = QPushButton("Back")
        self.back.clicked.connect(self.go_back)
        self.next = QPushButton("Next")
        self.next.setObjectName("primary")
        self.next.clicked.connect(self.go_next)
        self.next.setDefault(True)
        foot.addWidget(self.back)
        foot.addWidget(self.next)
        self.footer = QWidget()
        self.footer.setLayout(foot)
        main.addWidget(self.footer)
        root.addLayout(main, 1)
        self.set_accent(cfg.accent)
        self.show_page(0)
        if self.env.online and not os.environ.get("AUXO_TEST_DISKS"):
            threading.Thread(target=self._guess_timezone, daemon=True).start()

    # theme
    def set_accent(self, name):
        self.accent = ACCENTS.get(name, ACCENTS["violet"])
        self.setStyleSheet(stylesheet(self.accent[0]))
        for logo in self.findChildren(Logo):
            logo.accent = self.accent
            logo.update()
        if hasattr(self, "pages"):
            self.pages[3].set_accent(self.accent)
            disk = self.pages[2]
            disk.choose(self.cfg.disk if self.cfg.mode == "erase" and self.cfg.disk else "manual")
        self.update()

    def paintEvent(self, _e):
        p = QPainter(self)
        p.fillRect(self.rect(), QColor(BG))
        g = QRadialGradient(QPointF(self.width() * 0.85, -40), self.width() * 0.6)
        c = QColor(self.accent[0])
        c.setAlpha(46)
        g.setColorAt(0, c)
        g.setColorAt(1, QColor(0, 0, 0, 0))
        p.fillRect(self.rect(), g)
        g2 = QRadialGradient(QPointF(self.width() * 0.3, self.height() + 60), self.width() * 0.5)
        c2 = QColor(self.accent[1])
        c2.setAlpha(26)
        g2.setColorAt(0, c2)
        g2.setColorAt(1, QColor(0, 0, 0, 0))
        p.fillRect(self.rect(), g2)
        p.end()

    # navigation
    def show_page(self, i):
        self.index = i
        page = self.pages[i]
        page.enter()
        self.stack.setCurrentIndex(i)
        for j, lab in enumerate(self.step_labels):
            name = "stepdone" if j < i else ("stepnow" if j == i else "steplater")
            lab.setText(("✓  " if j < i else ("●  " if j == i else "    ")) + self.PAGES[j].title)
            lab.setObjectName(name)
            repolish(lab)
        installing = isinstance(page, (InstallPage, DonePage))
        self.footer.setVisible(not installing)
        self.back.setEnabled(i > 0)
        review = isinstance(page, ReviewPage)
        self.next.setText("Install" if review else "Next")
        self.next.setObjectName("dangerbtn" if review else "primary")
        repolish(self.next)
        self.problem.setText("")

    def go_back(self):
        if self.index > 0:
            self.show_page(self.index - 1)

    def go_next(self):
        page = self.pages[self.index]
        errs = page.problems()
        if errs:
            self.problem.setText(errs[0])
            return
        if isinstance(page, ReviewPage):
            if self.confirm():
                self.start_install()
            return
        self.show_page(self.index + 1)

    def confirm(self):
        what = (f"Everything on {self.cfg.disk} will be deleted." if self.cfg.mode == "erase"
                else f"{self.cfg.root_part} will be formatted.")
        box = QMessageBox(QMessageBox.Icon.Warning, "Install Auxo now?", f"{what}\n\nThis can't be undone.",
                          parent=self)
        install = box.addButton("Install", QMessageBox.ButtonRole.AcceptRole)
        box.addButton("Go back", QMessageBox.ButtonRole.RejectRole)
        box.exec()
        return box.clickedButton() is install

    # network
    def check_online(self):
        threading.Thread(target=lambda: self.signals.online.emit(online()), daemon=True).start()

    def online_changed(self, on):
        self.env.online = on
        self.pages[0].show_online()

    def _guess_timezone(self):
        try:
            import urllib.request
            tz = urllib.request.urlopen("http://ip-api.com/line/?fields=timezone", timeout=4).read().decode().strip()
            if tz:
                self.signals.timezone.emit(tz)
        except Exception:  # noqa: BLE001 — a guess; the list is right there anyway
            pass

    # the install
    def command(self):
        args = [self.installer, "--config", self.cfg_file, "--no-tui", "--progress"] + self.extra_args
        if self.dry_run:
            args.append("--dry-run")
        if not self.env.online:
            args.append("--offline")
        if not self.env.efi:
            args.append("--bios")
        argv = [sys.executable, *args]
        if not self.dry_run and os.geteuid() != 0:
            if shutil.which("sudo") and subprocess.run(["sudo", "-n", "true"], capture_output=True).returncode == 0:
                argv = ["sudo", "-n", *argv]
            elif shutil.which("pkexec"):
                argv = ["pkexec", *argv]
            else:
                argv = ["sudo", "-n", *argv]
        return argv

    def start_install(self):
        fd, self.cfg_file = tempfile.mkstemp(prefix="auxo-install-", suffix=".json")
        with os.fdopen(fd, "w") as f:                      # mkstemp: only you (and root) can read it
            json.dump(self.cfg.to_dict(redact=False), f)
        page = self.pages[9]
        steps, _notes = build_plan(self.cfg, self.env)
        page.set_steps([s.title for s in steps])
        page.set_step(0)
        page.tips.start(9000)
        self.show_page(9)
        argv = self.command()
        page.add_line("$ " + " ".join(a if a != self.cfg_file else "<your answers>" for a in argv))
        self.proc = QProcess(self)
        self.proc.setProcessChannelMode(QProcess.ProcessChannelMode.MergedChannels)
        self.proc.readyReadStandardOutput.connect(self.read_output)
        self.proc.finished.connect(self.finished)
        self.proc.errorOccurred.connect(lambda _e: self.proc.state() == QProcess.ProcessState.NotRunning and
                                        self.finished(127, None))
        self.result = {"notes": [], "error": None, "done": False}
        self.proc.start(argv[0], argv[1:])

    def read_output(self):
        self.buf += bytes(self.proc.readAllStandardOutput()).decode(errors="replace")
        *lines, self.buf = self.buf.split("\n")
        page = self.pages[9]
        for line in lines:
            line = line.rstrip("\r")
            if line.startswith("@@"):
                tag, _, value = line[2:].partition(" ")
                try:
                    value = json.loads(value)
                except ValueError:
                    continue
                if tag == "steps":
                    page.set_steps(value)
                elif tag == "step":
                    page.set_step(value)
                elif tag == "pct":
                    page.set_pct(value)
                elif tag == "notes":
                    self.result["notes"] += value
                elif tag == "error":
                    self.result["error"] = value
                elif tag == "done":
                    self.result["done"] = True
                    self.result["notes"] = list(dict.fromkeys(self.result["notes"] + value))
            elif line.strip():
                page.add_line(line)

    def finished(self, code, _status):
        if self.result is None or self.result.get("finished"):
            return
        self.result["finished"] = True
        self.read_output()
        page = self.pages[9]
        page.tips.stop()
        if self.cfg_file:
            try:
                os.unlink(self.cfg_file)
            except OSError:
                pass
        ok = code == 0 and self.result["done"]
        if ok:
            page.set_step(len(page.step_labels))
            page.set_pct(1.0)
        error = self.result["error"] or (None if ok else
                                         ("couldn't get admin rights to install (sudo/pkexec)" if code in (126, 127)
                                          else f"the installer stopped (exit {code})"))
        self.pages[10].show_result(ok, self.result["notes"], error)
        self.show_page(10)

    def restart(self):
        if self.dry_run:
            self.close()
            return
        for argv in (["loginctl", "reboot"], ["sudo", "-n", "reboot"]):
            if shutil.which(argv[0]) and subprocess.run(argv).returncode == 0:
                return

    def show_log(self):
        page = self.pages[9]
        self.show_page(9)
        if not page.log.isVisible():
            page.toggle_log()
        page.heading_label.setText("Install log")
        self.footer.setVisible(True)
        self.next.setVisible(False)
        self.back.setEnabled(True)
        self.back.clicked.disconnect()
        self.back.clicked.connect(lambda: (self.next.setVisible(True), self.back.clicked.disconnect(),
                                           self.back.clicked.connect(self.go_back), self.show_page(10)))

    def closeEvent(self, e):
        if self.proc and self.proc.state() != QProcess.ProcessState.NotRunning:
            QMessageBox.warning(self, "Installing", "Auxo is being installed. Please wait until it's done.")
            e.ignore()
            return
        super().closeEvent(e)


def main(dry_run=False, config=None, bios=False, offline=False, disks=None, timezones=None):
    app = QApplication.instance() or QApplication(sys.argv[:1])
    app.setApplicationName("Install Auxo Linux")
    app.setDesktopFileName("auxo-install")
    f = QFont("Inter")
    f.setPointSizeF(10.5)
    app.setFont(f)
    cfg = InstallConfig(**json.load(open(config))) if config else InstallConfig()
    env = Env(efi=disklib.is_efi() and not bios, efi_bits=disklib.efi_bits(), online=False if offline else online())
    if disks is None and os.environ.get("AUXO_TEST_DISKS"):      # the test-suite's fake disks
        disks = disklib.parse_lsblk(json.load(open(os.environ["AUXO_TEST_DISKS"])))
    w = Installer(cfg, env, dry_run=dry_run, disks=disks, timezones=timezones)
    if os.environ.get("AUXO_LIVE") or os.path.exists("/etc/auxo/live"):
        w.showMaximized()
    else:
        w.resize(1120, 760)
        w.show()
    return app.exec()

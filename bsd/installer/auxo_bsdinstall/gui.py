"""The AuxoBSD installer window (PyQt6)."""
import os
import re
import sys
import traceback

from PyQt6.QtCore import Qt, QThread, pyqtSignal, QSize
from PyQt6.QtGui import QPixmap, QFont, QColor, QPalette
from PyQt6.QtWidgets import (QApplication, QWidget, QLabel, QPushButton, QVBoxLayout, QHBoxLayout, QStackedWidget,
                             QListWidget, QListWidgetItem, QLineEdit, QComboBox, QRadioButton, QButtonGroup,
                             QCheckBox, QSpinBox, QProgressBar, QPlainTextEdit, QMessageBox, QFormLayout,
                             QGridLayout, QFrame)

from . import config, plan, runner, system

try:
    from auxo_accents import ACCENTS
except ImportError:  # the shared palette normally lives in /usr/local/lib/auxo
    ACCENTS = {"violet": ("#a78bfa", "#22d3ee"), "cyan": ("#22d3ee", "#818cf8"), "emerald": ("#34d399", "#a3e635"),
               "amber": ("#fbbf24", "#fb7185"), "rose": ("#fb7185", "#c084fc"), "blue": ("#60a5fa", "#34d399"),
               "mono": ("#e5e7eb", "#9ca3af")}
try:
    import auxo_gpu
except ImportError:
    auxo_gpu = None

PIXMAPS = os.environ.get("AUXO_PIXMAPS", "/usr/local/share/pixmaps")
FAKE = os.environ.get("AUXO_INSTALLER_FAKE") == "1"

STEPS = ["Welcome", "Keyboard", "Time zone", "Disk", "Graphics", "Personalise", "Account", "Summary",
         "Installing", "Done"]


def stylesheet(accent):
    a, a2 = ACCENTS[accent]
    return f"""
    QWidget {{ background: #0b0b10; color: #e8e8f0; font-size: 11pt; }}
    #sidebar {{ background: #111118; border-right: 1px solid #1f1f2b; }}
    #sidebar QLabel {{ background: transparent; color: #6b6b80; padding: 6px 18px; }}
    #sidebar QLabel[current="true"] {{ color: {a}; font-weight: 600; }}
    #sidebar QLabel[done="true"] {{ color: #b8b8c8; }}
    QLabel#title {{ font-size: 20pt; font-weight: 600; }}
    QLabel#muted {{ color: #8b8ba0; }}
    QLabel#warn {{ color: #fb7185; }}
    QPushButton {{ background: #1b1b25; border: 1px solid #2a2a38; border-radius: 8px; padding: 8px 18px; }}
    QPushButton:hover {{ border-color: {a}; }}
    QPushButton#primary {{ background: {a}; color: #0b0b10; border: none; font-weight: 600; }}
    QPushButton#primary:disabled {{ background: #2a2a38; color: #6b6b80; }}
    QListWidget, QLineEdit, QComboBox, QSpinBox, QPlainTextEdit {{ background: #15151e; border: 1px solid #2a2a38;
        border-radius: 8px; padding: 6px; selection-background-color: {a}; selection-color: #0b0b10; }}
    QLineEdit:focus, QComboBox:focus, QListWidget:focus {{ border-color: {a}; }}
    QListWidget::item {{ padding: 8px; border-radius: 6px; }}
    QListWidget::item:selected {{ background: {a}; color: #0b0b10; }}
    QRadioButton::indicator:checked, QCheckBox::indicator:checked {{ background: {a}; border: 2px solid {a};
        border-radius: 7px; }}
    QRadioButton::indicator, QCheckBox::indicator {{ width: 14px; height: 14px; border: 2px solid #4b4b5c;
        border-radius: 7px; background: #15151e; }}
    QProgressBar {{ background: #15151e; border: none; border-radius: 6px; height: 12px; text-align: center; }}
    QProgressBar::chunk {{ border-radius: 6px; background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
        stop:0 {a}, stop:1 {a2}); }}
    QFrame#card {{ background: #111118; border: 1px solid #1f1f2b; border-radius: 12px; }}
    """


def page(title, subtitle=""):
    w = QWidget()
    lay = QVBoxLayout(w)
    lay.setContentsMargins(36, 32, 36, 16)
    lay.setSpacing(12)
    t = QLabel(title)
    t.setObjectName("title")
    lay.addWidget(t)
    if subtitle:
        s = QLabel(subtitle)
        s.setObjectName("muted")
        s.setWordWrap(True)
        lay.addWidget(s)
    return w, lay


class InstallThread(QThread):
    step = pyqtSignal(int, str)
    progress = pyqtSignal(float)
    log = pyqtSignal(str)
    finished_ok = pyqtSignal()
    failed = pyqtSignal(str)

    def __init__(self, cfg, dry_run):
        super().__init__()
        self.cfg, self.dry = cfg, dry_run

    def run(self):
        try:
            r = runner.Runner(plan.build(self.cfg), on_step=self.step.emit, on_progress=self.progress.emit,
                              on_log=self.log.emit, dry_run=self.dry)
            r.run()
            self.finished_ok.emit()
        except Exception as e:  # noqa: BLE001 — shown to the user
            self.log.emit(traceback.format_exc())
            self.failed.emit(str(e))


class Installer(QWidget):
    def __init__(self, dry_run=False):
        super().__init__()
        self.dry = dry_run
        self.cfg = config.InstallConfig()
        self.cfg.firmware = "uefi" if FAKE else system.firmware()
        self.setWindowTitle("Install AuxoBSD")
        self.resize(1000, 680)
        self.disks = self._fake_disks() if FAKE else system.disks()
        self.gpus, self.vm = self._fake_gpus() if FAKE else system.gpus()
        self.zones = system.timezones()

        root = QHBoxLayout(self)
        root.setContentsMargins(0, 0, 0, 0)
        root.setSpacing(0)
        side = QWidget()
        side.setObjectName("sidebar")
        side.setFixedWidth(220)
        sl = QVBoxLayout(side)
        sl.setContentsMargins(0, 24, 0, 24)
        logo = QLabel()
        pm = QPixmap(os.path.join(PIXMAPS, "auxobsd-logo.png"))      # the logo from auxolinux.com/bsd
        if pm.isNull():
            pm = QPixmap(os.path.join(PIXMAPS, "auxo-logo.png"))
        if not pm.isNull():
            logo.setPixmap(pm.scaled(64, 64, Qt.AspectRatioMode.KeepAspectRatio,
                                     Qt.TransformationMode.SmoothTransformation))
        logo.setAlignment(Qt.AlignmentFlag.AlignCenter)
        sl.addWidget(logo)
        name = QLabel("AuxoBSD")
        name.setAlignment(Qt.AlignmentFlag.AlignCenter)
        name.setStyleSheet("color: #e8e8f0; font-size: 14pt; font-weight: 600; padding-bottom: 18px;")
        sl.addWidget(name)
        self.side_labels = []
        for s in STEPS:
            lab = QLabel(s)
            sl.addWidget(lab)
            self.side_labels.append(lab)
        sl.addStretch()
        root.addWidget(side)

        right = QVBoxLayout()
        right.setContentsMargins(0, 0, 0, 0)
        self.stack = QStackedWidget()
        right.addWidget(self.stack, 1)
        nav = QHBoxLayout()
        nav.setContentsMargins(36, 8, 36, 24)
        self.err = QLabel("")
        self.err.setObjectName("warn")
        self.err.setWordWrap(True)
        nav.addWidget(self.err, 1)
        self.back = QPushButton("Back")
        self.next = QPushButton("Next")
        self.next.setObjectName("primary")
        self.back.clicked.connect(lambda: self.go(self.stack.currentIndex() - 1))
        self.next.clicked.connect(self.on_next)
        nav.addWidget(self.back)
        nav.addWidget(self.next)
        right.addLayout(nav)
        rw = QWidget()
        rw.setLayout(right)
        root.addWidget(rw, 1)

        for build in (self.p_welcome, self.p_keyboard, self.p_timezone, self.p_disk, self.p_graphics,
                      self.p_personalise, self.p_account, self.p_summary, self.p_installing, self.p_done):
            self.stack.addWidget(build())
        self.apply_style()
        self.go(0)

    # ── test data ─────────────────────────────────────────────────────
    @staticmethod
    def _fake_disks():
        return [{"name": "nda0", "bytes": 1000204886016, "descr": "Samsung SSD 990 PRO 1TB"},
                {"name": "ada0", "bytes": 64424509440, "descr": "VBOX HARDDISK"}]

    @staticmethod
    def _fake_gpus():
        if auxo_gpu is None:
            return [], ""
        return auxo_gpu.detect([("8086", "a7a0", "030000", "Raptor Lake-P [Iris Xe Graphics]"),
                                ("10de", "2860", "030200", "AD106M [GeForce RTX 4070 Max-Q / Mobile]")]), ""

    # ── pages ─────────────────────────────────────────────────────────
    def p_welcome(self):
        w, lay = page("Install AuxoBSD",
                      "FreeBSD underneath, KDE Plasma on top, with Auxo's tools, accents and NVIDIA support "
                      "set up for you. This takes about ten minutes.")
        card = QFrame()
        card.setObjectName("card")
        cl = QVBoxLayout(card)
        cl.setContentsMargins(20, 16, 20, 16)
        for line in ("●  ZFS with boot environments: every update can be undone from the boot menu",
                     "●  The right graphics driver for your GPU, including every NVIDIA generation",
                     "●  FreeBSD's pkg for software, plus Void Linux packages through xbps if you want them",
                     "●  Change your accent, shell, drivers and more later with auxo-tweak"):
            l = QLabel(line)
            l.setWordWrap(True)
            l.setStyleSheet("background: transparent;")
            cl.addWidget(l)
        lay.addWidget(card)
        give = QLabel("♥  Auxo is free. If it's useful to you, please give to cancer research instead: "
                      "terryfox.org")
        give.setObjectName("muted")
        give.setWordWrap(True)
        lay.addWidget(give)
        lay.addStretch()
        return w

    def p_keyboard(self):
        w, lay = page("Keyboard", "Pick your layout and try it in the box below.")
        self.kb_list = QListWidget()
        for i, (label, layout, variant, keymap) in enumerate(system.KEYBOARDS):
            it = QListWidgetItem(label)
            it.setData(Qt.ItemDataRole.UserRole, (layout, variant, keymap))
            self.kb_list.addItem(it)
        self.kb_list.setCurrentRow(0)
        self.kb_list.currentItemChanged.connect(self.on_keyboard)
        lay.addWidget(self.kb_list, 1)
        test = QLineEdit()
        test.setPlaceholderText("Type here to test the layout")
        lay.addWidget(test)
        return w

    def p_timezone(self):
        w, lay = page("Time zone", "Where are you? The clock and calendar follow this.")
        row = QHBoxLayout()
        self.tz_region = QComboBox()
        self.tz_region.addItems(list(self.zones))
        self.tz_filter = QLineEdit()
        self.tz_filter.setPlaceholderText("Search cities")
        row.addWidget(self.tz_region)
        row.addWidget(self.tz_filter, 1)
        lay.addLayout(row)
        self.tz_city = QListWidget()
        lay.addWidget(self.tz_city, 1)
        self.tz_region.currentTextChanged.connect(self.fill_cities)
        self.tz_filter.textChanged.connect(lambda _: self.fill_cities(self.tz_region.currentText()))
        cur = "UTC" if FAKE else system.current_timezone()
        region, _, city = cur.partition("/")
        if region in self.zones:
            self.tz_region.setCurrentText(region)
        else:
            self.tz_region.setCurrentText("America" if "America" in self.zones else list(self.zones)[0])
        self.fill_cities(self.tz_region.currentText(), select=city)
        return w

    def p_disk(self):
        w, lay = page("Where to install", "AuxoBSD uses the whole disk you pick.")
        self.disk_list = QListWidget()
        for d in self.disks:
            it = QListWidgetItem(f"{d['name']}    {system.human(d['bytes'])}    {d['descr']}")
            it.setData(Qt.ItemDataRole.UserRole, d)
            self.disk_list.addItem(it)
        if not self.disks:
            self.disk_list.addItem("No disks found. Is the disk controller set to AHCI/NVMe?")
        self.disk_list.currentItemChanged.connect(self.on_disk)
        lay.addWidget(self.disk_list, 1)
        fs = QHBoxLayout()
        self.fs_zfs = QRadioButton("ZFS (recommended): compression, boot environments, auxo-rollback")
        self.fs_ufs = QRadioButton("UFS: simpler, a little lighter on small machines")
        self.fs_zfs.setChecked(True)
        g = QButtonGroup(w)
        g.addButton(self.fs_zfs)
        g.addButton(self.fs_ufs)
        col = QVBoxLayout()
        col.addWidget(self.fs_zfs)
        col.addWidget(self.fs_ufs)
        fs.addLayout(col, 1)
        sw = QFormLayout()
        self.swap = QSpinBox()
        self.swap.setRange(0, 64)
        self.swap.setValue(4)
        self.swap.setSuffix(" GB")
        sw.addRow("Swap", self.swap)
        fs.addLayout(sw)
        lay.addLayout(fs)
        self.disk_warn = QLabel("")
        self.disk_warn.setObjectName("warn")
        self.disk_warn.setWordWrap(True)
        lay.addWidget(self.disk_warn)
        return w

    def p_graphics(self):
        w, lay = page("Graphics", "AuxoBSD found this hardware and picks the driver for it.")
        card = QFrame()
        card.setObjectName("card")
        cl = QVBoxLayout(card)
        cl.setContentsMargins(20, 16, 20, 16)
        if self.gpus and auxo_gpu:
            for g in self.gpus:
                l = QLabel("●  " + auxo_gpu.describe(g))
                l.setWordWrap(True)
                l.setStyleSheet("background: transparent;")
                cl.addWidget(l)
        else:
            cl.addWidget(QLabel("No graphics card detected: AuxoBSD uses a basic framebuffer driver."))
        if self.vm:
            l = QLabel(f"Virtual machine: {self.vm}. Guest tools are installed for it.")
            l.setStyleSheet("background: transparent; color: #8b8ba0;")
            cl.addWidget(l)
        lay.addWidget(card)
        self.gpu_plan = QLabel("")
        self.gpu_plan.setObjectName("muted")
        self.gpu_plan.setWordWrap(True)
        self.nv_group = QButtonGroup(w)
        self.nv_buttons = {}
        has_nv = any(g["kind"] == "nvidia" for g in self.gpus)
        if has_nv:
            hybrid = auxo_gpu.is_hybrid(self.gpus)
            lab = QLabel("NVIDIA")
            lab.setStyleSheet("font-weight: 600; padding-top: 8px;")
            lay.addWidget(lab)
            opts = [("", "Automatic (recommended)")]
            if hybrid:
                opts.append(("prime", "Hybrid: the integrated GPU drives the screen, apps run on NVIDIA with "
                                      "prime-run (laptops)"))
            opts += [("primary", "NVIDIA drives the screen"),
                     ("off", "No NVIDIA driver (basic framebuffer for the NVIDIA GPU)")]
            for key, text in opts:
                b = QRadioButton(text)
                self.nv_group.addButton(b)
                self.nv_buttons[key] = b
                lay.addWidget(b)
                b.toggled.connect(self.update_gpu_plan)
            self.nv_buttons[""].setChecked(True)
        lay.addWidget(self.gpu_plan)
        lay.addStretch()
        self.update_gpu_plan()
        return w

    def p_personalise(self):
        w, lay = page("Make it yours", "Your accent colours the boot menu, login screen, Plasma, Konsole and "
                                       "your prompt. Change it any time with auxo-tweak accent.")
        grid = QGridLayout()
        self.accent_btns = {}
        for i, (name, (a, a2)) in enumerate(ACCENTS.items()):
            b = QPushButton(name)
            b.setCheckable(True)
            b.setMinimumSize(QSize(110, 56))
            b.setStyleSheet(f"QPushButton {{ background: qlineargradient(x1:0,y1:0,x2:1,y2:1, stop:0 {a}, stop:1 {a2});"
                            f" color: #0b0b10; font-weight: 600; border: 3px solid transparent; border-radius: 12px; }}"
                            f" QPushButton:checked {{ border: 3px solid #ffffff; }}")
            b.clicked.connect(lambda _=False, n=name: self.set_accent(n))
            grid.addWidget(b, i // 4, i % 4)
            self.accent_btns[name] = b
        lay.addLayout(grid)
        form = QFormLayout()
        self.shell = QComboBox()
        self.shell.addItems(["zsh", "fish", "bash"])
        form.addRow("Shell", self.shell)
        lay.addLayout(form)
        self.xbps = QCheckBox("Set up xbps: Void Linux packages next to FreeBSD's pkg (needs internet, ~100 MB)")
        lay.addWidget(self.xbps)
        lay.addStretch()
        self.accent_btns["violet"].setChecked(True)
        return w

    def p_account(self):
        w, lay = page("Your account", "You'll use this to log in. It can run admin commands with sudo.")
        form = QFormLayout()
        form.setVerticalSpacing(12)
        self.fullname = QLineEdit()
        self.username = QLineEdit()
        self.hostname = QLineEdit("auxobsd")
        self.pw1 = QLineEdit()
        self.pw2 = QLineEdit()
        for p in (self.pw1, self.pw2):
            p.setEchoMode(QLineEdit.EchoMode.Password)
        self.fullname.textChanged.connect(self.suggest_username)
        self.username.textEdited.connect(lambda _: setattr(self, "_user_edited", True))
        form.addRow("Your name", self.fullname)
        form.addRow("Username", self.username)
        form.addRow("Computer name", self.hostname)
        form.addRow("Password", self.pw1)
        form.addRow("Confirm password", self.pw2)
        lay.addLayout(form)
        lay.addStretch()
        return w

    def p_summary(self):
        w, lay = page("Ready to install", "Nothing has changed yet. Check the summary, then press Install.")
        self.summary = QLabel("")
        self.summary.setWordWrap(True)
        self.summary.setTextFormat(Qt.TextFormat.RichText)
        card = QFrame()
        card.setObjectName("card")
        cl = QVBoxLayout(card)
        cl.setContentsMargins(20, 16, 20, 16)
        self.summary.setStyleSheet("background: transparent;")
        cl.addWidget(self.summary)
        lay.addWidget(card)
        self.cmds_btn = QPushButton("Show the commands")
        self.cmds = QPlainTextEdit()
        self.cmds.setReadOnly(True)
        self.cmds.setFont(QFont("Noto Sans Mono", 9))
        self.cmds.hide()
        self.cmds_btn.clicked.connect(lambda: self.cmds.setVisible(not self.cmds.isVisible()))
        lay.addWidget(self.cmds_btn, 0, Qt.AlignmentFlag.AlignLeft)
        lay.addWidget(self.cmds, 1)
        lay.addStretch()
        return w

    def p_installing(self):
        w, lay = page("Installing AuxoBSD", "You can keep using the live session meanwhile.")
        self.step_label = QLabel("Starting…")
        lay.addWidget(self.step_label)
        self.bar = QProgressBar()
        self.bar.setRange(0, 1000)
        self.bar.setTextVisible(False)
        lay.addWidget(self.bar)
        self.log_box = QPlainTextEdit()
        self.log_box.setReadOnly(True)
        self.log_box.setFont(QFont("Noto Sans Mono", 9))
        lay.addWidget(self.log_box, 1)
        return w

    def p_done(self):
        w, lay = page("AuxoBSD is installed", "Remove the installation media and restart. Log in with your "
                                              "new account; `auxo-tweak` changes anything you picked here.")
        tips = QLabel("Next steps: <b>auxo-update</b> keeps everything current (with a boot environment first), "
                      "<b>auxo-tweak gpu</b> shows your graphics drivers, <b>auxo-tweak xbps on</b> adds Void "
                      "Linux packages.")
        tips.setWordWrap(True)
        tips.setTextFormat(Qt.TextFormat.RichText)
        lay.addWidget(tips)
        lay.addStretch()
        return w

    # ── behaviour ─────────────────────────────────────────────────────
    def apply_style(self):
        self.setStyleSheet(stylesheet(self.cfg.accent))
        pal = self.palette()
        pal.setColor(QPalette.ColorRole.Highlight, QColor(ACCENTS[self.cfg.accent][0]))
        self.setPalette(pal)

    def set_accent(self, name):
        self.cfg.accent = name
        for n, b in self.accent_btns.items():
            b.setChecked(n == name)
        self.apply_style()
        self.mark_sidebar()

    def on_keyboard(self, item):
        if not item:
            return
        layout, variant, keymap = item.data(Qt.ItemDataRole.UserRole)
        self.cfg.keyboard, self.cfg.keyboard_variant, self.cfg.keymap = layout, variant, keymap
        if not FAKE and not self.dry and os.environ.get("DISPLAY"):
            os.system(f"setxkbmap {layout} {variant} >/dev/null 2>&1")

    def fill_cities(self, region, select=None):
        f = self.tz_filter.text().lower()
        self.tz_city.clear()
        for c in self.zones.get(region, []):
            if f in c.lower():
                self.tz_city.addItem(c)
        items = self.tz_city.findItems(select or "", Qt.MatchFlag.MatchExactly) if select else []
        if items:
            self.tz_city.setCurrentItem(items[0])
        elif self.tz_city.count():
            self.tz_city.setCurrentRow(0)

    def on_disk(self, item):
        d = item.data(Qt.ItemDataRole.UserRole) if item else None
        if not d:
            return
        self.cfg.disk, self.cfg.disk_bytes = d["name"], d["bytes"]
        self.disk_warn.setText(f"Everything on {d['name']} ({d['descr'] or 'disk'}) will be erased.")

    def update_gpu_plan(self):
        if auxo_gpu is None:
            return
        mode = next((k for k, b in self.nv_buttons.items() if b.isChecked()), "")
        self.cfg.nvidia_mode = mode
        p = auxo_gpu.plan(self.gpus, mode or None, self.vm)
        lines = []
        if p["pkgs"]:
            lines.append("Packages: " + ", ".join(p["pkgs"]))
        if p["klds"]:
            lines.append("Kernel modules: " + ", ".join(p["klds"]))
        if p["nvidia"]:
            b = auxo_gpu.NVIDIA[p["nvidia"]]
            lines.append(f"NVIDIA {b['label']}" + (": KMS on, Wayland ready" if b["drm"] else ": X11 only"))
        lines += p["notes"]
        self.gpu_plan.setText("\n".join(lines))

    def suggest_username(self, text):
        if getattr(self, "_user_edited", False):
            return
        first = (text.strip().split() or [""])[0].lower()
        self.username.setText(re.sub(r"[^a-z0-9_-]", "", first)[:16])

    def collect(self):
        it = self.tz_city.currentItem()
        self.cfg.timezone = f"{self.tz_region.currentText()}/{it.text()}" if it else "UTC"
        self.cfg.fs = "zfs" if self.fs_zfs.isChecked() else "ufs"
        self.cfg.swap_gb = self.swap.value()
        self.cfg.shell = self.shell.currentText()
        self.cfg.xbps = self.xbps.isChecked()
        self.cfg.fullname = self.fullname.text().strip()
        self.cfg.username = self.username.text().strip()
        self.cfg.hostname = self.hostname.text().strip()
        self.cfg.password = self.pw1.text()

    def page_errors(self, i):
        self.collect()
        name = STEPS[i]
        errs = self.cfg.errors()
        if name == "Disk":
            return [e for e in errs if "disk" in e.lower()]
        if name == "Account":
            e = [x for x in errs if "disk" not in x.lower()]
            if self.pw1.text() != self.pw2.text():
                e.append("The passwords don't match.")
            return e
        return []

    def fill_summary(self):
        self.collect()
        c = self.cfg
        d = next((x for x in self.disks if x["name"] == c.disk), {"descr": "", "bytes": 0})
        a = ACCENTS[c.accent][0]
        gp = auxo_gpu.plan(self.gpus, c.nvidia_mode or None, self.vm) if auxo_gpu else {"nvidia": None, "klds": []}
        gpu = (f"NVIDIA {auxo_gpu.NVIDIA[gp['nvidia']]['label']}, {gp['mode']}" if gp.get("nvidia")
               else ", ".join(gp["klds"]) or "framebuffer")
        rows = [("Disk", f"<b>{c.disk}</b> {system.human(d['bytes'])} {d['descr']} — <span style='color:#fb7185'>"
                         "erased</span>"),
                ("File system", "ZFS with boot environments" if c.fs == "zfs" else "UFS"),
                ("Swap", f"{c.swap_gb} GB"), ("Keyboard", f"{c.keyboard} {c.keyboard_variant}".strip()),
                ("Time zone", c.timezone), ("Graphics", gpu),
                ("Accent", f"<span style='color:{a}'>●</span> {c.accent}"), ("Shell", c.shell),
                ("xbps", "yes" if c.xbps else "no"), ("Account", f"{c.fullname or c.username} ({c.username})"),
                ("Computer name", c.hostname)]
        self.summary.setText("<table cellspacing='6'>" + "".join(
            f"<tr><td style='color:#8b8ba0; padding-right:18px'>{k}</td><td>{v}</td></tr>" for k, v in rows)
            + "</table>")
        self.cmds.setPlainText(plan.describe(plan.build(c)))

    def mark_sidebar(self):
        cur = self.stack.currentIndex()
        for i, lab in enumerate(self.side_labels):
            lab.setProperty("current", i == cur)
            lab.setProperty("done", i < cur)
            lab.style().unpolish(lab)
            lab.style().polish(lab)

    def go(self, i):
        i = max(0, min(i, self.stack.count() - 1))
        self.stack.setCurrentIndex(i)
        name = STEPS[i]
        self.err.setText("")
        self.back.setVisible(name not in ("Welcome", "Installing", "Done"))
        self.next.setVisible(name != "Installing")
        self.next.setText({"Summary": "Install", "Done": "Restart now", "Welcome": "Start"}.get(name, "Next"))
        if name == "Summary":
            self.fill_summary()
        self.mark_sidebar()

    def on_next(self):
        i = self.stack.currentIndex()
        name = STEPS[i]
        if name == "Done":
            if not self.dry:
                os.system("shutdown -r now")
            self.close()
            return
        errs = self.page_errors(i)
        if errs:
            self.err.setText(" ".join(errs))
            return
        if name == "Summary":
            errs = self.cfg.errors()
            if errs:
                self.err.setText(" ".join(errs))
                return
            ok = QMessageBox.question(self, "Erase the disk?",
                                      f"Everything on {self.cfg.disk} will be erased and AuxoBSD installed. Continue?")
            if ok != QMessageBox.StandardButton.Yes:
                return
            self.go(i + 1)
            self.start_install()
            return
        self.go(i + 1)

    def start_install(self):
        self.thread = InstallThread(self.cfg, self.dry)
        self.thread.step.connect(lambda n, t: self.step_label.setText(t))
        self.thread.progress.connect(lambda f: self.bar.setValue(int(f * 1000)))
        self.thread.log.connect(self.log_box.appendPlainText)
        self.thread.finished_ok.connect(lambda: self.go(STEPS.index("Done")))
        self.thread.failed.connect(self.install_failed)
        self.thread.start()

    def install_failed(self, msg):
        self.step_label.setText("The installation stopped.")
        self.err.setText(f"{msg}. The log above has the details; nothing after this step was done.")
        self.back.setVisible(True)


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    dry = "--dry-run" in argv
    shots = argv[argv.index("--screenshots") + 1] if "--screenshots" in argv else None
    if shots:
        os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
    app = QApplication(sys.argv[:1])
    app.setApplicationName("auxo-installer")
    if os.geteuid() != 0 and not dry and not shots:
        QMessageBox.critical(None, "Install AuxoBSD", "The installer needs root. Start it from the desktop icon, "
                                                     "or run: sudo auxo-installer")
        return 1
    w = Installer(dry_run=dry or bool(shots))
    if shots:
        return screenshots(w, shots)
    w.show()
    return app.exec()


def screenshots(w, outdir):
    """Render every page to PNG (used by the tests and for the website)."""
    os.makedirs(outdir, exist_ok=True)
    w.resize(1000, 680)
    w.fullname.setText("Ada Lovelace")
    w.pw1.setText("pw")
    w.pw2.setText("pw")
    w.disk_list.setCurrentRow(1)
    w.set_accent("rose")
    for i, name in enumerate(STEPS):
        if name == "Installing":
            w.bar.setValue(420)
            w.step_label.setText("Copying AuxoBSD to the disk")
            w.log_box.setPlainText("── Copying AuxoBSD to the disk\n  105,500 files\n  106,000 files")
        w.go(i)
        QApplication.processEvents()
        w.grab().save(os.path.join(outdir, f"{i:02d}-{name.lower().replace(' ', '-')}.png"))
    return 0

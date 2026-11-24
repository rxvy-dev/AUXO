#!/usr/bin/env python3
"""Walks through the graphical installer offscreen like a user would, then lets it run a real
dry-run install (the same engine as the text installer, started as a child process).

  python3 tests/test-installer-gui.py            the walkthrough
  python3 tests/test-installer-gui.py --shots D  also save a PNG of every page into D
"""
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
os.environ["AUXO_TEST_DISKS"] = os.path.join(HERE, "tests/fixtures/lsblk.json")
os.environ.setdefault("AUXO_LIB", os.path.join(HERE, "packages/auxo-tools/files/lib"))
sys.path.insert(0, os.path.join(HERE, "installer"))

from PyQt6.QtWidgets import QApplication  # noqa: E402

from auxo_install import disks as disklib  # noqa: E402
from auxo_install.config import InstallConfig  # noqa: E402
from auxo_install.gui import Installer  # noqa: E402
from auxo_install.plan import Env  # noqa: E402

SHOTS = sys.argv[sys.argv.index("--shots") + 1] if "--shots" in sys.argv else None
passed = failed = 0


def check(name, cond):
    global passed, failed
    print(("  \033[32m✓\033[0m " if cond else "  \033[31m✗\033[0m ") + name)
    passed += bool(cond)
    failed += not cond


app = QApplication(sys.argv[:1])
disks = disklib.parse_lsblk(json.load(open(os.environ["AUXO_TEST_DISKS"])))
zones = ["America/Vancouver", "America/New_York", "Europe/London", "Europe/Berlin", "Asia/Tokyo"]


def make(online=True, efi=True):
    w = Installer(InstallConfig(), Env(efi=efi, online=online), dry_run=True, disks=disks, timezones=zones)
    w.resize(1180, 780)
    w.show()
    app.processEvents()
    return w


def shot(w, name):
    app.processEvents()
    if SHOTS:
        os.makedirs(SHOTS, exist_ok=True)
        w.grab().save(os.path.join(SHOTS, f"{name}.png"))


w = make()
shot(w, "01-welcome")
check("starts on Welcome, online", w.index == 0 and w.pages[0].net.text() == "Connected")
w.next.click()
region = w.pages[1]
check("Region page", w.index == 1)
region.region.setCurrentText("America")
region.city.setCurrentIndex(region.city.findData("America/Vancouver"))
check("time zone picked", w.cfg.timezone == "America/Vancouver")
w.touched_timezone = True                     # what picking from the lists does
w.signals.timezone.emit("Asia/Tokyo")         # a late geo-IP guess…
check("…doesn't override your pick", w.cfg.timezone == "America/Vancouver")
region.kb_filter.setText("german")
visible = [region.kb.item(i).data(256) for i in range(region.kb.count()) if not region.kb.item(i).isHidden()]
check("keyboard search", visible == ["de", "ch"] or visible == ["de"])
region.kb.setCurrentRow([region.kb.item(i).data(256) for i in range(region.kb.count())].index("gb"))
check("keyboard picked", w.cfg.keyboard == "gb")
region.lang.setCurrentIndex(region.lang.findData("en_GB.UTF-8"))
check("language picked", w.cfg.locale == "en_GB.UTF-8")
shot(w, "02-region")
w.next.click()

disk = w.pages[2]
check("Disk page: first usable disk preselected", w.index == 2 and w.cfg.disk == "/dev/nvme0n1")
check("USB stick and small disks greyed out", not disk.cards["/dev/sdb"].isEnabled())
check("partition bar shows the old partitions", any(s[2] == "" or s[2] for s in disk.cards["/dev/sda"].bar.segments))
disk.choose("/dev/sda")
check("pick another disk", w.cfg.mode == "erase" and w.cfg.disk == "/dev/sda" and "Auxo" in
      [s[2] for s in disk.cards["/dev/sda"].bar.segments])
disk.choose("manual")
check("existing partitions: box shown", disk.parts_box.isVisibleTo(w) and w.cfg.mode == "manual")
w.next.click()
check("can't go on without a partition", w.index == 2 and "partition" in w.problem.text().lower())
disk.root_part.setCurrentIndex(disk.root_part.findData("/dev/nvme0n1p3"))
check("EFI partition found by itself", w.cfg.efi_part == "/dev/nvme0n1p1" and not disk.format_efi.isChecked())
shot(w, "03-disk-manual")
disk.choose("/dev/nvme0n1")
shot(w, "03-disk")
w.next.click()

desk = w.pages[3]
check("Desktop page", w.index == 3 and not desk.style_box.isVisibleTo(w))
desk.choose("hyprland")
check("Hyprland shows the styles", desk.style_box.isVisibleTo(w))
desk.choose_style("mac")
check("Mac style picked", w.cfg.desktop == "hyprland" and w.cfg.style == "mac")
shot(w, "04-desktop")
w.next.click()

look = w.pages[4]
look.choose("emerald")
check("accent re-themes the installer", w.cfg.accent == "emerald" and "#34d399" in w.styleSheet())
shot(w, "05-look")
w.next.click()
sysp = w.pages[5]
[b for b in sysp.kernel.buttons() if b.property("key") == "linux-lts"][0].click()
[b for b in sysp.shell.buttons() if b.property("key") == "fish"][0].click()
check("kernel + shell", w.cfg.kernel == "linux-lts" and w.cfg.shell == "fish")
shot(w, "06-system")
w.next.click()

acc = w.pages[6]
w.next.click()
check("account needs a name and password", w.index == 6 and w.problem.text())
acc.fullname.setText("")
acc.fullname.textEdited.emit("Alex Smith")
acc.fullname.setText("Alex Smith")
check("username + computer name filled in", acc.username.text() == "alex" and acc.hostname.text() == "alex-pc")
acc.password.setText("correct horse 9")
acc.password2.setText("correct horse")
w.next.click()
check("passwords must match", w.index == 6 and "match" in w.problem.text())
acc.password2.setText("correct horse 9")
check("password strength shown", acc.strength.value() >= 3)
shot(w, "07-account")
w.next.click()

ex = w.pages[7]
check("extras: defaults ticked", w.cfg.extras == ["snapshots", "zram", "drivers", "gaming", "discord"])
ex.boxes["firewall"].setChecked(True)
ex.boxes["gaming"].setChecked(False)
check("extras changed", "firewall" in w.cfg.extras and "gaming" not in w.cfg.extras)
shot(w, "08-extras")
w.next.click()

rev = w.pages[8]
texts = [rev.grid.itemAt(i).widget().text() for i in range(rev.grid.count())]
check("summary: friendly names", "Hyprland" in " ".join(texts) and "Mac style" in " ".join(texts)
      and "English (UK)" in texts)
check("summary: erase warning", "/dev/nvme0n1 will be deleted" in rev.danger_text.text())
check("Install button", w.next.text() == "Install")
shot(w, "09-review")

w.confirm = lambda: False
w.next.click()
check("going back from the confirmation installs nothing", w.index == 8 and w.proc is None)
w.confirm = lambda: True
w.next.click()
check("install page", w.index == 9 and w.proc is not None)
end = time.time() + 120
while w.index != 10 and time.time() < end:
    app.processEvents()
    time.sleep(0.02)
    if w.index == 9 and w.pages[9].bar.value() > 300 and SHOTS and not os.path.exists(os.path.join(SHOTS, "10-install.png")):
        shot(w, "10-install")
inst = w.pages[9]
log = inst.log.toPlainText()
check("dry-run install finished", w.index == 10 and w.pages[10].big.text() == "Auxo is installed")
check("ran as a child with --progress", "--no-tui --progress --dry-run" in log and "<your answers>" in log)
check("every step ticked", all(lab.text().startswith("✓") for lab in inst.step_labels) and inst.bar.value() == 1000)
check("engine did the work", "sgdisk" in log and "grub-install" in log)
# the child checks the network itself: with internet it applies the style, without it installs Plasma
check("Mac style applied with auxo-tweak (or Plasma offline)", "auxo-tweak rice hyprland-mac --user alex" in log
      or any("hyprland needs internet" in n for n in w.result["notes"]))
check("firewall on, no gaming", "auxo-tweak firewall on" in log and "auxo-tweak gaming" not in log)
check("password never shown", "correct horse" not in log)
check("answers file removed", w.cfg_file and not os.path.exists(w.cfg_file))
shot(w, "11-done")
if SHOTS:                                     # the dry run is too quick to catch: show the page mid-way
    w.show_page(9)
    inst.heading_label.setText("Installing Auxo")
    inst.set_step(3)
    inst.set_pct(0.41)
    shot(w, "10-install")
w.close()

# offline + BIOS
w = make(online=False, efi=False)
check("offline welcome says so", "Offline" in w.pages[0].net.text())
w.show_page(2)
check("BIOS: no EFI partition", not w.pages[2].efi_part.isVisibleTo(w))
w.show_page(7)
check("offline: internet extras marked", "(needs internet)" in w.pages[7].boxes["gaming"].text())
w.close()

# a failing install shows the error page
w = make()
w.cfg.__dict__.update(disk="/dev/sda", username="alex", password="pw", fullname="Alex")
w.installer = os.path.join(HERE, "tests/fixtures/does-not-exist")
w.start_install()
end = time.time() + 20
while w.index != 10 and time.time() < end:
    app.processEvents()
    time.sleep(0.02)
check("a failed install says so", w.index == 10 and w.pages[10].big.text() == "The install stopped"
      and not w.pages[10].reboot.isVisible())
w.close()

print(f"\n{passed} passed, {failed} failed")
sys.exit(1 if failed else 0)

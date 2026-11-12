#!/usr/bin/env python3
"""Hardware support on a PC like the ones people have: no PS/2 controller, keyboard and mouse
on USB (one keyboard behind a hub), ACPI for power. Boots the ISO in QEMU (q35, i8042 off,
xHCI) and checks:

  - the xHCI driver finds the keyboards, the mouse and the hub (lsusb);
  - typing on the USB keyboard works, Shift included (keys go through QEMU's USB keyboard);
  - unplugging a USB device is noticed;
  - ACPI power-off really switches the machine off.

  python3 tests/test-hw.py build/auxobsd.iso
"""
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

ANSI = re.compile(rb"\x1b\[[0-9;]*[A-Za-z]")
PROMPT = re.compile(rb"\xe2\x9d\xaf $")
passed = failed = 0


def check(ok, what, detail=""):
    global passed, failed
    if ok:
        passed += 1
        print(f"  \x1b[32m✓\x1b[0m {what}", flush=True)
    else:
        failed += 1
        print(f"  \x1b[31m✗\x1b[0m {what}", flush=True)
        if detail:
            print("      " + detail.strip().replace("\n", "\n      ")[-1500:], flush=True)


class Machine:
    def __init__(self, tmp, iso):
        self.ser, self.mon = os.path.join(tmp, "ttyS0"), os.path.join(tmp, "mon")
        qemu = os.environ.get("QEMU", "qemu-system-x86_64")
        args = [qemu, "-machine", "q35,i8042=off", "-m", "1G", "-display", "none", "-vga", "std", "-no-reboot",
                "-cdrom", iso,
                "-device", "qemu-xhci,id=xhci",
                "-device", "usb-kbd,bus=xhci.0,port=1,id=kbd1",
                "-device", "usb-mouse,bus=xhci.0,port=2",
                "-device", "usb-hub,bus=xhci.0,port=3,id=hub",
                "-device", "usb-kbd,bus=xhci.0,port=3.2,id=kbd2",
                "-chardev", f"socket,id=s0,path={self.ser},server=on,wait=on", "-serial", "chardev:s0",
                "-monitor", f"unix:{self.mon},server=on,wait=off"]
        self.p = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=open(os.path.join(tmp, "qemu.log"), "wb"),
                                  stderr=subprocess.STDOUT)
        self.buf = b""
        self.s = None
        for _ in range(400):
            if self.p.poll() is not None:
                break
            try:
                c = socket.socket(socket.AF_UNIX)
                c.connect(self.ser)
                self.s = c
                break
            except OSError:
                time.sleep(0.05)
        if self.s:
            self.s.settimeout(0.3)

    def text(self, start=0):
        return ANSI.sub(b"", self.buf[start:]).decode("utf-8", "replace")

    def pump(self, timeout, until=None):
        end = time.time() + timeout
        while time.time() < end and self.s:
            try:
                d = self.s.recv(65536)
                if not d:
                    return False
                self.buf += d
            except socket.timeout:
                pass
            if until is not None and until.search(ANSI.sub(b"", self.buf[-400:])):
                return True
        return False

    def run(self, cmd, timeout=60):
        start = len(self.buf)
        self.s.sendall(cmd.encode() + b"\r")
        self.pump(timeout, PROMPT)
        return self.text(start)

    def monitor(self, *cmds):
        m = socket.socket(socket.AF_UNIX)
        m.connect(self.mon)
        time.sleep(0.2)
        for c in cmds:
            m.sendall(c.encode() + b"\n")
            time.sleep(0.12)
        time.sleep(0.3)
        m.close()

    def close(self):
        if self.s:
            self.s.close()
        if self.p.poll() is None:
            self.p.kill()
        self.p.wait()


ROW = re.compile(r"^(\S+)\s+[0-9a-f]{4}:[0-9a-f]{4}\s+\S+\s+(\S+)", re.M)


def rows(out, kind):
    """lsusb rows of one kind (announcements printed meanwhile don't count)"""
    return [m.group(1) for m in ROW.finditer(out) if kind in m.group(2)]


def main():
    iso = sys.argv[1] if len(sys.argv) > 1 else "build/auxobsd.iso"
    tmp = tempfile.mkdtemp(prefix="auxobsd-hw-")
    m = Machine(tmp, iso)
    try:
        print("▲ a PC with USB input only", flush=True)
        up = m.pump(240, PROMPT)
        check(up, "boots to auxsh", m.text()[-1500:])
        if not up:
            return
        boot = m.text()
        check("no PS/2 controller" in boot, "notices there's no PS/2 controller", boot[-1500:])
        check("xHCI controller" in boot, "starts the xHCI controller", boot[-1500:])
        check("acpi: ACPI" in boot and "power off ready" in boot, "reads the ACPI tables (power off ready)", boot[-1500:])
        m.run("")
        out = m.run("lsusb")
        check(len(rows(out, "keyboard")) == 2, "lsusb: both keyboards (one behind the hub)", out)
        check(rows(out, "mouse") and rows(out, "hub"), "lsusb: the mouse and the hub", out)
        check(any(re.fullmatch(r"1-\d+\.2", r) for r in rows(out, "keyboard")), "lsusb: the keyboard behind the hub (1-N.2)", out)

        print("▲ typing on the USB keyboard", flush=True)
        keys = "t o u c h spc slash t m p slash shift-u shift-s shift-b o k ret".split()
        m.monitor(*[f"sendkey {k}" for k in keys])
        time.sleep(2)
        out = m.run("ls /tmp")
        check("USBok" in out, "a command typed on the USB keyboard ran (with Shift)", out)

        print("▲ hot-plug", flush=True)
        m.monitor("device_del kbd2")
        time.sleep(3)
        out = m.run("lsusb")
        check(len(rows(out, "keyboard")) == 1, "an unplugged keyboard goes away", out)
        m.monitor("device_add usb-kbd,bus=xhci.0,port=3.4,id=kbd3")
        time.sleep(3)
        out = m.run("lsusb")
        check(any(re.fullmatch(r"1-\d+\.4", r) for r in rows(out, "keyboard")), "a keyboard plugged in later shows up", out)

        print("▲ ACPI power off", flush=True)
        m.s.sendall(b"poweroff\r")
        for _ in range(60):
            if m.p.poll() is not None:
                break
            time.sleep(0.5)
        check(m.p.poll() is not None, "poweroff switches the machine off")
    finally:
        m.close()
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
    print(f"\n{passed} passed, {failed} failed")
    sys.exit(1 if failed else 0)

#!/usr/bin/env python3
"""Hardware support on a PC like the ones people have: no PS/2 controller, keyboard and mouse
on USB (one keyboard behind a hub), ACPI for power. Boots the ISO in QEMU (q35, i8042 off,
xHCI) and checks:

  - the xHCI driver finds the keyboards, the mouse and the hub (lsusb);
  - typing on the USB keyboard works, Shift included (keys go through QEMU's USB keyboard);
  - unplugging a USB device is noticed;
  - ACPI power-off really switches the machine off;
  - then a second PC with an Intel I210-class card (QEMU's igb, an 82576), a USB stick and a
    USB tablet: DHCP on igb, a 3 MB download that checksums right, the stick as a disk usb0
    (write + read back), the tablet as an absolute pointer;
  - and a third with no network card at all, only a phone tethered over USB (RNDIS): DHCP
    and the same download over it.

  python3 tests/test-hw.py build/auxobsd.iso
"""
import os
import hashlib
import http.server
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
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
    def __init__(self, tmp, iso, devices=None, name="a"):
        self.ser, self.mon = os.path.join(tmp, name + "-ttyS0"), os.path.join(tmp, name + "-mon")
        qemu = os.environ.get("QEMU", "qemu-system-x86_64")
        if devices is None:
            devices = ["-device", "qemu-xhci,id=xhci",
                       "-device", "usb-kbd,bus=xhci.0,port=1,id=kbd1",
                       "-device", "usb-mouse,bus=xhci.0,port=2",
                       "-device", "usb-hub,bus=xhci.0,port=3,id=hub",
                       "-device", "usb-kbd,bus=xhci.0,port=3.2,id=kbd2"]
        args = [qemu, "-machine", "q35,i8042=off", "-m", "1G", "-display", "none", "-vga", "std", "-no-reboot",
                "-cdrom", iso] + devices + [
                "-chardev", f"socket,id=s0,path={self.ser},server=on,wait=on", "-serial", "chardev:s0",
                "-monitor", f"unix:{self.mon},server=on,wait=off"]
        self.p = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=open(os.path.join(tmp, name + "-qemu.log"), "wb"),
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


class Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *a):
        pass


def serve(directory):
    """a web server on the host; the guest reaches it as 10.0.2.2:PORT through QEMU's user net"""
    h = lambda *a, **k: Quiet(*a, directory=directory, **k)
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), h)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv.server_address[1]


def download(m, port, want, nic):
    """BusyBox wget and sha256sum: in the small root image CI builds as well as the full one"""
    out = m.run(f"wget -q -O /tmp/big.bin http://10.0.2.2:{port}/big.bin", timeout=180)
    out += m.run("sha256sum /tmp/big.bin", timeout=60)
    m.run("rm /tmp/big.bin")
    check(f"{want}  /tmp/big.bin" in out, f"a 3 MB download over {nic} arrives intact", out)


def nic_pc(tmp, iso, port, want):
    disk = os.path.join(tmp, "stick.img")
    with open(disk, "wb") as f:
        f.truncate(64 << 20)
    m = Machine(tmp, iso, ["-nic", "user,model=igb",
                           "-device", "qemu-xhci,id=xhci",
                           "-drive", f"if=none,id=stick,file={disk},format=raw",
                           "-device", "usb-storage,bus=xhci.0,drive=stick",
                           "-device", "usb-tablet,bus=xhci.0"], "b")
    try:
        print("▲ a PC with an Intel I210-class card, a USB stick and a tablet", flush=True)
        up = m.pump(240, PROMPT)
        check(up, "boots to auxsh", m.text()[-1500:])
        if not up:
            return
        m.run("")
        out = ""
        for _ in range(20):
            out = m.run("ifconfig")
            if "inet 10.0.2.15" in out:
                break
            time.sleep(1)
        check(re.search(r"^igb: .*link up", out, re.M) and "inet 10.0.2.15" in out, "igb: link up, DHCP address", out)
        download(m, port, want, "igb")
        out = m.run("lsusb")
        check(rows(out, "storage") and rows(out, "pointer"), "lsusb: the USB stick and the tablet", out)
        out = m.run("lsblk")
        check(re.search(r"^usb0\s+USB\s+64 MB", out, re.M), "the stick is a disk, usb0", out)
        out = m.run("sudo disktest usb0", timeout=60)
        check("usb0: write ok, read ok, data matches" in out, "writing to the stick and reading it back", out)
    finally:
        m.close()


def tether_pc(tmp, iso, port, want):
    m = Machine(tmp, iso, ["-nic", "none", "-device", "qemu-xhci,id=xhci",
                           "-netdev", "user,id=phone", "-device", "usb-net,bus=xhci.0,netdev=phone"], "c")
    try:
        print("▲ a PC with no network card, a phone tethered over USB", flush=True)
        up = m.pump(240, PROMPT)
        check(up, "boots to auxsh", m.text()[-1500:])
        if not up:
            return
        m.run("")
        out = ""
        for _ in range(30):
            out = m.run("ifconfig")
            if "inet 10.0.2.15" in out:
                break
            time.sleep(1)
        check(re.search(r"^rndis: .*link up", out, re.M) and "inet 10.0.2.15" in out, "USB tethering (RNDIS): DHCP address", out)
        download(m, port, want, "USB tethering")
    finally:
        m.close()


def main():
    iso = sys.argv[1] if len(sys.argv) > 1 else "build/auxobsd.iso"
    tmp = tempfile.mkdtemp(prefix="auxobsd-hw-")
    www = os.path.join(tmp, "www")
    os.mkdir(www)
    blob = os.urandom(3_000_000)
    with open(os.path.join(www, "big.bin"), "wb") as f:
        f.write(blob)
    port, want = serve(www), hashlib.sha256(blob).hexdigest()
    try:
        usb_pc(tmp, iso)
        nic_pc(tmp, iso, port, want)
        tether_pc(tmp, iso, port, want)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def usb_pc(tmp, iso):
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


if __name__ == "__main__":
    main()
    print(f"\n{passed} passed, {failed} failed")
    sys.exit(1 if failed else 0)

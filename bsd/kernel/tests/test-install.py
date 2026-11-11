#!/usr/bin/env python3
"""Install AuxoBSD onto a blank disk and boot the result, the way a user would:

  1. boot the ISO with an empty disk attached, leave a file in /home/root, run `install`;
  2. boot the disk alone with BIOS firmware (IDE): GRUB from the MBR, the hostname we chose,
     the file from step 1, then write a second file and `sync`;
  3. boot the same disk with UEFI firmware (OVMF, the disk on AHCI/SATA), if OVMF is installed:
     the second file is there.

  python3 tests/test-install.py build/auxobsd.iso
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
PROMPT = re.compile(rb"\xe2\x9d\xaf $")          # "❯ " at the end of the screen, colours stripped
OVMF_CODE = "/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS = "/usr/share/OVMF/OVMF_VARS_4M.fd"
HOST = "ci-box"
MARK = "made-before-install"

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
    def __init__(self, tmp, qemu_args):
        sock = os.path.join(tmp, "ttyS0")
        if os.path.exists(sock):
            os.remove(sock)
        qemu = os.environ.get("QEMU", "qemu-system-x86_64")
        args = [qemu, "-display", "none", "-m", "2G", "-no-reboot", "-vga", "std",
                "-chardev", f"socket,id=s0,path={sock},server=on,wait=on", "-serial", "chardev:s0",
                "-monitor", "none"] + qemu_args
        if os.environ.get("AUXO_KVM") == "1" and os.access("/dev/kvm", os.W_OK):
            args += ["-enable-kvm", "-cpu", "host"]
        self.p = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=open(os.path.join(tmp, "qemu-out.log"), "ab"),
                                  stderr=subprocess.STDOUT)
        self.buf = b""
        self.s = None
        for _ in range(400):
            if self.p.poll() is not None:
                break
            try:
                c = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                c.connect(sock)
                self.s = c
                break
            except OSError:
                time.sleep(0.05)
        if self.s:
            self.s.settimeout(0.3)

    def text(self, start=0):
        return ANSI.sub(b"", self.buf[start:]).decode("utf-8", "replace")

    def wait_prompt(self, timeout):
        end = time.time() + timeout
        while time.time() < end and self.s:
            try:
                d = self.s.recv(65536)
                if not d:
                    return False
                self.buf += d
            except socket.timeout:
                pass
            if PROMPT.search(ANSI.sub(b"", self.buf[-400:])):
                return True
        return False

    def run(self, cmd, timeout=60):
        start = len(self.buf)
        self.s.sendall(cmd.encode() + b"\r")
        self.wait_prompt(timeout)
        return self.text(start)

    def close(self):
        if self.s:
            self.s.close()
        self.p.kill()
        self.p.wait()


def boot(tmp, args, label):
    m = Machine(tmp, args)
    up = m.wait_prompt(float(os.environ.get("BOOT_TIMEOUT", "240")))
    check(up, f"{label}: boots to auxsh", m.text()[-1500:])
    if up:
        m.run("", 10)          # settle the line: the first byte typed right at the prompt can be lost
    return m, up


def main():
    iso = sys.argv[1] if len(sys.argv) > 1 else "build/auxobsd.iso"
    if not os.path.exists(iso):
        sys.exit(f"no ISO at {iso} (make iso)")
    tmp = tempfile.mkdtemp(prefix="auxobsd-install-")
    disk = os.path.join(tmp, "disk.img")
    with open(disk, "wb") as f:
        f.truncate(2 << 30)
    try:
        print("▲ install from the ISO", flush=True)
        m, up = boot(tmp, ["-cdrom", iso, "-drive", f"file={disk},format=raw,if=ide,index=0"], "ISO")
        if not up:
            m.close()
            return
        out = m.run("lsblk")
        check("sda" in out and "QEMU HARDDISK" in out, "lsblk lists the blank disk", out)
        m.run(f"busybox sh -c 'echo {MARK} > /home/root/note.txt'")
        out = m.run(f"install sda {HOST} --yes", 900)
        check("is installed on sda" in out, f"install sda {HOST} --yes finishes", out)
        m.close()
        with open(disk, "rb") as f:
            mbr = f.read(512)
            f.seek(34816 * 512)
            tar = f.read(512)
        check(mbr[510:512] == b"\x55\xaa" and mbr[446 + 16 + 4] == 0x83, "the disk has an MBR (boot signature, partitions)")
        check(tar.startswith(b"boot/") and tar[257:262] == b"ustar", "/boot is on partition 2")

        print("▲ boot the installed disk (BIOS, IDE)", flush=True)
        m, up = boot(tmp, ["-drive", f"file={disk},format=raw,if=ide,index=0"], "BIOS")
        if up:
            check(f"hostname {HOST}" in m.text() and f"root@{HOST}" in m.text(), f"the hostname is {HOST}", m.text()[-600:])
            check("/home/root is kept on sda" in m.text(), "/home/root is kept on the disk", m.text()[-600:])
            out = m.run("cat note.txt")
            check(MARK in out, "a file made before installing is still there", out)
            m.run("busybox sh -c 'echo second-boot > /home/root/two.txt'")
            out = m.run("sync")
            check("saved /home/root to sda" in out, "sync saves /home/root", out)
            out = m.run("gcc --version" if os.path.exists("build/rootfs.tar") else "uname -a")
            check("gcc" in out or "AuxoBSD" in out, "programs from the installed root run", out)
        m.close()

        if os.path.exists(OVMF_CODE) and os.path.exists(OVMF_VARS):
            print("▲ boot the installed disk (UEFI, SATA)", flush=True)
            vars_ = os.path.join(tmp, "vars.fd")
            shutil.copy(OVMF_VARS, vars_)
            m, up = boot(tmp, ["-machine", "q35",
                               "-drive", f"if=pflash,format=raw,readonly=on,file={OVMF_CODE}",
                               "-drive", f"if=pflash,format=raw,file={vars_}",
                               "-drive", f"file={disk},format=raw,if=none,id=d0",
                               "-device", "ahci,id=ahci", "-device", "ide-hd,drive=d0,bus=ahci.0"], "UEFI")
            if up:
                out = m.run("cat two.txt")
                check("second-boot" in out, "a file saved on the last boot is still there", out)
                out = m.run("lsblk")
                check("SATA" in out, "the disk is on AHCI", out)
            m.close()
        else:
            print("  (OVMF isn't installed: skipping the UEFI boot)")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
    print(f"\n{passed} passed, {failed} failed")
    sys.exit(1 if failed else 0)

#!/usr/bin/env python3
"""Install AuxoBSD onto a blank disk and boot the result, the way a user would:

  1. boot the ISO with an empty disk attached, leave a file in the live user's home, run
     `sudo install` with an account (user name and password) for the installed system;
  2. boot the disk alone with BIOS firmware (IDE): GRUB from the MBR, the hostname we chose,
     the account (the desktop starts locked; sudo wants the password), the file from step 1
     (now the new user's), then write a second file and `sync`;
     then pull the plug (kill QEMU) and boot again: what was saved survives, intact;
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
USER, PASSWORD = "ada", "Tr0ub4dor&3"

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

    def run_until(self, cmd, pattern, timeout=30):
        """send cmd, wait for pattern (bytes) in the output: for prompts that aren't the shell's"""
        start = len(self.buf)
        self.s.sendall(cmd.encode() + b"\r")
        end = time.time() + timeout
        while time.time() < end and pattern not in ANSI.sub(b"", self.buf[start:]):
            try:
                d = self.s.recv(65536)
                if not d:
                    break
                self.buf += d
            except socket.timeout:
                pass
        return pattern in ANSI.sub(b"", self.buf[start:])

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
        m.run(f"busybox sh -c 'echo {MARK} > $HOME/note.txt'")
        m.run("busybox sh -c 'mkdir -p $HOME/.config/browser && printf \"active\\t0\\ntab\\thttps://example.org/\\tExample\\n\" > $HOME/.config/browser/session'")
        out = m.run(f"install sda {HOST} --yes")
        check("needs root" in out and "installed" not in out, "install without sudo is refused", out)
        out = m.run(f"sudo install sda {HOST} --user {USER} --password {PASSWORD} --yes", 900)
        check("is installed on sda" in out, f"sudo install sda {HOST} --user {USER} ... finishes", out)
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
            check(f"hostname {HOST}" in m.text() and f"{USER}@{HOST}" in m.text(), f"the hostname is {HOST}, the user {USER}", m.text()[-600:])
            check(f"user: {USER} (uid 1000), home /home/{USER}" in m.text() and "no password" not in m.text(), "the account has a password (the desktop starts locked)", m.text()[-900:])
            check(re.search(r"root: AuxoFS on sda, \d+ files and folders", m.text()) is not None, "the root filesystem is AuxoFS on the disk (nothing loaded into memory first)", m.text()[-900:])
            out = m.run("cat note.txt")
            check(MARK in out, "a file made before installing is still there", out)
            out = m.run("busybox stat -c '%U %G %a' note.txt .config/browser/session")
            check(out.count(f"{USER} {USER}") == 2, f"and it's {USER}'s", out)
            out = m.run("cat .config/browser/session")
            check("https://example.org/" in out, "the browser's saved tabs are still there (hidden folders kept)", out)
            check(m.run_until("sudo busybox id -u", b"password for " + USER.encode()), "sudo asks for the password")
            out = m.run(PASSWORD)
            check(re.search(r"^0\s*$", out, re.M) is not None, "and with it, runs as root", out)
            out = m.run("busybox sh -c 'id -un; cat /etc/shadow'")
            check(USER in out and "Permission denied" in out, "the user can't read /etc/shadow", out)
            m.run("busybox sh -c 'echo second-boot > $HOME/two.txt'")
            out = m.run("sync")
            check("saved your files" in out, "sync saves them", out)
            out = m.run("gcc --version" if os.path.exists("build/rootfs.tar") else "uname -a")
            check("gcc" in out or "AuxoBSD" in out, "programs from the installed root run", out)
            # a crash: what was saved (every 5 s) survives, the tree is never half written
            m.run("busybox sh -c 'echo before-crash > $HOME/crash.txt; rm $HOME/two.txt; echo second-boot > $HOME/three.txt; busybox dd if=/dev/urandom of=$HOME/blob bs=1M count=8 2>/dev/null; busybox md5sum $HOME/blob > $HOME/blob.md5'", 60)
            time.sleep(8)
            m.run("busybox sh -c 'echo lost > $HOME/lost.txt'")
        m.p.kill()                                                   # pull the plug
        m.close()

        print("▲ boot again after pulling the plug", flush=True)
        m, up = boot(tmp, ["-drive", f"file={disk},format=raw,if=ide,index=0"], "after the crash")
        if up:
            out = m.run("busybox sh -c 'cat crash.txt; ls two.txt; busybox md5sum -c blob.md5'")
            check("before-crash" in out and "No such file" in out and "blob: OK" in out,
                  "what was saved before the crash is there, intact (and the deleted file stays deleted)", out)
            out = m.run("busybox sh -c 'ls lost.txt'")
            check("No such file" in out, "the change made just before it is gone, as it should be", out)
            m.run("sync")
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
                out = m.run("cat three.txt")
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

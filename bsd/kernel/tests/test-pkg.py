#!/usr/bin/env python3
"""pkg, the package manager, end to end: real Ubuntu 24.04 packages, installed on AuxoBSD and
kept on an installed system's disk.

A small mirror runs on the host (the guest reaches it as 10.0.2.2): its package lists are the
real ones from archive.ubuntu.com cut down to the packages this test uses, and its .deb files
are the real ones, fetched from archive.ubuntu.com (with curl, so a proxy is honoured) and
cached in build/test-mirror. Then:

  1. from the ISO: pkg update, search, install jq (it needs two libraries), run it; install
     AuxoBSD onto a blank disk;
  2. the installed system: jq came along; pkg install tree; sync; pkg remove jq; sync;
  3. after a restart: tree is still there, jq is gone.

  python3 tests/test-pkg.py build/auxobsd.iso
"""
import gzip
import http.server
import os
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
ARCHIVE = "https://archive.ubuntu.com/ubuntu"
LISTS = [("noble", "main"), ("noble-updates", "main"), ("noble-updates", "universe")]
WANT = {"jq", "libjq1", "libonig5", "tree", "libc6"}
CACHE = os.path.abspath("build/test-mirror")
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
            print("      " + str(detail).strip().replace("\n", "\n      ")[-1800:], flush=True)


def curl(url, dest):
    tmp = dest + ".part"
    r = subprocess.run(["curl", "-sSfL", "--retry", "3", "--max-time", "300", "-o", tmp, url])
    if r.returncode == 0:
        os.replace(tmp, dest)
    return r.returncode == 0


def build_lists():
    """the real package lists, cut down to WANT"""
    for suite, comp in LISTS:
        d = os.path.join(CACHE, "ubuntu", "dists", suite, comp, "binary-amd64")
        os.makedirs(d, exist_ok=True)
        full = os.path.join(CACHE, f"full-{suite}-{comp}.gz")
        if not os.path.exists(full) and not curl(f"{ARCHIVE}/dists/{suite}/{comp}/binary-amd64/Packages.gz", full):
            sys.exit(f"can't download the {suite}/{comp} list from {ARCHIVE}")
        keep = []
        for stanza in gzip.decompress(open(full, "rb").read()).decode("utf-8", "replace").split("\n\n"):
            m = re.match(r"Package: (\S+)", stanza)
            if m and m.group(1) in WANT:
                keep.append(stanza.strip() + "\n")
        with gzip.open(os.path.join(d, "Packages.gz"), "wb") as f:
            f.write("\n".join(keep).encode())


class Mirror(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **k):
        super().__init__(*a, directory=CACHE, **k)

    def log_message(self, *a):
        pass

    def do_GET(self):
        p = os.path.join(CACHE, self.path.lstrip("/").split("?")[0])
        if "/pool/" in self.path and not os.path.exists(p):
            os.makedirs(os.path.dirname(p), exist_ok=True)
            curl(ARCHIVE + self.path[len("/ubuntu"):], p)
        return super().do_GET()


class Machine:
    def __init__(self, tmp, args):
        self.ser = os.path.join(tmp, "ttyS0")
        if os.path.exists(self.ser):
            os.remove(self.ser)
        cmd = [os.environ.get("QEMU", "qemu-system-x86_64"), "-m", "2G", "-display", "none", "-vga", "std", "-no-reboot",
               "-nic", "user,model=e1000", "-chardev", f"socket,id=s0,path={self.ser},server=on,wait=on",
               "-serial", "chardev:s0", "-monitor", "none"] + args
        self.p = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=open(os.path.join(tmp, "qemu.log"), "ab"), stderr=subprocess.STDOUT)
        self.buf, self.s = b"", None
        for _ in range(400):
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

    def pump(self, timeout):
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

    def run(self, cmd, timeout=120):
        start = len(self.buf)
        self.s.sendall(cmd.encode() + b"\r")
        self.pump(timeout)
        return self.text(start)

    def close(self):
        if self.s:
            self.s.close()
        self.p.kill()
        self.p.wait()


def boot(tmp, args, label):
    m = Machine(tmp, args)
    up = m.pump(float(os.environ.get("BOOT_TIMEOUT", "300")))
    check(up, f"{label}: boots to auxsh", m.text()[-1500:])
    if up:
        m.run("", 10)
    return m, up


def main():
    iso = sys.argv[1] if len(sys.argv) > 1 else "build/auxobsd.iso"
    build_lists()
    srv = http.server.ThreadingHTTPServer(("0.0.0.0", 0), Mirror)
    port = srv.server_address[1]
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    sources = "\\n".join(f"http://10.0.2.2:{port}/ubuntu {s} {c}" for s, c in LISTS)
    tmp = tempfile.mkdtemp(prefix="auxobsd-pkg-")
    disk = os.path.join(tmp, "disk.img")
    with open(disk, "wb") as f:
        f.truncate(2 << 30)
    drive = ["-drive", f"file={disk},format=raw,if=ide,index=0"]
    try:
        print("▲ pkg on the live system", flush=True)
        m, up = boot(tmp, ["-cdrom", iso] + drive, "ISO")
        if not up:
            return
        out = m.run("pkg update")
        check("needs root" in out and "sudo pkg update" in out, "pkg update without sudo says it needs root", out)
        m.run(f"sudo busybox sh -c \"printf '{sources}\\n' > /etc/pkg/sources\"")
        out = m.run("sudo pkg update", 300)
        check(re.search(r"\b[4-9] packages available", out) is not None, "pkg update reads the lists", out)
        out = m.run("pkg search json")
        check("jq" in out and "JSON" in out, "pkg search finds jq", out)
        out = m.run("sudo pkg install -y jq", 600)
        check("To install (3): jq libjq1 libonig5" in out.replace("\n", " ") or all(w in out for w in ("jq", "libjq1", "libonig5")),
              "pkg install resolves jq's libraries (and leaves libc6 to the base system)", out)
        check("Done." in out and "libc6" not in out.split("Done.")[0].split("To install")[-1], "…downloads, checks and unpacks them", out)
        out = m.run("busybox sh -c 'echo {\\\"a\\\":[1,2,3]} | jq .a[2]'")
        check(re.search(r"^3\s*$", out, re.M) is not None, "jq runs", out)
        out = m.run("sudo install sda pkgtest --yes", 900)
        check("is installed on sda" in out, "install AuxoBSD onto the disk", out)
        m.close()

        print("▲ the installed system", flush=True)
        m, up = boot(tmp, drive, "disk")
        if not up:
            return
        check("root: AuxoFS on sda" in m.text(), "the installed root filesystem is on the disk", m.text()[-800:])
        out = m.run("busybox sh -c 'echo [5,6] | jq add'")
        check(re.search(r"^11\s*$", out, re.M) is not None, "jq, installed from the live system, came along", out)
        out = m.run("sudo pkg install -y tree", 600)
        check("Done." in out, "pkg install tree", out)
        out = m.run("sync", 300)
        out = m.run("sudo pkg remove jq")
        check("Removed jq" in out, "pkg remove jq", out)
        m.run("sync", 300)
        m.close()

        print("▲ after a restart", flush=True)
        m, up = boot(tmp, drive, "disk again")
        if not up:
            return
        out = m.run("tree -L 1 /etc/pkg")
        check(re.search(r"(└──|`--) sources", out) is not None, "tree, installed on the installed system, is still there", out)   # ASCII without a UTF-8 locale
        out = m.run("pkg list")
        check("tree" in out and not re.search(r"^jq\s", out, re.M), "pkg list: tree yes, jq no", out)
        out = m.run("busybox sh -c 'command -v jq || echo no-jq'")
        check("no-jq" in out, "jq's files are gone", out)
        m.close()
    finally:
        srv.shutdown()
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
    print(f"\n{passed} passed, {failed} failed")
    sys.exit(1 if failed else 0)

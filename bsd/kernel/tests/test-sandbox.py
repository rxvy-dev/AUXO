#!/usr/bin/env python3
"""The sandbox: mount, PID, network, UTS and IPC namespaces, seccomp and /proc, as bubblewrap
(and through it WebKit's and Flatpak's sandboxes) uses them. Boots the ISO in QEMU, builds
tests/sandbox-escape.c with the GCC inside AuxoBSD, and runs it:
  - in bubblewrap with every namespace, as root: read-only binds stay read-only, files that
    weren't bound in can't be reached (not through "..", not by climbing out of a chroot), it
    has its own PIDs, hostname and network (no way out), and what it mounts stays inside;
  - the same with WebKit's seccomp filter: mount, unshare, chroot, pivot_root are refused;
  - seccomp on its own: errno and kill answers, stacking, inheritance across fork and exec.
Then checks the host saw none of it (hostname, files) and that /proc tells the truth.
  python3 tests/test-sandbox.py build/auxobsd.iso
"""
import importlib.util
import os
import re
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("hw", os.path.join(HERE, "test-hw.py"))
hw = importlib.util.module_from_spec(spec)
spec.loader.exec_module(hw)

RUN = """cd /tmp
mkdir -p /tmp/esc
wget -q -O /tmp/esc.c http://10.0.2.2:{port}/sandbox-escape.c || exit 1
gcc -O1 -o /tmp/esc/escape /tmp/esc.c || exit 1
/tmp/esc/escape mkfilter > /tmp/filter.bpf
B="bwrap --ro-bind /usr /usr --symlink usr/lib /lib --symlink usr/lib64 /lib64 --symlink usr/bin /bin --proc /proc --dev /dev --tmpfs /tmp --dir /mnt --ro-bind /tmp/esc /esc --unshare-all --hostname sbx"
echo "== plain"; $B /esc/escape inside {port}
echo "== filtered"; $B --seccomp 9 /esc/escape inside {port} filtered 9< /tmp/filter.bpf
echo "== seccomp"; /tmp/esc/escape seccomp
echo "== host hostname: $(hostname)"
"""


def main():
    iso = sys.argv[1] if len(sys.argv) > 1 else "build/auxobsd.iso"
    tmp = tempfile.mkdtemp(prefix="auxobsd-sandbox-")
    www = os.path.join(tmp, "www")
    os.mkdir(www)
    shutil.copy(os.path.join(HERE, "sandbox-escape.c"), www)
    port = hw.serve(www)
    with open(os.path.join(www, "run.sh"), "w") as f:
        f.write(RUN.format(port=port))
    m = hw.Machine(tmp, iso, ["-nic", "user,model=e1000"], "s")
    try:
        print("▲ boot", flush=True)
        up = m.pump(240, hw.PROMPT)
        hw.check(up, "boots to auxsh", m.text()[-1500:])
        if not up:
            return
        m.run("")
        m.run("busybox sh -c 'echo topsecret > /home/root/secret.txt'")
        m.run(f"busybox wget -q -O /tmp/run.sh http://10.0.2.2:{port}/run.sh")
        out = m.run("busybox sh /tmp/run.sh", timeout=900)
        section = "?"
        for line in out.splitlines():
            line = line.strip()
            if line.startswith("== "):
                section = line[3:]
                if section.startswith("host hostname"):
                    hw.check(section.endswith("auxobsd"), "the host's hostname is untouched", section)
                else:
                    print(f"▲ {section}", flush=True)
            elif line.startswith("OK "):
                hw.check(True, line[3:])
            elif line.startswith("FAIL "):
                hw.check(False, line[5:])
        hw.check(out.count("DONE all held") == 3, "all three runs finished", out[-2000:])
        out = m.run("busybox ls /tmp/mine")
        hw.check("No such file" in out, "a file the sandbox made in its /tmp isn't in the host's", out)
        out = m.run("busybox wc -l /proc/self/mountinfo")
        hw.check(re.search(r"^\s*2\s", out, re.M), "nor are its mounts in the host's table (just / and /proc)", out)
        print("▲ /proc", flush=True)
        out = m.run("busybox sh -c 'cat /proc/self/mountinfo; grep -E \"^(Pid|NSpid|Seccomp)\" /proc/self/status; ls -l /proc/self/ns/mnt'")
        hw.check(re.search(r"^\d+ \d+ 0:\d+ / / rw.* - rootfs rootfs", out, re.M), "mountinfo lists / ", out)
        hw.check(re.search(r"/proc rw,nosuid,nodev,noexec.* - proc proc", out), "and /proc, a proc filesystem", out)
        hw.check(re.search(r"Seccomp:\s+0", out), "status: no filter on the shell", out)
        hw.check(re.search(r"mnt -> mnt:\[\d+\]", out), "/proc/self/ns links name the namespaces", out)
    finally:
        m.close()
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
    print(f"\n{hw.passed} passed, {hw.failed} failed")
    sys.exit(1 if hw.failed else 0)

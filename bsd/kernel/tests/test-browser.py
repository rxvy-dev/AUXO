#!/usr/bin/env python3
"""The Chrono browser remembers what a person expects it to: open tabs, logins (cookies) and
saved passwords. Runs a small sign-in site on the host (the guest reaches it as 10.0.2.2),
boots the ISO's desktop and drives the browser with QEMU's keyboard and mouse:

  - a saved session (two tabs) comes back when the browser opens;
  - signing in sends a POST form (hidden CSRF field, Origin), the cookie set on the redirect
    is stored and sent back;
  - "Save password" writes the login; cookies, logins and tabs are in /home/auxo/.config/browser;
  - a new tab (Ctrl+T) is still signed in;
  - the sign-in form is filled in from the saved login;
  - Ctrl+W closes a tab and the session file follows.

  python3 tests/test-browser.py build/auxobsd.iso
"""
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
import urllib.parse

ANSI = re.compile(rb"\x1b\[[0-9;]*[A-Za-z]")
PROMPT = re.compile(rb"\xe2\x9d\xaf $")
passed = failed = 0
log = []


def check(ok, what, detail=""):
    global passed, failed
    if ok:
        passed += 1
        print(f"  \x1b[32m✓\x1b[0m {what}", flush=True)
    else:
        failed += 1
        print(f"  \x1b[31m✗\x1b[0m {what}", flush=True)
        if detail:
            print("      " + str(detail).strip().replace("\n", "\n      ")[-1500:], flush=True)


class Site(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def page(self, body, code=200):
        b = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def do_GET(self):
        ck = self.headers.get("Cookie", "")
        log.append(("GET", self.path, ck, ""))
        if self.path.startswith("/login"):
            return self.page('<html><head><title>Sign in</title></head><body><h1>Sign in</h1>'
                             '<form method="post" action="/session"><input type="hidden" name="csrf" value="tok123">'
                             '<p>Username <input type="text" name="user"></p><p>Password <input type="password" name="pass"></p>'
                             '<input type="submit" value="Sign in"></form></body></html>')
        if self.path.startswith("/other"):
            return self.page("<html><head><title>Other</title></head><body><p>another page</p></body></html>")
        if "sid=abc123" in ck:
            return self.page("<html><head><title>Home</title></head><body><h1>Welcome back, alice</h1></body></html>")
        return self.page("<html><head><title>Testsite</title></head><body><h1>Not signed in</h1></body></html>")

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0))).decode()
        log.append(("POST", self.path, self.headers.get("Origin", ""), body))
        f = urllib.parse.parse_qs(body)
        if f.get("user") == ["alice"] and f.get("pass") == ["hunter2"] and f.get("csrf") == ["tok123"]:
            self.send_response(303)
            self.send_header("Location", "/")
            self.send_header("Set-Cookie", "sid=abc123; Path=/; Max-Age=86400; HttpOnly")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        self.page("<h1>Wrong</h1>", 403)


class Machine:
    def __init__(self, tmp, iso):
        self.ser, self.mon = os.path.join(tmp, "ttyS0"), os.path.join(tmp, "mon")
        args = [os.environ.get("QEMU", "qemu-system-x86_64"), "-m", "1G", "-display", "none", "-vga", "std",
                "-no-reboot", "-cdrom", iso, "-nic", "user,model=e1000",
                "-chardev", f"socket,id=s0,path={self.ser},server=on,wait=on", "-serial", "chardev:s0",
                "-monitor", f"unix:{self.mon},server=on,wait=off"]
        self.p = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=open(os.path.join(tmp, "qemu.log"), "wb"), stderr=subprocess.STDOUT)
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
        return ANSI.sub(b"", self.buf[start:]).decode("utf-8", "replace")

    def monitor(self, *cmds):
        m = socket.socket(socket.AF_UNIX)
        m.connect(self.mon)
        time.sleep(0.2)
        for c in cmds:
            m.sendall(c.encode() + b"\n")
            time.sleep(0.12)
        time.sleep(0.3)
        m.close()

    def keys(self, text):
        names = {".": "dot", "/": "slash", ":": "shift-semicolon", " ": "spc", "\n": "ret", "\t": "tab", "-": "minus"}
        self.monitor(*[f"sendkey {names.get(ch, ch)}" for ch in text])

    def click(self, x, y):
        steps = []
        while x > 0 or y > 0:
            dx, dy = min(x, 60), min(y, 60)
            steps.append(f"mouse_move {dx} {dy}")
            x, y = x - dx, y - dy
        self.monitor(*(["mouse_move -100 -100"] * 14 + steps + ["mouse_button 1", "mouse_button 0"]))

    def close(self):
        if self.s:
            self.s.close()
        self.p.kill()
        self.p.wait()


def wait_for(pred, timeout=20):
    end = time.time() + timeout
    while time.time() < end:
        if pred():
            return True
        time.sleep(0.5)
    return False


def main():
    iso = sys.argv[1] if len(sys.argv) > 1 else "build/auxobsd.iso"
    srv = http.server.ThreadingHTTPServer(("0.0.0.0", 0), Site)
    port = srv.server_address[1]
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    site = f"10.0.2.2:{port}"
    tmp = tempfile.mkdtemp(prefix="auxobsd-browser-")
    m = Machine(tmp, iso)
    try:
        if not m.pump(240, PROMPT):
            check(False, "boots to auxsh", ANSI.sub(b"", m.buf[-1500:]).decode("utf-8", "replace"))
            return
        m.run("")
        print("▲ the session comes back", flush=True)
        m.run(f"busybox sh -c \"mkdir -p /home/auxo/.config/browser && printf 'active\\t1\\ntab\\thttp://{site}/other\\tOther\\n"
              f"tab\\thttp://{site}/login\\tSign in\\n' > /home/auxo/.config/browser/session\"")
        m.run("chrono open browser")
        check(wait_for(lambda: any(r[0] == "GET" and r[1] == "/login" for r in log)), "the saved active tab (sign-in page) loads", log)
        time.sleep(2)

        print("▲ signing in", flush=True)
        m.keys("\talice\thunter2\n")
        check(wait_for(lambda: any(r[0] == "POST" for r in log)), "the form is sent as a POST", log)
        post = next((r for r in log if r[0] == "POST"), ("", "", "", ""))
        check("csrf=tok123" in post[3] and "user=alice" in post[3] and "pass=hunter2" in post[3], "with its hidden field and both inputs", post)
        check(post[2] == f"http://{site}", "with an Origin header", post)
        check(wait_for(lambda: any(r[0] == "GET" and r[1] == "/" and "sid=abc123" in r[2] for r in log)),
              "the cookie set on the redirect comes back on the next request", log[-3:])
        time.sleep(2)
        m.click(717, 189)                                   # "Save" on the save-password bar
        time.sleep(2)

        print("▲ what's kept in /home/auxo/.config/browser", flush=True)
        out = m.run("cat /home/auxo/.config/browser/logins")
        check(re.search(rf"login\t{re.escape(site)}\talice\thunter2", out) is not None, "the saved login", out)
        out = m.run("cat /home/auxo/.config/browser/cookies")
        check("sid\tabc123" in out and site.split(":")[0] in out, "the session cookie", out)
        out = m.run("cat /home/auxo/.config/browser/session")
        check(out.count("tab\t") == 2 and f"http://{site}/" in out, "both tabs, the signed-in page among them", out)

        print("▲ a new tab, autofill, closing a tab", flush=True)
        n0 = len(log)
        m.monitor("sendkey ctrl-t")
        time.sleep(1)
        m.keys(f"{site}/\n")
        check(wait_for(lambda: any(r[0] == "GET" and r[1] == "/" and "sid=abc123" in r[2] for r in log[n0:])),
              "a new tab (Ctrl+T) is still signed in", log[n0:])
        time.sleep(2)
        m.monitor("sendkey ctrl-l")
        m.keys(f"{site}/login\n")
        wait_for(lambda: any(r[1] == "/login" for r in log[n0:]))
        time.sleep(3)
        n1 = len(log)
        m.keys("\t\n")                                      # focus the (filled-in) username, press Enter
        check(wait_for(lambda: any(r[0] == "POST" and "pass=hunter2" in r[3] for r in log[n1:])),
              "the sign-in form is filled in from the saved login", log[n1:])
        time.sleep(3)
        out = m.run("cat /home/auxo/.config/browser/session")
        tabs_before = out.count("tab\t")
        m.monitor("sendkey ctrl-w")
        time.sleep(2)
        out = m.run("cat /home/auxo/.config/browser/session")
        check(tabs_before == 3 and out.count("tab\t") == 2, "Ctrl+W closes a tab, and the session follows", out)
    finally:
        m.close()
        srv.shutdown()
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
    print(f"\n{passed} passed, {failed} failed")
    sys.exit(1 if failed else 0)

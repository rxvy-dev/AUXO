"""VPNs the easy way: Mullvad and Proton VPN (and any WireGuard or OpenVPN config) through
NetworkManager, so they show up in the network menu and Auxo Settings > VPN.

Neither company ships a Void Linux package of its app, so Auxo uses what both support on any
Linux: WireGuard. Two ways in:

  * a config file you download from your account page (Mullvad's WireGuard configuration
    generator, Proton's downloads page): `auxo-tweak vpn import FILE`, or `auxo-tweak vpn
    mullvad|proton`, which opens that page and imports the file as soon as it lands in Downloads
  * Mullvad only: your account number. Auxo makes a WireGuard key on this computer, registers
    the public half with Mullvad (the private key never leaves the computer), and adds the
    servers you pick.

Everything is a NetworkManager connection: `nmcli connection up NAME` works as well.
`runner` and `fetch` are swappable so the tests run without a network.
"""
import json
import os
import re
import shutil
import subprocess
import tempfile
import time
import urllib.parse
import urllib.request

PROVIDERS = {
    "mullvad": {"name": "Mullvad VPN", "prefix": "mullvad",
                "page": "https://mullvad.net/en/account/wireguard-config",
                "check": "https://am.i.mullvad.net/connected",
                "how": "Log in, pick Linux, generate a key, choose a country and download the file."},
    "proton": {"name": "Proton VPN", "prefix": "proton",
               "page": "https://account.protonvpn.com/downloads#wireguard-configuration",
               "check": "https://api.protonvpn.ch/vpn/location",
               "how": "Log in (the free plan works), choose WireGuard > GNU/Linux, pick a server and "
                      "press Download."},
}
MULLVAD_KEY_API = "https://api.mullvad.net/wg/"
MULLVAD_RELAYS = ("https://api.mullvad.net/www/relays/wireguard/",
                  "https://api.mullvad.net/public/relays/wireguard/v1/")
MULLVAD_DNS = "10.64.0.1"
NAME_RE = re.compile(r"[^a-z0-9-]")


class VPNError(Exception):
    pass


def _run(argv, input=None, timeout=30):
    try:
        r = subprocess.run(argv, capture_output=True, text=True, input=input, timeout=timeout)
        return r.returncode, r.stdout + r.stderr
    except (OSError, subprocess.TimeoutExpired) as e:
        return 1, str(e)


def _fetch(url, data=None, timeout=15):
    req = urllib.request.Request(url, data=urllib.parse.urlencode(data).encode() if data else None,
                                 headers={"User-Agent": "auxo-vpn"})
    return urllib.request.urlopen(req, timeout=timeout).read().decode()


def nm_name(text, prefix=""):
    """A NetworkManager connection / interface name: lowercase, at most 15 characters (the
    kernel's interface-name limit, which `nmcli connection import` uses the file name for)."""
    base = NAME_RE.sub("-", os.path.splitext(os.path.basename(text))[0].lower()).strip("-") or "vpn"
    if prefix and not base.startswith(prefix):
        base = f"{prefix}-{base}"
    return re.sub(r"-+", "-", base)[:15].strip("-")


def parse_wireguard(text):
    """Check a WireGuard config. Returns {'endpoint', 'address', 'dns'}; raises VPNError."""
    sect, vals = None, {}
    for raw in text.splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        if line.startswith("["):
            sect = line.strip("[]").lower()
            continue
        k, sep, v = line.partition("=")
        if sep:
            vals[(sect, k.strip().lower())] = v.strip()
    need = [("interface", "privatekey"), ("interface", "address"), ("peer", "publickey"), ("peer", "endpoint")]
    missing = [f"{s.title()} {k}" for s, k in need if not vals.get((s, k))]
    if missing:
        raise VPNError("that isn't a complete WireGuard config (missing: " + ", ".join(missing) + ")")
    return {"endpoint": vals[("peer", "endpoint")], "address": vals[("interface", "address")],
            "dns": vals.get(("interface", "dns"), "")}


def provider_of(text, filename=""):
    t = (text + " " + filename).lower()
    if "mullvad" in t or MULLVAD_DNS in t:
        return "mullvad"
    if "proton" in t or re.search(r"\b10\.2\.0\.1\b", t):
        return "proton"
    return ""


class VPN:
    def __init__(self, runner=None, fetch=None, tmpdir=None):
        self.run = runner or _run
        self.fetch = fetch or _fetch
        self.tmpdir = tmpdir         # None: a private folder (0700) per import, removed afterwards

    # ── NetworkManager ──
    def connections(self):
        """[{'name', 'type', 'active', 'provider'}] for VPN-ish connections."""
        rc, out = self.run(["nmcli", "-t", "-f", "NAME,TYPE,ACTIVE", "connection", "show"])
        conns = []
        for line in out.splitlines() if rc == 0 else []:
            parts = re.split(r"(?<!\\):", line)
            if len(parts) < 3:
                continue
            name, typ, active = parts[0].replace("\\:", ":"), parts[1], parts[2]
            if typ in ("wireguard", "vpn"):
                conns.append({"name": name, "type": "WireGuard" if typ == "wireguard" else "OpenVPN",
                              "active": active == "yes", "provider": provider_of("", name)})
        return conns

    def import_file(self, path, name=None, provider=""):
        """Import a WireGuard (.conf) or OpenVPN (.ovpn) file. Returns the connection's name."""
        try:
            text = open(path, encoding="utf-8", errors="replace").read()
        except OSError as e:
            raise VPNError(f"can't read {path}: {e.strerror}")
        provider = provider or provider_of(text, path)
        if path.endswith(".ovpn") or "\nremote " in "\n" + text and "[Interface]" not in text:
            rc, out = self.run(["nmcli", "connection", "import", "type", "openvpn", "file", path])
            if rc != 0:
                raise VPNError("NetworkManager couldn't import it (is NetworkManager-openvpn installed? "
                               "sudo xbps-install NetworkManager-openvpn): " + out.strip())
            m = re.search(r"'([^']+)'", out)
            return m.group(1) if m else os.path.splitext(os.path.basename(path))[0]
        parse_wireguard(text)
        conn = nm_name(name or path, PROVIDERS.get(provider, {}).get("prefix", ""))
        work = self.tmpdir or tempfile.mkdtemp(prefix="auxo-vpn-")   # never a guessable path in /tmp
        os.makedirs(work, mode=0o700, exist_ok=True)
        tmp = os.path.join(work, f"{conn}.conf")            # nmcli names the interface after the file
        if os.path.lexists(tmp):
            os.remove(tmp)
        fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "w") as f:
            f.write(text)
        try:
            self.run(["nmcli", "connection", "delete", conn])          # re-import replaces the old one
            rc, out = self.run(["nmcli", "connection", "import", "type", "wireguard", "file", tmp])
        finally:
            try:
                os.remove(tmp)
            except OSError:
                pass
            if not self.tmpdir:
                shutil.rmtree(work, ignore_errors=True)
        if rc != 0:
            raise VPNError("NetworkManager couldn't import it: " + out.strip())
        # connect when you say so, not at every boot (change with auxo-tweak vpn auto NAME)
        self.run(["nmcli", "connection", "modify", conn, "connection.autoconnect", "no"])
        self.run(["nmcli", "connection", "down", conn])
        return conn

    def up(self, name):
        rc, out = self.run(["nmcli", "connection", "up", name], timeout=45)
        if rc != 0:
            raise VPNError(f"couldn't connect {name}: {out.strip()}")

    def down(self, name=None):
        names = [name] if name else [c["name"] for c in self.connections() if c["active"]]
        for n in names:
            self.run(["nmcli", "connection", "down", n])
        return names

    def remove(self, name):
        rc, out = self.run(["nmcli", "connection", "delete", name])
        if rc != 0:
            raise VPNError(out.strip() or f"no VPN called {name}")

    def autoconnect(self, name, on=True):
        self.run(["nmcli", "connection", "modify", name, "connection.autoconnect", "yes" if on else "no"])

    def check(self, provider):
        """What the provider says about this connection (Mullvad: 'You are connected...')."""
        url = PROVIDERS.get(provider, {}).get("check")
        if not url:
            return ""
        try:
            return self.fetch(url).strip()[:200]
        except Exception as e:
            return f"couldn't check ({e})"

    # ── Mullvad with an account number ──
    def mullvad_relays(self, country=None):
        """[(hostname, ipv4, public key, 'City, Country')] of active WireGuard servers."""
        last = None
        for url in MULLVAD_RELAYS:
            try:
                data = json.loads(self.fetch(url))
            except Exception as e:
                last = e
                continue
            out = []
            if isinstance(data, list):           # /www/relays/wireguard/: a flat list
                for r in data:
                    if r.get("active", True) and r.get("ipv4_addr_in") and r.get("pubkey"):
                        out.append((r["hostname"], r["ipv4_addr_in"], r["pubkey"],
                                    f"{r.get('city_name', '')}, {r.get('country_name', '')}", r.get("country_code", "")))
            else:                                  # /public/relays/wireguard/v1/: countries > cities > relays
                for c in data.get("countries", []):
                    for city in c.get("cities", []):
                        for r in city.get("relays", []):
                            if r.get("ipv4_addr_in") and r.get("public_key"):
                                out.append((r["hostname"], r["ipv4_addr_in"], r["public_key"],
                                            f"{city.get('name', '')}, {c.get('name', '')}", c.get("code", "")))
            if country:
                cc = country.lower()
                out = [r for r in out if r[4].lower() == cc or cc in r[3].lower() or r[0].startswith(cc)]
            return out
        raise VPNError(f"couldn't get Mullvad's server list ({last})")

    def mullvad_account(self, account, country=None, count=3):
        """Register a new key for this computer and add up to `count` servers. Returns the names."""
        acct = re.sub(r"\s+", "", account or "")
        if not re.fullmatch(r"\d{16}", acct):
            raise VPNError("a Mullvad account number is 16 digits (spaces are fine)")
        rc, priv = self.run(["wg", "genkey"])
        if rc != 0:
            raise VPNError("wg isn't installed: sudo xbps-install wireguard-tools")
        priv = priv.strip()
        rc, pub = self.run(["wg", "pubkey"], input=priv + "\n")
        pub = pub.strip()
        try:
            addr = self.fetch(MULLVAD_KEY_API, {"account": acct, "pubkey": pub}).strip()
        except Exception as e:
            raise VPNError(f"Mullvad didn't accept the key ({e}). Accounts allow 5 devices: remove an old one on "
                           "mullvad.net, or use a config file: auxo-tweak vpn mullvad")
        if not re.match(r"^[0-9a-fA-F.:/,]+$", addr):
            raise VPNError(f"Mullvad said: {addr[:160]}")
        relays = self.mullvad_relays(country)
        if not relays:
            raise VPNError(f"no Mullvad WireGuard servers for '{country}'")
        made = []
        for host, ip, key, _where, _cc in relays[:count]:
            conf = (f"# Mullvad {host} — added by auxo-tweak vpn mullvad\n[Interface]\nPrivateKey = {priv}\n"
                    f"Address = {addr}\nDNS = {MULLVAD_DNS}\n\n[Peer]\nPublicKey = {key}\n"
                    f"AllowedIPs = 0.0.0.0/0, ::/0\nEndpoint = {ip}:51820\n")
            work = self.tmpdir or tempfile.mkdtemp(prefix="auxo-vpn-")
            os.makedirs(work, mode=0o700, exist_ok=True)
            tmp = os.path.join(work, f"auxo-{host}.conf")
            fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(fd, "w") as f:
                f.write(conf)
            try:
                made.append(self.import_file(tmp, name=f"mullvad-{host.split('-wg')[0]}", provider="mullvad"))
            finally:
                os.remove(tmp)
                if not self.tmpdir:
                    shutil.rmtree(work, ignore_errors=True)
        return made


def newest_download(since, folder, exts=(".conf", ".ovpn")):
    """A VPN config that appeared in `folder` after `since` (for the open-the-page-and-wait flow)."""
    best = None
    try:
        for f in os.listdir(folder):
            p = os.path.join(folder, f)
            if f.lower().endswith(exts) and os.path.getmtime(p) > since and not f.endswith((".part", ".crdownload")):
                if not best or os.path.getmtime(p) > os.path.getmtime(best):
                    best = p
    except OSError:
        pass
    return best


def wait_for_download(folder, since=None, timeout=600, poll=1.0, sleep=time.sleep):
    since = since if since is not None else time.time()
    end = time.time() + timeout
    while time.time() < end:
        p = newest_download(since, folder)
        if p:
            sleep(0.5)                               # let the browser finish writing it
            return p
        sleep(poll)
    return None

"""GPU detection and driver plans for AuxoBSD.

Everything here is pure (no commands run) so the installer, auxo-tweak and the tests share it:
detect() turns `pciconf -lv` devices into GPUs, plan() decides packages, kernel modules,
loader tunables and Xorg config for them.

NVIDIA on FreeBSD (ports, 2026):
  current  nvidia-drm-kmod      Turing and newer (GTX 16xx, RTX 20xx and up)    DRM/KMS, Wayland
  580      nvidia-drm-kmod-580  Maxwell, Pascal, Volta (GTX 750 … 10xx, Titan V) DRM/KMS, Wayland
  470      nvidia-driver-470    Kepler (GTX 600/700)                              X11 only
  390      nvidia-driver-390    Fermi (GTX 400/500)                               X11 only
  340      nvidia-driver-340    Tesla (8000 … 300 series)                         X11 only
"""
import re

VENDORS = {"10de": "nvidia", "8086": "intel", "1002": "amd", "80ee": "virtualbox", "15ad": "vmware",
           "1234": "qemu", "1af4": "virtio", "1b36": "qemu"}

NVIDIA = {
    "current": {"pkgs": ["nvidia-drm-kmod"], "driver": "nvidia-driver", "kld": "nvidia-drm", "drm": True,
                "linux": "linux-nvidia-libs", "label": "current (Turing and newer)"},
    "580": {"pkgs": ["nvidia-drm-kmod-580"], "driver": "nvidia-driver-580", "kld": "nvidia-drm", "drm": True,
            "linux": "linux-nvidia-libs-580", "label": "580 legacy (Maxwell, Pascal, Volta)"},
    "470": {"pkgs": ["nvidia-driver-470"], "driver": "nvidia-driver-470", "kld": "nvidia-modeset", "drm": False,
            "linux": "linux-nvidia-libs-470", "label": "470 legacy (Kepler)"},
    "390": {"pkgs": ["nvidia-driver-390"], "driver": "nvidia-driver-390", "kld": "nvidia-modeset", "drm": False,
            "linux": "linux-nvidia-libs-390", "label": "390 legacy (Fermi)"},
    "340": {"pkgs": ["nvidia-driver-340"], "driver": "nvidia-driver-340", "kld": "nvidia", "drm": False,
            "linux": "linux-nvidia-libs-340", "label": "340 legacy (Tesla)"},
}
NVIDIA_TOOLS = ["nvidia-settings", "nvidia-xconfig"]

# every NVIDIA package any branch installs, for switching branches cleanly
NVIDIA_ALL = sorted({p for b in NVIDIA.values() for p in b["pkgs"] + [b["driver"]]} |
                    {"nvidia-kmod", "nvidia-kmod-580", "nvidia-kmod-470", "nvidia-kmod-390", "nvidia-kmod-340",
                     "nvidia-drm-66-kmod", "nvidia-drm-612-kmod", "nvidia-drm-latest-kmod",
                     "nvidia-drm-66-kmod-580", "nvidia-drm-612-kmod-580", "nvidia-drm-latest-kmod-580"})

VBOX_ADDITIONS = ["virtualbox-ose-additions-72", "virtualbox-ose-additions"]


def nvidia_branch(device_id):
    """Driver branch for an NVIDIA PCI device id (hex string or int). Same generation
    boundaries as `auxo-tweak nvidia` on Auxo Linux."""
    d = int(device_id, 16) if isinstance(device_id, str) else device_id
    if d >= 0x1e00:
        return "current"
    if d >= 0x1340:
        return "580"
    if d >= 0x0fc0:
        return "470"
    if d >= 0x06c0:
        return "390"
    return "340"


def _amd_legacy(desc):
    """Pre-GCN Radeons (HD 2000-6000, X-series) use radeonkms, everything newer amdgpu."""
    return bool(re.search(r"Radeon (HD [2-6]\d{3}|X\d{3,4}|Xpress)|R[1-6]\d{2}\b|RV\d{3}|Cedar|Caicos|Turks|Barts|Cayman|"
                          r"Juniper|Redwood|Cypress|Wrestler|Sumo|Palm", desc or ""))


def detect(devices):
    """[(vendor, device, class, description)] → list of GPU dicts:
    {vendor, device, desc, kind, branch?, legacy?}. Only PCI display devices (class 03xxxx)."""
    gpus = []
    for vendor, device, cls, desc in devices:
        if not (cls or "").startswith("03"):
            continue
        kind = VENDORS.get(vendor.lower(), "unknown")
        g = {"vendor": vendor.lower(), "device": device.lower(), "desc": desc or "", "kind": kind}
        if kind == "nvidia":
            g["branch"] = nvidia_branch(device)
        if kind == "amd":
            g["legacy"] = _amd_legacy(desc)
        gpus.append(g)
    return gpus


def is_hybrid(gpus):
    """A laptop-style pair: an integrated Intel/AMD GPU driving the screen plus an NVIDIA GPU."""
    kinds = {g["kind"] for g in gpus}
    return "nvidia" in kinds and bool(kinds & {"intel", "amd"})


def describe(g):
    name = g["desc"] or "unknown display controller"
    extra = ""
    if g["kind"] == "nvidia":
        extra = f" → NVIDIA {NVIDIA[g['branch']]['label']}"
    elif g["kind"] == "intel":
        extra = " → i915kms (drm-kmod)"
    elif g["kind"] == "amd":
        extra = " → radeonkms (drm-kmod)" if g.get("legacy") else " → amdgpu (drm-kmod)"
    elif g["kind"] == "virtualbox":
        extra = " → VirtualBox guest additions"
    elif g["kind"] == "vmware":
        extra = " → VMware guest tools"
    elif g["kind"] in ("qemu", "virtio"):
        extra = " → framebuffer (scfb)"
    return f"{name} [{g['vendor']}:{g['device']}]{extra}"


def xorg_nvidia_conf():
    return ('# AuxoBSD: NVIDIA drives the display (auxo-tweak nvidia)\n'
            'Section "Device"\n    Identifier "NVIDIA"\n    Driver     "nvidia"\n'
            '    Option     "NoLogo" "true"\nEndSection\n')


PRIME_RUN = """#!/bin/sh
# prime-run — run a program on the NVIDIA GPU of a hybrid (iGPU + NVIDIA) laptop.
#   prime-run glxinfo -B      prime-run firefox
export __NV_PRIME_RENDER_OFFLOAD=1
export __GLX_VENDOR_LIBRARY_NAME=nvidia
export __VK_LAYER_NV_optimus=NVIDIA_only
exec "$@"
"""


def vm_kind(product, vendor=""):
    """smbios.system.product / maker → 'virtualbox' | 'vmware' | 'qemu' | 'bhyve' | ''.
    Needed because VirtualBox's default VMSVGA adapter has VMware's PCI id."""
    t = f"{product} {vendor}".lower()
    for key, name in (("virtualbox", "virtualbox"), ("innotek", "virtualbox"), ("vmware", "vmware"),
                      ("bhyve", "bhyve"), ("qemu", "qemu"), ("kvm", "qemu"), ("standard pc", "qemu")):
        if key in t:
            return name
    return ""


def plan(gpus, nvidia_mode=None, vm=""):
    """What to set up for these GPUs.

    nvidia_mode: None (auto), "primary" (NVIDIA drives the screen), "prime" (iGPU drives the
    screen, NVIDIA renders on request with prime-run) or "off" (nouveau-free: no NVIDIA driver,
    framebuffer only). Returns a dict:
      pkgs      packages to install          klds      kernel modules for rc.conf kld_list
      loader    loader.conf tunables          xorg      {filename: content} for xorg.conf.d
      services  rc.conf services to enable    nvidia    branch name or None
      mode      'primary' | 'prime' | None     prime_run True when prime-run should be installed
      notes     human-readable lines
    """
    p = {"pkgs": [], "klds": [], "loader": {}, "xorg": {}, "services": [], "nvidia": None, "mode": None,
         "prime_run": False, "notes": []}
    kinds = {g["kind"] for g in gpus}
    hybrid = is_hybrid(gpus)

    if kinds & {"intel", "amd"}:
        p["pkgs"].append("drm-kmod")
        if "intel" in kinds:
            p["klds"].append("i915kms")
        for g in gpus:
            if g["kind"] == "amd":
                k = "radeonkms" if g.get("legacy") else "amdgpu"
                if k not in p["klds"]:
                    p["klds"].append(k)

    nv = [g for g in gpus if g["kind"] == "nvidia"]
    if nv and nvidia_mode != "off":
        # several NVIDIA cards: the oldest decides the branch (one driver serves them all)
        order = list(NVIDIA)
        branch = max((g["branch"] for g in nv), key=order.index)
        b = NVIDIA[branch]
        mode = nvidia_mode or ("prime" if hybrid else "primary")
        if mode == "prime" and not b["drm"]:
            # no offload with the old drivers; on most laptops the screen is wired to the iGPU,
            # so forcing NVIDIA would give a black screen. Keep the iGPU unless asked.
            p["notes"].append(f"NVIDIA {b['label']} can't do PRIME offload on FreeBSD, so the integrated GPU "
                              "stays in charge. If your screen is wired to the NVIDIA GPU: auxo-tweak nvidia primary")
            nv = []
    if nv and nvidia_mode != "off":
        p["nvidia"], p["mode"] = branch, mode
        p["pkgs"] += b["pkgs"] + [b["driver"]] + NVIDIA_TOOLS
        if b["kld"] not in p["klds"]:
            p["klds"].append(b["kld"])
        if b["drm"]:
            p["loader"]["hw.nvidiadrm.modeset"] = "1"
        if mode == "primary":
            p["xorg"]["20-nvidia.conf"] = xorg_nvidia_conf()
        else:
            p["prime_run"] = True
            p["notes"].append("hybrid graphics: the integrated GPU drives the screen; "
                              "run games and apps on the NVIDIA GPU with prime-run")
        if not b["drm"]:
            p["notes"].append(f"NVIDIA {b['label']}: X11 only (Plasma Wayland needs a DRM-capable driver)")
    elif nv:
        p["notes"].append("NVIDIA driver off: the NVIDIA GPU uses the basic framebuffer")

    if "vmware" in kinds:                       # VMware SVGA, also VirtualBox's VMSVGA adapter
        p["pkgs"].append("xf86-video-vmware")
    if vm == "virtualbox" or "virtualbox" in kinds:
        p["pkgs"].append(VBOX_ADDITIONS[0])
        p["services"] += ["vboxguest", "vboxservice"]
    elif vm == "vmware":
        p["pkgs"].append("open-vm-tools")
        p["services"] += ["vmware_guest_vmblock", "vmware_guest_vmhgfs", "vmware_guest_vmmemctl", "vmtoolsd"]
    if kinds & {"qemu", "virtio", "unknown"} and not p["klds"] and not p["nvidia"]:
        p["pkgs"].append("xf86-video-scfb")
        p["notes"].append("no accelerated driver for this display; using the scfb framebuffer")
    if not gpus:
        p["notes"].append("no PCI display controller found")
    p["pkgs"] = list(dict.fromkeys(p["pkgs"]))
    return p

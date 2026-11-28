# Security & trust

"How do I know nothing bad is hidden in AUXO?" is a fair question to ask of any distro. You
shouldn't have to take anyone's word for it, so AUXO is set up so you can check for yourself.

## Everything is public and built in the open

- **All of AUXO is source code in this repository.** Its tools are Python and shell scripts you can
  read as they are. There are no prebuilt binaries in the repo. A CI check fails the build if one is
  ever committed.
- **The ISO is built by GitHub Actions from this repo**, not on anyone's PC.
  [`.github/workflows/build-iso.yml`](.github/workflows/build-iso.yml) shows every step. The
  packages underneath come from Void Linux's official repositories. Hyprland, which Void doesn't
  package, is built from its upstream source by [`build-hyprland.yml`](.github/workflows/build-hyprland.yml).
- **Every change goes through the same checks**, whoever or whatever wrote it (a person or an AI tool):
  - [`lint.yml`](.github/workflows/lint.yml) runs `shellcheck` on every shell script,
    `ruff` on every Python file, and `bandit`, a security scanner for Python. It fails on any
    high-severity finding.
  - Over 600 automated tests cover the tools, installer, Settings, shell apps and rices.
  - Each Hyprland style is checked by Hyprland itself (`Hyprland --verify-config`).
  - The finished ISO is booted in a virtual machine before it's published.

## Check your download

Every release lists the ISO's SHA-256 checksum next to it (`auxo-linux-….iso.sha256`):

```bash
sha256sum -c auxo-linux-*.iso.sha256        # must say: OK
```

Releases since 5.0 also carry a **build provenance attestation**: a signed statement, from GitHub,
that this exact file was built by this repository's workflow, from a named commit. Nobody can
make one for a file they built themselves.

```bash
gh attestation verify auxo-linux-*.iso -R rxvy-dev/AUXO
```

## What AUXO does on your computer

- `auxo-tweak` and the installer change your system only when you run them, and they print every
  command first (`AUXO_DRY_RUN=1 auxo-tweak …` shows the commands without running anything).
- AUXO doesn't collect analytics or telemetry. The only servers it talks to are:
  - Void's package mirrors, for updates and for the bar's "updates available" check every 30 minutes
  - this repository's releases, for Auxo's own packages
  - Flathub, when you use the App Store
  - wttr.in, only while the weather widget is on
  - Mullvad or Proton, only while you set up a VPN

  "Ask Auxo" runs a model on your own machine (Ollama), not in the cloud.
- VPN keys are created on your computer, and the private key never leaves it.

## Reporting a vulnerability

Please don't open a public issue for a security problem. Use **[Report a vulnerability](../../security/advisories/new)**
(GitHub's private advisories), or message rxvy on [Discord](https://discord.gg/XbQ66dH5a7).
You'll get an answer within a few days, and fixes ship in the next build.

Supported: the newest stable release and the current beta. Older ISOs should be updated
(`auxo-update`) or replaced.

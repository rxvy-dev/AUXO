#!/bin/sh
# Install the AuxoBSD tools and the shared Auxo assets into a root directory.
#   bsd/scripts/stage-tools.sh DEST
# Used by build.sh for the ISO (DEST = the image root) and by the tests (DEST = a temp dir).
# Shared with Auxo Linux (packages/auxo-tools): accent palette, KDE theme code, Plasma
# look-and-feel, wallpapers, shell prompts. Runs on FreeBSD and Linux.
set -eu
DEST=${1:?usage: stage-tools.sh DEST}
HERE=$(cd "$(dirname "$0")/.." && pwd)          # bsd/
TOP=$(cd "$HERE/.." && pwd)                     # repository root
LNX=$TOP/packages/auxo-tools/files
BSDT=$HERE/tools/files
LOGO_URL=${AUXOBSD_LOGO_URL:-https://i.ibb.co/VWn2Vb7H/untitled.png}   # the logo on auxolinux.com/bsd

d() { mkdir -p "$DEST$1"; }
d /usr/local/bin; d /usr/local/lib/auxo/installer; d /usr/local/share/auxo/shell
d /usr/local/share/wallpapers/auxo; d /usr/local/share/pixmaps; d /usr/local/share/applications
d /usr/local/share/plasma/look-and-feel; d /etc/auxo

# tools
for t in auxo-tweak auxo-fetch auxo-motd auxo-update auxo-rollback; do
    install -m 755 "$BSDT/bin/$t" "$DEST/usr/local/bin/$t"
done
install -m 755 "$HERE/installer/bin/auxo-installer" "$DEST/usr/local/bin/auxo-installer"
for f in "$BSDT"/lib/*.py "$LNX/lib/auxo_accents.py" "$LNX/lib/auxo_kde.py"; do
    install -m 644 "$f" "$DEST/usr/local/lib/auxo/"
done
rm -rf "$DEST/usr/local/lib/auxo/installer/auxo_bsdinstall"
cp -R "$HERE/installer/auxo_bsdinstall" "$DEST/usr/local/lib/auxo/installer/"
find "$DEST/usr/local/lib/auxo" -name __pycache__ -type d -prune -exec rm -rf {} +

# shell: the Auxo prompts are shared; the zshrc uses FreeBSD paths
for p in prompt.zsh prompt.fish prompt.bash; do
    install -m 644 "$LNX/share/auxo/shell/$p" "$DEST/usr/local/share/auxo/shell/$p"
done
sed -e 's|/usr/share/zsh/plugins/zsh-autosuggestions/|/usr/local/share/zsh-autosuggestions/|g' \
    -e 's|/usr/share/zsh/plugins/zsh-syntax-highlighting/|/usr/local/share/zsh-syntax-highlighting/|g' \
    -e 's|/usr/share/auxo/|/usr/local/share/auxo/|g' \
    -e "s|alias ls='ls --color=auto'|alias ls='ls -G'|" \
    -e 's|Auxo defaults|AuxoBSD defaults|' \
    "$LNX/share/auxo/shell/zshrc" > "$DEST/usr/local/share/auxo/shell/zshrc"

# artwork
cp "$LNX"/share/wallpapers/auxo-*.png "$DEST/usr/local/share/wallpapers/auxo/"
ln -sf auxo-violet.png "$DEST/usr/local/share/wallpapers/auxo/current.png"
cp "$LNX"/share/pixmaps/auxo-* "$DEST/usr/local/share/pixmaps/"
LOGO=$TOP/branding/logo/auxobsd-logo.png
if [ ! -s "$LOGO" ]; then
    echo "stage-tools: branding/logo/auxobsd-logo.png missing; downloading it from $LOGO_URL" >&2
    mkdir -p "$TOP/branding/logo"
    if command -v fetch >/dev/null; then fetch -q -o "$LOGO" "$LOGO_URL" || rm -f "$LOGO"
    else curl -fsSL -o "$LOGO" "$LOGO_URL" 2>/dev/null || rm -f "$LOGO"; fi
fi
if [ -s "$LOGO" ]; then
    install -m 644 "$LOGO" "$DEST/usr/local/share/pixmaps/auxobsd-logo.png"
else
    echo "stage-tools: WARNING: no AuxoBSD logo; using the Auxo logo instead" >&2
    install -m 644 "$LNX/share/pixmaps/auxo-logo.png" "$DEST/usr/local/share/pixmaps/auxobsd-logo.png"
fi

# KDE Plasma: the Auxo look-and-feel, with FreeBSD paths and the AuxoBSD logo on the launcher
LAF=org.auxolinux.desktop
rm -rf "$DEST/usr/local/share/plasma/look-and-feel/$LAF"
cp -R "$LNX/share/plasma/look-and-feel/$LAF" "$DEST/usr/local/share/plasma/look-and-feel/"
JS=$DEST/usr/local/share/plasma/look-and-feel/$LAF/contents/layouts/org.kde.plasma.desktop-layout.js
sed -e 's|/usr/share/pixmaps/auxo-logo.svg|/usr/local/share/pixmaps/auxobsd-logo.png|' \
    -e 's|Auxo Linux default Plasma layout|AuxoBSD default Plasma layout|' "$JS" > "$JS.new" && mv "$JS.new" "$JS"

# menu entries
for f in "$BSDT"/applications/*.desktop; do
    install -m 644 "$f" "$DEST/usr/local/share/applications/"
done

# default settings
[ -f "$DEST/etc/auxo/auxo.conf" ] || printf '# AuxoBSD system settings — managed by auxo-tweak\nACCENT="violet"\n' \
    > "$DEST/etc/auxo/auxo.conf"
echo "stage-tools: AuxoBSD tools staged into $DEST"

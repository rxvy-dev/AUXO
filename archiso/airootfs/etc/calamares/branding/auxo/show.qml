/* Auxo Linux — installer slideshow */
import QtQuick 2.15
import calamares.slideshow 1.0

Presentation {
    id: presentation

    Timer {
        interval: 7000
        running: presentation.activatedInCalamares
        repeat: true
        onTriggered: presentation.goToNextSlide()
    }

    component AuxoSlide: Slide {
        property string heading
        property string body
        property string glyph: "▲"
        Rectangle { anchors.fill: parent; color: "#0b0b10" }
        Rectangle {
            width: parent.width * 0.9; height: 4; radius: 2
            anchors.horizontalCenter: parent.horizontalCenter; anchors.bottom: parent.bottom; anchors.bottomMargin: 26
            gradient: Gradient { orientation: Gradient.Horizontal
                GradientStop { position: 0.0; color: "#a78bfa" }
                GradientStop { position: 1.0; color: "#22d3ee" } }
        }
        Column {
            anchors.centerIn: parent; spacing: 16; width: parent.width * 0.78
            Text { text: glyph; color: "#a78bfa"; font.pixelSize: 54; anchors.horizontalCenter: parent.horizontalCenter }
            Text { text: heading; color: "#f4f4f8"; font.pixelSize: 30; font.bold: true; font.family: "Inter"
                   anchors.horizontalCenter: parent.horizontalCenter; horizontalAlignment: Text.AlignHCenter; width: parent.width; wrapMode: Text.WordWrap }
            Text { text: body; color: "#a3a3b8"; font.pixelSize: 17; font.family: "Inter"; lineHeight: 1.25
                   width: parent.width; wrapMode: Text.WordWrap; horizontalAlignment: Text.AlignHCenter }
        }
    }

    AuxoSlide {
        heading: "Welcome to Auxo Linux"
        body: "Arch underneath, modular on top. Sit back — your system is being assembled with exactly the pieces you picked."
    }
    AuxoSlide {
        glyph: "◐"
        heading: "One accent, everywhere"
        body: "The colour you chose flows into the boot menu, the login screen, your prompt, terminals, bars and notifications.\nChange it any time:  auxo-tweak accent rose"
    }
    AuxoSlide {
        glyph: "⟲"
        heading: "Time-travel snapshots"
        body: "On btrfs, every update takes a snapshot automatically. Something broke? Pick an older snapshot straight from the boot menu, then make it permanent with auxo-rollback."
    }
    AuxoSlide {
        glyph: "⚙"
        heading: "Swap anything, never reinstall"
        body: "Desktop, kernel, shell, drivers, mirrors, zram, services — auxo-tweak changes them all after install. Try: auxo-tweak desktop hyprland"
    }
    AuxoSlide {
        glyph: "⬆"
        heading: "One-command updates"
        body: "auxo-update reads the Arch news for you, snapshots, updates repo + AUR + Flatpak, then tells you about .pacnew files and whether to reboot."
    }
    AuxoSlide {
        glyph: "◆"
        heading: "Hardware sorted"
        body: "Auxo detects your GPU during install: NVIDIA RTX gets the open kernel driver with modesetting, AMD and Intel get Vulkan and video acceleration. Unused drivers are removed."
    }

    function onActivate() { presentation.currentSlide = 0; }
    function onLeave() { }
}

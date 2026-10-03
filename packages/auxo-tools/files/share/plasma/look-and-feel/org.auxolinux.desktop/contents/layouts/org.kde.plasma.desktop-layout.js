// Auxo Linux default Plasma layout: Auxo wallpaper + a floating, centred dock-style panel.
// Runs once when a user's desktop is first created (or after "auxo-tweak rice kde").

// ~/.config/auxo/wallpaper.png is a link to the wallpaper of the current accent
var home = userDataPath().replace(/\/\.local\/share\/?$/, "");
var wallpaper = "file://" + home + "/.config/auxo/wallpaper.png";

var all = desktops();
for (var i = 0; i < all.length; i++) {
    var d = all[i];
    d.wallpaperPlugin = "org.kde.image";
    d.currentConfigGroup = ["Wallpaper", "org.kde.image", "General"];
    d.writeConfig("Image", wallpaper);
    d.writeConfig("FillMode", 2);
}

var panel = new Panel;
panel.location = "bottom";
panel.height = 2 * Math.floor(gridUnit * 2.6 / 2);
panel.floating = true;
panel.lengthMode = "fit";
panel.alignment = "center";

var launcher = panel.addWidget("org.kde.plasma.kickoff");
launcher.currentConfigGroup = ["General"];
launcher.writeConfig("icon", "/usr/share/pixmaps/auxo-logo.svg");
launcher.currentConfigGroup = ["Shortcuts"];
launcher.writeConfig("global", "Alt+F1");

var tasks = panel.addWidget("org.kde.plasma.icontasks");
tasks.currentConfigGroup = ["General"];
tasks.writeConfig("launchers", [
    "applications:org.kde.dolphin.desktop",
    "applications:firefox.desktop",
    "applications:org.kde.konsole.desktop",
    "applications:auxo-tweak.desktop"
]);

panel.addWidget("org.kde.plasma.marginsseparator");
panel.addWidget("org.kde.plasma.systemtray");
var clock = panel.addWidget("org.kde.plasma.digitalclock");
clock.currentConfigGroup = ["Appearance"];
clock.writeConfig("showDate", true);
panel.addWidget("org.kde.plasma.showdesktop");

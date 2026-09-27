-- ── Auxo Hyprland rice ─────────────────────────────────────────────
-- Hyprland 0.56+ Lua config (hyprland.conf / hyprlang is being removed upstream).
-- Accent colours live in ~/.config/auxo/hypr_colors.lua and follow
--   auxo-tweak accent <name>
-- Reference: https://wiki.hypr.land/Configuring/

local HOME = os.getenv("HOME")
package.path = HOME .. "/.config/auxo/?.lua;" .. package.path

local ok, c = pcall(require, "hypr_colors")
if not ok or type(c) ~= "table" then
    c = { accent = "rgb(a78bfa)", accent2 = "rgb(22d3ee)", inactive = "rgba(2a2a38aa)" }
end

local function file_exists(p)
    local f = io.open(p, "r")
    if f then f:close() return true end
    return false
end


---- MONITORS ----
hl.monitor({ output = "", mode = "preferred", position = "auto", scale = "auto" })


---- PROGRAMS ----
local terminal    = "kitty"
local fileManager = "thunar"
local menu        = "wofi --show drun"
local mainMod     = "SUPER"


---- AUTOSTART ----
hl.on("hyprland.start", function()
    hl.exec_cmd("waybar")
    hl.exec_cmd("hyprpaper")
    hl.exec_cmd("hypridle")
    hl.exec_cmd("mako")
    hl.exec_cmd("nm-applet --indicator")
    hl.exec_cmd("blueman-applet")
    hl.exec_cmd("/usr/lib/polkit-kde-authentication-agent-1")
    hl.exec_cmd("auxo-tweak session-apply --once")
end)


---- ENVIRONMENT ----
hl.env("XCURSOR_SIZE", "24")
hl.env("HYPRCURSOR_SIZE", "24")
hl.env("QT_QPA_PLATFORM", "wayland;xcb")
hl.env("GDK_BACKEND", "wayland,x11,*")

-- NVIDIA only when the proprietary/open driver is actually loaded
-- (setting these on AMD/Intel breaks video acceleration)
if file_exists("/proc/driver/nvidia/version") then
    hl.env("LIBVA_DRIVER_NAME", "nvidia")
    hl.env("__GLX_VENDOR_LIBRARY_NAME", "nvidia")
    hl.env("NVD_BACKEND", "direct")
end


---- LOOK AND FEEL ----
hl.config({
    general = {
        gaps_in     = 6,
        gaps_out    = 14,
        border_size = 2,
        col = {
            active_border   = { colors = { c.accent, c.accent2 }, angle = 45 },
            inactive_border = c.inactive,
        },
        resize_on_border = true,
        allow_tearing    = false,
        layout           = "dwindle",
    },

    decoration = {
        rounding         = 12,
        rounding_power   = 2,
        active_opacity   = 1.0,
        inactive_opacity = 0.94,
        shadow = {
            enabled      = true,
            range        = 18,
            render_power = 3,
            color        = 0x88000000,
        },
        blur = {
            enabled  = true,
            size     = 6,
            passes   = 3,
            vibrancy = 0.18,
        },
    },

    animations = { enabled = true },

    dwindle = { preserve_split = true },

    misc = {
        force_default_wallpaper = 0,
        disable_hyprland_logo   = true,
    },

    input = {
        kb_layout    = "us",
        follow_mouse = 1,
        sensitivity  = 0,
        touchpad = { natural_scroll = true },
    },
})

-- animations: "climb" curve — fast start, soft landing
hl.curve("climb",  { type = "bezier", points = { {0.16, 1}, {0.3, 1} } })
hl.curve("linear", { type = "bezier", points = { {0, 0},    {1, 1}   } })
hl.animation({ leaf = "global",      enabled = true, speed = 10, bezier = "default" })
hl.animation({ leaf = "windows",     enabled = true, speed = 5,  bezier = "climb", style = "slide" })
hl.animation({ leaf = "windowsOut",  enabled = true, speed = 4,  bezier = "climb", style = "popin 80%" })
hl.animation({ leaf = "border",      enabled = true, speed = 8,  bezier = "default" })
hl.animation({ leaf = "borderangle", enabled = true, speed = 60, bezier = "linear", style = "loop" })
hl.animation({ leaf = "fade",        enabled = true, speed = 5,  bezier = "default" })
hl.animation({ leaf = "workspaces",  enabled = true, speed = 5,  bezier = "climb", style = "slidefade 20%" })

-- touchpad: 3-finger swipe between workspaces
hl.gesture({ fingers = 3, direction = "horizontal", action = "workspace" })


---- KEYBINDINGS ----
local bind, dsp = hl.bind, hl.dsp

bind(mainMod .. " + Return",      dsp.exec_cmd(terminal))
bind(mainMod .. " + D",           dsp.exec_cmd(menu))
bind(mainMod .. " + E",           dsp.exec_cmd(fileManager))
bind(mainMod .. " + T",           dsp.exec_cmd(terminal .. " -e auxo-tweak"))
bind(mainMod .. " + Q",           dsp.window.close())
bind(mainMod .. " + SHIFT + E",   dsp.exit())
bind(mainMod .. " + F",           dsp.window.fullscreen())
bind(mainMod .. " + V",           dsp.window.float({ action = "toggle" }))
bind(mainMod .. " + P",           dsp.window.pseudo())
bind(mainMod .. " + J",           dsp.layout("togglesplit"))
bind(mainMod .. " + L",           dsp.exec_cmd("hyprlock"))
bind("Print",                     dsp.exec_cmd('grim -g "$(slurp)" - | swappy -f -'))
bind(mainMod .. " + Print",       dsp.exec_cmd("grim - | wl-copy"))

for _, dir in ipairs({ "left", "right", "up", "down" }) do
    bind(mainMod .. " + " .. dir,           dsp.focus({ direction = dir }))
    bind(mainMod .. " + SHIFT + " .. dir,   dsp.window.move({ direction = dir }))
end

for i = 1, 10 do
    local key = i % 10
    bind(mainMod .. " + " .. key,           dsp.focus({ workspace = i }))
    bind(mainMod .. " + SHIFT + " .. key,   dsp.window.move({ workspace = i }))
end

bind(mainMod .. " + S",         dsp.workspace.toggle_special("magic"))
bind(mainMod .. " + SHIFT + S", dsp.window.move({ workspace = "special:magic" }))
bind(mainMod .. " + mouse_down", dsp.focus({ workspace = "e+1" }))
bind(mainMod .. " + mouse_up",   dsp.focus({ workspace = "e-1" }))
bind(mainMod .. " + mouse:272",  dsp.window.drag(),   { mouse = true })
bind(mainMod .. " + mouse:273",  dsp.window.resize(), { mouse = true })

local media = { locked = true, repeating = true }
bind("XF86AudioRaiseVolume",  dsp.exec_cmd("wpctl set-volume -l 1 @DEFAULT_AUDIO_SINK@ 5%+"), media)
bind("XF86AudioLowerVolume",  dsp.exec_cmd("wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%-"),      media)
bind("XF86AudioMute",         dsp.exec_cmd("wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle"),     media)
bind("XF86AudioMicMute",      dsp.exec_cmd("wpctl set-mute @DEFAULT_AUDIO_SOURCE@ toggle"),   media)
bind("XF86MonBrightnessUp",   dsp.exec_cmd("brightnessctl -e4 -n2 set 5%+"),                  media)
bind("XF86MonBrightnessDown", dsp.exec_cmd("brightnessctl -e4 -n2 set 5%-"),                  media)
bind("XF86AudioNext",  dsp.exec_cmd("playerctl next"),       { locked = true })
bind("XF86AudioPause", dsp.exec_cmd("playerctl play-pause"), { locked = true })
bind("XF86AudioPlay",  dsp.exec_cmd("playerctl play-pause"), { locked = true })
bind("XF86AudioPrev",  dsp.exec_cmd("playerctl previous"),   { locked = true })


---- WINDOW RULES ----
hl.window_rule({
    name  = "auxo-floating-tools",
    match = { class = "^(pavucontrol|org.pulseaudio.pavucontrol|blueman-manager|nm-connection-editor|auxo-welcome)$" },
    float = true,
    center = true,
})

hl.window_rule({
    name  = "auxo-pip",
    match = { title = "^(Picture-in-Picture)$" },
    float = true,
    pin   = true,
})

hl.window_rule({
    name  = "suppress-maximize-events",
    match = { class = ".*" },
    suppress_event = "maximize",
})

hl.window_rule({
    name  = "fix-xwayland-drags",
    match = { class = "^$", title = "^$", xwayland = true, float = true, fullscreen = false, pin = false },
    no_focus = true,
})

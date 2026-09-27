-- strict mock of the Hyprland 0.56.2 Lua API
local opts = {}
for l in io.lines(arg[2]) do opts[l] = true end
local errors = {}
local function err(m) errors[#errors+1] = m end
local TOP = {config=1,get_config=1,device=1,monitor=1,window_rule=1,layer_rule=1,workspace_rule=1,env=1,permission=1,load=1,gesture=1,curve=1,animation=1,on=1,bind=1,define_submap=1,timer=1,dispatch=1,version=1,exec_cmd=1,unbind=1,is_key_down=1}
local DSP = {exec_cmd=1,exec_raw=1,exit=1,submap=1,pass=1,send_shortcut=1,send_key_state=1,layout=1,dpms=1,event=1,global=1,force_renderer_reload=1,force_idle=1,release_input_capture=1,focus=1,no_op=1,
  window={close=1,kill=1,signal=1,float=1,fullscreen=1,fullscreen_state=1,pseudo=1,move=1,swap=1,center=1,cycle_next=1,tag=1,clear_tags=1,toggle_swallow=1,pin=1,bring_to_top=1,alter_zorder=1,set_prop=1,deny_from_group=1,drag=1,resize=1},
  workspace={rename=1,change_id=1,move=1,swap_monitors=1,toggle_special=1},
  group={toggle=1,next=1,prev=1,active=1,move_window=1,lock=1,lock_active=1}, cursor={move_to_corner=1,move=1}}
local EFFECTS = {} for w in ("float tile fullscreen maximize center pseudo no_initial_focus pin fullscreen_state move size monitor workspace group suppress_event content no_close_for scrolling_width rounding border_size rounding_power scroll_mouse scroll_touchpad animation idle_inhibit opacity tag max_size min_size border_color persistent_size allows_input dim_around decorate focus_on_activate keep_aspect_ratio nearest_neighbor no_anim no_blur no_dim no_focus no_follow_mouse no_max_size no_shadow no_shortcuts_inhibit opaque force_rgbx sync_fullscreen immediate xray render_unfocused no_screen_share no_vrr no_auto_hdr stay_focused confine_pointer tonemap name match"):gmatch("%S+") do EFFECTS[w]=true end
local function flatten(t, prefix)
  for k, v in pairs(t) do
    local key = prefix and (prefix .. ":" .. k) or k
    if k == "col" then for ck in pairs(v) do if not opts[key..":"..ck] and not opts[prefix..":col."..ck] then err("unknown option "..prefix..":col."..ck) end end
    elseif type(v) == "table" then flatten(v, key)
    elseif not opts[key] then err("unknown option " .. key) end
  end
end
local function dsp_table(spec, path)
  return setmetatable({}, {__index = function(_, k)
    local s = spec[k]
    if not s then err("unknown dispatcher " .. path .. "." .. k) return function() return {} end end
    if type(s) == "table" then return dsp_table(s, path .. "." .. k) end
    return function(...) return {dsp = path .. "." .. k} end
  end})
end
hl = setmetatable({ dsp = dsp_table(DSP, "hl.dsp") }, {__index = function(_, k)
  if not TOP[k] then err("unknown function hl." .. k) end
  if k == "config" then return function(t) flatten(t) end end
  if k == "window_rule" then return function(t) for f in pairs(t) do if not EFFECTS[f] then err("unknown window_rule field " .. f) end end end end
  if k == "bind" then return function(keys, action, o) if type(action) ~= "table" or not action.dsp then err("bind " .. keys .. " has no dispatcher") end end end
  if k == "on" then return function(ev, fn) if ev ~= "hyprland.start" and ev ~= "hyprland.shutdown" then err("unknown event " .. ev) end fn() end end
  return function() end
end})
local ok, e = pcall(dofile, arg[1])
if not ok then err("runtime: " .. tostring(e)) end
if #errors == 0 then print("hyprland.lua: OK against Hyprland 0.56.2 API") else for _, m in ipairs(errors) do print("ERROR: " .. m) end os.exit(1) end

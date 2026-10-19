-- Live check for the clip / event / transform host. Writes results with print
-- (OutputDebugString) and a "DONE" line at the end.
local log = {}
local function say(...) print(...) end

-- While the walk clip plays, actually run across the desktop with the
-- edge-bounce movement from scripts/bounce.lua. Collisions are omitted: this
-- demo has a single pet.
SPEED = 150        -- pixels per second
ROAM = true        -- true = the whole virtual desktop (including monitor gaps);
                   -- false = only the starting monitor's work area
FACE_RIGHT = true  -- walk.gif faces right; flip when running left
FLIP = true

local walking = false
local vx, vy
local home

local function face()
  if FLIP then yagp.set_flip((vx < 0) == FACE_RIGHT) end
end

-- Keep the visible area (hitbox) inside r; transparent window margins may
-- stick out.
local function clamp_to(r, x, y)
  local dx, dy, hw, hh = yagp.hitbox()
  x = math.max(r.x - dx, math.min(x, r.x + r.w - dx - hw))
  y = math.max(r.y - dy, math.min(y, r.y + r.h - dy - hh))
  return x, y
end

local function clamp_into(r)
  yagp.set_pos(clamp_to(r, yagp.pos()))
end

local function reset()
  home = yagp.monitor().work
  local x, y = yagp.pos()
  if not ROAM or not yagp.fits(x, y) then clamp_into(home) end
end

do -- initial direction, avoiding nearly horizontal/vertical angles
  local a = math.rad(25 + math.random() * 40) + (math.random(4) - 1) * math.pi / 2
  vx, vy = SPEED * math.cos(a), SPEED * math.sin(a)
end

local function step_home(nx, ny)
  local r = home
  local dx, dy, hw, hh = yagp.hitbox()
  local lo_x, hi_x = r.x - dx, math.max(r.x - dx, r.x + r.w - dx - hw)
  local lo_y, hi_y = r.y - dy, math.max(r.y - dy, r.y + r.h - dy - hh)
  if nx < lo_x then nx, vx = lo_x, math.abs(vx)
  elseif nx > hi_x then nx, vx = hi_x, -math.abs(vx) end
  if ny < lo_y then ny, vy = lo_y, math.abs(vy)
  elseif ny > hi_y then ny, vy = hi_y, -math.abs(vy) end
  return nx, ny
end

-- Cross-monitor mode: check each axis against every work area, including the
-- gaps between monitors.
local function step_roam(x, y, nx, ny)
  if not yagp.fits(nx, y) then vx, nx = -vx, x end
  if not yagp.fits(nx, ny) then vy, ny = -vy, y end
  return nx, ny
end

function update(dt)
  if not walking then return end
  local x, y = yagp.pos()
  local old_vx = vx
  local nx, ny = x + vx * dt, y + vy * dt
  if ROAM then nx, ny = step_roam(x, y, nx, ny)
  else nx, ny = step_home(nx, ny) end
  yagp.set_pos(nx, ny)
  if (vx < 0) ~= (old_vx < 0) then
    face()
    if not ROAM then yagp.set_pos(step_home(yagp.pos())) end
  end
end

on_moved = reset
on_screens_changed = reset

reset()
face()

assert(yagp.clip("walk", "walk.gif"))
assert(yagp.clip("sit", "sit.gif"))
local ok, err = yagp.clip("bad", "../x.gif")
say("bad path ->", ok, err)
local w0, h0 = yagp.size()
local x0, y0 = yagp.pos()
say("main size", w0, h0, "pos", x0, y0)

local phase = 0
local ends = 0
yagp.on("clip_end", function(name)
  ends = ends + 1
  local w, h = yagp.size()
  local x, y = yagp.pos()
  say("clip_end", name, "size", w, h, "bottom", x + w / 2, y + h)
end)

local frames_seen = {}
yagp.on("frame", function(f, clip) frames_seen[#frames_seen + 1] = clip .. f end, {clip = "walk", frames = {3, 5}})

yagp.after(0.5, function()
  yagp.play("walk")
  walking = true
  local w, h = yagp.size()
  local x, y = yagp.pos()
  say("walk size", w, h, "bottom", x + w / 2, y + h, "expect", x0 + w0 / 2, y0 + h0)
end)
yagp.after(1.6, function()
  say("walk frames seen", table.concat(frames_seen, ","))
  local info = yagp.clip_info("walk")
  say("walk info ready", info.ready, info.frames, info.duration)
  walking = false
  yagp.play("sit", {loop = false})
end)
yagp.after(3.0, function()
  local name, f, n = yagp.frame()
  say("after sit", name, f, n, "ends", ends)
  yagp.play("walk", {from = 4, to = 6})
  walking = true
end)
yagp.after(3.8, function()
  local name, f = yagp.frame()
  say("range frame", name, f)
  yagp.set_transform{sx = 1.3, sy = 0.7, duration = 0.3}
end)
local tend = 0
yagp.on("transform_end", function() tend = tend + 1 say("transform_end", tend) end)
yagp.after(4.5, function()
  local dx, dy, hw, hh = yagp.hitbox()
  say("hitbox after squash", dx, dy, hw, hh)
  yagp.reset_transform(0.2)
  yagp.set_opacity(0.5, 0.2)
end)
yagp.after(5.2, function()
  yagp.set_opacity(1, 0.3) -- 透明度测试结束：恢复不透明
  local ok2 = yagp.seek(2)
  local name, f = yagp.frame()
  say("seek", ok2, name, f)
  local cnt = 0
  local h = yagp.every(0.1, function() cnt = cnt + 1 end)
  yagp.after(1.05, function() yagp.cancel(h) say("every count", cnt) say("DONE") end)
end)

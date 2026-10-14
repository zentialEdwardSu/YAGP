-- 边缘反弹：沿固定速度直线运动，碰到边缘时对应方向反向。
-- COLLIDE = true 时也会在其他 GIF 上反弹（没有脚本的 GIF 相当于一堵墙）。
-- 边缘和碰撞都按 GIF 的可见区域（去掉透明边后的矩形）判断。
-- 坐标均为物理像素，指 GIF 窗口左上角。

SPEED = 150       -- 像素/秒
ROAM = false      -- false = 只在起始显示器的工作区内反弹；
                  -- true  = 可跨越相邻显示器，在显示器之间的空隙和外缘处反弹
COLLIDE = true    -- true = 碰到其他 GIF 也反弹；false = 互相穿过
FACE_RIGHT = true -- GIF 原图朝右；向左走时水平翻转。原图朝左请设为 false
FLIP = true       -- false = 不翻转

local vx, vy
local home -- 非 ROAM 模式的活动范围：工作区

local function face()
  if FLIP then yagp.set_flip((vx < 0) == FACE_RIGHT) end
end

-- 让 GIF 的可见区域（yagp.hitbox，去掉透明边后的矩形）留在 r 内，
-- 窗口里透明的部分可以伸出 r。
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

do -- 随机初始方向，避开接近水平或垂直的角度。
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

-- 跨屏：分别检测两个方向，下一步越出所有工作区（包括显示器之间的空隙）就反向。
local function step_roam(x, y, nx, ny)
  if not yagp.fits(nx, y) then vx, nx = -vx, x end
  if not yagp.fits(nx, ny) then vy, ny = -vy, y end
  return nx, ny
end

-- 与其他 GIF 的碰撞，同样分轴检测。已经重叠的 GIF（比如刚好叠在一起生成）
-- 不算阻挡，而是把速度转向远离它的方向，让两者自然分开。
local function step_collide(x, y, nx, ny, dt)
  local touching = {}
  local cx, cy = yagp.center()
  for _, p in ipairs(yagp.overlaps()) do
    touching[p.id] = true
    -- 中心重合时按 id 决定方向，否则两个 GIF 会朝同一边走、永远分不开。
    local left = cx < p.cx or (cx == p.cx and yagp.id < p.id)
    local up = cy < p.cy or (cy == p.cy and yagp.id < p.id)
    vx = left and -math.abs(vx) or math.abs(vx)
    vy = up and -math.abs(vy) or math.abs(vy)
  end
  if next(touching) then return x + vx * dt, y + vy * dt end

  local function blocked(tx, ty)
    for _, p in ipairs(yagp.overlaps(tx, ty)) do
      if not touching[p.id] then return true end
    end
    return false
  end
  if blocked(nx, y) then vx, nx = -vx, x end
  if blocked(nx, ny) then vy, ny = -vy, y end
  return nx, ny
end

function update(dt)
  local x, y = yagp.pos()
  local old_vx = vx
  local nx, ny = x + vx * dt, y + vy * dt
  if ROAM then nx, ny = step_roam(x, y, nx, ny)
  else nx, ny = step_home(nx, ny) end
  if COLLIDE then
    nx, ny = step_collide(x, y, nx, ny, dt)
    -- 分开时的推动也要遵守边界。
    if ROAM then
      if not yagp.fits(nx, ny) then nx, ny = x, y end
    else
      nx, ny = step_home(nx, ny)
    end
  end
  yagp.set_pos(nx, ny)
  if (vx < 0) ~= (old_vx < 0) then
    face()
    -- 翻转会镜像可见区域；可见部分不在正中的 GIF 需要再夹紧一次。
    if not ROAM then yagp.set_pos(step_home(yagp.pos())) end
  end
end

on_moved = reset
on_screens_changed = reset

reset()
face()

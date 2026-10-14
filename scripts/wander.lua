-- 随机游走：在当前显示器的工作区里随机选一个目标点，匀速走过去，停一会儿再选下一个。
-- 坐标均为物理像素，指 GIF 窗口左上角。

SPEED = 90                    -- 像素/秒
PAUSE_MIN, PAUSE_MAX = 0.5, 3 -- 到达后停顿的秒数范围
FACE_RIGHT = true             -- GIF 原图朝右；向左走时水平翻转。原图朝左请设为 false
FLIP = true                   -- false = 不翻转

local tx, ty = nil, nil
local wait = 0
local home -- 活动范围：工作区

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
  clamp_into(home)
  tx, ty = nil, nil
end

-- 目标点让可见区域落在工作区内。
local function pick()
  local dx, dy, hw, hh = yagp.hitbox()
  tx = home.x - dx + math.random() * math.max(0, home.w - hw)
  ty = home.y - dy + math.random() * math.max(0, home.h - hh)
end

function update(dt)
  if wait > 0 then
    wait = wait - dt
    return
  end
  if not tx then pick() end
  local x, y = yagp.pos()
  local dx, dy = tx - x, ty - y
  local d = math.sqrt(dx * dx + dy * dy)
  local step = SPEED * dt
  if FLIP and math.abs(dx) > 1 then yagp.set_flip((dx < 0) == FACE_RIGHT) end
  -- 翻转会镜像可见区域；目标点按当前朝向重新夹紧。
  tx, ty = clamp_to(home, tx, ty)
  if d <= step then
    yagp.set_pos(tx, ty)
    tx, ty = nil, nil
    wait = PAUSE_MIN + math.random() * (PAUSE_MAX - PAUSE_MIN)
  else
    yagp.set_pos(x + dx / d * step, y + dy / d * step)
  end
end

on_moved = reset
on_screens_changed = reset

reset()

-- 左右巡逻：在 GIF 所在显示器工作区的整个宽度内水平往返，高度保持不变。
-- 坐标均为物理像素，指 GIF 窗口左上角。拖到另一块显示器后就在那块屏幕上巡逻。

SPEED = 120       -- 像素/秒
FACE_RIGHT = true -- GIF 原图朝右；向左走时水平翻转。原图朝左请设为 false
FLIP = true       -- false = 不翻转

local work, y, dir = nil, nil, 1

local function face()
  if FLIP then yagp.set_flip((dir < 0) == FACE_RIGHT) end
end

-- 让 GIF 的可见区域（yagp.hitbox，去掉透明边后的矩形）留在 r 内，
-- 窗口里透明的部分可以伸出 r。
local function clamp_to(r, x, y)
  local dx, dy, hw, hh = yagp.hitbox()
  x = math.max(r.x - dx, math.min(x, r.x + r.w - dx - hw))
  y = math.max(r.y - dy, math.min(y, r.y + r.h - dy - hh))
  return x, y
end

local function reset()
  local x
  x, y = yagp.pos()
  work = yagp.monitor().work
  -- 被拖到屏幕外或显示器被拔掉时，先拉回可见区域。
  x, y = clamp_to(work, x, y)
  yagp.set_pos(x, y)
  face()
end

function update(dt)
  local x = yagp.pos()
  -- 可见区域贴着工作区左右边缘转身，窗口里透明的部分可以伸出屏幕。
  local dx, _, hw = yagp.hitbox()
  local lo, hi = work.x - dx, math.max(work.x - dx, work.x + work.w - dx - hw)
  x = x + dir * SPEED * dt
  if x >= hi then x, dir = hi, -1; face()
  elseif x <= lo then x, dir = lo, 1; face() end
  yagp.set_pos(x, y)
end

on_moved = reset
on_screens_changed = reset

reset()

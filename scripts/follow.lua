-- 跟随鼠标：GIF 中心向鼠标靠拢，离得越远走得越快，到达 STOP 距离内就停下。
-- 鼠标在其他显示器上时也会跟过去；遇到显示器之间的空隙会沿边缘滑行绕过。
-- 坐标均为物理像素。

SPEED = 260       -- 最大速度（像素/秒）
STOP = 60         -- GIF 中心与鼠标相距小于此值时不再靠近
EASE = 4          -- 越大越快贴近（速度 = 距离 * EASE，上限 SPEED）
FACE_RIGHT = true -- GIF 原图朝右；向左走时水平翻转。原图朝左请设为 false
FLIP = true       -- false = 不翻转

local function face(dx)
  if FLIP and math.abs(dx) > 2 then yagp.set_flip((dx < 0) == FACE_RIGHT) end
end

-- 让 GIF 的可见区域（yagp.hitbox，去掉透明边后的矩形）留在 work 内，
-- 窗口里透明的部分可以伸出去。
local function clamp(x, y, work)
  local dx, dy, hw, hh = yagp.hitbox()
  x = math.max(work.x - dx, math.min(x, work.x + work.w - dx - hw))
  y = math.max(work.y - dy, math.min(y, work.y + work.h - dy - hh))
  return x, y
end

function update(dt)
  local mx, my, valid = yagp.cursor()
  if not valid then return end -- 锁屏等情况下读不到鼠标
  local cx, cy = yagp.center()
  local dx, dy = mx - cx, my - cy
  local d = math.sqrt(dx * dx + dy * dy)
  if d <= STOP then return end
  local step = math.min(SPEED, (d - STOP) * EASE) * dt
  if step > d - STOP then step = d - STOP end
  local x, y = yagp.pos()
  local nx, ny = x + dx / d * step, y + dy / d * step
  -- 整步越界时分别尝试只走水平 / 只走垂直，沿工作区边缘滑行。
  if not yagp.fits(nx, ny) then
    if yagp.fits(nx, y) then ny = y
    elseif yagp.fits(x, ny) then nx = x
    else return end
  end
  yagp.set_pos(nx, ny)
  face(dx)
end

local function reset()
  local x, y = yagp.pos()
  if not yagp.fits(x, y) then yagp.set_pos(clamp(x, y, yagp.monitor().work)) end
end

on_moved = reset
on_screens_changed = reset

reset()

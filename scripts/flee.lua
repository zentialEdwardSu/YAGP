-- 躲避鼠标：鼠标靠近到 RADIUS 以内时 GIF 逃开，越近逃得越快；鼠标离开后慢慢回到原位。
-- 被逼到工作区边缘时沿边缘滑开，逃不动（被困在角落）时跳到工作区另一侧。
-- 鼠标不在附近、GIF 也已回到原位时不运行任何代码：只订阅"鼠标进入 RADIUS 范围"事件，
-- 进入后才开始逐帧更新。坐标均为物理像素。

RADIUS = 220      -- 开始躲避的距离（GIF 中心到鼠标）
SPEED = 700       -- 最大逃跑速度（像素/秒）
RETURN = true     -- 鼠标离开后是否回到原位
RETURN_SPEED = 80 -- 回原位的速度（像素/秒）
RETURN_DELAY = 1.5 -- 鼠标离开多少秒后开始回原位
FACE_RIGHT = true -- GIF 原图朝右；向左走时水平翻转。原图朝左请设为 false
FLIP = true       -- false = 不翻转

local home_x, home_y
local calm = 0     -- 鼠标离开后的秒数
local stuck = 0    -- 连续逃不动的秒数

local function face(dx)
  if FLIP and math.abs(dx) > 1 then yagp.set_flip((dx < 0) == FACE_RIGHT) end
end

-- 让 GIF 的可见区域（yagp.hitbox，去掉透明边后的矩形）留在 work 内，
-- 窗口里透明的部分可以伸出去。
local function clamp(x, y, work)
  local dx, dy, hw, hh = yagp.hitbox()
  x = math.max(work.x - dx, math.min(x, work.x + work.w - dx - hw))
  y = math.max(work.y - dy, math.min(y, work.y + work.h - dy - hh))
  return x, y
end

local function reset()
  local x, y = yagp.pos()
  home_x, home_y = clamp(x, y, yagp.monitor().work)
  yagp.set_pos(home_x, home_y)
  calm, stuck = 0, 0
end

-- 困在角落：跳到工作区里离鼠标最远的那一侧。
local function escape(work, mx, my)
  local dx, dy, hw, hh = yagp.hitbox()
  local x = (mx < work.x + work.w / 2) and (work.x + work.w - dx - hw) or (work.x - dx)
  local y = (my < work.y + work.h / 2) and (work.y + work.h - dy - hh) or (work.y - dy)
  yagp.set_pos(x, y)
  face(x + dx + hw / 2 - mx)
end

local function step(dt)
  local mx, my, valid = yagp.cursor()
  if not valid then return end
  local work = yagp.monitor().work
  local cx, cy = yagp.center()
  local dx, dy = cx - mx, cy - my
  local d = math.sqrt(dx * dx + dy * dy)
  local x, y = yagp.pos()

  if d < RADIUS then
    calm = 0
    if d < 1 then dx, dy, d = 1, 0, 1 end -- 鼠标正好在中心：随便选个方向
    local speed = SPEED * (1 - d / RADIUS) + SPEED * 0.15
    local nx, ny = clamp(x + dx / d * speed * dt, y + dy / d * speed * dt, work)
    local moved = math.abs(nx - x) + math.abs(ny - y)
    if moved < speed * dt * 0.2 then
      stuck = stuck + dt
      if stuck > 0.3 then escape(work, mx, my); stuck = 0; return end
    else
      stuck = 0
    end
    yagp.set_pos(nx, ny)
    face(dx)
    return
  end

  stuck = 0
  if not RETURN then update = nil return end -- 安全了：停止逐帧更新，等鼠标再靠近
  calm = calm + dt
  if calm < RETURN_DELAY then return end
  local hx, hy = home_x - x, home_y - y
  local hd = math.sqrt(hx * hx + hy * hy)
  if hd < 0.5 then update = nil return end -- 已回到原位
  -- 回家的路上不要撞进鼠标的躲避范围：等鼠标离开再走。
  local step = math.min(hd, RETURN_SPEED * dt)
  local nx, ny = x + hx / hd * step, y + hy / hd * step
  local hdx, hdy, hw, hh = yagp.hitbox()
  local ncx, ncy = nx + hdx + hw / 2, ny + hdy + hh / 2
  if (ncx - mx) ^ 2 + (ncy - my) ^ 2 < RADIUS * RADIUS then return end
  yagp.set_pos(nx, ny)
  face(hx)
end

-- 鼠标进入 RADIUS 范围（距离由宿主按可见区域中心计算）时恢复逐帧更新。
local function wake() update = step end
yagp.on("cursor", function(e) if e.near then wake() end end, {radius = RADIUS})

-- 用户拖动后，新位置就是新的“家”。
on_moved = function() reset() wake() end
on_screens_changed = on_moved

reset()
update = step

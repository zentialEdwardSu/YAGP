-- 看向鼠标：按鼠标相对 GIF 的方向播放预制的方向动画，点击时播放一次反应动画。
-- 需要在主 GIF 所在目录放好这些文件（名字可在下面修改）：
--   look_0.gif ... look_7.gif   8 个方向，0 = 右，按顺时针：1 = 右下，2 = 下 ... 6 = 上，7 = 右上
--   react.gif                    被点击时播放一次（可选）
-- 缺少方向动画时会退回主 GIF，并在 DebugView 里打印提示。
-- 坐标均为物理像素。

DIRS = 8               -- 方向数：8，或 4（0 = 右，1 = 下，2 = 左，3 = 上）
PATTERN = "look_%d.gif" -- 方向动画的文件名，%d 为方向编号
REACT = "react.gif"    -- 点击反应动画，设为 nil 关闭
DEAD_ZONE = 40         -- 鼠标离 GIF 中心不到这么远时不转向（像素）
MIN_MOVE = 8           -- 鼠标移动超过这么多像素才重新计算方向

local have_dirs = true
local dir = nil        -- 当前方向
local reacting = false

for i = 0, DIRS - 1 do
  local ok, err = yagp.clip("d" .. i, string.format(PATTERN, i))
  if not ok then
    have_dirs = false
    print("look.lua: " .. string.format(PATTERN, i) .. ": " .. tostring(err))
    break
  end
end
local have_react = REACT ~= nil and yagp.clip("react", REACT) == true

local function direction(mx, my)
  local cx, cy = yagp.center()
  local dx, dy = mx - cx, my - cy
  if dx * dx + dy * dy < DEAD_ZONE * DEAD_ZONE then return nil end
  -- 屏幕坐标 y 向下，所以 atan(dy, dx) 顺时针增大，0 = 右。
  local a = math.atan(dy, dx)
  local step = 2 * math.pi / DIRS
  return math.floor((a + step / 2) / step) % DIRS
end

local function look(mx, my)
  if not have_dirs or reacting then return end
  local d = direction(mx, my)
  if d == nil or d == dir then return end
  dir = d
  -- sync：保持动画进度，转向时不从第一帧重新开始。
  yagp.play("d" .. d, {sync = true})
end

-- 只订阅鼠标移动：鼠标不动时脚本完全不运行。
yagp.on("cursor", function(e) if e.valid then look(e.x, e.y) end end, {min_move = MIN_MOVE})

yagp.on("click", function(e)
  if e.button ~= "left" or not have_react or reacting then return end
  reacting = true
  yagp.play("react", {loop = false})
  return true -- 已处理：不再交给默认行为（左键单击本来也没有默认行为）
end)

yagp.on("clip_end", function(name)
  if name ~= "react" then return end
  reacting = false
  dir = nil
  local x, y = yagp.cursor()
  look(x, y)
end)

-- 拖动、缩放或更换 GIF 后，按当前鼠标位置重新转向。
yagp.on("moved", function() dir = nil local x, y = yagp.cursor() look(x, y) end)

local x, y = yagp.cursor()
look(x, y)

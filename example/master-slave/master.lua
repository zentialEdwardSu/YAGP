-- Master demo: move itself, and when a slave is bound control it through the
-- chainable yagp.slaves() handle. Binding is done from the tray menu.
local home_x, home_y = yagp.pos()
local t = 0
local last_notify = 0
local last_log = 0

yagp.notify("YAGP demo", "Master demo: right-click the slave and choose Belongs to…")

local function clamp(x, y, w, h)
  local work = yagp.monitor().work
  x = math.max(work.x, math.min(x, work.x + work.w - w))
  y = math.max(work.y, math.min(y, work.y + work.h - h))
  return x, y
end

local function move_self(dt)
  t = t + dt
  local w, h = yagp.size()
  local x, y = clamp(home_x + math.sin(t * 0.5) * 140,
                     home_y + math.cos(t * 0.45) * 50, w, h)
  yagp.set_pos(x, y)
end

function update(dt)
  move_self(dt)
  local slaves = yagp.slaves()
  if #slaves == 0 then
    if t - last_notify > 6 then
      last_notify = t
      yagp.notify("YAGP demo", "Master: bind the slave via Belongs to…")
    end
    return
  end

  local s = slaves[1]
  local mx, my = yagp.pos()
  local sw, sh = s:size()
  local tx, ty = clamp(mx + math.cos(t * 1.2) * 90,
                       my + math.sin(t * 1.2) * 70, sw, sh)
  s:set_pos(tx, ty)
   :set_opacity(0.65 + 0.2 * math.sin(t * 1.5))
   :set_flip(math.cos(t * 1.2) < 0)
   :set_playing(true)
   :set_transform{angle = 10 * math.sin(t * 1.4), duration = 0.2}

  if t - last_log > 8 then
    last_log = t
    log.info("master demo: controlling slave", s:id(), s:name())
  end
end

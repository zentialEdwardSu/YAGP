log.info("GROUP MEMBER pet1.gif")
local x0, y0 = yagp.pos()
local t = 0

function on_moved()
  x0, y0 = yagp.pos()
end

function update(dt)
  t = t + dt
  local work = yagp.monitor().work
  local w, h = yagp.size()
  local x = x0 + math.sin(t * 0.8) * 70
  local y = y0
  x = math.max(work.x, math.min(x, work.x + work.w - w))
  y = math.max(work.y, math.min(y, work.y + work.h - h))
  yagp.set_pos(x, y)
end

-- JPEG static-image demo: opaque image, movement and rotation.
local x0, y0 = yagp.pos()
local t = 0
local last_notify = 0

function on_moved()
  x0, y0 = yagp.pos()
end

local ok, err = yagp.clip("thumb", "file.png")
log.info("JPEG static demo: clip ->", tostring(ok), tostring(err))
yagp.notify("YAGP demo", "JPEG static image: no alpha, no animation controls")

function update(dt)
  t = t + dt
  local work = yagp.monitor().work
  local w, h = yagp.size()
  local x = x0 + math.cos(t * 0.4) * 120
  local y = y0 + math.sin(t * 0.6) * 50
  x = math.max(work.x, math.min(x, work.x + work.w - w))
  y = math.max(work.y, math.min(y, work.y + work.h - h))
  yagp.set_pos(x, y)
  yagp.set_opacity(0.8 + 0.15 * math.sin(t * 0.9))
  yagp.set_transform{angle = 6 * math.sin(t * 0.8), duration = 0.3}
  if t - last_notify > 18 then
    last_notify = t
    yagp.notify("YAGP demo", "JPEG static image: opaque, moved and rotated by Lua")
  end
end

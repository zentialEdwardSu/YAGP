-- PNG static-image demo: movement + opacity + transform, no animation.
local x0, y0 = yagp.pos()
local t = 0
local last_notify = 0

function on_moved()
  x0, y0 = yagp.pos()
end

local ok, err = yagp.play("main")
log.info("PNG static demo: play ->", tostring(ok), tostring(err))
local dx, dy, hw, hh = yagp.hitbox()
log.info("PNG static demo: hitbox", dx, dy, hw, hh)
yagp.notify("YAGP demo", "PNG static image: playback controls are unavailable")

function update(dt)
  t = t + dt
  local work = yagp.monitor().work
  local w, h = yagp.size()
  local x = x0 + math.sin(t * 0.35) * 150
  local y = y0 + math.sin(t * 0.55) * 60
  x = math.max(work.x, math.min(x, work.x + work.w - w))
  y = math.max(work.y, math.min(y, work.y + work.h - h))
  yagp.set_pos(x, y)
  yagp.set_opacity(0.65 + 0.25 * math.sin(t * 0.7))
  yagp.set_transform{sx = 1 + 0.05 * math.sin(t * 1.1),
                     sy = 1 - 0.04 * math.sin(t * 1.1), duration = 0.2}
  if t - last_notify > 15 then
    last_notify = t
    yagp.notify("YAGP demo", "PNG static image: motion and transforms are script-driven")
  end
end

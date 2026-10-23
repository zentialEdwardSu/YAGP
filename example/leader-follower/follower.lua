-- Follower demo: this output stops while the GIF is bound to a leader, and appears
-- again after the relation is removed.
log.info("FOLLOWER SCRIPT RAN")
yagp.notify("YAGP demo", "Follower script is running; bind it via Belongs to…")

local x0, y0 = yagp.pos()
local t = 0

function update(dt)
  t = t + dt
  yagp.set_pos(x0 + math.sin(t * 0.9) * 55, y0 + math.cos(t * 1.1) * 35)
  yagp.set_opacity(0.8 + 0.15 * math.sin(t * 1.4))
end

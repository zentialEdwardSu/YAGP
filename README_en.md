# YAGP: Yet-Another-Gif-Player

[中文](README.md) | English

Shows animated GIFs as borderless, transparent desktop overlays with no taskbar
or Alt+Tab entry. It lives in the notification area and avoids GDI,
UpdateLayeredWindow, SDL, and shader compilation entirely:

`giflib LZW -> premultiplied BGRA canvas (CPU) -> UpdateSubresource into the
swap-chain back buffer -> composition swap chain -> DirectComposition visual -> DWM`

Each GIF can also be moved around the desktop by a sandboxed Lua script
(patrol, wander, bounce, follow or avoid the mouse, or your own), with full
multi-monitor support.

Menu names below are those of the English build (`-DYAGP_LANG=en`). The
default build shows the same entries in Chinese.

## Build

Windows 10/11, Visual Studio 2022+ (MSVC), CMake 3.25+. giflib 5.2.2 and
Lua 5.4.7 (both MIT) are pulled in by `FetchContent` at configure time
(SHA-256 pinned). For offline builds pass
`-DFETCHCONTENT_SOURCE_DIR_GIFLIB=<dir>` / `-DFETCHCONTENT_SOURCE_DIR_LUA=<dir>`.

From any shell (no VS developer prompt needed):

```powershell
cmake --preset release            # Visual Studio generator, x64, into out\
cmake --build --preset release    # yagp, script_test, gif_dump
.\out\Release\yagp.exe [file.gif ...]
```

The exe is self-contained (static CRT, icon and Lua presets embedded).

- `release-en` builds the English UI into `out-en\`.
- The presets name no generator, so CMake uses the newest installed Visual
  Studio and finds MSVC and the Windows SDK by itself.
- Ninja still works, but only from a developer shell. Without one, CMake would
  pick up whatever `link`/`ld` is first on PATH (e.g. a MinGW `ld.exe`), so the
  configure step stops with a hint instead.

### Language

The UI language is chosen at build time: `-DYAGP_LANG=zh_CN` (default) or
`-DYAGP_LANG=en`. All user-visible strings live in `i18n.h` as `TXT_*` macros
(`TXT_U8_*` for UTF-8 messages raised inside Lua). To add a language, copy a
block there, translate it, and add the value to `YAGP_LANG` in `CMakeLists.txt`.
Every block must define every macro, so a missing translation is a compile
error. Keep printf specifiers and `\0` file-filter separators intact.

### Icon

`app.ico` (16-256 px, rounded corners) is generated from `specturebot.png`:

```powershell
python tools/make_icon.py    # needs Pillow; edit RADIUS in the script to change the corners
```

## Usage

- Tray icon (left or right click): add GIFs (multi-select), a per-GIF submenu,
  pause/resume all, start with Windows, delete settings data, exit.
  GIFs with a motion script are tagged "(moving)" in the tray menu.
- Right-click a GIF (or its tray submenu): hide/show, plus pause/resume, replace, scale, colour key, motion script,
  always on top, lock position, click-through, move to screen centre, remove.
- Scale: a slider from 5% to 250% with live preview. You can also type a value
  or use the 100% button. Arrow keys move 1%, PgUp/PgDn 25%. Cancel restores
  the previous size. The GIF stays centred on the same point while scaling.
- Colour key: enter a hex colour (`#00FF00`, `00ff00`, `#0F0`) and a per-channel
  tolerance (0-255, default 16), or use the first frame's top-left pixel colour.
  Matching pixels are made transparent during compositing.
- Drag a GIF with the left mouse button unless it is locked.
- A click-through GIF ignores the mouse; turn it off from the tray submenu.
- Hide takes a GIF off the screen without removing it. It stays in the tray
  menu (tagged "(hidden)"); "Show" in its submenu brings it back with the same
  position, scale, colour key and script. While hidden it does not animate or
  run its script, other scripts do not see it, and its swap chain and canvas
  are freed. The hidden state is saved across restarts.
- Running `yagp.exe file.gif` again adds the file to the running instance.
- "Start with Windows" writes `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\YAGP`
  (per-user, no admin rights). It shows as checked only if the entry points at
  the running exe. Startup is silent: no notification is shown.
- State is saved to `%APPDATA%\YAGP\config.txt` (1 s debounce, and on exit),
  including each GIF's script binding. "Delete settings data" removes
  `%APPDATA%\YAGP`, the frame cache in `%LOCALAPPDATA%\YAGP` and the startup
  entry; after that nothing is saved until the next launch. Your own `.lua`
  files are never touched.

## Motion scripts

Right-click a GIF → Motion script:

| Entry | |
|---|---|
| None | No script |
| Patrol left/right | Walks left and right across the full width of its monitor's work area |
| Wander | Walks to random points on its monitor, pausing in between |
| Bounce off edges | Moves diagonally and bounces off the work-area edges and off other GIFs (a GIF without a script acts as a wall; `COLLIDE = false` turns this off). `ROAM = true` lets it cross monitors and bounce off the gaps between them |
| Follow mouse | Walks towards the cursor and stops `STOP` px away; follows across monitors, sliding along edges around gaps |
| Avoid mouse | Runs away when the cursor comes within `RADIUS`, jumps clear when cornered, drifts back home after the cursor leaves |
| Choose script file… | Bind any `.lua` file |
| New script from preset | Save a copy of a preset (default `Documents\my_<preset>.lua`), open it in an editor, and bind it |
| Reload script | Re-read the bound script after editing it |

- Dragging a GIF suspends its script; on release the script continues from the
  new position (the "home" for Avoid mouse). Pausing the GIF pauses the motion
  too.
- Presets turn the GIF to face its direction of travel. Set `FACE_RIGHT = false`
  if the GIF faces left, or `FLIP = false` to disable mirroring.
- If a monitor is unplugged or the taskbar moves, scripts pull their GIF back
  into view.
- Scripts stay where you saved them; the config stores the absolute path. If
  the file is moved away, the binding is dropped silently on the next start
  and the GIF stays still.
- "New script from preset" opens the file with the system's "edit" verb
  (Notepad as a fallback), never "open", so a `.lua` association with an
  interpreter cannot run it.
- Any error stops the script, leaves the GIF where it is, and shows the
  message with its line number as a tray notification.

### Writing a script

The built-in presets are in `scripts/*.lua` and double as examples (their
comments are in Chinese). Tunables such as `SPEED`, `ROAM`, `RADIUS` and
`FACE_RIGHT` are globals at the top.

The top level runs once on load. After that the global `update(dt)` is called
at the script's rate (`dt` in seconds, capped at 0.25). Optional callbacks:
`on_moved()` after the user drags, re-centres, rescales or replaces the GIF,
and `on_screens_changed()` after monitors or work areas change.
`on_collide(other)` is called once when this GIF's visible area starts overlapping
another GIF's; `other` is that GIF's peer table (same fields as
`yagp.others()`). `on_separate(other)` is called when they stop overlapping;
if the other GIF was removed, `other` only has its `id`.

All coordinates are physical pixels on the virtual desktop (negative left of /
above the primary monitor). Positions refer to the window's top-left corner.

| Function | |
|---|---|
| `yagp.pos()` → x, y | Current position (sub-pixel, as last set) |
| `yagp.set_pos(x, y)`, `yagp.move(dx, dy)` | Move the GIF |
| `yagp.size()` → w, h | Size including scale |
| `yagp.center()` → x, y | Centre of the visible area |
| `yagp.hitbox()` → dx, dy, w, h | Visible area relative to `yagp.pos()`, display pixels (scale and flip applied) |
| `yagp.set_hitbox(x, y, w, h)`, `yagp.set_hitbox(nil)` | Override the visible area in GIF pixels (follows scale and flip); nil = automatic again |
| `yagp.monitors()` | `{ {x, y, w, h, work = {x, y, w, h}, dpi, primary, index}, ... }` |
| `yagp.monitor()` → m, index | Monitor under the GIF's centre (or nearest) |
| `yagp.monitor_at(x, y)` → m, index | Monitor containing a point (or nearest) |
| `yagp.desktop()` → x, y, w, h | Bounding box of all monitors |
| `yagp.fits(x, y [, w, h])` | With the window at x, y, the visible area lies on the work areas; with an explicit w, h, that rect at x, y does. Gaps between monitors count as outside |
| `yagp.cursor()` → x, y, valid | Mouse position. `valid` is false when it cannot be read (lock screen, UAC prompt); x, y are then the last known position |
| `yagp.mouse()` | `{x, y, left, right, middle, over, valid, monitor}`: logical buttons (left-handed swap applied), `over` = cursor on the GIF, `monitor` = index |
| `yagp.time()` | Monotonic seconds |
| `yagp.set_flip(bool)` | Mirror horizontally |
| `yagp.set_rate(hz)` | `update` calls per second, 1-144, default 60 |
| `yagp.name` | GIF file name |
| `yagp.id` | This GIF's id, unique for this run (changes on restart) |
| `yagp.others()` | Every other GIF: `{ {id, name, x, y, w, h, hx, hy, hw, hh, cx, cy, vx, vy, scripted, paused}, ... }`, including ones without a script and paused ones. `x, y, w, h` is the window, `hx, hy, hw, hh` the visible area, `cx, cy` its centre. `vx`, `vy` are px/s over its last tick, 0 when not moving |
| `yagp.peer(id)` | One GIF's peer table, or nil |
| `yagp.overlaps([x, y [, w, h]])` | Other GIFs whose visible area overlaps this GIF's. No arguments: where it is now; x, y: with the window moved there; explicit w, h: that rect at x, y. Sharing an edge is not an overlap |

Other GIFs are read-only: a script cannot move them or message them.
A script sees at most 64 other GIFs.

**Visible area (hitbox).** Edges and collisions use the GIF's visible area,
not its whole window: the bounding box of every non-transparent pixel over all
frames (after the colour key). It is computed while the frame cache is built,
so it costs nothing at runtime. `fits`, `overlaps`, `center`, `monitor`,
`mouse().over`, `on_collide` and all presets use it, so a GIF with a wide
transparent margin bounces with its visible part against screen edges and
other GIFs, and the transparent part may hang over the edge. `yagp.pos`,
`size` and `set_pos` still refer to the whole window. A fully transparent GIF
uses the whole window. If the automatic box is wrong (say, a faint shadow that
should not collide), set one with `yagp.set_hitbox`; it resets to automatic
when the script or the GIF is replaced.

A minimal script:

```lua
SPEED = 100
function update(dt)
  local x, y = yagp.pos()
  local work = yagp.monitor().work
  local w = yagp.size()
  x = x + SPEED * dt
  if x > work.x + work.w - w then x = work.x end -- wrap around
  yagp.set_pos(x, y)
end
```

### Sandbox

- Only `base`, `math`, `string`, `table`, `utf8` and `coroutine` are compiled
  into the binary. `io`, `os`, `package` and `debug` do not exist.
- `load`, `loadfile`, `dofile`, `require`, `collectgarbage` and `string.dump`
  are removed, and bytecode is rejected.
- Each script's Lua state is capped at 4 MB.
- A single call is stopped after about 10 million instructions, so an endless
  loop cannot freeze the UI thread, even inside `pcall`.
- `print` goes to `OutputDebugString` (view it with DebugView).

## Design notes

- File dialogs run in a short-lived helper process (`yagp.exe --pick
  add|replace|script <id>`, or `--pick newscript <id> <preset> <name>`), which
  sends the chosen path back with `WM_COPYDATA` and exits. In-process, the
  dialog loaded about 100 shell DLLs, third-party shell extensions and about 20
  threads, none of which unload, raising the tray from 3 MB to 18 MB private.
  With the helper the tray stays at about 3.5 MB. Results carry a random id, so
  stale or forged messages are ignored.
- The scale slider is a small self-drawn control, not the comctl32 Trackbar.
- One D3D11 device and one DirectComposition device are shared by all overlays.
  Each overlay owns only a 2-buffer flip-model composition swap chain.
- The D3D11 device is created on WARP (the software rasterizer) first, falling
  back to hardware. The GPU does no work here: frames are CPU-composited and
  copied into the back buffer, and DWM does the composition and scaling. Loading
  a hardware user-mode driver costs 20-60 MB of private memory per process
  (measured on AMD); WARP costs about 1 MB. Swap-chain buffers are still handed
  to DWM as normal composition surfaces.
- The colour key is resolved per palette entry (at most 256 comparisons per
  frame), so compositing stays one table lookup per pixel. Changing the key
  re-decodes from frame 0 so pixels already on the canvas are re-keyed too.
- When a GIF is added, it is decoded once, synchronously, to build a cache:
  a per-frame bounding box of changed pixels. It is stored in
  `%LOCALAPPDATA%\YAGP\cache\<hash>.ygc` and is keyed by file size,
  modification time and colour key, so an edited GIF is rebuilt automatically.
  Playback uploads only the changed box (plus the previous one, because
  flip-model buffers alternate), and skips identical frames entirely.
  `Present1` dirty rects were measured to cost more than they save under WARP,
  so presents stay full. For a 480×360, 40-frame GIF the cache file is 22 KB.
  A test process that builds it took 21 ms, and one that loads it took 8.5 ms
  (both including process start).
- GIF alpha is 0 or 255, so premultiplied and straight alpha are identical for
  every pixel. The canvas goes straight into the back buffer: no frame texture,
  no shaders, no `d3dcompiler` dependency.
- Upscaling (100% and above) and the facing flip are a DComp visual transform
  with nearest-neighbour sampling; the buffers stay at native size, so zooming
  in costs no extra memory or CPU.
- Below 100% the canvas and swap chain are built at the displayed size: each
  canvas pixel takes the source pixel nearest its centre while compositing,
  which is pixel-identical to compositing full size and then downscaling with
  nearest neighbour. A 1000×1000 GIF at 20% keeps a 156 KB canvas instead of
  3.9 MB. The scale slider previews with the visual transform only; the canvas
  is re-decoded from frame 0 at the new size on OK. Frame-cache dirty boxes
  stay in GIF pixels and are mapped to canvas coordinates during playback.
- One UI thread blocks in `MsgWaitForMultipleObjectsEx` on a high-resolution
  waitable timer armed for the earliest due frame or script tick. With
  everything paused it waits indefinitely, using 0% CPU. Menus, dialogs and
  drags run nested loops, so a USER timer keeps animations moving during those.
- Files are memory-mapped; looping rewinds the read offset instead of reopening.
  giflib's per-frame `SavedImages` bookkeeping is freed after every frame so
  memory stays flat.
- Device loss (`DXGI_ERROR_DEVICE_REMOVED/RESET`) rebuilds the device and every
  swap chain, retrying every 2 s if the GPU is not back yet.
- The monitor layout (`monitors.c`) is cached and refreshed on
  `WM_DISPLAYCHANGE`, `WM_DPICHANGED` and `WM_SETTINGCHANGE(SPI_SETWORKAREA)`.
  The process is PerMonitorV2, so monitor rects and window positions share the
  same physical-pixel coordinates.
- `script.c` talks to the overlay only through a small callback table
  (`ScriptHostOps`), so `script_test` can drive it with a fake window.
- The config is a tab-separated text file, now v3: an optional trailing
  `\t<script>` column holds `builtin:<name>` or a `.lua` path. v1/v2 files load
  unchanged. Older builds would read the extra column as part of the GIF path
  and skip that GIF.
- `dx11_renderer.cpp` is C++ only because the Windows SDK's `dcomp.h` has no C
  bindings. The rest is C11.

### Performance

Measured with one 200×200 GIF at 25 fps: about 3 MB private, 17 MB working set
(mostly shared system DLLs), 5 threads, and about 3 ms of CPU per second.

Memory per W×H GIF: CPU canvas W·H·4 + swap chain 2·W·H·4, plus another W·H·4
only for GIFs that use disposal method 3. The limit is 2048×2048 pixels in total
(each side ≤ 4096). Each script adds its own Lua state (capped at 4 MB).

Motion cost is dominated by moving the window, not by Lua (one 200×200 GIF,
60 Hz, Release build, % of one core on a 16-thread machine):

| | CPU | per call |
|---|---|---|
| Lua `update(dt)`, any preset | negligible | 0.13-0.95 µs |
| Not moving (baseline) | 0.3-0.5% | – |
| `SetWindowPos`, default flags | 4.4-6.1% | 0.98-1.15 ms |
| `SetWindowPos`, lean flags (used) | 5.0-5.5% | 0.69-0.77 ms |

The lean flags (`SWP_NOSENDCHANGING | SWP_NOREDRAW | SWP_NOCOPYBITS |
SWP_NOOWNERZORDER`) cut the per-move cost by about 30%, and CPU by about 20%
with four moving GIFs. Moving a DComp visual inside a fixed full-screen window
instead would cost about 1% CPU, but that window must be click-through, so
the GIF could no longer be dragged or right-clicked. `yagp.set_rate(30)` halves
the cost for slow motion. A move is skipped when the rounded position does not
change. Because Lua is not the bottleneck, LuaJIT is not used: it would also
lose the memory cap and the instruction limit.

## Tests

After `cmake --build --preset release`:

```powershell
python tests/test_gif_stream.py out\Release\gif_dump.exe   # needs Pillow
python tests/test_frame_cache.py out\Release\gif_dump.exe
out\Release\script_test.exe
```

The decoder test compares composited frames and delays against Pillow,
covering disposal 1/2/3, interlacing, looping via `gif_stream_restart`, colour
keying, and rejection of malformed hex colours. It also decodes every case into
shrunken canvases (half, a fifth, an odd 37×29, 1×1) and checks them pixel by
pixel against a nearest-centre sample of the full-size reference.

`script_test` runs the Lua host against a fake window, a fake cursor and a fake
two-monitor layout (different sizes, offset, so there is a gap). It checks the
sandbox, the instruction and memory limits, error line numbers and the mouse
API. It runs each preset for up to 200k ticks while asserting that it stays on
screen and never enters the gap. It also checks that patrol spans its own
monitor's width, that roam and follow cross to the second monitor, that avoid
escapes a corner and returns home, and that unplugging a monitor pulls the GIF
back. For GIF-to-GIF contacts it checks begin/end events (edges don't count,
removed GIFs end their contacts, deterministic order), the peer API, and runs
two bouncing GIFs plus a static obstacle for 100k ticks: with `COLLIDE` they
never overlap, without it they pass through, and two GIFs spawned on top of
each other push apart.

## Known limitations

- Overlays are not resized on DPI changes. One GIF pixel always equals one
  device pixel, multiplied by the chosen scale.
- Delays of 0/1 cs are played at 100 ms, as browsers do. The GIF loop count is
  ignored and playback loops forever.
- The UI language is fixed at build time.
- Built-in preset comments are in Chinese only.

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

Windows 10/11, Visual Studio 2022+ (MSVC), CMake 3.25+. giflib 5.2.2, Lua 5.4.7,
rxi/log.c and libyaml 0.2.5 (all MIT) are pulled in by `FetchContent` at configure
time (SHA-256 pinned; log.c is pinned to commit `f9ea349`). For offline builds pass
`-DFETCHCONTENT_SOURCE_DIR_GIFLIB=<dir>`,
`-DFETCHCONTENT_SOURCE_DIR_LUA=<dir>`,
`-DFETCHCONTENT_SOURCE_DIR_RXILOG=<dir containing src/log.c>` and
`-DFETCHCONTENT_SOURCE_DIR_YAML=<dir>`. stb_image v2.30 is vendored as
`third_party/stb_image.h` (MIT/public domain) and needs no download.

From any shell (no VS developer prompt needed):

```powershell
cmake --preset release            # Visual Studio generator, x64, into out\
cmake --build --preset release    # yagp, script_test, log_test, gif_dump, image_dump, group_test, compose_test, config_test
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

- Tray icon (left or right click): add media (GIF/PNG/JPG, multi-select), a
  per-media submenu, groups, pause/resume all, start with Windows, delete
  settings data, exit. Media with a motion script are tagged "(moving)".
- Right-click media (or its tray submenu): hide/show, plus pause/resume, replace,
  scale, colour key, motion script, leader/follower relation, always on top, lock
  position, click-through, move to screen centre, remove.
- Images: add/replace/group import accept `.png`, `.jpg`, `.jpeg`. PNG alpha is
  preserved; JPEG is opaque. Still images have no animation: pause/resume,
  seek, clip and speed controls are unavailable, but movement, scale,
  transforms, opacity, colour key, scripts, hiding and leader/follower control
  remain available. APNG is shown as its first frame; animated WebP/AVIF is not
  supported.
- Groups: tray "Groups → Import group…" selects a `.compose` file (not a
  folder). The YAML declares members, scripts, positions, attributes and
  leader/follower relations; a folder without a compose file cannot be imported.
  "Create group…" in the same menu opens a save dialog and writes a commented
  `yagp.compose` template. Each group gets a tray submenu with hide/show and
  remove; group hiding does not overwrite members' own hidden flags. See also
  `example/yagp.compose.sample`.
- Leader/follower: right-click media → "Leader…" to choose one leader. A
  leader may have multiple followers, only one level is allowed and cycles are
  rejected. A follower's script is stopped but its binding is preserved; unlinking
  or removing the leader restores it. The leader script controls followers through
  the chainable `yagp.followers()` / `yagp.follower(id)` handles. A `.compose`
  group can bind the relation automatically.
- Hide takes media off the screen without removing it. It stays in the tray
  menu (tagged "(hidden)"); "Show" in its submenu brings it back with the same
  position, scale, colour key and script. While hidden it does not animate or
  run its script, other scripts do not see it, and its swap chain and canvas
  are freed. The hidden state is saved across restarts.
- Scale: a slider from 5% to 250% with live preview. You can also type a value
  or use the 100% button. Arrow keys move 1%, PgUp/PgDn 25%. Cancel restores
  the previous size. The GIF stays centred on the same point while scaling.
- Colour key: enter a hex colour (`#00FF00`, `00ff00`, `#0F0`) and a per-channel
  tolerance (0-255, default 16), or use the first frame's top-left pixel colour.
  Matching pixels are made transparent during compositing.
- Drag a GIF with the left mouse button unless it is locked.
- A click-through GIF ignores the mouse; turn it off from the tray submenu.
- Running `yagp.exe file.gif` again adds the file to the running instance.
- "Start with Windows" writes `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\YAGP`
  (per-user, no admin rights). It shows as checked only if the entry points at
  the running exe. Startup is silent: no notification is shown.
- State is saved to `%APPDATA%\YAGP\config.txt` (1 s debounce, and on exit),
  including each media's script binding, groups and leader/follower relations. The
  format is v4; older v1-v3 files are read automatically. "Delete settings data"
  removes `%APPDATA%\YAGP`, the frame cache in `%LOCALAPPDATA%\YAGP` and the
  startup entry; after that nothing is saved until the next launch. Your own
  `.lua` files are never touched.
- "Enable logging…" asks for a folder and appends to
  `<folder>\yagp-YYYYMMDD.log`. Clicking "Stop logging" writes a final line and
  closes the file. The switch and the folder are per run and are never saved;
  every enable asks again. At midnight (while still enabled) the next lines go
  to the new date's file in the same folder. One session writes at most 16 MB,
  after which logging stops and a balloon says so. Log wording is English; file
  names, paths and script messages keep their UTF-8 bytes.

### Terminal output

```powershell
.\out\Release\yagp.exe --verbose
.\out\Release\yagp.exe -v .\example\legacy-demo\main.gif
.\out\Release\yagp.exe --verbose > yagp.log 2> yagp.err
```

`--verbose`/`-v` mirrors every following log line to the terminal that started
YAGP. Tray logging does not need to be enabled and no log file is created. The
line format is the same as the file log (timestamp, level, source, message):
`TRACE`/`DEBUG`/`INFO` go to stdout, `WARN`/`ERROR`/`FATAL` to stderr.
Redirected output is UTF-8; a console is written with `WriteConsoleW`.

- Started from Explorer or a shortcut, there is no terminal to attach to: lines
  only go to DebugView, with no dialog.
- If YAGP is already running, the new process prints how many paths it forwarded
  and exits; the running instance's later lines do not appear in that terminal.
- `--verbose` does not change the tray logging switch or the config; both can be
  used together. Closing the terminal or Ctrl+C may end the attached process.
- The `--pick` helper produces no terminal output.

## Examples

`example/` contains standalone demos for the new features:

- `example/image/`: PNG/JPG still images, alpha/hitbox, movement/transforms,
  disabled playback controls
- `example/leader-follower/`: `.compose` auto-binding or manual leader menu
- `example/group/`: `.compose` import, group hide/show/remove, individual-vs-group
  hiding
- `example/yagp.compose.sample`: commented fill-in template

See [example/README.md](example/README.md).

### Group compose

Group import accepts only `.compose` files. The file lives in the group folder
and member paths are relative to it:

```yaml
version: 1
name: my-group
members:
  - media: leader.gif
    script: leader.lua
    position: [200, 200]
    topmost: true
  - media: follower.png
    script: follower.lua
    leader: leader.gif
```

`media` is required and `script` is optional. `scale`, `paused` (GIF only),
`hidden`, `topmost`, `locked`, `clickthrough` and `key` are supported. Unlisted
files are ignored; a follower's `leader` points to another member's `media`, only
one level is allowed and cycles are rejected. See the commented
[example/yagp.compose.sample](example/yagp.compose.sample).

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
| `yagp.others()` | Every other media: `{ {id, name, x, y, w, h, hx, hy, hw, hh, cx, cy, vx, vy, scripted, paused, still, follower, leader, leader_id}, ... }`, including ones without a script and paused ones. `x, y, w, h` is the window, `hx, hy, hw, hh` the visible area, `cx, cy` its centre. `vx`, `vy` are px/s over its last tick, 0 when not moving. `still` marks images, `follower` marks followers, `leader` marks overlays with followers |
| `yagp.peer(id)` | One GIF's peer table, or nil |
| `yagp.overlaps([x, y [, w, h]])` | Other GIFs whose visible area overlaps this GIF's. No arguments: where it is now; x, y: with the window moved there; explicit w, h: that rect at x, y. Sharing an edge is not an overlap |
| `log.trace(...)`, `log.debug(...)`, `log.info(...)`, `log.warn(...)`, `log.error(...)`, `log.fatal(...)` | Write a log line tagged `script <name>#<id>`. Arguments are converted without metamethods and joined with tabs; long messages keep 1024 bytes and a `...` marker. At most 100 lines per second per script; the excess is dropped and summarised |
| `yagp.notify(text)`, `yagp.notify(title, text)` | Show a tray balloon (default title `YAGP`). Returns `true` if it was shown, `false` when throttled (at most one per 3 s per script) or unavailable. Values are converted like `log.*`; the tray field limits are applied |
| `yagp.followers()` | Array of chainable handles for this overlay's direct followers; empty when it is not a leader |
| `yagp.follower(id)` | Handle for one direct follower, or nil |
| `yagp.set_locked(bool)`, `yagp.set_topmost(bool)` | Runtime lock/topmost override for this media; not saved and cleared when the script reloads |
| `yagp.hide()`, `yagp.show()` | Hide/show this media; persisted. Hiding stops the script, so use the tray menu to show it again |
| `yagp.set_paused(bool)` | Pause/resume this GIF; persisted. Still images return nil, err |
| `follower:set_locked(bool)`, `follower:set_topmost(bool)` | Runtime override on the follower; returns the handle |
| `follower:pos()`, `size()`, `center()`, `hitbox()`, `frame()`, `clip_info(name)`, `name()`, `id()`, `paused()`, `hidden()` | Query follower state; nil after the follower is gone |
| `follower:set_pos(...)`, `move(...)`, `set_flip(...)`, `set_hitbox(...)` | Move/flip/hitbox; returns the same handle |
| `follower:clip(...)`, `unload(...)`, `play(...)`, `seek(...)`, `set_speed(...)`, `set_playing(...)`, `set_paused(...)` | Follower GIF playback; returns the handle, or nil, err on failure. On still images these return "no animation" |
| `follower:set_matrix(...)`, `set_transform(...)`, `reset_transform(...)`, `set_opacity(...)` | Follower transform/opacity; returns the handle |
| `follower:set_cursor(...)`, `set_draggable(...)`, `set_clickthrough(...)`, `hide()`, `show()` | Follower input/visibility; returns the handle |

`set_locked`, `set_topmost`, `set_clickthrough` and `set_draggable` affect this run
only and are not saved. The tray menu shows the effective state; a user menu
choice clears the script override, and reloading the script restores the menu
settings. `hide/show` and `set_paused` modify persisted state directly.

A follower's own script is stopped; the leader cannot remove followers, replace their
files, change their script binding or change the relation. Chain example:

```lua
yagp.follower(id):set_pos(x, y):set_flip(true):play("walk", {loop = true}):set_opacity(0.5, 0.2)
```

Other overlays are read-only; only overlays bound as this leader's followers can be
controlled. A script sees at most 64 other overlays.

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
- Pattern matching (`string.find`/`match`/`gmatch`/`gsub`) runs in C, where
  the instruction limit cannot interrupt it, so it has its own work limit
  (about 20 million matcher steps per call; `cmake/patch_lstrlib.cmake`).
  A backtracking pattern fails with "pattern too complex" instead of hanging.
- `setmetatable` refuses metatables with `__gc`: Lua runs finalizers with
  hooks disabled, so an endless one could not be stopped.
- Handlers (`update`, `on_moved`, ...) are looked up as raw fields of `_G`, and
  every call into the script is protected. A strict-mode `_G` metatable works,
  and no script error can take the process down.
- `print` writes an INFO log line (the same sink as `log.info`), tagged with the
  GIF's name and script id. Every line also goes to `OutputDebugString`
  (DebugView), whether or not file logging is enabled.
- `log.*` never calls `__tostring` and never raises a script error; `print` keeps
  its old `tostring` formatting for tables and userdata.

## Design notes

- File dialogs run in a short-lived helper process (`yagp.exe --pick
  add|replace|script|logdir <id>`, or `--pick newscript <id> <preset> <name>`),
  which sends the chosen path back with `WM_COPYDATA` and exits. In-process, the
  dialog loaded about 100 shell DLLs, third-party shell extensions and about 20
  threads, none of which unload, raising the tray from 3 MB to 18 MB private.
  With the helper the tray stays at about 3.5 MB. Results carry a random id, so
  stale or forged messages are ignored.
- Logging uses rxi/log.c (pinned commit, MIT) for levels, formatting and
  callbacks, plus a small policy layer (`applog.c`) that owns the date-stamped
  file, the 16 MB session cap, the day rollover and the `OutputDebugString`
  mirror. rxi's lock hook is a critical section, so the frame-cache worker and
  the UI thread cannot interleave a line. The sink escapes CR/LF so one call is
  always one line and caps a line at 4096 bytes. While logging is off no file is
  open and no write happens; only the debugger mirror remains.
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
- When a GIF is added, it is decoded once to build a cache: a per-frame
  bounding box of changed pixels. It is stored in
  `%LOCALAPPDATA%\YAGP\cache\<hash>.ygc` (one file per path and colour key)
  and is checked against file size, modification time and colour key, so an
  edited GIF is rebuilt automatically. An existing cache file is read on the
  UI thread; a missing one is built on a low-priority worker thread, one GIF at
  a time, while the GIF already plays with full uploads. The build only scans
  the area each frame can change (its rect plus the previous frame's
  disposal), so a small sprite on a large canvas costs its own size per frame.
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
python tests/test_still_image.py out\Release\image_dump.exe
out\Release\group_test.exe
out\Release\compose_test.exe
out\Release\config_test.exe
out\Release\script_test.exe
out\Release\log_test.exe
```

The still-image test generates PNG/JPEG fixtures with Pillow and covers alpha,
colour key, hitbox, JPEG decoding, corrupt files and oversized-image rejection;
`script_test` covers chainable follower handles, safe invalidation after deletion
and rejection of animation calls on still followers.

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
It also checks that no script can abort or hang the host: strict-mode `_G`,
a script at its memory cap, non-string errors, backtracking patterns, `__gc`
finalizers, and a timeout inside a coroutine. The `log` table and `yagp.notify`
are covered too: all six levels, argument conversion, the 1024-byte marker, the
100-lines-per-second limit and its summary, notification titles/truncation,
return values and the 3-second throttle.

`log_test` checks the file sink: the `yagp-YYYYMMDD.log` name, append across
enables, line order and levels, CR/LF escaping and truncation, no writes after
stop, the 8 KB test cap, a day rollover and multi-threaded line integrity. It
also checks the `--verbose` terminal mirror: stdout/stderr routing, output while
file logging is off, and stopping after detach. It writes only into `%TEMP%` (or
the current directory when a sandbox hides the temp folder) and never touches
the user's profile.

## Known limitations

- Overlays are not resized on DPI changes. One GIF pixel always equals one
  device pixel, multiplied by the chosen scale.
- Delays of 0/1 cs are played at 100 ms, as browsers do. The GIF loop count is
  ignored and playback loops forever.
- The UI language is fixed at build time.
- Built-in preset comments are in Chinese only.
- Logging is enabled per run from the tray (no level selector, no persisted log
  path) and its fixed wording is English; paths and messages keep their bytes.
- Terminal output requires `--verbose`/`-v`; hosts without a console (Explorer,
  some Git Bash/pty setups) only get redirected handles or DebugView.
- Still images have no animation; APNG shows its first frame and animated
  WebP/AVIF is not supported. Image decoding runs on the UI thread and is
  bounded by a 64M-pixel budget.
- Groups are imported only from a `.compose` file. The file lists members and
  exposes media/script paths; unlisted files are ignored. There is no tab window
  and no manual member rename/add/remove.
- Leader/follower is one level only: a follower cannot be a leader and a leader cannot
  become a follower. A follower's script stays stopped while bound and resumes after
  unlinking or when its leader is removed.

## License

YAGP is released under the MIT License; see [LICENSE](LICENSE) for the full
text.

### Third-party components

| Component | Version/commit | License | Use |
|---|---|---|---|
| [giflib](https://giflib.sourceforge.net/) | 5.2.2 | MIT (Copyright (c) 1997 Eric S. Raymond) | GIF/LZW decoding |
| [Lua](https://www.lua.org/) | 5.4.7 | MIT (Copyright (c) 1994-2024 Lua.org, PUC-Rio) | Sandboxed scripting runtime; the `lstrlib.c` pattern-work patch remains under the Lua license |
| [rxi/log.c](https://github.com/rxi/log.c) | commit `f9ea349` | MIT (Copyright (c) 2020 rxi) | Logging core |
| [libyaml](https://github.com/yaml/libyaml) | 0.2.5 | MIT (Copyright (c) 2017-2020 Ingy döt Net; 2006-2016 Kirill Simonov) | `.compose` YAML parsing |
| [stb_image](https://github.com/nothings/stb) | v2.30 (`third_party/stb_image.h`) | MIT / public domain (MIT chosen) | PNG/JPG/JPEG decoding |

The complete third-party license texts are in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). CMake, Python, Pillow and
Visual Studio/MSVC are build/test tools only; they are not linked into
`yagp.exe` and are not distributed with it.

Except for the third-party components above, the source code, scripts,
documentation and the demo assets in `example/` are released under the MIT
License.

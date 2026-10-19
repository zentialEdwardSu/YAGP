# YAGP

中文 | [English](README_en.md)

YAGP把 GIF 动画显示成无边框、背景透明的桌面挂件，不占任务栏，也不出现在 Alt+Tab 里。程序常驻托盘，完全不用 GDI、UpdateLayeredWindow、SDL，也不编译着色器：

`giflib LZW 解码 -> 预乘 BGRA 画布（CPU） -> UpdateSubresource 写入交换链后缓冲区 -> 合成交换链 -> DirectComposition visual -> DWM`

每个 GIF 还可以绑定一个沙箱化的 Lua 脚本，让它在桌面上移动：左右巡逻、随机游走、碰边反弹、跟随或躲避鼠标，也可以自己写。完整支持多显示器。

## 编译

需要 Windows 10/11、Visual Studio 2022 及以上（MSVC）、CMake 3.25 及以上。giflib 5.2.2、Lua 5.4.7 和 rxi/log.c（均为 MIT 许可）在配置阶段通过 `FetchContent` 下载，并校验 SHA-256（log.c 固定到提交 `f9ea349`）。离线编译时传入 `-DFETCHCONTENT_SOURCE_DIR_GIFLIB=<目录>`、`-DFETCHCONTENT_SOURCE_DIR_LUA=<目录>` 和 `-DFETCHCONTENT_SOURCE_DIR_RXILOG=<含 src/log.c 的目录>`。

在任意终端里执行即可，不需要 VS 开发者终端：

```powershell
cmake --preset release            # Visual Studio 生成器，x64，输出到 out\
cmake --build --preset release    # 编译 yagp、script_test、log_test、gif_dump
.\out\Release\yagp.exe [file.gif ...]
```

生成的 exe 不依赖其他文件：静态链接 CRT，图标和 Lua 预设都嵌在 exe 里。

- `release-en` 预设编译英文界面，输出到 `out-en\`。
- 预设没有写死生成器，CMake 会使用已安装的最新版 Visual Studio，并自己找到 MSVC 和 Windows SDK。
- Ninja 仍然可用，但只能在开发者终端里用。在普通终端里，CMake 会用上 PATH 中第一个找到的 `link`/`ld`（比如 MinGW 的 `ld.exe`），导致编译器检查失败，所以配置阶段会直接停下并给出提示。

### 界面语言

界面语言在编译时选择：`-DYAGP_LANG=zh_CN`（默认）或 `-DYAGP_LANG=en`。所有界面文字都以 `TXT_*` 宏的形式集中在 `i18n.h` 里，Lua 内部抛出的 UTF-8 消息用 `TXT_U8_*`。

新增一种语言的步骤：在 `i18n.h` 里复制一整块，翻译后在 `CMakeLists.txt` 的 `YAGP_LANG` 中加上对应的值。每一块都必须定义全部宏，漏翻译会直接编译失败。翻译时保留 printf 格式符和文件过滤器里的 `\0` 分隔符。

### 图标

`app.ico`（16–256 px，圆角）：

```powershell
python tools/make_icon.py    # 需要 Pillow；改脚本里的 RADIUS 可调整圆角大小
```

## 使用

- 托盘图标（左键或右键）：添加 GIF（可多选）、每个 GIF 的子菜单、全部暂停/继续、开机自启动、删除配置数据、退出。绑定了运动脚本的 GIF 在托盘菜单里会标上"(运动)"。
- 右键 GIF：暂停/继续、更换 GIF、缩放、色键抠图、运动脚本、置顶、锁定位置、鼠标穿透、移到屏幕中央、隐藏、删除。
- 隐藏：GIF 从屏幕上消失，但不会被删除，仍保留在托盘菜单里（标为"(已隐藏)"），在它的子菜单里点"显示"即可恢复，位置、缩放、色键和脚本都不变。隐藏期间不播放、不运行脚本，其他脚本也看不到它，并会释放交换链和画布内存。隐藏状态会保存，重启后仍是隐藏的。
- 缩放：5% 到 250% 的滑块，实时预览。也可以直接输入数值，或点 100% 按钮。方向键每次 1%，PgUp/PgDn 每次 25%。取消会恢复原尺寸。缩放时 GIF 的中心点保持不动。
- 色键抠图：输入十六进制颜色（`#00FF00`、`00ff00`、`#0F0`）和每通道容差（0–255，默认 16），或者直接取第一帧左上角像素的颜色。匹配的像素在合成时变为透明。
- 未锁定时，可以用鼠标左键拖动 GIF。
- 开启鼠标穿透的 GIF 不响应鼠标，可以在托盘菜单里对应 GIF 的子菜单中关闭。
- 再次运行 `yagp.exe file.gif` 会把文件添加到已经在运行的实例中。
- "开机自启动"会写入 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\YAGP`（仅当前用户，不需要管理员权限）。只有当这一项指向当前运行的 exe 时才显示为勾选。开机启动时不弹通知。
- 状态保存在 `%APPDATA%\YAGP\config.txt`（1 秒防抖，退出时也会保存），包括每个 GIF 绑定的脚本。"删除配置数据"会删除 `%APPDATA%\YAGP`、`%LOCALAPPDATA%\YAGP` 中的帧缓存和开机自启动项，之后直到下次启动都不再保存。你自己的 `.lua` 文件不会被删除。
- "启用日志记录…"会让你选一个文件夹，日志追加到 `<文件夹>\yagp-YYYYMMDD.log`。再次点击"停止日志记录"会写入最后一行并关闭文件。开关和文件夹都只在本次运行有效，不会保存，每次启用都要重新选择。启用期间跨过午夜时，后续日志写入同一文件夹下新日期的文件。每次启用最多写 16 MB，达到后自动停止并弹出托盘通知。日志措辞固定为英文；文件名、路径和脚本消息按原样以 UTF-8 字节保存。

### 从终端运行

```powershell
.\out\Release\yagp.exe --verbose
.\out\Release\yagp.exe -v .\example\main.gif
.\out\Release\yagp.exe --verbose > yagp.log 2> yagp.err
```

`--verbose`/`-v` 会把之后所有日志行同时写到启动它的终端；不需要在托盘里启用日志记录，也不会创建日志文件。行格式与文件日志一致（时间戳、级别、来源、消息）：`TRACE`/`DEBUG`/`INFO` 走 stdout，`WARN`/`ERROR`/`FATAL` 走 stderr。重定向到文件或管道时写入 UTF-8 行；直接写控制台时使用 `WriteConsoleW`。

- 从资源管理器或快捷方式带 `--verbose` 启动时没有可附着的终端，只会写 DebugView，不弹窗。
- 已经有 YAGP 在运行时，新进程只打印转发的路径数量并退出；运行中实例的后续日志不会出现在这个终端里。
- `--verbose` 不改变托盘日志开关和配置；两者可以同时使用。关闭终端或按 Ctrl+C 可能结束这个附着进程。
- `--pick` 辅助进程不产生终端日志。

## 运动脚本

右键 GIF → 运动脚本：

| 菜单项 | 说明 |
|---|---|
| 无 | 不使用脚本 |
| 左右巡逻 | 在所在显示器工作区的整个宽度内左右往返 |
| 随机游走 | 在所在显示器上随机选点走过去，中途会停顿 |
| 边缘反弹 | 斜向移动，碰到工作区边缘反弹，也会在其他 GIF 上反弹（没有脚本的 GIF 相当于墙，设 `COLLIDE = false` 可关闭）。设 `ROAM = true` 可以跨显示器移动，并在显示器之间的空隙处反弹 |
| 跟随鼠标 | 朝鼠标走去，距离 `STOP` 像素时停下；可以跟到其他显示器上，遇到空隙会沿边缘绕行 |
| 躲避鼠标 | 鼠标进入 `RADIUS` 范围时逃开；被逼到角落会跳到另一侧；鼠标离开后慢慢回到原位 |
| 选择脚本文件… | 绑定任意 `.lua` 文件 |
| 基于预设新建脚本 | 把预设另存一份（默认 `Documents\my_<预设名>.lua`），在编辑器中打开，并绑定到这个 GIF |
| 重新加载脚本 | 修改脚本后重新读取 |

- 拖动 GIF 时脚本暂停，松手后从新位置继续。对"躲避鼠标"来说，新位置就成了它的原位。暂停 GIF 时运动也会暂停。
- 预设会让 GIF 朝向运动方向。如果 GIF 原图朝左，设 `FACE_RIGHT = false`；不想翻转就设 `FLIP = false`。
- 拔掉显示器或任务栏位置变化时，脚本会把 GIF 拉回可见区域。
- 脚本文件留在你保存的位置，配置里只记录它的绝对路径。如果文件被移走，下次启动时这个绑定会被静默丢弃，GIF 停在原地不动。
- "基于预设新建脚本"用系统的"编辑"方式打开文件（没有就用记事本），从不用"打开"，以免 `.lua` 被关联到解释器时脚本被直接运行。
- 脚本出任何错误都会停止运行，GIF 停在原地，错误信息（带行号）以托盘通知显示。

### 编写脚本

内置预设在 `scripts/*.lua`，同时也是示例。`SPEED`、`ROAM`、`RADIUS`、`FACE_RIGHT` 等可调参数是文件开头的全局变量。

脚本的顶层代码只在加载时执行一次。之后按脚本设定的频率调用全局函数 `update(dt)`，`dt` 单位为秒，上限 0.25。可选的回调：

- `on_moved()`：用户拖动、移到屏幕中央、缩放或更换 GIF 之后调用。
- `on_screens_changed()`：显示器或工作区变化之后调用。
- `on_collide(other)`：这个 GIF 的可见区域开始和另一个 GIF 的可见区域重叠时调用一次，`other` 是对方的信息表（字段同 `yagp.others()`）。
- `on_separate(other)`：两者不再重叠时调用。如果对方已被删除，`other` 里只有 `id`。

所有坐标都是虚拟桌面上的物理像素（主显示器左边和上方为负数），位置指窗口左上角。

| 函数 | 说明 |
|---|---|
| `yagp.pos()` → x, y | 当前位置（保留小数，即上次设置的值） |
| `yagp.set_pos(x, y)`、`yagp.move(dx, dy)` | 移动 GIF |
| `yagp.size()` → w, h | 尺寸（已含缩放） |
| `yagp.center()` → x, y | 可见区域的中心点 |
| `yagp.hitbox()` → dx, dy, w, h | 可见区域相对 `yagp.pos()` 的偏移和尺寸（显示像素，已含缩放和翻转） |
| `yagp.set_hitbox(x, y, w, h)`、`yagp.set_hitbox(nil)` | 手动指定可见区域（GIF 原尺寸像素，随缩放和翻转变化）；传 nil 恢复自动 |
| `yagp.monitors()` | `{ {x, y, w, h, work = {x, y, w, h}, dpi, primary, index}, ... }` |
| `yagp.monitor()` → m, index | GIF 中心所在的显示器（不在任何显示器上时取最近的） |
| `yagp.monitor_at(x, y)` → m, index | 某个点所在的显示器（或最近的） |
| `yagp.desktop()` → x, y, w, h | 所有显示器的外接矩形 |
| `yagp.fits(x, y [, w, h])` | 窗口放在 x、y 时，可见区域是否完全落在工作区内；显式传 w、h 时检查的是 x、y 处的这个矩形。显示器之间的空隙算作区域外 |
| `yagp.cursor()` → x, y, valid | 鼠标位置。读不到时（锁屏、UAC 弹窗）`valid` 为 false，x、y 为最后一次读到的位置 |
| `yagp.mouse()` | `{x, y, left, right, middle, over, valid, monitor}`：按键为逻辑按键（已处理左手鼠标设置），`over` 表示鼠标在 GIF 上，`monitor` 为显示器序号 |
| `yagp.time()` | 单调递增的秒数 |
| `yagp.set_flip(bool)` | 水平镜像 |
| `yagp.set_rate(hz)` | 每秒调用 `update` 的次数，1–144，默认 60 |
| `yagp.name` | GIF 文件名 |
| `yagp.id` | 这个 GIF 的编号，本次运行内唯一，重启后会变 |
| `yagp.others()` | 其他所有 GIF：`{ {id, name, x, y, w, h, hx, hy, hw, hh, cx, cy, vx, vy, scripted, paused}, ... }`，包括没有脚本和已暂停的。`x, y, w, h` 是窗口，`hx, hy, hw, hh` 是可见区域，`cx, cy` 是可见区域的中心。`vx`、`vy` 是上一次 tick 的速度（像素/秒），不在运动时为 0 |
| `yagp.peer(id)` | 某一个 GIF 的信息表，不存在时为 nil |
| `yagp.overlaps([x, y [, w, h]])` | 可见区域与自己重叠的其他 GIF 列表。不传参数时按当前位置算，传 x、y 时按窗口移到那里算，显式传 w、h 时用 x、y 处的这个矩形。只是边贴着边不算重叠 |
| `log.trace(...)`、`log.debug(...)`、`log.info(...)`、`log.warn(...)`、`log.error(...)`、`log.fatal(...)` | 写一行带 `script <名称>#<id>` 标识的日志。参数不调用元方法，用制表符连接；超长消息保留 1024 字节并追加 `...`。每个脚本每秒最多 100 行，超出部分丢弃并汇总 |
| `yagp.notify(text)`、`yagp.notify(title, text)` | 显示托盘气泡（标题默认 `YAGP`）。显示成功返回 `true`；因频率限制（每个脚本 3 秒一次）或宿主不可用而抑制时返回 `false`。参数转换规则同 `log.*`，并按托盘字段截断 |

其他 GIF 的信息是只读的，脚本不能移动别的 GIF，也不能给它们发消息。最多能看到 64 个其他 GIF。

**可见区域（hitbox）**：边缘检测和碰撞不看整个窗口，而是看 GIF 的可见区域，也就是所有帧里不透明像素的外接矩形（考虑色键）。它在建立帧缓存时顺带算出，运行时没有额外开销。`fits`、`overlaps`、`center`、`monitor`、`mouse().over`、`on_collide` 和所有预设都用它，所以带大片透明边的 GIF 会让可见部分贴着屏幕边缘和其他 GIF 反弹，透明的部分可以伸出屏幕。`yagp.pos`、`size`、`set_pos` 仍然指整个窗口。全透明的 GIF 用整个窗口。如果自动算出的区域不合适（比如有一圈不想参与碰撞的淡阴影），可以在脚本里用 `yagp.set_hitbox` 指定；更换脚本或 GIF 后会恢复自动。

一个最简单的脚本：

```lua
SPEED = 100
function update(dt)
  local x, y = yagp.pos()
  local work = yagp.monitor().work
  local w = yagp.size()
  x = x + SPEED * dt
  if x > work.x + work.w - w then x = work.x end -- 走到右边后回到左边
  yagp.set_pos(x, y)
end
```

### 沙箱

- 程序只编译了 `base`、`math`、`string`、`table`、`utf8` 和 `coroutine` 这几个库，`io`、`os`、`package`、`debug` 根本不存在。
- `load`、`loadfile`、`dofile`、`require`、`collectgarbage` 和 `string.dump` 已被移除，也不接受预编译的字节码。
- 每个脚本的 Lua 状态内存上限为 4 MB。
- 单次调用超过约 1000 万条指令就会被终止，死循环卡不住 UI 线程，写在 `pcall` 里也一样。
- 模式匹配（`string.find`/`match`/`gmatch`/`gsub`）在 C 里执行，指令数限制打断不了它，所以它有自己的工作量上限（每次调用约 2000 万步，见 `cmake/patch_lstrlib.cmake`）。回溯爆炸的模式会报 "pattern too complex"，而不是卡死。
- `setmetatable` 拒绝带 `__gc` 的元表：Lua 执行终结器时会关闭钩子，死循环的终结器无法被终止。
- 事件函数（`update`、`on_moved` 等）按 `_G` 的原始字段查找，每次调用脚本都在保护模式下进行。严格模式的 `_G` 元表可以正常使用，任何脚本错误都不会让进程崩溃。
- `print` 写一行 INFO 日志（和 `log.info` 用同一个输出通道），带上 GIF 文件名和脚本 id。无论是否启用文件日志，所有行也会输出到 `OutputDebugString`（可用 DebugView 查看）。
- `log.*` 不调用 `__tostring`，也不会抛出脚本错误；`print` 对其参数仍保留原来的 `tostring` 行为。

## 设计说明

- 文件对话框在一个短命的辅助进程里运行（`yagp.exe --pick add|replace|script|logdir <id>`，或 `--pick newscript <id> <预设> <文件名>`）。它用 `WM_COPYDATA` 把选中的路径发回后退出。如果在主进程里打开对话框，会加载约 100 个 shell DLL、第三方 shell 扩展和约 20 个线程，而且都不会卸载，托盘进程的私有内存会从 3 MB 涨到 18 MB。用辅助进程后托盘保持在约 3.5 MB。结果带有随机 id，过期或伪造的消息会被忽略。
- 日志用 rxi/log.c（固定提交，MIT）提供级别、格式化和回调，再由一个小的策略层（`applog.c`）负责按日期命名的文件、16 MB 会话上限、跨日切换和 `OutputDebugString` 镜像。rxi 的锁钩子是一把临界区，所以帧缓存工作线程和 UI 线程不会写出交错的行。输出回调把 CR/LF 转义，保证一次调用只产生一行，并把单行限制在 4096 字节。日志关闭时不打开文件、不写盘，只有调试器镜像还在。
- 缩放滑块是自绘的小控件，没用 comctl32 的 Trackbar。
- 所有挂件共用一个 D3D11 设备和一个 DirectComposition 设备，每个挂件只占一个双缓冲的 flip 模型合成交换链。
- D3D11 设备优先在 WARP（软件光栅器）上创建，失败才用硬件。GPU 在这里不做任何工作：帧在 CPU 上合成并拷进后缓冲区，合成和缩放由 DWM 完成。加载硬件的用户态驱动每个进程要多占 20–60 MB 私有内存（在 AMD 上测得），WARP 只要约 1 MB。交换链缓冲区照样作为普通合成表面交给 DWM。
- 色键按调色板项计算（每帧最多比较 256 次），所以合成时每个像素仍然只查一次表。修改色键后会从第 0 帧重新解码，画布上已有的像素也会重新抠图。
- 添加 GIF 时会解码一遍来建立缓存，内容是每一帧变化像素的外接矩形。
  - 缓存存在 `%LOCALAPPDATA%\YAGP\cache\<hash>.ygc`（每个路径和色键一个文件），并校验文件大小、修改时间和色键，GIF 被修改后会自动重建。
  - 已有的缓存文件在 UI 线程上直接读取；没有缓存时由一个低优先级的后台线程逐个建立，期间 GIF 已经以整帧上传的方式开始播放。
  - 建立缓存时每帧只扫描它可能改变的区域（本帧矩形加上一帧的 disposal 区域），大画布上的小精灵每帧只花它自身大小的开销。
  - 播放时只上传变化的矩形，外加上一帧的矩形（因为 flip 模型的缓冲区是交替使用的），完全相同的帧直接跳过。
  - 实测在 WARP 下 `Present1` 的脏矩形开销比省下的还多，所以 Present 仍然是整帧。
  - 一个 480×360、40 帧的 GIF，缓存文件 22 KB。建立缓存的测试进程用时 21 ms，加载缓存的用时 8.5 ms（都包含进程启动时间）。
- GIF 的 alpha 只有 0 和 255，所以对每个像素来说预乘 alpha 和直通 alpha 完全相同。画布直接写进后缓冲区：没有帧纹理，没有着色器，也不依赖 `d3dcompiler`。
- 放大（100% 及以上）和朝向翻转都是 DComp visual 的变换，用最近邻采样，缓冲区保持原始尺寸，放大不增加内存或 CPU。
- 缩小（低于 100%）时，画布和交换链直接按显示尺寸建立：合成时每个画布像素只取离它中心最近的源像素，结果和"原尺寸合成后再最近邻缩小"逐像素相同。一个 1000×1000 的 GIF 缩到 20% 时，画布从 3.9 MB 降到 156 KB。拖动缩放滑块时只改 visual 变换做预览，点"确定"后才按新尺寸从第 0 帧重新解码。帧缓存里的变化矩形仍按 GIF 原尺寸存储，播放时换算到画布坐标。
- 只有一个 UI 线程，阻塞在 `MsgWaitForMultipleObjectsEx` 上，等待一个高精度可等待定时器，定时器设为最早到期的帧或脚本 tick。全部暂停时无限期等待，CPU 占用为 0%。菜单、对话框和拖动会运行自己的嵌套消息循环，这期间由一个 USER 定时器保持动画继续。
- 文件用内存映射读取，循环播放时只把读取位置倒回开头，不重新打开文件。giflib 每帧的 `SavedImages` 记录在每帧后释放，内存保持平稳。
- 设备丢失（`DXGI_ERROR_DEVICE_REMOVED/RESET`）时会重建设备和所有交换链。GPU 还没恢复时每 2 秒重试一次。
- 显示器布局（`monitors.c`）有缓存，在 `WM_DISPLAYCHANGE`、`WM_DPICHANGED` 和 `WM_SETTINGCHANGE(SPI_SETWORKAREA)` 时刷新。进程是 PerMonitorV2 的，所以显示器矩形和窗口位置用的是同一套物理像素坐标。
- `script.c` 只通过一个小的回调表（`ScriptHostOps`）和挂件交互，所以 `script_test` 可以用一个假窗口来驱动它。
- 配置是 Tab 分隔的文本文件，现在是 v3：末尾可选的 `\t<脚本>` 列存放 `builtin:<名称>` 或 `.lua` 路径。v1/v2 文件可以照常读取。旧版程序会把多出的这一列当成 GIF 路径的一部分，从而跳过这个 GIF。
- `dx11_renderer.cpp` 用 C++ 只是因为 Windows SDK 的 `dcomp.h` 没有 C 接口，其余代码都是 C11。

### 性能

一个 200×200、25 fps 的 GIF：私有内存约 3 MB，工作集 17 MB（大部分是共享的系统 DLL），5 个线程，每秒约 3 ms CPU。

每个 W×H 的 GIF 占用的内存：CPU 画布 W·H·4，加交换链 2·W·H·4，使用 disposal 方法 3 的 GIF 再加 W·H·4。像素总数上限 2048×2048（每边不超过 4096）。每个脚本另有自己的 Lua 状态（上限 4 MB）。

运动的开销主要在移动窗口，而不是 Lua。以下是一个 200×200 的 GIF、60 Hz、Release 版本，按单核百分比计（16 线程的机器）：

| | CPU | 每次调用 |
|---|---|---|
| Lua `update(dt)`，任意预设 | 可忽略 | 0.13–0.95 µs |
| 不移动（基线） | 0.3–0.5% | – |
| `SetWindowPos`，默认标志 | 4.4–6.1% | 0.98–1.15 ms |
| `SetWindowPos`，精简标志（当前使用） | 5.0–5.5% | 0.69–0.77 ms |

- 精简标志是 `SWP_NOSENDCHANGING | SWP_NOREDRAW | SWP_NOCOPYBITS | SWP_NOOWNERZORDER`，单次移动快约 30%，4 个 GIF 同时移动时 CPU 降约 20%。
- 换成在固定的全屏窗口里移动 DComp visual，CPU 只要约 1%，但那个窗口必须设为鼠标穿透，GIF 就不能拖动和右键了。
- 慢速运动可以用 `yagp.set_rate(30)` 把开销减半。取整后位置没变时，会直接跳过这次移动。
- Lua 不是瓶颈，所以没用 LuaJIT。换成它还会失去内存上限和指令数限制。

## 测试

执行 `cmake --build --preset release` 之后：

```powershell
python tests/test_gif_stream.py out\Release\gif_dump.exe   # 需要 Pillow
python tests/test_frame_cache.py out\Release\gif_dump.exe
out\Release\script_test.exe
out\Release\log_test.exe
```

解码测试把合成后的帧和帧延时与 Pillow 的结果对比，覆盖 disposal 1/2/3、隔行扫描、用 `gif_stream_restart` 循环、色键，以及拒绝格式错误的十六进制颜色。它还会把每个用例解码到缩小的画布（一半、五分之一、奇数尺寸 37×29、1×1），与原尺寸参考帧按"取中心最近像素"采样的结果逐像素对比。

`script_test` 用一个假窗口、一个假鼠标和一个假的双显示器布局（尺寸不同、上下错开，中间有空隙）运行 Lua 宿主。它检查：

- 沙箱、指令数和内存限制、错误行号、鼠标 API。
- 每个预设最多跑 20 万个 tick，始终不离开屏幕、不进入空隙。
- 巡逻覆盖所在显示器的整个宽度。
- 跨屏反弹和跟随能到达第二块显示器。
- 躲避能逃出角落并回到原位。
- 拔掉显示器后 GIF 会被拉回。
- GIF 之间的接触：开始和结束事件（边贴边不算、被删除的 GIF 会结束接触、回调顺序确定），以及 `others`/`peer`/`overlaps` 接口。
- 两个反弹 GIF 加一个静止障碍物跑 10 万个 tick：开启 `COLLIDE` 时从不重叠，关闭时会互相穿过；两个叠在一起生成的 GIF 会自动分开。
- 任何脚本都不能让宿主崩溃或卡死：严格模式的 `_G`、内存用满的脚本、非字符串错误、回溯爆炸的模式、`__gc` 终结器、协程内超时。
- `log` 表和 `yagp.notify` 也覆盖到了：六个级别、参数转换、1024 字节截断标记、每秒 100 行的限流与摘要、通知标题/截断/返回值以及 3 秒节流。

`log_test` 检查文件输出：`yagp-YYYYMMDD.log` 文件名、多次启用的追加语义、行顺序与级别、CR/LF 转义与截断、停止后不再写入、8 KB 测试上限、跨日切换和多线程整行完整性。它还检查 `--verbose` 的终端镜像：stdout/stderr 分流、文件日志关闭时仍输出、detach 后停止。它只写 `%TEMP%`（沙箱隐藏临时目录时退回当前目录），不碰用户配置。

## 已知限制

- DPI 变化时挂件不会调整大小。一个 GIF 像素始终等于一个设备像素，再乘以选定的缩放比例。
- 0 或 1 厘秒的帧延时按 100 ms 播放，和浏览器一致。GIF 的循环次数被忽略，始终无限循环。
- 界面语言只能在编译时选定。
- 内置预设里的注释只有中文。
- 日志只能在每次运行时从托盘启用（没有级别选择，也不保存日志路径），固定措辞为英文；路径和消息保留原始字节。
- 终端日志只在带 `--verbose`/`-v` 启动时输出；没有控制台宿主（资源管理器、部分 Git Bash/pty）时只能靠重定向句柄或 DebugView。

## 许可

YAGP 以 MIT 许可证发布，完整文本见 [LICENSE](LICENSE)。

### 第三方组件

| 组件 | 版本/提交 | 许可证 | 用途 |
|---|---|---|---|
| [giflib](https://giflib.sourceforge.net/) | 5.2.2 | MIT（Copyright © 1997 Eric S. Raymond） | GIF/LZW 解码 |
| [Lua](https://www.lua.org/) | 5.4.7 | MIT（Copyright © 1994–2024 Lua.org, PUC-Rio） | 沙箱脚本运行时；`lstrlib.c` 的模式匹配工作量补丁仍属 Lua |
| [rxi/log.c](https://github.com/rxi/log.c) | commit `f9ea349` | MIT（Copyright © 2020 rxi） | 日志核心 |

完整第三方许可证文本见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。CMake、Python、Pillow 和 Visual Studio/MSVC 只用于构建和测试，不会被链接进 `yagp.exe`，也不随二进制分发。

除上述第三方组件外，本仓库的源代码、脚本、文档和 `example/` 中的演示素材均按 MIT 许可证发布。

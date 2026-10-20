# 旧 clip/事件测试 / Legacy clip and event test

## 中文

这里收集了原来散放在 `example/` 根目录的旧版测试素材。所有文件名和相对路径保持不变：

- `main.gif` + `demo.lua`：clip、事件、变换和日志的实时检查脚本；
- `walk.gif`、`sit.gif`：脚本用 `yagp.clip` 加载的动画；
- `look_*.gif`、`react.gif`：look/事件相关素材。

运行方式：

1. 把 `main.gif` 和 `demo.lua` 一起绑定（右键 `main.gif` →“运动脚本”→“选择脚本文件…” → `demo.lua`）；
2. 脚本会在约 6 秒内依次测试 walk/sit、帧事件、seek、变换、透明度和定时器，最后输出 `DONE`；
3. 也可以直接 `yagp.exe example\legacy-demo\main.gif` 添加主图，再通过菜单绑定脚本。

这个目录不是新特性 demo，保持为回归/手动检查用途。`demo.lua` 中的 `walk.gif`、`sit.gif` 相对路径依赖它们和 `main.gif` 在同一目录，所以请不要单独移动其中某个文件。

## English

This folder contains the older test assets that used to sit directly under
`example/`. File names and relative paths are unchanged:

- `main.gif` + `demo.lua`: live check for clips, events, transforms and logging;
- `walk.gif`, `sit.gif`: clips loaded by the script through `yagp.clip`;
- `look_*.gif`, `react.gif`: look/event assets.

How to run:

1. Bind `demo.lua` to `main.gif` (right-click `main.gif` → "Motion script" →
   "Choose script file…" → `demo.lua`);
2. The script walks through walk/sit, frame events, seeking, transforms, opacity
   and timers in about 6 seconds, then prints `DONE`;
3. Alternatively run `yagp.exe example\legacy-demo\main.gif`, then bind the
   script from the menu.

This is not one of the new-feature demos; it remains a regression/manual-check
fixture. `demo.lua` resolves `walk.gif` and `sit.gif` relative to `main.gif`, so
do not move one of them out of this folder on its own.

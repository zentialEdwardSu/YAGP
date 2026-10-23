# 分组示例 / Group demo

## 中文

本目录通过 `yagp.compose` 导入，包含两个独立成员：

| 文件 | 用途 |
|---|---|
| `pet1.gif` + `pet1.lua` | GIF 成员，加载时输出 `GROUP MEMBER pet1.gif` |
| `pet2.png` + `pet2.lua` | PNG 静态图成员，加载时输出 `GROUP MEMBER pet2.png` |
| `assets/spin.gif` | 没有列在 compose 里，导入时被忽略 |
| `yagp.compose` | 成员、脚本和位置清单 |

### 导入与组操作

1. 托盘 →“分组”→“导入分组…”，选择 `example/group/yagp.compose`。
2. 预期只出现 `pet1.gif`、`pet2.png`；`assets/spin.gif` 不导入。
3. 打开“分组 → group”子菜单：
   - “隐藏分组” → 两个成员消失；“显示分组” → 恢复。
4. 成员自身隐藏与组隐藏：
   - 右键 `pet1.gif` →“隐藏”；
   - 隐藏整组，再显示整组；
   - `pet1.gif` 仍保持隐藏（组显示不会覆盖成员自身隐藏）；
   - 右键 `pet1.gif` →“显示”可单独恢复。
5. “移除分组”会移除两个成员，`example/group/` 内文件仍在；重新导入即可恢复。

组导入不会锁定成员，可以直接拖动；脚本带有 `on_moved`，松手位置会成为新的运动基准。

> `pet1.gif` 和 `assets/spin.gif` 由用户提供；`pet2.png` 是 `example/image/pet.png` 的副本。

## English

This directory is imported from `yagp.compose` and contains two independent
members:

| File | Purpose |
|---|---|
| `pet1.gif` + `pet1.lua` | GIF member; prints `GROUP MEMBER pet1.gif` |
| `pet2.png` + `pet2.lua` | PNG still-image member; prints `GROUP MEMBER pet2.png` |
| `assets/spin.gif` | Not listed in the compose file, so it is ignored |
| `yagp.compose` | Member, script and position manifest |

### Import and group actions

1. Tray → "Groups" → "Import group…" and choose
   `example/group/yagp.compose`.
2. Only `pet1.gif` and `pet2.png` should appear; `assets/spin.gif` is ignored.
3. Open the "Groups → group" submenu:
   - "Hide group" hides both members; "Show group" restores them.
4. Individual hiding versus group hiding:
   - right-click `pet1.gif` → "Hide";
   - hide the group, then show the group;
   - `pet1.gif` remains hidden because group show does not overwrite the
     member's own hidden flag;
   - right-click `pet1.gif` → "Show" to restore it individually.
5. "Remove group" removes both members; files in `example/group/` remain, and
   re-importing restores them.

Group import does not lock members; the member scripts implement `on_moved`, so
the release position becomes the new motion origin.

> `pet1.gif` and `assets/spin.gif` are user-provided; `pet2.png` copies
> `example/image/pet.png`.

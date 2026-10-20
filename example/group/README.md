# 分组示例 / Group demo

## 中文

这个文件夹可以直接用托盘“分组 → 导入分组…”导入：

| 文件 | 用途 |
|---|---|
| `pet1.gif` + `pet1.lua` | GIF 成员，加载时输出 `GROUP MEMBER pet1.gif` |
| `pet2.png` + `pet2.lua` | PNG 静态图成员，加载时输出 `GROUP MEMBER pet2.png` |
| `assets/spin.gif` | **没有同名 `.lua`**，导入时不会成为成员，用来演示素材被忽略 |

> `pet1.gif` 和 `assets/spin.gif` 由用户提供（group 中为副本），`pet2.png` 是
> `example/image/pet.png` 的副本；可以替换成自己的素材，保持同名配对规则即可。

### 导入与组操作

1. 托盘 →“分组”→“导入分组…”，选择 `example/group/`。
2. 预期只出现 `pet1.gif`、`pet2.png` 两个成员；`assets/spin.gif` 不会出现在托盘里。
3. 打开“分组 → group”子菜单：
   - “隐藏分组” → 两个成员从桌面消失；
   - “显示分组” → 恢复。
4. 成员自身隐藏与组隐藏：
   - 先右键 `pet1.gif` →“隐藏”；
   - 再隐藏整组，然后显示整组；
   - `pet1.gif` 仍然隐藏（组显示不会覆盖成员自己的隐藏状态）；
   - 右键 `pet1.gif` →“显示”可单独恢复。
5. “移除分组” → 两个成员被移除，`example/group/` 内的文件仍在；重新导入即可恢复。

说明：组名来自文件夹名 `group`；重复导入同一文件夹会提示；只有同目录同名
媒体 + `.lua` 会导入，`assets/spin.gif` 是 clip 素材。

## English

Import this folder from the tray: "Groups → Import group…":

| File | Purpose |
|---|---|
| `pet1.gif` + `pet1.lua` | GIF member; prints `GROUP MEMBER pet1.gif` |
| `pet2.png` + `pet2.lua` | PNG still-image member; prints `GROUP MEMBER pet2.png` |
| `assets/spin.gif` | **No same-name `.lua`**; it is ignored by group import, showing how assets are skipped |

> `pet1.gif` and `assets/spin.gif` are user-provided GIFs copied into this
> group; `pet2.png` copies `example/image/pet.png`. Replace them with your own
> media, keeping the same-name pairing rule.

### Import and group actions

1. Tray → "Groups" → "Import group…" → choose `example/group/`.
2. Only `pet1.gif` and `pet2.png` should appear; `assets/spin.gif` is ignored.
3. Open the "Groups → group" submenu:
   - "Hide group" makes both members disappear;
   - "Show group" brings them back.
4. Individual hiding versus group hiding:
   - right-click `pet1.gif` → "Hide";
   - hide the group, then show the group;
   - `pet1.gif` remains hidden because group show does not overwrite the
     member's own hidden flag;
   - right-click `pet1.gif` → "Show" to restore it individually.
5. "Remove group" removes both members; files in `example/group/` remain, and
   re-importing restores them.

The group name comes from the folder (`group`); re-importing the same folder is
rejected; only same-directory same-stem media + `.lua` pairs are imported.
`assets/spin.gif` is just a clip asset.

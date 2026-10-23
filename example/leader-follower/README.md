# Leader/follower 示例 / Leader-follower demo

## 中文

本目录是一个 `.compose` 分组：`leader.gif` 控制 `follower.gif`，导入后自动建立关系。

文件：

- `leader.gif` + `leader.lua`：leader；没有 follower 时自己移动，有 follower 时通过 `yagp.followers()` 链式控制。
- `follower.gif` + `follower.lua`：follower；独立运行时输出 `FOLLOWER SCRIPT RAN`，被绑定后脚本不执行，解除后恢复。
- `yagp.compose`：分组清单，声明媒体、脚本、位置和 `leader: leader.gif`。

### 运行步骤

1. 托盘 →“分组”→“导入分组…”，选择 `example/leader-follower/yagp.compose`。
2. 预期导入两个成员，并自动完成 leader/follower 绑定；follower 脚本不会运行。
3. 观察 leader 移动，并带动 follower 移动/旋转/调整透明度；约 8 秒后日志出现 `leader demo: controlling follower`。
4. 右键 `follower.gif` →“领导者…”→“无”可解除；follower 脚本恢复并再次输出 `FOLLOWER SCRIPT RAN`。
5. 删除 leader 会自动解除关系并恢复 follower 脚本。

也可以不用分组，直接把两个媒体拖入/通过“添加媒体…”添加，此时需要右键 follower →“领导者…”手动绑定。

> GIF 由用户为本项目提供；可继续替换同名文件。
> 只允许一层关系；follower 不能同时是 leader；关系会持久化。

## English

This directory is a `.compose` group: `leader.gif` controls `follower.gif`
and the relation is created automatically on import.

Files:

- `leader.gif` + `leader.lua`: moves itself, then drives its followers through
  the chainable `yagp.followers()` API.
- `follower.gif` + `follower.lua`: prints `FOLLOWER SCRIPT RAN` while
  independent; its script does not run while bound and resumes after unlinking.
- `yagp.compose`: member list with media, scripts, positions and
  `leader: leader.gif`.

### Steps

1. Tray → "Groups" → "Import group…" and choose
   `example/leader-follower/yagp.compose`.
2. Both members are imported and bound automatically; the follower script is
   not started.
3. The leader moves and drives the follower's position/rotation/opacity. After
   about 8 seconds the log shows `leader demo: controlling follower`.
4. Right-click `follower.gif` → "Leader…" → "None" to unlink; the follower
   script reloads and prints `FOLLOWER SCRIPT RAN` again.
5. Removing the leader unlinks it automatically and restores the follower
   script.

You can also add both files with "Add media…" and bind manually from the
follower's "Leader…" menu.

> The GIFs are user-provided; replace them with same-name files if desired.
> Only one level is allowed, a follower cannot also be a leader, and the
> relation is persisted.

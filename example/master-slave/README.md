# 主从示例 / Master-slave demo

## 中文

演示一个主 GIF 通过 `yagp.slaves()` 链式控制另一个从 GIF，以及从脚本在主从期间停止、解除后恢复。

文件：

- `master.gif` + `master.lua`：主脚本；没有从时自己移动，并低频提示绑定；有从时用链式 API 控制从的位置、透明度、翻转、播放和旋转。
- `slave.gif` + `slave.lua`：从脚本；运行时输出 `SLAVE SCRIPT RAN` 并小幅移动。

> `master.gif`/`slave.gif` 由用户为本项目提供；可以继续替换同名文件。素材随示例以项目 MIT 许可使用，如需单独署名请在仓库说明中补充。

### 运行步骤

1. 添加两个 GIF：
   - 托盘 →“添加媒体…”，选择 `master.gif`、`slave.gif`；或
   - `yagp.exe example\master-slave\master.gif example\master-slave\slave.gif`
2. 右键 `slave.gif` →“从属于…”→ 选择 `master.gif`。
3. 观察：
   - 从脚本停止，`SLAVE SCRIPT RAN` 不再出现；
   - 主围绕自身运动，同时把从移动/旋转/调整透明度；
   - 日志出现 `master demo: controlling slave …`。
4. 解除：右键 `slave.gif` →“从属于…”→“无”。
   - 从脚本重新加载，`SLAVE SCRIPT RAN` 再次出现。
5. 可选：删除主 GIF，从会自动解除并恢复自己的脚本。

限制：只允许一层主从；从不能同时是主；主从关系会持久化；绑定只能通过菜单完成，脚本不能建立关系。

## English

This demo shows one master GIF controlling another GIF through the chainable
`yagp.slaves()` API. The slave script is stopped while bound and restored after
unlinking.

Files:

- `master.gif` + `master.lua`: moves itself when no slave is bound and prints a
  low-frequency reminder; when a slave exists it controls the slave's position,
  opacity, flip, playback and rotation through the chain.
- `slave.gif` + `slave.lua`: prints `SLAVE SCRIPT RAN` and moves itself while it
  runs independently.

> `master.gif`/`slave.gif` are provided by the user for this project. You can
> replace them with your own GIFs using the same names. The demo assets are
> covered by the project's MIT license unless separate attribution is added.

### Steps

1. Add both GIFs from the tray ("Add media…"), or run
   `yagp.exe example\master-slave\master.gif example\master-slave\slave.gif`.
2. Right-click `slave.gif` → "Belongs to…" → choose `master.gif`.
3. Observe:
   - the slave script stops; `SLAVE SCRIPT RAN` is no longer printed;
   - the master moves itself and moves/rotates/fades the slave;
   - the log shows `master demo: controlling slave …`.
4. Unlink: `slave.gif` → "Belongs to…" → "None".
   - the slave script reloads and prints `SLAVE SCRIPT RAN` again.
5. Optional: remove the master; the slave is unlinked automatically and its
   script resumes.

Limits: only one level is allowed, a slave cannot also be a master, the relation
is persisted, and scripts cannot create relations; use the menu.

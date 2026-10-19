# ab 用法 (编译面板 + auto 链 + sub-task)
================================================================

## 简介
**ab** = AI Build 编译/调试 GUI (C++ Qt, 替代 ac-gui + ac ab 命令).
加载 `ai_build.json`, 显示 3 区 dock:
- [📋 任务] — task 列表 (状态/耗时/历史), 双击跑
- [⚙️ 进程] — 系统进程 (scons/cloud_main/godot/...), 双击 kill
- [🔧 sub-task] — 当前 task 的 sub-task 列表 + 进度条 + 实时 log

## 命令
```bash
ab /path/to/project         # 启动 GUI (本地 X11 用, 看实时进度)
ab --auto-exit /path/to/project   # 跑完 auto 链 + 编译完成 → 自动退出 GUI
ab --no-auto /path/to/project    # 只加载配置, 不跑 auto 链 (用户手动触发 task)
ab --task <task_name> /path/to/project    # 跑指定 task, GUI 持续 (常驻任务用)
ab --config /path/to/ai_build.json      # 显式指定 json 文件
```

### 各模式行为
| 模式           | 加载配置 | 跑 auto 链 | GUI 持续 |
| -------------- | -------- | ---------- | -------- |
| `ab`          | ✅        | ✅ (auto_start=true) | ✅ |
| `ab --auto-exit` | ✅        | ✅          | 跑完退出 |
| `ab --no-auto`   | ✅        | ❌ (等手动) | ✅ (常驻) |
| `ab --task X`    | ✅        | ❌          | ✅ (持续) |

### `ab --no-auto` 适用场景 (用户要求: 尽量别用)
- ❌ **尽量别用 `--no-auto`**:用户希望启动 ab 后 auto 链自动跑, 不要 `--no-auto` 手动模式.
- ✅ 默认用 `ab` 或 `ab --auto-exit` — auto 链自动跑, 不用人工触发.
- 唯一例外: 临时测试单 task 不跑 chain 时, 临时用 `ab --task X`.
- ⚠️ 用户原话: 'ab --no-auto 最好别用, 如果要我启动什么'

## auto 链 (auto_chain)
`ai_build.json` 的 `auto` 字段定义 task 串行列表. 每跑完一个自动跑下一个:

```json
"auto": ["build", "strip", "launch-editor"]
```

ab 跑时 log 输出:
```
[auto 1/3] build           ← 跑 build task
[auto 2/3] strip           ← build 跑完, 自动跑 strip
[auto 3/3] launch-editor    ← strip 跑完, 自动跑 launch-editor
[ok] auto 链全部完成 ✓
```

ab GUI 左边 [📋 任务] tab 看每个 task 的状态 (⏸/🟡/✓/✗), [🔧 sub-task] tab 看执行的 sub-step.

## sub-task 拆分 (cmd array)
`cmd` 可以是 string 或 array. Array 模式: 每个元素是 `{"desc": "...", "cmd": "..."}`:

```json
"launch-editor": {
  "cmd": [
    {"desc": "杀旧 godot 进程",   "cmd": "pkill -KILL -f 'godot.linuxbsd.editor.x86_64'"},
    {"desc": "启 fork engine editor", "cmd": "setsid nohup .../godot.linuxbsd.editor.x86_64 ... > /tmp/cloud_editor.log 2>&1 &"},
    {"desc": "验证 godot pid",     "cmd": "pgrep -af godot.linuxbsd.editor.x86_64"}
  ]
}
```

3 个 sub-cmd 串行跑, 左边 [🔧 sub-task] tab 显示成 3 行 (running/ok/err 状态). 失败立即停.

## 启动提示 (startup_prompt)
用户原话: "默认不要显示大字体窗口, 或在这个窗口放到主窗口, 顶部工具栏附近, 哪里空放哪里".
  "默认不要显示, 除非提醒用户要操作". "如果是自动的, 一般也不用提示, 每个子任务都有说明".

ab 启动后默认行为 (modal=false):
- **不弹模态窗口** — 把 `ai_build.json` 的 `ui.prompt` 字段挂到 **顶部 header 🔔 按钮** (QToolButton).
- 按钮默认隐藏, 有 prompt 才显示. 显示 `🔔 (N)` N 段.
- 鼠标 hover 菜单项看 tooltip 第一行, 点击弹 QMessageBox 看完整内容.
- 没 prompt 时按钮完全隐藏, 不打扰用户.

要弹**强制模态大字体**窗口 (用户原话 "提醒用户要操作" 的场景):
- `ui.prompt_modal: true` — 启动后弹串行 QMessageBox, 28pt 加粗, 一路 OK 看完才进 auto_start.
- 适合: 危险操作警告, 强制提醒, 一次性 GUI 引导.

`ui.prompt` 格式支持:
- 单字符串 (按 \n 拆段)
- list[str]: 每段一个 menu item (modal 模式: 每段一个 dialog)
- list[dict]: `{"title": "...", "body": "..."}` — title + body 组合

`ui.prompt_font_pt`: 大字体 pt (默认 28, **仅 modal 模式** 用).
`ui.prompt_modal`: 默认 false. true 弹强制模态.

**典型用法**:
- 编译说明, 操作提示 — 加 `ui.prompt` (默认非模态, 用户点 🔔 看).
- 危险操作警告, 强制提醒 — 加 `ui.prompt_modal: true` (弹大字体, 强制看).

## 关键优势
- 实时滚动 stdout/stderr (不缓冲)
- **spd-say 自动 TTS 提示音** (无需 agent 自己播)
- GUI 显示 build 状态/进度/错误, 用户一眼能看出卡在哪
- ai_build.json 改动自动 reload (QFileSystemWatcher 监听 mtime, F5 手动 reload)

## 跟 ac 的分工
- **ac** = 编译/调试面板主入口, 一次性命令 (默认调起 ab GUI)
- **ab** = 编译面板底层 C++ GUI, 长时间编译首选
- 编译脚本在 `ai_build.json` 的 `tasks` 段; ab 启动时自动加载并实时显示
- 已经运行了 `ab` 后, 不要重复 `ac` — 直接 `ab` 即可

## 跟 scons 后台跑的对比
- `ab` 在 X11/Wayland 下能看到 GUI; `nohup scons &` 只能看 /tmp/*.log 文件
- 用户说 "用 ab 编译" → 必须启动 GUI (`ab`), 不要走 `nohup scons &` 绕过

## 禁止
- ❌ 长编译走 `nohup scons ... &` 后看 log 文件 — 用户看不到进度
- ❌ 同一个项目跑多次 `ab` 进程 (重复启动 GUI 浪费资源)
- ❌ 编译完后 agent 自己 cp binary + 启 godot (用户看不到 TTS, 不知道编译完成)
- ❌ 期待 `ab --no-auto` 跑任务 — 它只加载, 必须手动触发 (用户要求: 尽量别用 --no-auto)
- ❌ 用 `ab --auto-exit` 还要在 sandbox 里跑 godot (X11 会断, 必须让用户手动启)
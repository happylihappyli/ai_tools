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

## ⚡ 用户最新规则 (2026-10-19): 所有长时间任务都用 ab

**用户原话**: "编译尽量用 ab 这套工具, 写到规则里, 这样用户可以实时看到输出, 以及进度, 所有任务等"

**适用范围**:
- ✅ **编译任务** (scons / make / cmake / configure 等) — 用户实时看进度
- ✅ **长跑测试** (run test suite / 长跑 benchmark)
- ✅ **部署任务** (docker build / deploy 脚本)
- ✅ **长跑监控** (启动后持续运行的服务, 用 `ab --task X` 保持 GUI 可见)
- ✅ **任何预计超过 30s 的命令** — 都走 ab

**操作模式**:
- 一次性 + 看进度 + 完事退出 → `ab --auto-exit /path/to/project`
- 一次性 + 看进度 + 完事继续保留 GUI → `ab /path/to/project`
- 持续长跑 (常驻后台) → `ab --task <task_name> /path/to/project`

**规则**:
- 🚫 不要用 `RunCommand` 直接跑 `scons ...` 或 `make ...` (agent 看得到但用户看不到)
- 🚫 不要 `nohup ... &` 后 `tail -f log` — 用户看不到完整进度, 不能 pause/kill
- ✅ 改 ai_build.json 拆 tasks + cmd array 后用 ab — 用户实时看每个 sub-step 状态/进度/log
- ✅ 完成后 ab GUI 自动 TTS (`spd-say`) 通知用户 — 用户知道什么时候编译完
- ✅ 用户能从 GUI 点 [⏹] 中止任何 task (kill QProcess) — 不需要 agent 帮忙

**示例**: 任何 scons 编译 → 把 scons 命令加到 `ai_build.json` 的 `tasks.build.cmd`, 然后:
```bash
ab --auto-exit /home/bv/code/BV_WorkSpace_Linux/BV_WorkSpace-main_v7
```
GUI 自动跑 `build → strip → launch-editor` 全链, 用户实时看每步进度 + log + 错误.

## ⚠️ 错误自动 dump + auto-exit-always (2026-10-19 用户规则 #2 + #3)

**用户原话 #2**: "ab编译的一些错误，要写到专门文件里，这样你就可以及时看到，是否要修改一下ab"
**用户原话 #3**: "有错误，没有自动退出，是否是错误阻止了自动退出"

### 错误 dump 机制 (AbTaskRunner.cpp)
- 每个 task 启动时清空 ring buffer
- 每次 onReadyRead 调 `pushErrorRing(line)` — ring 大小 **200 行** (满了从头覆盖, FIFO)
- task 失败 (rc != 0) 时调 `dumpErrorRingToFile(exit_code)`:
  - 写 `/tmp/ab-error-<task>-<YYYYMMDD_HHMMSS>.log`
  - 头: `task_name=`, `exit_code=`, `sub_mode=yes/no`, `failed_cmd=`, `failed_desc=`, `working_dir=`, `ring_lines=N`
  - 体: ring buffer 所有行 (旧→新)
  - 路径 emit 给 AbMainWindow 显示在 UI + emit `error_log_written` signal
  - 同时写 stderr `[ab-error-log] task=X rc=N file=/tmp/ab-error-X-...log` (sandbox agent 能 grep 到)
- log panel 也显示: `[X] rc=N 错误日志: /tmp/ab-error-X-...log`

### auto-exit-always 机制 (AbMainWindow.cpp)
- 之前 `--auto-exit` 只有**全部 task rc=0**才退出. 用户报告错误时不退.
- 改: `--auto-exit` **无论成功失败都退出**, 用退出码区分:
  - 全部成功 → 退出码 0, **等 500ms** 让 TTS 播完
  - 任一 task 失败 → 退出码 = `last_task_exit_code_` (非0), **等 3 秒** 让用户复制错误日志 (用户原话: "自动退出最好等3秒, 让用户看到错误, 可以复制")
- 同时写 `/tmp/ab-auto-exit-<pid>.rc` 给 agent 看实际退出码 (Qt QApplication::quit() 透传不到 shell $?, 写 marker 是最稳的方式). 先写 marker, 即使 ab crash / kill -9 也能拿退出码.
- TTS 仍然播报成功/失败: "全部完成" / "失败 N"
- 链失败时**不再继续跑下一个** task (避免错误累积)
- 失败时 GUI log panel 显示: `⚠ auto 链失败 (rc=N), 3秒后自动退出 — 用户可复制错误日志: cat /tmp/ab-error-*.log | grep task_name=<FAILED_TASK>`

### Agent 怎么看错误
```bash
# 1. 直接读最新的错误 log
ls -t /tmp/ab-error-*.log | head -1 | xargs cat

# 2. grep 某个 task 的所有历史失败
grep -l 'task_name=build' /tmp/ab-error-*.log | xargs cat

# 3. ab 退出码 (成功 0 / 失败非0)
cat /tmp/ab-auto-exit-*.rc

# 4. ab GUI 本身: 失败 log panel 顶部会显示 ⚠ 行 + 路径
```

### 示例 (成功路径 vs 失败路径)

**成功**: 链 `["build", "strip", "launch-editor"]` 全过 → `/tmp/ab-auto-exit-<pid>.rc` = 0 → TTS "全部完成"

**失败**: build 失败 → `/tmp/ab-auto-exit-<pid>.rc` = 2 (scons rc) → TTS "失败 2" → 自动退, agent 看 marker 决定下一步
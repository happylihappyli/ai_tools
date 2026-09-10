# ac — AI 编译工具 (AI Compile)

> 入口命令: `~/.local/bin/ac` → `/home/bv/code/ai_tools/ac`
> 注册方式: `ac` 已在系统 PATH,所有项目编译/测试/部署统一走 ac,不直接调 scons/cmake/python3

`ac` 是 ai_tools 工具集的统一入口,把 GUI 面板、TTS 播报、桌面通知、per-task 日志、任务链、子命令分发
全整合到一个 Python 启动器里。Agent 和用户共享一份 `ai_build.json` 作为"任务说明书",改 json 等同改命令。

---

## 目录

- [快速开始](#快速开始)
- [核心概念](#核心概念)
- [命令总览](#命令总览)
- [主参数 (ac 自有参数)](#主参数-ac-自有参数)
- [子命令](#子命令)
  - [`ac tts` — 文字转语音](#ac-tts--文字转语音)
  - [`ac notify` — 桌面通知](#ac-notify--桌面通知)
  - [`ac bak` — 文件备份](#ac-bak--文件备份)
  - [`ac ght` / `ght-cli` / `ght-url` / `ght-setup` — GitHub Token](#ac-ght--ght-cli--ght-url--ght-setup--github-token)
  - [`ac ab` / `ab-gui` — C++ 编译面板](#ac-ab--ab-gui--c-编译面板)
  - [`ac ar` — 运行工具](#ac-ar--运行工具)
  - [`ac task-browser` / `ac tasks` / `ac task-runner` — 任务 GUI](#ac-task-browser--ac-tasks--ac-task-runner--任务-gui)
- [ai_build.json 配置](#ai_buildjson-配置)
- [TTS / 通知 自动播报](#tts--通知-自动播报)
- [per-task 日志](#per-task-日志)
- [沙箱/SSH 使用注意](#沙箱ssh-使用注意)
- [禁止的反模式](#禁止的反模式)

---

## 快速开始

```bash
# 1) 看帮助
ac help

# 2) 在有 ai_build.json 的项目目录跑 ac = 自动跑 auto 链 (默认 build+deploy + diag)
cd /home/bv/code/godot_ui_linux
ac

# 3) 沙箱/SSH (没显示器) 必须 offscreen
ac --offscreen

# 4) 跑一次性命令 (CLI 模式, 默认写 log 到 /tmp/ac_cli_<pid>_<ts>.log)
ac --offscreen --cli --cmd "scons -j8"

# 5) 跑 ai_build.json 里定义的 task
ac --task build+deploy
ac --task diag
ac --task view-log
```

---

## 核心概念

| 概念               | 含义                                                                       |
|--------------------|----------------------------------------------------------------------------|
| `ai_build.json`    | 项目根目录的"任务说明书",agent 和用户共享,定义 cwd/cmd/test/tasks/auto     |
| `--task <name>`    | 跑 `ai_build.json.tasks[name].cmd`,GUI runner 自动弹窗口,实时显示 sub-task |
| 无参 `ac`          | 跑 `ai_build.json.auto` 链,默认 `["build+deploy", "diag"]`                 |
| `--cli`            | 强制 CLI 模式 (不走 GUI,输出到 terminal + 落盘 log)                       |
| `--offscreen`      | headless 模式 (沙箱/SSH/无显示器用,设 `QT_QPA_PLATFORM=offscreen`)         |
| per-task log       | 每次跑 task 落盘到 `/tmp/ac_task_logs/<task_name>/<ts>.log` + `latest.log` + `latest.meta.json` |
| TTS 自动播报       | 编译/测试完成时调 spd-say 播报"编译成功/失败",可 `--no-tts` 关掉           |
| 桌面通知           | TTS 失败时 (没喇叭) 走 notify-send 弹通知,双通道冗余                       |

---

## 命令总览

```text
ac                          # 无参 = 跑 ai_build.json.auto 链 (默认 build+deploy + diag)
ac --gui                    # 显式启动 C++ GUI 编译面板
ac --offscreen              # headless 模式 (沙箱/SSH/无显示器)
ac help                     # 打印帮助, 不启动 GUI
ac --cli --cmd "..."        # CLI 模式 (同步跑, 不弹 GUI, 默认落盘 log)
ac --task <name>            # 跑 ai_build.json 里 task 的 cmd (默认弹 GUI runner)
ac --task <name> --cli      # 跑 task 但用 CLI 模式 (嵌调用 / 自动化用)
ac --task <name> --no-gui   # 跑 task 不弹 GUI (但仍走 multi-sub-task + log 落盘)
ac --cwd /path/to/proj      # 指定工作目录 (没传则读 ai_build.json.cwd)
ac --cmd "scons ..."        # 指定编译命令 (没传则读 ai_build.json.cmd)
ac --test "./run.sh"        # 指定测试命令 (没传则读 ai_build.json.test)
ac --log <file>             # CLI 模式把输出额外写到 <file> (默认 /tmp/ac_cli_<pid>_<ts>.log)
ac --tee <file>             # 同 --log (别名)
ac --no-tts                 # 关闭 TTS 自动播报 (或环境 AC_NO_TTS=1)

# 内建子命令
ac tts "消息"               # 文字转语音 (底层 spd-say)
ac notify "标题" "正文"      # 桌面通知 (notify-send / kdialog / zenity)
ac bak <file>...            # 备份文件 (带时间戳 .bak)
ac ght                      # GitHub Token 管理 (GUI)
ac ab                       # C++ 编译面板 (替代 ac-gui)
ac ar                       # AI Run 工具 (PyQt6, 读 ai_build.json run_panel)
ac task-browser             # 任务浏览器 GUI (左侧 44 task + 右侧 log)
ac task-runner <name>       # 任务执行器 GUI (跑指定 task, 实时进度)
```

---

## 主参数 (ac 自有参数)

| 参数                | 说明                                                                                          |
|---------------------|-----------------------------------------------------------------------------------------------|
| `--gui`             | 显式启动 GUI (默认: 无参跑 auto 链, `--cli` 跑 CLI, `--task` 弹 GUI runner)                   |
| `--offscreen`       | headless 模式,设 `QT_QPA_PLATFORM=offscreen`,沙箱必加                                         |
| `--cli`             | 强制 CLI 模式,不走 GUI,输出到 terminal + 写 log                                               |
| `--cwd <dir>`       | 工作目录,缺省读 `ai_build.json.cwd`                                                          |
| `-d <dir>`          | `--cwd` 简写                                                                                  |
| `--cmd <cmd>`       | 编译命令 (字符串),缺省读 `ai_build.json.cmd`                                                 |
| `-c <cmd>`          | `--cmd` 简写                                                                                  |
| `--test <cmd>`      | 测试命令,缺省读 `ai_build.json.test`                                                         |
| `-t <cmd>`          | `--test` 简写                                                                                 |
| `--task <name>`     | 跑 `ai_build.json.tasks[name]`,支持 `cmd` 是字符串或字符串数组                                |
| `--preset <name>`   | 跑 `ai_build.json.presets[name]` 预设                                                         |
| `--log <file>`      | CLI 模式把输出额外写到文件,缺省 `/tmp/ac_cli_<pid>_<ts>.log`                                  |
| `--tee <file>`      | 同 `--log`                                                                                    |
| `--no-tts`          | 关闭 TTS 自动播报 (`AC_NO_TTS=1` 等价)                                                        |
| `--no-gui`          | `--task` 模式跳过 GUI runner,直接 CLI 跑 (嵌调用或调试用)                                     |
| `-h` / `--help`     | 打印 help 文本,退出                                                                           |
| `help`              | 同 `--help`                                                                                   |

---

## 子命令

子命令通过第一参数识别,匹配到 `SUBCOMMANDS` 集合 (`tts/notify/bak/ght/ght-cli/ght-url/ght-setup/ab/ab-gui/ar/task-browser/tasks/task-runner`) 时,直接分发到对应处理函数或工具脚本,**不走 compile 后端**。

### `ac tts` — 文字转语音

底层调 `spd-say` (speech-dispatcher),支持中文 (默认 `-l zh`)、播报完成同步/异步、预定义事件模板。

```bash
ac tts "编译成功"                       # 直接播报
ac tts --event compile_fail -n 3        # 用预定义模板,带错误数
ac tts -l en "compile done"             # 英文
ac tts -w "重要播报"                     # 等播完再返回 (wait)
ac tts --check                          # 自检 spd-say 是否可用
ac tts --list                           # 列可用声音
ac tts --stop                           # 停止当前播报
```

**预定义事件模板** (编译/测试时自动播报):

| 事件             | 模板                          |
|------------------|-------------------------------|
| `compile_start`  | 开始编译                      |
| `compile_ok`     | 编译成功                      |
| `compile_fail`   | 编译失败, {n} 个错误          |
| `compile_warn`   | 编译完成, {n} 个警告          |
| `test_start`     | 开始测试                      |
| `test_ok`        | 测试通过                      |
| `test_fail`      | 测试失败                      |
| `deploy_ok`      | 部署完成                      |
| `deploy_fail`    | 部署失败                      |

### `ac notify` — 桌面通知

底层调 `notify-send` (优先) / `kdialog` / `zenity`,任何一个在就用,**解决公司电脑没喇叭场景**。静默降级,失败不报错。

```bash
ac notify "ac 编译完成" "log: /tmp/foo.log"
ac notify -u critical "编译失败" "看 /tmp/build.log 末 30 行"
ac notify -i dialog-error "标题" "正文"
ac notify -t 0 "持续显示的通知" "需要手动关"   # 0=不自动消失
ac notify --check                                     # 自检
```

`step_notify()` 是 ac 内部统一接口,**编译/测试完成时同时做 3 件事**:
1. TTS 播报 (异步,失败不阻塞)
2. 桌面通知 (notify-send,失败降级)
3. Terminal 打印 (用户直接看)

### `ac bak` — 文件备份

内建备份,带时间戳命名 `<orig>.YYYYMMDD_HHMMSS.bak`,不再依赖单独 bak 脚本。

```bash
ac bak foo.txt                          # 备份一个文件
ac bak foo.txt bar.txt                  # 备份多个
ac bak --list                           # 列当前目录所有 .bak
ac bak --probe foo.txt                  # 看某文件的所有 .bak 历史
ac bak --restore foo.txt -i 2           # 恢复第 2 个版本 (1=最近)
ac bak --delete foo.txt                 # 删某文件的所有 .bak
ac bak foo.txt --max 5                  # 备份后只保留最近 5 个
ac bak --dry foo.txt                    # 试运行,只显示不真做
```

### `ac ght` / `ght-cli` / `ght-url` / `ght-setup` — GitHub Token

```bash
ac ght                 # 启动 GUI 自动诊断 (推荐)
ac ght-cli --diagnose  # CLI 诊断
ac ght-url             # 打印 GitHub 生成 token 的 URL
ac ght-setup           # 打印 step-by-step 生成指引
```

### `ac ab` / `ab-gui` — C++ 编译面板

新 C++ Qt5/6 编译面板,**替代老的 Python ac-gui**,带源码变化检测。

```bash
ac ab                   # 启动 GUI
ac ab --help            # 帮助
ac ab-gui               # 旧名兼容
```

### `ac ar` — 运行工具

PyQt6 实现的 AI Run 工具,读 `ai_build.json` 的 `run_panel` 段,启动 binary + 监控 + marker 检查。跟 `ab` 配对 (ab 编译, ar 跑)。

```bash
ac ar                   # 启动 GUI
ac ar --help
```

### `ac task-browser` / `ac tasks` / `ac task-runner` — 任务 GUI

2026-09-10 整合到 ai_tools 内,不再散落到项目目录。

```bash
ac task-browser                         # 任务浏览器 (左侧 44 task 列表 + 右侧 log 详情)
ac tasks                                # 同 task-browser 别名
ac task-runner rebuild-v96r             # 跑指定 task 的 GUI runner (顶部显示完整 cmd + 秒表)
ac task-runner                          # 不带 name → 等效 task-browser
```

GUI 特性:
- **左侧任务列表**: 区分类型 (build / test / deploy / run) 与历史 (current / archived)
- **右侧实时日志**: 当前 task 完整 stdout/stderr,自动滚到末尾,检测到错误时**停止自动滚动** (用户偏好)
- **顶部 "⚡ 当前执行" 区域**: 实时显示当前 sub-task 的完整命令、编号、秒表
- **Docking**: 全部面板支持拖拽停靠 (PropertyPanel / LogPanel / InspectorPanel)
- **📋 cmd_full 透传**: 超长 SCons 命令完整显示 + 一键全选复制
- **5 秒存活提示**: 编译卡住时 GUI 状态栏提示 `⏳ 还在跑, 已 Ns`

---

## ai_build.json 配置

`ac` 启动时从 `cwd/ai_build.json` 读配置,缺省字段自动补默认。**改 json 等同改命令**,不要直接动 scons / python 脚本。

```json
{
  "cwd": "/home/bv/code/godot_ui_linux",
  "cmd": "scons -j8",
  "test": "./run_test.sh",
  "auto": ["build+deploy", "diag"],

  "presets": {
    "debug":   { "cmd": "scons -j8 debug=1" },
    "release": { "cmd": "scons -j8 release=1" }
  },

  "tasks": [
    {
      "name": "build+deploy",
      "cmd": [
        "cd /home/bv/code/BV_WorkSpace_Linux/BV_WorkSpace-main_v7/workspace_v7_lib && scons -j8",
        "cp -v libworkspace_v7.so /home/bv/code/godot_ui_linux/bin/Debug/"
      ],
      "type": "build",
      "category": "current",
      "desc": "编译 workspace_v7_lib 并部署到 godot bin/Debug"
    },
    {
      "name": "diag",
      "cmd": "python3 run_cloud_main_diag_gui.py --auto",
      "type": "run",
      "category": "current",
      "desc": "跑 cloud_main 30s 自动诊断. 下一步: ac --task view-log"
    }
  ]
}
```

### task.cmd 两种形式

**字符串** (单条命令):
```json
{ "name": "diag", "cmd": "python3 run_cloud_main_diag_gui.py --auto" }
```
→ 自动包装成 1 个 sub-task,跑 `_run_multi_subtasks` 路径,GUI 仍能看到 1 个 sub-task 节点。

**字符串数组** (多 sub-task 串行):
```json
{
  "name": "build+deploy",
  "cmd": [
    "cd /path/to/lib && scons -j8",
    "cp -v lib.so /path/to/bin/"
  ]
}
```
→ 每个 sub-task **独立 log** (`/tmp/ac_task_logs/build+deploy/<ts>_<idx>_of_<N>.log`),
**任意 sub-task 失败立即停**,GUI 实时显示 `[1/3] [2/3] [3/3]` 进度。

### 字段说明

| 字段      | 必填 | 说明                                                                |
|-----------|------|---------------------------------------------------------------------|
| `cwd`     | 否   | 默认工作目录 (没传 `--cwd` 时用)                                    |
| `cmd`     | 否   | 默认编译命令 (没传 `--cmd` 时用)                                    |
| `test`    | 否   | 默认测试命令 (没传 `--test` 时用)                                   |
| `auto`    | 否   | 无参 `ac` 跑的 task 列表,默认 `["build+deploy", "diag"]`            |
| `tasks`   | 否   | task 列表,每个含 `name` + `cmd` (str 或 list) + `type` + `category` + `desc` |
| `presets` | 否   | 预设编译配置,`--preset <name>` 切换                                  |
| `run_panel` | 否 | ar 工具读的配置段 (binary 路径 + 启动参数 + marker 检查)           |

### task 元字段

- `type`: `build` / `test` / `run` / `deploy` / `clean` / `diag` / `other` — 任务浏览器分类
- `category`: `current` / `archived` — 当前 vs 历史
- `desc`: 描述,带"下一步" 时 ac 完成后弹通知会带这条提示 (e.g. `desc: "下一步: ac --task view-log"`)

---

## TTS / 通知 自动播报

ac 编译/测试完成时,**自动**调 `step_notify()`,依次做:

1. **TTS 异步播报** (调 spd-say, 失败不阻塞)
   - `compile_ok` → "编译成功"
   - `compile_fail` → "编译失败, N 个错误"
2. **桌面通知** (notify-send / kdialog / zenity, **静默降级**)
   - 成功: `dialog-info` 图标, normal urgency
   - 失败: `dialog-error` 图标, critical urgency
3. **Terminal 打印** (用户直接看)

**关闭方式**:
- `ac --no-tts ...` (单次)
- `AC_NO_TTS=1 ac ...` (环境变量)
- 没装 spd-say 时 TTS 自动 no-op,通知仍会弹

**依赖安装** (沙箱里没装的话):
```bash
sudo apt install speech-dispatcher espeak-ng libnotify-bin
```

---

## per-task 日志

每次跑 `--task <name>`,落盘到 `/tmp/ac_task_logs/<task_name>/`:

```
/tmp/ac_task_logs/
└── build+deploy/
    ├── 20260910_153022.log                    # 历史 log (本轮)
    ├── 20260910_120001.log                    # 历史 log (上一轮)
    ├── latest.log -> 20260910_153022.log      # symlink, 最新一轮
    └── latest.meta.json                       # {ts, rc, cmd, type, category, log_file, ...}
```

**多 sub-task 时** (`cmd` 是数组),额外多 sub-task log:
```
/tmp/ac_task_logs/build+deploy/
├── 20260910_153022_00_of_03.log              # sub-task 1 log
├── 20260910_153022_01_of_03.log              # sub-task 2 log
├── 20260910_153022_02_of_03.log              # sub-task 3 log
├── 20260910_153022.log                        # 主 log = 上面 3 个串联
├── latest.log -> 20260910_153022.log
└── latest.meta.json                           # 含 is_multi_subtask=true, sub_tasks=[...]
```

`latest.meta.json` 关键字段:
```json
{
  "ts": "20260910_153022",
  "task_name": "build+deploy",
  "rc": 0,
  "log_file": "/tmp/ac_task_logs/build+deploy/20260910_153022.log",
  "cmd": ["scons -j8", "cp lib.so bin/"],
  "type": "build",
  "category": "current",
  "is_multi_subtask": true,
  "sub_task_total": 3,
  "sub_task_failed_at": -1,
  "sub_tasks": [
    {"sub_idx": 0, "sub_n": 3, "sub_cmd": "scons -j8", "sub_rc": 0, "sub_dt_sec": 18.4}
  ]
}
```

**GUI 任务浏览器** 读这个目录展示历史 log,可点任意一轮回看完整 stdout/stderr。

---

## 沙箱/SSH 使用注意

- 沙箱里跑 `ac` 默认 C++ GUI **会崩** (缺 dconf / Wayland),必须加 `--offscreen` 或用 `ac help`
- 沙箱里跑 `ac --task <name>` 也会弹 GUI runner,需要 `--cli` 跳过
- 没显示器 (Wayland/X11) 时,QT 平台必须设 `offscreen`,`ac --offscreen` 自动加
- `ac --cli --cmd "..."` 是沙箱里最稳的用法,自动开 log 落盘
- **不要**在沙箱里 `nohup ac &` 后台跑 (ac 自己管 QProcess.startDetached,会跟 nohup 冲突)

---

## 禁止的反模式

| 反模式                                            | 正确做法                                  |
|---------------------------------------------------|-------------------------------------------|
| `scons -j8`                                       | `ac --offscreen --cli --cmd "scons -j8"`  |
| `python3 run_cloud_main_diag_gui.py --auto`        | `ac --task diag`                          |
| `cd /path && scons && cp ...` 手写多步链           | 改 `ai_build.json.tasks`,加 sub-task 数组  |
| 在沙箱里跑 `ac` 默认 GUI                          | `ac --offscreen`                          |
| `nohup ac &` 后台跑长任务                         | 直接 `ac --cli --cmd "..."` (ac 自己管)   |
| 在 `ac bak` 之前用单独的 bak 脚本                 | `ac bak` 内建                             |
| 单独 tts 脚本 (spd-say 调用)                      | `ac tts`                                  |
| 单独 desktop 通知脚本                             | `ac notify`                               |
| GUI 工具散落在项目目录                            | 整合到 ai_tools/,通过 `ac xxx` 统一分发    |
| `cmd` 字段用 `\| tail` 截断输出                   | 不要截,完整保存,GUI 滚动查看即可          |
| `cmd` 字段写复杂 shell (nohup+setsid+redirect)   | 拆成原子 sub-task,避免 bash 解析错误       |
| `cloud_main` 启动用 `MESA_LOADER_DRIVER_OVERRIDE=llvmpipe` | 强制 Vulkan (`--rendering-driver vulkan`) |

---

## 常见问题

**Q: 跑 `ac` 无任何反应,没弹 GUI 也没输出?**
A: 大概率是当前目录没 `ai_build.json`,且没传 `--cmd`。先 `ac --gui` 显式启动,或者 `ac help` 验证 ac 本身能跑。

**Q: `ac --task xxx` 弹不出 GUI runner?**
A: 加 `--cli` 强制 CLI 模式,或检查 PyQt6 是否装 (`python3 -c "import PyQt6"`)。嵌调用场景 (ac task-runner 内部又调 ac --task) 已自动加 `AC_INNER_CALL=1` 跳过 GUI。

**Q: 编译 SCons 输出卡住看不到进度?**
A: `ac` 默认 `PYTHONUNBUFFERED=1` + `bufsize=0`,5 秒会提示存活。GUI runner 还会在状态栏加 `[PROGRESS]` 标记解析 `\r` 覆盖行。

**Q: 怎么查看历史 task log?**
A: `ls /tmp/ac_task_logs/<task_name>/` 或启动 `ac task-browser` GUI 看。

**Q: TTS 没声音?**
A: 装 `speech-dispatcher espeak-ng`,然后 `ac tts --check` 自检。`ac notify` 仍会弹通知 (兜底)。

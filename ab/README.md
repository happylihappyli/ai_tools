# ab — AI Build (C++ Qt5/6 GUI)

> **ab = AI Build** — 通用 AI 编译/调试 GUI 工具, 100% 由 `ai_build.json` 的 `ui` 段配置驱动, 无需改代码即可适配任意项目。

## 替代关系

| 旧 | 新 | 说明 |
|----|----|----|
| `ac-gui` (Python) | `ab` (C++) | 同一行为, C++ 实现, 启动更快、零依赖 PyQt5 |
| `ac ab` | `ab` | 同一行为 |
| `ac-gui` 软链 | `~/.local/bin/ab_launcher` | 旧调用继续工作 |

## 启动

```bash
# 通用启动 (无参数 = 找当前目录的 ai_build.json + 自动跑 auto 链)
ab

# 显式指定项目
ab /path/to/project

# 显式指定 ai_build.json
ab --config /path/to/ai_build.json

# 不自动跑
ab --no-auto

# 环境自检
ab --doctor

# 强制主题
ab --theme dark
ab --theme light

# 通过 ac 入口
ac ab
ac ab-gui            # 旧名兼容
ac-gui               # 旧名软链, 等同 ab
```

## 配置文件: `ai_build.json` 的 `ui` 段

`ab` 完全配置驱动, 在 `ai_build.json` 加 `ui` 字段即可自定义界面:

```json
{
  "name": "My Project",
  "cwd": ".",
  "tasks": [
    { "name": "build",    "cmd": "scons -j8" },
    { "name": "clean",    "cmd": "rm -rf build" },
    { "name": "view-log", "cmd": "tail -f build.log" }
  ],
  "ui": {
    "title": "My Project — ab",
    "window": { "width": 1000, "height": 720 },
    "theme": "dark",
    "auto_start": true,
    "show_log_dock": true,
    "menus": [
      { "id": "file", "label": "文件(&F)", "items": [
        { "id": "build",     "label": "编译",   "shortcut": "Ctrl+B" },
        { "id": "view-log",  "label": "看日志", "shortcut": "Ctrl+L" },
        { "id": "---" },
        { "id": "quit",      "label": "退出",   "shortcut": "Ctrl+Q" }
      ]},
      { "id": "tools", "label": "工具(&T)", "items": [
        { "id": "open_ght",   "label": "GitHub Token 管理" },
        { "id": "tts",        "label": "TTS 语音播报" },
        { "id": "bak",        "label": "备份当前项目" }
      ]}
    ],
    "toolbar": [
      { "id": "build",      "label": "🔨 编译",        "color": "primary" },
      { "id": "stop",       "label": "■ 停止",          "color": "danger"  },
      { "id": "build_and_run", "label": "⚡ 编译并启动", "color": "primary" }
    ],
    "buttons": [
      { "id": "build",         "label": "🔨 编译",         "color": "primary" },
      { "id": "build_and_run", "label": "⚡ 编译并启动",  "color": "primary" },
      { "id": "run_cloud",     "label": "🚀 启动目标",    "color": "success" },
      { "id": "stop",          "label": "■ 停止",          "color": "danger"  }
    ],
    "run_after_build": {
      "binary_path": "bin/Debug/cloud_main",
      "auto_run": false,
      "args": ["--rendering-driver", "vulkan", "--rendering-method", "forward_plus"],
      "env": {
        "VK_ICD_FILENAMES": "/usr/share/vulkan/icd.d/nvidia_icd.json"
      }
    }
  }
}
```

## 按钮/菜单的内置 action id

`id` 字段决定点击后执行什么, 支持以下内置 id (不区分大小写):

| id | 行为 |
|----|------|
| `build+deploy` / `build` / `<task_name>` | 跑 `tasks` 数组里对应 name 的 cmd |
| `build_and_run` | 先跑 `build+deploy`, 完成后自动启动 `run_after_build.binary_path` |
| `run_cloud` | 启动 `run_after_build.binary_path` (带 args + env) |
| `run_selected` | 跑任务列表里当前选中的任务 |
| `run_auto` | 跑 `auto` 数组里的所有 task (按顺序, 前一个成功后下一个) |
| `stop` | 停止当前运行的子进程 |
| `diag` | 跑 cloud_main 30s 自动诊断 |
| `view-log` | tail 关键诊断日志行 |
| `clean` | 清掉临时日志文件 |
| `toggle_theme` | dark ↔ light 切换 |
| `toggle_log` | 显示/隐藏日志 Dock |
| `open_ght` | 调 `ac ght` 打开 GitHub Token 管理子窗口 |
| `tts` / `bak` | 调 `ac tts` / `ac bak` 子命令 |
| `about` | 关于对话框 |
| `quit` | 退出 ab |

未识别的 id 当作 `tasks` 数组里的 name 查找。

## 按钮 color 取值

| 值 | 颜色 |
|----|------|
| `primary` | 蓝 (主操作) |
| `success` | 绿 (启动) |
| `danger`  | 红 (停止/危险) |
| `warning` | 橙 (警告) |
| `info`    | 灰蓝 (中性) |

## run_after_build

`run_after_build` 定义了 "编译完成后启动什么":

- `binary_path` — 相对 cwd 的可执行文件路径 (会同时尝试 `bin/Debug/` 和 `bin/Release/`)
- `auto_run` — `true` = build_and_run 完成后自动跑, `false` = 留按钮让用户点
- `args` — 传给 binary 的参数 (数组)
- `env` — 额外环境变量 (对象, key=value 字符串)

## 主题

- `dark` (默认) — 暗黑色护眼, 提示信息淡黄色背景
- `light` — 浅色, 提示信息也是淡黄

主题状态持久化在 `~/.config/ai_tools/theme.json`。
GUI 菜单 视图 → 切换主题, 或点工具栏上的 ☀/🌙 按钮。

## 文件结构

```
ai_tools/
├── ac                # CLI 分发器 (Python), 含 ac ab 子命令
├── ac_gui.py         # DEPRECATED, 旧 Python 版 ac-gui
├── ab/
│   ├── CMakeLists.txt
│   ├── src/
│   │   ├── main.cpp              # 入口
│   │   ├── AbConfig.{h,cpp}      # ai_build.json 解析
│   │   ├── AbTheme.{h,cpp}       # dark/light 主题
│   │   ├── AbTaskRunner.{h,cpp}  # QProcess 包装
│   │   ├── AbLogDock.{h,cpp}     # 日志面板
│   │   ├── AbMainWindow.{h,cpp}  # 主窗口
│   │   ├── AbJson.{h,cpp}        # 备用 JSON 解析器 (未使用, QJsonDocument 已够)
│   │   └── (moc_*.cpp 自动生成)
│   ├── ui/
│   │   ├── ab_dark.qss           # 占位 (实际 QSS 在 AbTheme.cpp)
│   │   └── ab_light.qss
│   └── build/
│       └── ab                    # 编译产物
├── ab_launcher        # shell 包装 (自动 cmake 构建, offscreen 兜底)
├── ~/.local/bin/
│   ├── ab            # → ab_launcher 软链
│   ├── ac-gui        # → ab_launcher 软链 (兼容旧调用)
│   └── ac            # → ai_tools/ac 软链
```

## 编译

```bash
# 方式 1: 通过 ab_launcher 自动编译
ab --doctor          # 触发 ab_launcher, 缺二进制会自动 cmake + make

# 方式 2: 手动
cd /home/bv/code/ai_tools/ab
mkdir -p build && cd build
cmake ..
make -j$(nproc)

# 产物
ls -lh ab            # /home/bv/code/ai_tools/ab/build/ab
```

依赖: Qt5 (Core/Gui/Widgets/Network) 或 Qt6、CMake 3.16+、GCC 7+ / Clang 5+ (C++17)。

## 与 ac 工具链的关系

`ab` 是 `ac` 生态的 GUI 入口之一:

```
ac (CLI 分发器)
├── ac ab / ac-gui   → 启动 ab (C++ Qt GUI, 通用编译面板)
├── ac tts           → 语音播报
├── ac bak           → 文件备份
└── ac ght           → GitHub Token 管理
```

## 经验教训 / 设计决策

1. **C++ vs Python** — 原 Python 版 `ac_gui.py` 依赖 PyQt5, 启动慢、环境敏感 (dconf / Wayland 兼容问题), 改为 C++ Qt 后启动快、依赖少。
2. **配置驱动 UI** — 不同项目按钮/菜单不同, 全部塞 `ai_build.json` 的 `ui` 段, 改 UI 不动代码。
3. **shell wrapper + 软链** — `ab_launcher` 自动 cmake 构建, 沙箱里没显示自动 `QT_QPA_PLATFORM=offscreen`。
4. **行为兼容** — `ac-gui` 软链继续指向 ab_launcher, 旧脚本/alias 无需改。
5. **主题持久化** — `~/.config/ai_tools/theme.json`, 跟 ac 工具链保持一致。

## 已知限制

- `ab_dark.qss` / `ab_light.qss` 当前是占位, 实际样式硬编码在 `AbTheme.cpp` 的 `DARK_QSS` / `LIGHT_QSS` raw string 里。后续可改成从外部 qss 文件加载。
- 自定义 action id 只支持上文列表 + `tasks` 数组里的 name。需要新行为需改 `AbMainWindow::onActionTriggered`。
- 暂不支持多任务并行 (任务队列串行, 完成一个跑下一个)。

## 相关项目

- `/home/bv/code/godot_ui_linux/godot-ui-standalone-skia/ai_build.json` — Godot Skia UI 项目, 含完整 `ui` 段示例 (cloud_main 编译/启动面板)
- `/home/bv/code/godot_ui_linux/.trae/rules/ac_only.md` — ac 用法详解
- `/home/bv/code/godot_ui_linux/.trae/rules/godot_vulkan.md` — Godot 3D Vulkan 强制规则

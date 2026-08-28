# ai_tools 工程规则
================================================================

## 入口命令
- **ac** 是 ai_tools 的统一入口, 已注册到 `~/.local/bin/ac`
- 任何 AI agent 在本项目或调用本项目的子模块 (godot_ui_linux / BV_WorkSpace) 做编译/测试, **必须用 ac**
- 直接调 scons / cmake / python3 run_*.py 视为违规

## ac 标准用法
```bash
ac --gui                     # 启动 C++ GUI 编译面板 (有 X11/Wayland)
ac --offscreen               # headless 模式 (沙箱/SSH)
ac help                      # 打印帮助, 不启动 GUI
ac --offscreen --cli --cmd "..."    # 跑一次性命令
ac --task <name>             # 跑 ai_build.json 里的 task
ac                           # 无参数 = 自动跑 ai_build.json 的 auto 链 (build+deploy + diag)
```

## ai_build.json 是命令单一来源
- 用户在每个项目根放一个 `ai_build.json` (cwd/cmd/test/deploy/tasks)
- ac 启动时自动加载, --cwd/--cmd/--test 没传就用 json 里的
- 任务链 (tasks) 是 agent 和用户共享的"任务说明书", 加新任务直接改 json

## 改 ai_build.json 的场景
- 加新的链式任务 (build+deploy / diag / view-log / clean)
- 改编译/测试命令
- 加新预设 (preset) 切换不同的 build flag

## 禁止
- ❌ 直接调 scons / make / cmake (走 ac)
- ❌ 直接 python3 run_*.py (走 ac)
- ❌ 在沙箱里跑 `ac` 默认 GUI (会崩 dconf / Wayland), 必须 `ac --offscreen`
- ❌ 用 `nohup &` 跑长任务 (ac 用 QProcess.startDetached 管理)

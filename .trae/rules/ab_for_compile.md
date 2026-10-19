# 长时间编译用 ab GUI 显示进度
================================================================

## 适用场景
- 编译 / 链接 / 大型 build 任务 (>10s)
- 用户希望看到实时进度 (scons compile / link / make)
- 任何需要 "实时滚动 log + 编译面板" 的任务
- **编译后立刻启动要测试的程序** (godot editor / cloud_ros2_bridge / 其他 binary)

## 标准用法
```bash
# 启动 ab GUI 编译面板 (用户能看到进度, 编译完成 TTS 提示音)
ab /path/to/project         # 当前项目 ai_build.json 配置
ab /home/bv/code/BV_WorkSpace_Linux/BV_WorkSpace-main_v7
ab --auto-exit              # 编译完成自动退出 GUI (CI/agent 调用模式)
ab --config /path/to/ai_build.json

# 编译后自动启动要测试的程序 (一次性 build+run, agent 调试首选)
ab --auto-exit /path/to/project    # 编译完成 → 退出 → 用户手动启 binary
```

## 关键优势
- 实时滚动 stdout/stderr (不缓冲)
- 编译完成 **spd-say 自动 TTS 提示音** (无需 agent 自己播)
- GUI 显示 build 状态/进度/错误, 用户一眼能看出卡在哪
- 后台跑 (`--auto-exit`) 时 exit code 通过 log 检查
- **编译 → 部署 → 启 binary 一气呵成**: ai_build.json 的 `auto` 链可以包含
  `build` → `deploy` → `launch-editor` 多个 task, ab 跑完 auto 链后 TTS 提示

## 编译后立刻启动 binary 的标准流程
```bash
# 1. 在 ai_build.json 里把 launch-editor 加到 auto 链
"auto": ["build+strip", "launch-editor"]
# 2. 跑 ab --auto-exit — 自动 build → deploy → launch godot
ab --auto-exit /home/bv/code/BV_WorkSpace_Linux/BV_WorkSpace-main_v7
# 3. ab 完成 auto 链后 TTS "编译完成" → 用户手动启 binary 或 ab 自动启
```

## 跟 ac 的分工
- **ac** = 编译/调试面板主入口, 一次性命令 (默认 GUI 启动 ab)
- **ab** = 编译面板底层 C++ GUI, 长时间编译首选
- 编译脚本在 `ai_build.json` 的 `tasks` 段; ab 启动时自动加载并实时显示
- 已经运行了 `ab` 后, 不要重复 `ac` — 直接 `ab` 即可

## 跟 scons 后台跑的对比
- `ab` 在 X11/Wayland 下能看到 GUI; `nohup scons &` 只能看 /tmp/*.log 文件
- 用户说 "用 ab 编译" → 必须启动 GUI (`ab`), 不要走 `nohup scons &` 绕过
- `ab --auto-exit` = nohup + log file 的 GUI 模式 (agent 调用首选)

## 禁止
- ❌ 长编译走 `nohup scons ... &` 后看 log 文件 — 用户看不到进度
- ❌ 同一个编译跑多次 ab (重复启动 GUI 浪费资源)
- ❌ 用 `ab --no-auto` + 不用 UI 模式 (跟 scons 后台跑没区别)
- ❌ 编译完后 agent 自己 cp binary + 启 godot (用户看不到 TTS, 不知道编译完成)
- ❌ 用 `ab --auto-exit` 还要在 sandbox 里跑 godot (X11 会断, 必须让用户手动启)
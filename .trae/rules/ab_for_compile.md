# 长时间编译用 ab GUI 显示进度
================================================================

## 适用场景
- 编译 / 链接 / 大型 build 任务 (>10s)
- 用户希望看到实时进度 (scons compile / link / make)
- 任何需要 "实时滚动 log + 编译面板" 的任务

## 标准用法
```bash
# 启动 ab GUI 编译面板 (用户能看到进度, 编译完成 TTS 提示音)
ab /path/to/project         # 当前项目 ai_build.json 配置
ab /home/bv/code/BV_WorkSpace_Linux/BV_WorkSpace-main_v7
ab --auto-exit              # 编译完成自动退出 GUI (适合 CI/agent 调用)
ab --config /path/to/ai_build.json
```

## 关键优势
- 实时滚动 stdout/stderr (不缓冲)
- 编译完成 **spd-say 自动 TTS 提示音** (无需 agent 自己播)
- GUI 显示 build 状态/进度/错误, 用户一眼能看出卡在哪
- 后台跑 (`--auto-exit`) 时 exit code 通过 log 检查

## 跟 ac 的分工
- **ac** = 编译/调试面板主入口, 一次性命令 (默认 GUI 启动 ab)
- **ab** = 编译面板底层 C++ GUI, 长时间编译首选
- 编译脚本在 `ai_build.json` 的 `tasks` 段; ab 启动时自动加载并实时显示
- 已经运行了 `ab` 后, 不要重复 `ac` — 直接 `ab` 即可

## 跟 scons 后台跑的对比
- `ab` 在 X11/Wayland 下能看到 GUI; `nohup scons &` 只能看 /tmp/*.log 文件
- 用户说 "用 ab 编译" → 必须启动 GUI (`ab`), 不要走 `nohup scons &` 绕过

## 禁止
- ❌ 长编译走 `nohup scons ... &` 后看 log 文件 — 用户看不到进度
- ❌ 同一个编译跑多次 ab (重复启动 GUI 浪费资源)
- ❌ 用 `ab --no-auto` + 不用 UI 模式 (跟 scons 后台跑没区别)
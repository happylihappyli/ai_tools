#!/usr/bin/env python3
# -*- coding: utf-8 -*-
import sys
import os
import subprocess
from pathlib import Path

# 工具路径
TOOL_PATH = Path("/home/bv/code/ai_tools/compile_tool.py")

def launch(preset_name=None, task=None):
    cmd = [sys.executable, "-u", str(TOOL_PATH)]
    if preset_name:
        cmd.extend(["--preset", preset_name])
    if task:
        cmd.extend(["--task", task])
    
    # 传递额外的命令行参数（如 --cmd）
    extra_args = []
    if len(sys.argv) > 2:
        extra_args = sys.argv[2:]
        # 移除可能重复的预设参数
        if extra_args and extra_args[0] == preset_name:
            extra_args = extra_args[1:]
    if extra_args:
        cmd.extend(extra_args)
        
    print(f"🚀 正在启动驾驶面板...")
    if preset_name:
        print(f"📂 项目预设: {preset_name}")
    
    # 启动 GUI (不重定向 stderr 以便观察错误)
    subprocess.Popen(cmd)

def main():
    if len(sys.argv) < 2:
        print("用法:")
        print("  python3 dash.py skia      - 启动 Skia Standalone 编译面板")
        print("  python3 dash.py ws        - 启动 Workspace (Godot Editor) 编译面板")
        print("  python3 dash.py <preset>  - 启动指定预设")
        launch() # 默认启动
        return

    arg = sys.argv[1].lower()
    
    if arg in ["skia", "godot-skia"]:
        launch("godot-standalone-skia", "编译 Skia 版本")
    elif arg in ["ws", "workspace"]:
        launch("godot-editor-build", "编译 Workspace Editor")
    else:
        launch(sys.argv[1])

if __name__ == "__main__":
    main()

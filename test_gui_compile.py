import sys
import subprocess
import os

def main():
    # Target directory for compilation
    target_dir = "/home/bv/code/godot_ui_linux/godot-ui-standalone-skia"
    
    # Command to run (using scons)
    cmd = "scons"
    
    print(f"Starting compile_tool.py GUI for directory: {target_dir}")
    print(f"Command: {cmd}")
    
    # Launch the GUI compile tool with auto-start
    subprocess.Popen([
        sys.executable,
        "/home/bv/code/ai_tools/compile_tool.py",
        "--cmd", cmd,
        "--cwd", target_dir,
        "--task", "测试编译 standalone-skia"
    ])

if __name__ == "__main__":
    main()

#!/bin/bash
# v37c gdb 抓真实崩溃栈
# dev 版含调试符号 738MB. SIGSEGV 在 gdb 里停住时显示真实函数名.
set -e
pkill -KILL -f 'godot.linuxbsd' 2>/dev/null || true
sleep 2
export DISPLAY=${DISPLAY:-:0}
cd /home/bv/code/BV_WorkSpace_Linux/BV_WorkSpace-main_v7/game/cloud_ros2

gdb -batch \
  -ex 'set pagination off' \
  -ex 'set print thread-events off' \
  -ex 'catch signal SIGSEGV' \
  -ex 'run --path . --rendering-driver vulkan --editor' \
  -ex 'bt 30' \
  -ex 'frame 0' \
  -ex 'info locals' \
  -ex 'info args' \
  -ex 'thread apply all bt 5' \
  /home/bv/code/BV_WorkSpace_Linux/BV_WorkSpace-main_v7/bin/godot.linuxbsd.editor.dev.x86_64.llvm 2>&1 | tee /tmp/gdb_v37c.log
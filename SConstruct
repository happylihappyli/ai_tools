# SPDX-License-Identifier: MIT
#
# 顶层 SConstruct — ai_tools 统一 C++ 编译入口
# 2026-09-02: 新增, 解决 `scons` 在 ai_tools 根目录报 "No SConstruct" 问题
#
# 跑法:
#   cd /home/bv/code/ai_tools && scons -j$(nproc)   # 编译所有 C++ 子项目
#   scons ab -j8                                     # 只编译 ab
#   scons cpp_panel -j4                              # 只编译 cpp_panel
#   scons -c                                         # clean 所有
#   scons ab -c                                      # clean ab
#
# 子项目:
#   ab/         C++ Qt5/6 GUI 编译面板 (SConstruct)
#   cpp_panel/  C++ Qt5 旧版编译面板 (SConstruct)
#
# 设计:
#   - 用 SConscript() 引用子项目 SConstruct, 自动建依赖图
#   - 子项目独立 variant_dir 隔离, 不互相污染
#   - 加新 C++ 子项目: 在 ai_tools/<proj>/ 放 SConstruct, 在本文件加一行 SConscript

# ===== 全局环境 =====
import os
env = Environment(ENV=os.environ, tools=['default'])
env.Append(CXXFLAGS=['-std=c++17', '-fPIC', '-Wall', '-Wextra'])
if not env.get('debug', False):
    env.Append(CXXFLAGS=['-O2'])

# 帮助文本
Help("""
ai_tools 顶层 scons — 统一编译所有 C++ 子项目

  scons                  # 编译所有 (ab + cpp_panel + ...)
  scons ab -j8           # 只编译 ab
  scons cpp_panel        # 只编译 cpp_panel
  scons -c               # clean 所有
  scons ab -c            # clean ab
  scons debug=1          # debug build (-O0)
""", append=True)

# ===== 子项目 dispatcher =====
# 每个子项目必须有 SConstruct, 否则报错
SUBDIRS = ['ab', 'cpp_panel']

# 用户在命令行指定了子项目 (scons ab -j8) → 只编那个, 否则全编
targets = COMMAND_LINE_TARGETS
if not targets:
    targets = SUBDIRS  # 无参 = 全编

for sub in targets:
    sconstruct_path = os.path.join(sub, 'SConstruct')
    if not os.path.exists(sconstruct_path):
        print(f"⚠ 跳过 {sub}: 没找到 {sconstruct_path} (子项目要么有 SConstruct 要么不在 SUBDIRS)")
        continue
    print(f"  → {sub}/ (SConstruct)")
    SConscript(sconstruct_path, exports='env')

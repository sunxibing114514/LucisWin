#!/bin/bash
# build_hello.sh — 编译 hello.c 为 i386 PE (hello.exe)
#
# 在 macOS CI (macos-latest) 与 Linux 上均可运行, 依赖 mingw-w64:
#   macOS: brew install mingw-w64
#   Linux: apt-get install mingw-w64
#
# 编译产物 hello.exe 作为:
#   1. WineCoreTests 端到端测试的输入 (放 WineCoreTests/)
#   2. LucisWinApp 的 bundle 资源 (放 LucisWinApp/Resources/)
set -e

cd "$(dirname "$0")"

# -O0: 关闭优化, 让生成的指令更简单 (push/call/mov/ret 为主), 便于解释器覆盖
# -fno-stack-protector: 去掉 stack canary 检查, 减少未实现指令
# -g0 -s: 去掉 DWARF 调试节 (mingw 默认会塞 10+ 个 /N 调试节), 简化 PE 结构
# -nostartfiles -e _main: 跳过 mingw CRT 启动代码 (__tmainCRTStartup),
#   直接以 main 为入口点, 避免解释器需执行 CRT 初始化 + 实现 msvcrt 的 22 个函数。
#   导入表将只含 hello.c 显式调用的 user32!MessageBoxA 与 kernel32!ExitProcess。
i686-w64-mingw32-gcc -O0 -fno-stack-protector -g0 -nostartfiles \
    -e _main \
    -o hello.exe hello.c \
    -luser32 -lkernel32
i686-w64-mingw32-strip hello.exe

echo "[build_hello] 生成 hello.exe:"
file hello.exe

# 拷贝到 WineCoreTests 与 LucisWinApp/Resources (CI 中执行)
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$ROOT/WineCoreTests"
cp hello.exe "$ROOT/WineCoreTests/hello.exe"
mkdir -p "$ROOT/LucisWinApp/Resources"
cp hello.exe "$ROOT/LucisWinApp/Resources/hello.exe"

echo "[build_hello] 已拷贝到 WineCoreTests/ 与 LucisWinApp/Resources/"

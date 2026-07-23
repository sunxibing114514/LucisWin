#!/bin/bash
# build_fib.sh — 编译 fib.c 为 i386 PE (fib.exe)
#
# 与 build_hello.sh 同构, 编译产物 fib.exe 作为 Phase 2.1 端到端测试输入。
#   1. WineCoreTests 端到端测试的输入 (放 WineCoreTests/)
#   2. LucisWinApp 的 bundle 资源 (放 LucisWinApp/Resources/)
set -e

cd "$(dirname "$0")"
SCRIPT_DIR="$PWD"

# 参数同 build_hello.sh: -nostartfiles -e _main 跳过 CRT, 导入表仅含显式调用的函数
i686-w64-mingw32-gcc -O0 -fno-stack-protector -g0 -nostartfiles \
    -e _main \
    -o fib.exe fib.c \
    -luser32 -lkernel32
i686-w64-mingw32-strip fib.exe

echo "[build_fib] 生成 fib.exe:"
file fib.exe

# 拷贝到 WineCoreTests 与 LucisWinApp/Resources
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cp fib.exe "$ROOT/WineCoreTests/fib.exe"
mkdir -p "$ROOT/LucisWinApp/Resources"
cp fib.exe "$ROOT/LucisWinApp/Resources/fib.exe"

echo "[build_fib] 已拷贝到 WineCoreTests/ 与 LucisWinApp/Resources/"

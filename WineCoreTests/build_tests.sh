#!/bin/bash
# build_tests.sh — Linux 本地编译运行 WineCoreTests (TDD 验证)
#
# 用途: 在开发阶段快速跑 TDD 红绿循环, 不依赖 xcodebuild。
#       macOS CI 用 xcodebuild -scheme WineCoreTests 跑同一份源。
set -e

cd "$(dirname "$0")/.."   # 到仓库根
ROOT="$PWD"

# 头文件搜索路径
INC="-I$ROOT/WineCore/include"

# 编译所有 WineCore 源 + 测试源 (pe_loader.c 等实现后加入)
SOURCES=""
for f in \
    WineCore/loader/pe_loader.c \
    WineCore/translator/interpreter.c \
    WineCore/dlls/ntdll/ntdll.c \
    WineCore/dlls/kernel32/kernel32.c \
    WineCore/dlls/user32/user32.c \
    WineCore/dlls/msvcrt/msvcrt.c \
    WineCore/codepage/codepage.c \
    WineCore/core.c; do
    [ -f "$f" ] && SOURCES="$SOURCES $f"
done

TESTS="WineCoreTests/test_registry.c $(ls WineCoreTests/*_tests.c 2>/dev/null)"

echo "[build] sources:$SOURCES"
echo "[build] tests:$TESTS"

gcc -std=c11 -Wall -Wextra -g -O0 $INC \
    $SOURCES $TESTS WineCoreTests/test_main.c \
    -o /tmp/wine_tests -lm 2>&1 | head -40 || true

if [ -f /tmp/wine_tests ]; then
    # 设置 hello.exe / fib.exe 路径供测试加载 (Linux/macOS 通用)
    export LUCISWIN_HELLO_EXE="$ROOT/WineCoreTests/hello.exe"
    export LUCISWIN_FIB_EXE="$ROOT/WineCoreTests/fib.exe"
    echo "[run] LUCISWIN_HELLO_EXE=$LUCISWIN_HELLO_EXE"
    echo "[run] LUCISWIN_FIB_EXE=$LUCISWIN_FIB_EXE"
    echo "[run] === 测试输出 ==="
    /tmp/wine_tests
else
    echo "[build] 编译失败 (RED: 实现未就绪)"
    exit 1
fi

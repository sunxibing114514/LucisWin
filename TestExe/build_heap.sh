#!/bin/bash
# build_heap.sh — 编译 heap.c 为 i386 PE (heap.exe)
set -e
cd "$(dirname "$0")"
SCRIPT_DIR="$PWD"

i686-w64-mingw32-gcc -O0 -fno-stack-protector -g0 -nostartfiles \
    -e _main \
    -o heap.exe heap.c \
    -luser32 -lkernel32
i686-w64-mingw32-strip heap.exe

echo "[build_heap] 生成 heap.exe:"
file heap.exe

ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cp heap.exe "$ROOT/WineCoreTests/heap.exe"
mkdir -p "$ROOT/LucisWinApp/Resources"
cp heap.exe "$ROOT/LucisWinApp/Resources/heap.exe"
echo "[build_heap] 已拷贝到 WineCoreTests/ 与 LucisWinApp/Resources/"

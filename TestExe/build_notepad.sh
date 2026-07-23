#!/bin/bash
# build_notepad.sh — 编译 notepad.c 为 i386 PE (notepad.exe)
set -e
cd "$(dirname "$0")"
SCRIPT_DIR="$PWD"

i686-w64-mingw32-gcc -O0 -fno-stack-protector -g0 -nostartfiles \
    -e _main \
    -o notepad.exe notepad.c \
    -luser32 -lgdi32 -lcomctl32 -lkernel32
i686-w64-mingw32-strip notepad.exe

echo "[build_notepad] 生成 notepad.exe:"
file notepad.exe

ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cp notepad.exe "$ROOT/WineCoreTests/notepad.exe"
mkdir -p "$ROOT/LucisWinApp/Resources"
cp notepad.exe "$ROOT/LucisWinApp/Resources/notepad.exe"
echo "[build_notepad] 已拷贝到 WineCoreTests/ 与 LucisWinApp/Resources/"

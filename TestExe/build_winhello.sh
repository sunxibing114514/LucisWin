#!/bin/bash
# build_winhello.sh — 编译 winhello.c 为 i386 PE (winhello.exe)
set -e
cd "$(dirname "$0")"
SCRIPT_DIR="$PWD"

i686-w64-mingw32-gcc -O0 -fno-stack-protector -g0 -nostartfiles \
    -e _main \
    -o winhello.exe winhello.c \
    -luser32 -lgdi32 -lkernel32
i686-w64-mingw32-strip winhello.exe

echo "[build_winhello] 生成 winhello.exe:"
file winhello.exe

ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cp winhello.exe "$ROOT/WineCoreTests/winhello.exe"
mkdir -p "$ROOT/LucisWinApp/Resources"
cp winhello.exe "$ROOT/LucisWinApp/Resources/winhello.exe"
echo "[build_winhello] 已拷贝到 WineCoreTests/ 与 LucisWinApp/Resources/"

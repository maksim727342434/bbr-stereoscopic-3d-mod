#!/bin/sh
# CRT-free build (no msvcrt.dll dependency): only kernel32 + d3d11
F="-O2 -std=c++17 -shared -nostdlib -nostartfiles -fno-exceptions -fno-rtti -fno-stack-protector -fno-builtin -fno-tree-loop-distribute-patterns -Wl,-e,DllMain"
x86_64-w64-mingw32-g++ $F -o SDL2.dll bb_anaglyph.cpp SDL2.def -ld3d11 -ldxguid -luuid -lkernel32 -lgcc
x86_64-w64-mingw32-g++ $F -o SDL2_stage0.dll stage0.cpp SDL2_stage0.def -lkernel32 -lgcc

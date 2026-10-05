# SGL (Small Graphics Library)
# [中文](README_CN.md)

![SGL_LOGO](SGL_logo.png)

## Continuous Integration Status

| Status (main) | Status (devel) | Description |
| :------------- | :------------ | :--------- |
| [![build status](https://github.com/sgl-org/sgl/actions/workflows/ubuntu-gcc.yml/badge.svg?branch=main)](https://github.com/sgl-org/sgl/actions/workflows/ubuntu-gcc.yml?query=branch%3Amain) | [![build status](https://github.com/sgl-org/sgl/actions/workflows/ubuntu-gcc.yml/badge.svg?branch=devel)](https://github.com/sgl-org/sgl/actions/workflows/ubuntu-gcc.yml?query=branch%3Adevel) | Build default config on Ubuntu GCC |
| [![build status](https://github.com/sgl-org/sgl/actions/workflows/ubuntu-clang.yml/badge.svg?branch=main)](https://github.com/sgl-org/sgl/actions/workflows/ubuntu-clang.yml?query=branch%3Amain) | [![build status](https://github.com/sgl-org/sgl/actions/workflows/ubuntu-clang.yml/badge.svg?branch=devel)](https://github.com/sgl-org/sgl/actions/workflows/ubuntu-clang.yml?query=branch%3Adevel) | Build default config on Ubuntu Clang |
| [![build status](https://github.com/sgl-org/sgl/actions/workflows/macos.yml/badge.svg?branch=main)](https://github.com/sgl-org/sgl/actions/workflows/macos.yml?query=branch%3Amain) | [![build status](https://github.com/sgl-org/sgl/actions/workflows/macos.yml/badge.svg?branch=devel)](https://github.com/sgl-org/sgl/actions/workflows/macos.yml?query=branch%3Adevel) | Build default config on Macos |
| [![build status](https://github.com/sgl-org/sgl/actions/workflows/windows.yml/badge.svg?branch=main)](https://github.com/sgl-org/sgl/actions/workflows/windows.yml?query=branch%3Amain) | [![build status](https://github.com/sgl-org/sgl/actions/workflows/windows.yml/badge.svg?branch=devel)](https://github.com/sgl-org/sgl/actions/workflows/windows.yml?query=branch%3Adevel) | Build default config on Windows |
| [![build status](https://github.com/sgl-org/sgl/actions/workflows/ubuntu-armv7-gcc.yml/badge.svg?branch=main)](https://github.com/sgl-org/sgl/actions/workflows/ubuntu-armv7-gcc.yml?query=branch%3Amain) | [![build status](https://github.com/sgl-org/sgl/actions/workflows/ubuntu-armv7-gcc.yml/badge.svg?branch=devel)](https://github.com/sgl-org/sgl/actions/workflows/ubuntu-armv7-gcc.yml?query=branch%3Adevel) | Cross build ARMv7 default on Ubuntu GCC |
| [![build status](https://github.com/sgl-org/sgl/actions/workflows/ubuntu-aarch64-gcc.yml/badge.svg?branch=main)](https://github.com/sgl-org/sgl/actions/workflows/ubuntu-aarch64-gcc.yml?query=branch%3Amain) | [![build status](https://github.com/sgl-org/sgl/actions/workflows/ubuntu-aarch64-gcc.yml/badge.svg?branch=devel)](https://github.com/sgl-org/sgl/actions/workflows/ubuntu-aarch64-gcc.yml?query=branch%3Adevel) | Cross build AArch64 default on Ubuntu GCC |
| [![build status](https://github.com/sgl-org/sgl/actions/workflows/ubuntu-mips-gcc.yml/badge.svg?branch=main)](https://github.com/sgl-org/sgl/actions/workflows/ubuntu-mips-gcc.yml?query=branch%3Amain) | [![build status](https://github.com/sgl-org/sgl/actions/workflows/ubuntu-mips-gcc.yml/badge.svg?branch=devel)](https://github.com/sgl-org/sgl/actions/workflows/ubuntu-mips-gcc.yml?query=branch%3Adevel) | Cross build MIPS default on Ubuntu GCC |
| [![build status](https://github.com/sgl-org/sgl/actions/workflows/ubuntu-mips64-gcc.yml/badge.svg?branch=main)](https://github.com/sgl-org/sgl/actions/workflows/ubuntu-mips64-gcc.yml?query=branch%3Amain) | [![build status](https://github.com/sgl-org/sgl/actions/workflows/ubuntu-mips64-gcc.yml/badge.svg?branch=devel)](https://github.com/sgl-org/sgl/actions/workflows/ubuntu-mips64-gcc.yml?query=branch%3Adevel) | Cross build MIPS64 default on Ubuntu GCC |
| [![build status](https://github.com/sgl-org/sgl/actions/workflows/generic-arm-gcc.yml/badge.svg?branch=main)](https://github.com/sgl-org/sgl/actions/workflows/generic-arm-gcc.yml?query=branch%3Amain) | [![build status](https://github.com/sgl-org/sgl/actions/workflows/generic-arm-gcc.yml/badge.svg?branch=devel)](https://github.com/sgl-org/sgl/actions/workflows/generic-arm-gcc.yml?query=branch%3Adevel) | Cross build ARM default on Generic GCC |
| [![build status](https://github.com/sgl-org/sgl/actions/workflows/codeql.yml/badge.svg?branch=main)](https://github.com/sgl-org/sgl/actions/workflows/codeql.yml?query=branch%3Amain) | [![build status](https://github.com/sgl-org/sgl/actions/workflows/codeql.yml/badge.svg?branch=devel)](https://github.com/sgl-org/sgl/actions/workflows/codeql.yml?query=branch%3Adevel) | Code analyse on codeql |

## SGL UI Library Features
- Lightweight, requiring only `3KB RAM` and `15KB ROM` to run at minimum
- Partial frame buffer support, requiring only one line of screen resolution buffer at minimum
- Bounding box + greedy algorithm based dirty rectangle algorithm
- Frame buffer controller support, direct write to frame buffer controller, zero copy
- Color depth support: `8bit`, `16bit`, `24bit`, `32bit`
- Modernized font extraction tool
- `SGL UI designer` tool, graphical drag-and-drop interface drawing with one-click code generation

### Minimum Hardware Requirements
| Flash Size | RAM Size |
| :---------:| :------: |
| 15kB       |    3KB   |

### Introduction
SGL (Small Graphics Library) is a lightweight and fast graphics library specifically designed to provide beautiful and lightweight GUI (Graphical User Interface) for MCU-level processors.
Please refer to the `docs` directory for documentation.

## Quick Start (Based on SDL2 Simulation)
Steps as follows:
1. Ensure `make` tool is installed
2. Install gcc compiler, (https://github.com/niXman/mingw-builds-binaries/releases/download/13.2.0-rt_v11-rev0/x86_64-13.2.0-release-posix-seh-ucrt-rt_v11-rev0.7z)
3. git clone https://github.com/sgl-org/sgl-port-windows.git    
4. cd sgl-port-windows && git submodule init    
5. git submodule update --remote    
6. cd demo && make -j8    
7. make run    

Note: Please use git terminal or VSCode's git bash terminal to enter the above commands

## 1. Install gcc Compiler
Download `gcc` from this address (https://github.com/niXman/mingw-builds-binaries/releases/download/13.2.0-rt_v11-rev0/x86_64-13.2.0-release-posix-seh-ucrt-rt_v11-rev0.7z)
Then extract to any directory and add to environment variables

## 2. Pull SGL Demo Source Code
git clone https://github.com/sgl-org/sgl-port-windows.git

## 3. Configure Project
cd sgl-port-windows && git submodule init    
git submodule update --remote      
cd demo && make -j8   

## 4. Run
Execute `make run` command to run    

Community Forum: https://sgl.openbfdev.com

QQ Group: 544602724

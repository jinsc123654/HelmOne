# SGL (Small Graphics Library)
# [English](README.md)

![SGL_LOGO](SGL_logo.png)

## SGL UI库特点
- 轻量级，最小只需要`3KBytes RAM`和`15KBytes ROM`即可运行
- 部分帧缓冲支持，最小只需要一行屏幕分辨率的缓冲即可
- 包围盒+贪心算法的脏矩形算法
- 支持帧缓冲控制器，直接向帧缓冲控制器写入数据，零拷贝
- 颜色深度支持: `8bit`, `16bit`, `24bit`, `32bit`
- 现代化的字体取模工具
- `SGL`自己的`UI`设计器，图形化拖动绘制界面后一键可生成代码

### 最低硬件要求
| Flash大小 | Ram大小 |
| :---------:| :------: |
| 15kB       |    3kb   |


### 简介
SGL (Small Graphics Library) 是一个轻量级且快速的图形库，专为MCU级别处理器提供美观轻量的GUI（图形用户界面）。
请参考 `docs` 目录获取文档。

## 快速开始 (基于SDL2模拟)
步骤如下：
1. 确保安装了make工具
2. 安装gcc编译器，(https://github.com/niXman/mingw-builds-binaries/releases/download/13.2.0-rt_v11-rev0/x86_64-13.2.0-release-posix-seh-ucrt-rt_v11-rev0.7z)
3. git clone https://github.com/sgl-org/sgl-port-windows.git    
4. cd sgl-port-windows && git submodule init    
5. git submodule update --remote    
6. cd demo && make -j8    
7. make run    

注意：请使用git终端或VSCode的git bash终端输入上述命令

## 1. 安装gcc编译器
从该地址下载 `gcc` (https://github.com/niXman/mingw-builds-binaries/releases/download/13.2.0-rt_v11-rev0/x86_64-13.2.0-release-posix-seh-ucrt-rt_v11-rev0.7z)
然后解压到任意目录并添加到环境变量

## 2. 拉取sgl演示源码
git clone https://github.com/sgl-org/sgl-port-windows.git

## 3. 配置项目
cd sgl-port-windows && git submodule init    
git submodule update --remote      
cd demo && make -j8   

## 4. 运行
执行 `make run` 命令运行    
     
社区论坛: https://sgl.openbfdev.com

QQ群: 544602724

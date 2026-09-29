# Helm One

骑行码表。240×320 半透半反屏 · 三个实体键（无触摸）· 离线矢量地图与导航 · BLE 心率/踏频/功率传感器 · 伴生手机 App。
**基于 [openvela](https://github.com/open-vela) 开发**（内核 openvela / NuttX）。

![Helm One：整机正面，屏幕正在导航](00_doc/images/hw-hero.jpg)

**文档都在 [`00_doc/`](00_doc/README.md)** —— 本文件只做入口。

## 仓库

| 目录 | 内容 |
|---|---|
| [`00_doc/`](00_doc/README.md) | 产品与工程文档 ← 从这里开始 |
| `01_hw/` | 硬件设计（Altium） |
| `02_sw/` | 车机固件（git submodule）+ 台架传感器模拟器（ESP32-S3） |
| `03_app/` | 手机 App（Flutter） |
| `04_tools/` | PC 端工具链（当前是地图） |

## 想干什么，去哪看

| 目的 | 文档 |
|---|---|
| 这东西长什么样、怎么用 | [01-产品概览](00_doc/01-产品概览.md) |
| 三层架构与仓库布局 | [02-系统架构](00_doc/02-系统架构.md) |
| 改固件、改界面 | [03-固件说明](00_doc/03-固件说明.md) |
| 改手机 App | [04-手机App](00_doc/04-手机App.md) |
| 出地图包、不烧录看地图 | [05-地图工具链](00_doc/05-地图工具链.md) |
| **把固件跑起来** | [06-构建与烧录](00_doc/06-构建与烧录.md) |
| 动这个仓库本身（submodule 等） | [07-仓库维护](00_doc/07-仓库维护.md) |
| 这是不是 openvela 作品、跟比赛什么关系 | [08-openvela与参赛](00_doc/08-openvela与参赛.md) |

## 两个快速入口

```bash
# 不烧录、不生成，直接看设备地图包长什么样
cd 04_tools/vmap && ./view_map.sh

# 固件是 submodule，首次克隆要拉下来
git submodule update --init --recursive
```

> ⚠️ 固件**不能在本仓库里编译** —— NuttX 与工具链在 openvela SDK 里，
> 固件是以 `vendor/my_vendor` 的身份挂在 SDK 下的。见 [06-构建与烧录](00_doc/06-构建与烧录.md)。

## 待补

> 🖼️ **待补图** → `00_doc/images/hw-mounted.jpg`：装上车把的照片。
> 还需要哪些图、什么规格，见 [00_doc/images/README.md](00_doc/images/README.md)。

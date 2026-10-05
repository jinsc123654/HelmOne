# bicycle 开发说明（Vela 模拟器）

本文说明 **goldfish 模拟器** 上板级应用 `bicycle` 的 **由来**、**配置方式**、编译运行与日常开发。

---

## 目录

1. [由来：为什么有 bicycle](#1-由来为什么有-bicycle)
2. [与 x_track、test_app 的对比](#2-与-x_tracktest_app-的对比)
3. [目录结构](#3-目录结构)
4. [配置说明（重点）](#4-配置说明重点)
5. [编译与运行](#5-编译与运行)
6. [资源（ROMFS /etc）](#6-资源romfs-etc)
7. [持久化路径：JSON 配置与 GPX 轨迹](#7-持久化路径json-配置与-gpx-轨迹)
8. [启动方式](#8-启动方式)
9. [涉及文件一览](#9-涉及文件一览)
10. [日常开发](#10-日常开发)
11. [从 x_track 再同步（可选）](#11-从-x_track-再同步可选)
12. [常见问题](#12-常见问题)
13. [LiveMap 矢量地图与资源（SF32 当前）](#13-livemap-矢量地图与资源sf32-当前)

---

## 1. 由来：为什么有 bicycle

### 1.1 上游：X-TRACK

[bicycle](../) 的 UI 与业务逻辑来自开源自行车码表项目 **[X-TRACK](https://github.com/FASTSHIFT/X-TRACK)**。openvela 中的官方 Demo 位于：

```
apps/packages/demos/x_track/
```

该 Demo 由 X-TRACK 原作者移植，功能包括时速、里程、轨迹显示等，分辨率 240×320，使用 LVGL + 触摸屏。官方文档见 [X_Track_zh-cn.md](../../../../../docs/zh-cn/demo/X_Track_zh-cn.md)。

在模拟器上跑 x_track 的**传统方式**是：

1. `menuconfig` 打开 `LVX_USE_DEMO_X_TRACK` 及 PNG、UIKIT 等依赖；
2. 全量编译；
3. 启动模拟器后 `adb push` 资源；
4. NSH 里手动执行 `x_track &`。

这属于 **packages 层 Demo**，和板级产品应用的集成方式不同。

### 1.2 需求：像 test_app 一样挂在板级

在真机板级（如 `vendor/my_vendor/.../test_app`）中，应用通常：

- 放在 **board 目录** 下（`nuttx_add_application`）；
- 用 **Kconfig + defconfig** 控制是否编入固件；
- 通过 **rcS 或 bringup** 默认启动；
- 日常只改板级目录里的代码。

为了在 **goldfish 模拟器** 上学习同一套模式，而不是每次切 Demo Kconfig，我们把 x_track **完整复制** 到：

```
vendor/openvela/boards/vela/bicycle/
```

并改名为 **`bicycle`**（NSH 命令名、builtin 符号 `bicycle_main`），作为 **vela 板级默认应用**。

### 1.3 当前策略

| 原则 | 说明 |
|------|------|
| **独立副本** | `src/`、`etc/` 与 `x_track` 同源但已 fork，**后续只改 `bicycle/`** |
| **板级构建** | 由 `bicycle/CMakeLists.txt` 编译，**不依赖** `LVX_USE_DEMO_X_TRACK` |
| **模拟器默认固件** | `goldfish-arm64-v8a-ap` 的 defconfig 默认 `CONFIG_VELA_BICYCLE=y` |
| **配置分层** | Kconfig/defconfig 只保留 **总开关**；栈、路径等写在 **CMakeLists.txt** |

### 1.4 数据流简图

```
x_track (packages Demo)          bicycle (板级 fork)
        │                                │
        │  rsync 复制 src/resource       │
        └──────────────────────────────► │
                                         │
defconfig: CONFIG_VELA_BICYCLE=y         │
        │                                │
        ▼                                ▼
  CMake 编入固件                  rcS: bicycle &
        │                                │
        └──────── ROMFS 编入 /etc ─────────┘
                      │
                      ▼
              设备 /etc/font|images|track
```

---

## 2. 与 x_track、test_app 的对比

| 项目 | **bicycle（本目录）** | **x_track（packages）** | **test_app（my_vendor 示例）** |
|------|----------------------|-------------------------|-------------------------------|
| 位置 | `boards/vela/bicycle/` | `apps/packages/demos/x_track/` | `boards/.../my_vendor/test_app/` |
| 命令名 | `bicycle` | `x_track` | `test_app` |
| 开关 | `CONFIG_VELA_BICYCLE` | `LVX_USE_DEMO_X_TRACK` | `CONFIG_MYVENDOR_TEST_APP` |
| 典型启用 | defconfig 默认 y | `vela_emulator_tools.py build -d` | 板级 defconfig |
| 复杂度 | 大型 LVGL 应用 | 同左（上游 Demo） | 简单 C 演示 |
| 资源 | `bicycle/etc/` → ROMFS `/etc` | `x_track/resource/` → adb `/data` | 无 |
| 用途 | **模拟器默认学习固件** | 官方 Demo 文档流程 | 真机启动路径示例 |

三者共同点：都是 NuttX **builtin**（`nuttx_add_application`），不是从文件系统 `exec` 的独立 ELF。

---

## 3. 目录结构

```
vendor/openvela/boards/vela/bicycle/
├── README.md              # 简要说明
├── doc/
│   └── bicycle_guide.md   # 本文
├── bicycle_main.cpp       # 入口（对应 x_track 的 x_track_main.cpp）
├── CMakeLists.txt         # 编译参数 + nuttx_add_application
├── src/                   # 自 x_track 复制的完整源码
│   ├── App/
│   ├── Frameworks/
│   └── Vendor/Simulator/
└── etc/                   # 资源（编译进 ROMFS → /etc/...）
    ├── font/
    ├── images/
    └── track/
```

---

## 4. 配置说明（重点）

bicycle 的配置分 **三层**。日常改参数优先看 **CMakeLists**；只有要「整块关掉 bicycle」时才动 Kconfig/defconfig。

### 4.1 第一层：Kconfig — 总开关

文件：`boards/vela/Kconfig`

```kconfig
config VELA_BICYCLE
    bool "Build and autostart 'bicycle' (X-TRACK UI)"
    default y
    depends on GRAPHICS_LVGL && LV_USE_LIBPNG && LIB_PNG
             && NETUTILS_CJSON && UIKIT_FONT_MANAGER
```

| `CONFIG_VELA_BICYCLE` | 效果 |
|-----------------------|------|
| **y（默认）** | 编译 `bicycle` + rcS 执行 `bicycle &` |
| **未设置 / n** | 不编译、不自启 |

栈、优先级、资源路径、分辨率等 **不在 Kconfig**，见 `bicycle/CMakeLists.txt`。

### 4.2 第二层：defconfig

文件：`boards/vela/configs/goldfish-arm64-v8a-ap/defconfig`

```kconfig
CONFIG_VELA_BICYCLE=y
CONFIG_UIKIT_FONT_MANAGER=y
```

修改 defconfig 后建议删除 `cmake_out/vela_goldfish-arm64-v8a-ap` 再全量 `build`。

### 4.3 第三层：CMakeLists.txt — 本地参数

文件：`../CMakeLists.txt` 顶部

```cmake
set(BICYCLE_PROGNAME bicycle)
set(BICYCLE_PRIORITY 100)
set(BICYCLE_STACKSIZE 32768)
set(BICYCLE_RESOURCE_DIR "/etc")
set(BICYCLE_PAGE_HOR_RES 240)
set(BICYCLE_PAGE_VER_RES 320)
```

### 4.4 rcS 自启

文件：`boards/vela/src/etc/init.d/rcS`

```sh
#ifdef CONFIG_VELA_BICYCLE
bicycle &
#endif
```

### 4.5 配置责任一览

```
你想改什么？                    改哪里？
─────────────────────────────────────────────────────────
关掉整个 bicycle               Kconfig / defconfig
改栈、优先级、资源路径、分辨率   bicycle/CMakeLists.txt
改 UI / 业务逻辑               bicycle/src/
改字体图片轨迹文件             bicycle/etc/ + 重新 build
```

### 4.6 注册链路

```
boards/vela/CMakeLists.txt       → add_subdirectory(bicycle)
bicycle/CMakeLists.txt           → nuttx_add_application
boards/vela/Kconfig              → VELA_BICYCLE
defconfig                        → CONFIG_VELA_BICYCLE=y
rcS                              → bicycle &
boards/vela/src/CMakeLists.txt   → add_board_rcraws(bicycle/etc)
```

`add_board_rcraws` 在 NuttX 编译末尾由 `process_all_directory_romfs()` 打包进 ROMFS（需 `CONFIG_ETC_ROMFS=y`），开机挂载为 `/etc`。

---

## 5. 编译与运行

```bash
python3 vela_emulator_tools.py build
python3 vela_emulator_tools.py run
```

产物：`cmake_out/vela_goldfish-arm64-v8a-ap/`

---

## 6. 资源（ROMFS /etc）

`bicycle/etc/` 通过一行登记编入固件：

```cmake
if(CONFIG_VELA_BICYCLE)
  add_board_rcraws(${NUTTX_BOARD_ABS_DIR}/bicycle/etc)
endif()
```

| 源码 | 设备路径 |
|------|----------|
| `etc/font/` | `/etc/font/` |
| `etc/images/` | `/etc/images/` |
| `etc/track/` | `/etc/track/` |

改 `etc/` 后重新 `build`，无需 `adb push`。

---

## 7. 持久化路径：JSON 配置与 GPX 轨迹

运行时读写路径由 `Config.h` 的宏拼出，`CMakeLists.txt` 注入资源根目录：

```cmake
set(BICYCLE_RESOURCE_DIR "/etc")
# → 编译期 CONFIG_RESOURCE_DIR_PATH="/etc"
```

```c
// Config.h
#define RESOURCE_MAKE_PATH(path)  "/" CONFIG_RESOURCE_DIR_PATH path
// 例：RESOURCE_MAKE_PATH("/SystemSave.json") → "/etc/SystemSave.json"
```

以下路径均为 **设备内逻辑路径**（经 LVGL `lv_fs_*` 访问）。模拟器上 SdCard HAL 的 `mkdir`/`remove` 会去掉前导 `/`，与 `lv_fs` 使用同一套路径约定。

### 7.1 系统配置 JSON（`SystemSave.json`）

| 项目 | 路径 |
|------|------|
| 主配置文件 | **`/etc/SystemSave.json`** |
| 备份目录 | **`/etc/Backup/`** |
| 备份文件名 | **`/etc/Backup/SystemSave_YYYYMMDD_HHMMSS.json`** |

定义见 `DP_Storage.cpp`：

```c
#define SYSTEM_SAVE_PATH       RESOURCE_MAKE_PATH("/SystemSave.json")
#define SYSTEM_SAVE_BACKUP_DIR RESOURCE_MAKE_PATH("/Backup")
```

**读写时机**

| 操作 | 触发 |
|------|------|
| **LOAD** | 启动后 `DATA_PROC_INIT_FINISHED`（`DP_Storage` 订阅 Global 事件） |
| **SAVE** | 关机流程中 `DP_Power::requestShutdown()` → `_env.set("storage", "save")` |

SAVE 时会写两份：先覆盖主文件，再按当前时钟生成带时间戳的备份。

**实现链路**：各 `DP_*` 在构造时通过 `Storage_Helper` 把字段指针注册到 `DP_Storage` → `StorageService` 用 cJSON 序列化/反序列化整文件。

**写入 JSON 的字段（按节点）**

| JSON 键 | 来源节点 | 含义 |
|---------|----------|------|
| `timeZone` | DP_Clock | 时区 |
| `language` | DP_i18n | 语言 |
| `autoRec` | DP_Recorder | 自动记轨迹 |
| `sportStatus.*` | DP_SportStatus | 里程、时长、速度、体重、经纬度等 |
| `backlight.*` | DP_Backlight | 当前/白天/夜间亮度 |
| `batteryUseTime` / `autoShutdownTime` | DP_Power | 电池使用时间、自动关机 |
| `theme` / `dispRotation` | DP_Theme | 主题名、屏幕旋转 |
| `map.*` | DP_MapInfo | 地图目录、扩展名、坐标转换 |

示例结构（字段随版本略有增减）：

```json
{
  "timeZone": 8,
  "language": "zh-CN",
  "autoRec": false,
  "sportStatus": {
    "totalDistance": 0,
    "totalTime": [0, 0],
    "speedMaxKph": 0,
    "weight": 65,
    "longitude": 116.391332,
    "latitude": 39.907415
  },
  "backlight": { "current": 100, "day": 100, "night": 30 },
  "map": { "path": "/etc/MAP", "ext": ".png", "coordTrans": false }
}
```

首次启动若文件不存在，LOAD 会失败并打日志，各节点使用 `Config.h` 中的默认值。

### 7.2 轨迹 GPX 文件（运行时写入）

| 项目 | 路径 |
|------|------|
| 轨迹根目录 | **`/etc/Track/`** |
| 按月分子目录 | **`/etc/Track/{年}_{月}/`**，如 `/etc/Track/2026_06/` |
| 单次录制文件 | **`/etc/Track/{年}_{月}/TRK_{YYYYMMDD}_{HHMMSS}.gpx`** |

定义见 `Config.h` 与 `DP_Recorder.cpp`：

```c
#define CONFIG_TRACK_RECORD_FILE_DIR_NAME  RESOURCE_MAKE_PATH("/Track")
// RECORDER_GPX_FILE_NAME → /etc/Track/%d_%02d/TRK_%d%02d%02d_%02d%02d%02d.gpx
```

**写入流程**

1. 开始录制（`recStart`）：`SdCard` 创建月目录 → `lv_fs_open` 创建 GPX → 写 header/metadata。
2. 录制中（`recPoint`）：每次 GNSS 更新追加 `<trkpt>`。
3. 结束录制（`recStop`）：写闭合标签并 `lv_fs_close`。
4. 关机 SAVE 时 `DP_Recorder` 会先停止当前录制，再落盘 JSON。

### 7.3 预置 GPX（ROMFS，只读）

| 项目 | 说明 |
|------|------|
| 源码 | `bicycle/etc/track/TRK_EXAMPLE.gpx` |
| ROMFS 挂载 | `/etc/track/`（**小写** `track`） |
| 模拟 GNSS 读取 | `HAL_GNSS.cpp` 使用 **`/etc/Track/TRK_EXAMPLE.gpx`**（**大写** `Track`） |

代码里轨迹目录统一为 **`/etc/Track`**（大写 T），与 ROMFS 打包目录 **`/etc/track`**（小写）不一致。在大小写敏感的文件系统上，模拟 GNSS 可能找不到示例 GPX；若遇此问题，可将 `etc/track` 改为 `etc/Track` 后重新 build，或在源码侧统一大小写。

### 7.4 路径对照总表

```
CMake BICYCLE_RESOURCE_DIR="/etc"
        │
        ├─ 只读 ROMFS（build 时打包 bicycle/etc/）
        │     /etc/font/          ← etc/font/
        │     /etc/images/        ← etc/images/
        │     /etc/track/         ← etc/track/   （预置 GPX 示例）
        │
        └─ 运行时读写（lv_fs + SdCard HAL）
              /etc/SystemSave.json
              /etc/Backup/SystemSave_*.json
              /etc/Track/{Y_M}/TRK_*.gpx      ← 用户录制的轨迹
              /etc/MAP/                         ← 地图瓦片（若存在）
```

### 7.5 模拟器上查看文件

NSH 或 adb shell 进入设备后：

```sh
ls /etc/SystemSave.json
ls /etc/Backup/
ls /etc/Track/
cat /etc/SystemSave.json
```

从宿主机拉取（模拟器 adb 可用时）：

```sh
adb pull /etc/SystemSave.json .
adb pull /etc/Track/ ./Track/
```

`etc/upload_res.sh` 是 x_track 旧流程（push 到 `/data`），**bicycle 板级走 ROMFS，一般不需要**。

---

## 8. 启动方式

| 方式 | 条件 | 行为 |
|------|------|------|
| rcS 自启（默认） | `CONFIG_VELA_BICYCLE=y` | `bicycle &` |
| NSH 手动 | 已编入固件 | `bicycle` / `bicycle &` |
| 完全关闭 | `# CONFIG_VELA_BICYCLE is not set` | 不编译、不自启 |

---

## 9. 涉及文件一览

| 文件 | 作用 |
|------|------|
| `doc/bicycle_guide.md` | 本文 |
| `CMakeLists.txt` | 本地参数、注册 builtin |
| `bicycle_main.cpp` | 入口 |
| `src/` | 应用源码 |
| `etc/` | ROMFS 资源 |
| `boards/vela/src/CMakeLists.txt` | `add_board_rcraws(bicycle/etc)` |
| `boards/vela/Kconfig` | 总开关 |
| `defconfig` | 默认开启 |
| `rcS` | 自启 |

---

## 10. 日常开发

1. UI / 逻辑 → `src/`  
2. 参数 → `CMakeLists.txt`  
3. 资源 → `etc/` + `build`  
4. 验证 → `vela_emulator_tools.py run`

不要改 `apps/packages/demos/x_track/`（除非有意合并上游）。

---

## 11. 从 x_track 再同步（可选）

```bash
SRC=apps/packages/demos/x_track
DST=vendor/openvela/boards/vela/bicycle
rsync -a --delete "${SRC}/src/" "${DST}/src/"
rsync -a --delete "${SRC}/resource/font/" "${DST}/etc/font/"
rsync -a --delete "${SRC}/resource/images/" "${DST}/etc/images/"
rsync -a --delete "${SRC}/resource/track/" "${DST}/etc/track/"
```

---

## 12. 常见问题

### `help` 里没有 `bicycle`

确认 `CONFIG_VELA_BICYCLE=y` 且已 `build`；defconfig 变更后删 `cmake_out/vela_goldfish-arm64-v8a-ap` 再编。

### 黑屏 / 缺字缺图

改 `etc/` 后未 `build`；或 NSH 检查 `ls /etc/font`。

### 和 x_track Demo 混淆

默认固件是板级 **bicycle**；`x_track` 需 `build -d` 走 packages Kconfig。

---

## 13. LiveMap 矢量地图与资源（SF32 当前）

真机板级（`vendor/my_vendor/.../bicycle`）当前以 **LiveMap 为默认主页**，矢量地图资源在 LittleFS，与模拟器 ROMFS `/etc` 路径不同。

### 13.1 地图资源路径

| 项 | SF32LB52（CMake） | 说明 |
|----|-------------------|------|
| 资源根 | `/mnt/lfs` | `CMakeLists.txt` → `CONFIG_RESOURCE_DIR_PATH` |
| 矢量瓦片 | `/map/z/x/y.vt` | `CONFIG_VMAP_DIR_PATH`，打包脚本 `tools/build_vmap.py` / `pack_vmap.py` |
| 默认中心 | test.gpx 起点（滁州 demo 区域） | `LiveMapView.cpp` 中 `VMAP_TRACK_START_*` |
| 默认 zoom | 瓦片 z14 + scale **1.3** | `VMAP_DEMO_ZOOM`、`LIVEMAP_DEFAULT_SCALE` |

### 13.2 与文档其它章节的关系

| 主题 | 文档 |
|------|------|
| 页面路由、MTP、主题、转场动画、LiveMap 常驻 | [ui_framework.md](ui_framework.md) |
| 开机 / 关机 splash、地图首帧 | [splash.md](splash.md) |
| VectorMapView 道路/路名算法 | [utils_components.md §VectorMapView](utils_components.md#vectormapview) |
| MVP 数据流 | [data_and_mvp.md §5.2](data_and_mvp.md#52-启动后进入-livemap当前主页) |

### 13.3 CMake 构建注意

仅使用 **CMake 出树构建**（`./build.sh ... --cmake`）时，勿在 `nuttx/` 源码树留下 make 残留的 `.config`，否则 CMake 会报错要求 `make distclean`。清理：

```bash
rm -f nuttx/{Make.defs,defconfig,.config,.config.*,.gdbinit,.dirlinks}
# 或删除整个 cmake 输出目录后重新 lunch + build
```

---

## 参考

- [X-TRACK 上游文档](../../../../../docs/zh-cn/demo/X_Track_zh-cn.md)
- [模拟器快速入门](../../../../../docs/zh-cn/quickstart/openvela_ubuntu_quick_start.md)
- [test_app 板级示例](../../../../../vendor/my_vendor/docs/test_app_guide.md)

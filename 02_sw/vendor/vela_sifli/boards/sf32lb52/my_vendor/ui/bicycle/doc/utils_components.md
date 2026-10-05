# bicycle Utils 组件说明

路径：`src/App/Utils/`

Utils 是 **应用层工具库**，供 UI（View）和 DataProc（`DP_*`）复用。各组件 **不是** DataBroker 节点，一般不负责业务状态机。

---

## 目录

1. [UI / LVGL 扩展](#1-ui--lvgl-扩展)
2. [地图与轨迹](#2-地图与轨迹)
3. [GPS / 文件格式](#3-gps--文件格式)
4. [数据滤波与容器](#4-数据滤波与容器)
5. [存储与绑定](#5-存储与绑定)
6. [基础库](#6-基础库)
7. [示例目录（可忽略）](#7-示例目录可忽略)
8. [组件依赖关系简图](#8-组件依赖关系简图)

---

## 1. UI / LVGL 扩展

### lv_msg

| 项 | 说明 |
|----|------|
| **路径** | `Utils/lv_msg/` |
| **作用** | View 层 **进程内消息总线**：`lv_msg_send` / `lv_msg_subscribe_obj`，配合 `LV_EVENT_MSG_RECEIVED` 刷新控件 |
| **典型用法** | Presenter 转发 Model 数据 → `DashboardView::publish(MSG_ID, data)` → 各 label 订阅更新 |
| **依赖** | LVGL |

### lv_ext

| 项 | 说明 |
|----|------|
| **路径** | `Utils/lv_ext/` |
| **作用** | LVGL 小扩展：`lv_anim_timeline_wrapper`（批量注册 timeline 动画）、`lv_ext_func`（如 `lv_indev_search`）、`LV_ANIM_EXEC` 宏 |
| **典型用法** | Dashboard 进场动画、`PageManager` 无关的控件动画 |

### lv_anim_label

| 项 | 说明 |
|----|------|
| **路径** | `Utils/lv_anim_label/` |
| **作用** | 自定义 LVGL 控件：数字/文字 **滚动切换** 动画（进出场方向、时长可配） |
| **典型用法** | StatusBar 录制状态 `lv_anim_label_push_text` |

### lv_toast

| 项 | 说明 |
|----|------|
| **路径** | `Utils/lv_toast/` |
| **作用** | 屏幕底部/浮层 **Toast 提示** UI 组件（C API） |
| **说明** | 业务侧 `DP_Toast` 节点负责逻辑；本模块为 LVGL 展示层，可与 DataProc 配合 |

### lv_poly_line

| 项 | 说明 |
|----|------|
| **路径** | `Utils/lv_poly_line/` |
| **作用** | 在 LVGL 画布上绘制 **折线**（轨迹线渲染底层） |
| **典型用法** | `TrackView` 绘制实时轨迹 |

### TileView

| 项 | 说明 |
|----|------|
| **路径** | `Utils/TileView/` |
| **作用** | **地图瓦片** 视图：管理多块 tile 图片的拼接、平移、缩放锚点 |
| **典型用法** | 被 `MapView` 组合使用 |

### MapView

| 项 | 说明 |
|----|------|
| **路径** | `Utils/MapView/` |
| **作用** | **地图 UI 控件**：瓦片地图 + 当前位置点 + 路径点，封装 `MapConv` + `TileView` |
| **典型用法** | LiveMap 页面 |

### TrackView

| 项 | 说明 |
|----|------|
| **路径** | `Utils/TrackView/` |
| **作用** | **轨迹 UI 控件**：在父容器上绘制运动轨迹线，内置点/线滤波 |
| **典型用法** | 仪表盘或地图上的轨迹预览 |

### easing

| 项 | 说明 |
|----|------|
| **路径** | `Utils/easing/` |
| **作用** | **缓动函数库**（ease-in/out 等），用于非线性插值 |
| **典型用法** | `DP_Backlight` 背光亮度渐变 |

---

## 2. 地图与轨迹

### MapConv

| 项 | 说明 |
|----|------|
| **路径** | `Utils/MapConv/` |
| **作用** | **坐标转换**：经纬度 ↔ 瓦片坐标（tileX/tileY/subX/subY），Web 墨卡托相关 |
| **子模块** | `TileSystem.h/cpp` — 瓦片编号与像素换算 |
| **典型用法** | `MapView`、`DP_TrackFilter`、`DP_MapInfo` |

### Geo

| 项 | 说明 |
|----|------|
| **路径** | `Utils/Geo/` |
| **作用** | **地理计算**：两点距离（米）、方位角、目的地推算等 |
| **典型用法** | `DP_SportStatus` 根据连续 GPS 点累计里程 |

### PointContainer

| 项 | 说明 |
|----|------|
| **路径** | `Utils/PointContainer/` |
| **作用** | **二维点序列容器**（screen/map 坐标），支持 push、遍历、裁剪 |
| **典型用法** | `DP_TrackFilter` 缓存滤波后的轨迹点 |

### TrackPointFilter

| 项 | 说明 |
|----|------|
| **路径** | `Utils/TrackPointFilter/` |
| **作用** | **轨迹点级滤波**：GPS 跳点剔除、最小距离阈值、平滑 |
| **典型用法** | `TrackView`、`DP_TrackFilter` |

### TrackLineFilter

| 项 | 说明 |
|----|------|
| **路径** | `Utils/TrackLineFilter/` |
| **作用** | **轨迹线级简化/滤波**（减少绘制点数、Douglas-Peucker 类逻辑） |
| **典型用法** | `TrackView` 长轨迹显示优化 |

### VectorMapView

| 项 | 说明 |
|----|------|
| **路径** | `Utils/VectorMap/`（`VectorMapView.cpp`、`VectorTileCache.cpp` 等） |
| **作用** | **矢量瓦片（`.vt`）软件栅格化** 到 `lv_canvas`（RGB565），供 LiveMap 上半屏底图 |
| **瓦片路径** | `CONFIG_VMAP_DIR_PATH`（默认 `/map`，见 `Config.h`） |
| **画布** | 视口 + `CONFIG_VMAP_CANVAS_MARGIN`  overscan，平移时仅移动 canvas 对象 |

**道路绘制（当前）**

- 单层 fill，**无 casing 描边**（避免分段 junction 横线伪影）。
- `drawThickLine` 沿线段法向挤出四边形；顶点 `fillDisc` 圆角连接。
- 按道路等级固定屏幕像素线宽（`ROAD_STYLE[]`）。

**道路名称标签（当前）**

| 规则 | 说明 |
|------|------|
| 收集 | 沿几何折线按视口间距放置；并在折线上离 **导航箭头** 最近点额外锚定一个候选 |
| 优先级 | 纯 **屏幕像素距离**（箭头中心 ↔ 标签锚点），近的优先占位；不依赖道路等级 |
| 放置顺序 | 按距离从近到远尝试 `tryPlace`，碰撞时近处路名胜出 |
| 方向排版 | `\|dy\| > \|dx\|` 视为竖向路：UTF-8 逐字插入 `\n`，`lv_text_get_size(..., max_width=单字宽)` 竖排；**不旋转** canvas |
| 横向路 | 单行横排，如「恒丰路」 |

**LiveMap 组合**

```
LiveMapView（矢量底图 + 烘焙路名）
    ↑ 同一 mapArea
MapView（透明：轨迹折线 + 位置，栅格瓦片关闭）
    ↑ bindArrowLayer
导航箭头（跟随模式居中）
```

---

## 3. GPS / 文件格式

### GPX

| 项 | 说明 |
|----|------|
| **路径** | `Utils/GPX/` |
| **作用** | **生成 GPX XML** 字符串（metadata、trk、trkpt、时间、海拔） |
| **典型用法** | `DP_Recorder` 写 `.gpx` 轨迹文件 |
| **依赖** | `WString` |

### GPX_Parser

| 项 | 说明 |
|----|------|
| **路径** | `Utils/GPX_Parser/` |
| **作用** | **流式解析 GPX**，逐点回调（经纬度、海拔、时间） |
| **典型用法** | 导入/回放历史轨迹（继承 `Stream` 接口） |

### SunRiseCalc

| 项 | 说明 |
|----|------|
| **路径** | `Utils/SunRiseCalc/` |
| **作用** | 根据 **经纬度、日期、时区** 计算 **日出/日落** 时刻 |
| **典型用法** | `DP_SunRise` — 自动日/夜背光策略 |

---

## 4. 数据滤波与容器

### Filters

| 项 | 说明 |
|----|------|
| **路径** | `Utils/Filters/` |
| **作用** | 通用 **一维信号滤波器** 集合（头文件聚合 `Filters.h`） |

| 子文件 | 说明 |
|--------|------|
| `FilterBase.h` | 滤波器基类接口 |
| `LowpassFilter.h` | 低通滤波 |
| `MedianFilter.h` | 中值滤波 |
| `MedianQueueFilter.h` | 滑动窗口中值 |
| `SlidingFilter.h` | 滑动平均 |
| `HysteresisFilter.h` | 滞后/施密特滤波（防抖） |

**典型用法**：传感器数据平滑（速度、坡度等，按具体 DP 引用）。

### FifoQueue

| 项 | 说明 |
|----|------|
| **路径** | `Utils/FifoQueue/` |
| **作用** | 模板化 **FIFO 环形队列**，固定容量、无动态分配 |
| **典型用法** | 缓冲短时数据流 |

---

## 5. 存储与绑定

### StorageService

| 项 | 说明 |
|----|------|
| **路径** | `Utils/StorageService/` |
| **作用** | **JSON 持久化引擎**：注册字段指针 → `save()`/`load()` 读写文件 |
| **典型用法** | `DP_Storage` 写 `/etc/SystemSave.json` |
| **依赖** | cJSON、LVGL 内存钩子 |

### Binding

| 项 | 说明 |
|----|------|
| **路径** | `Utils/Binding/Binding.h` |
| **作用** | MVP **双向绑定** 模板：`Binding<T, Context>`，getter/setter 回调 |
| **典型用法** | Dashboard `Rec` 录制开关 ↔ `DashboardModel` notify/pull Recorder |

---

## 6. 基础库

### WString

| 项 | 说明 |
|----|------|
| **路径** | `Utils/WString/` |
| **作用** | Arduino 风格 **动态字符串** `String`，以及 `itoa`/`dtostrf` 等 C 字符串工具 |
| **典型用法** | `GPX` 拼 XML、`DP_Env` 键值存储 |

### Stream / Print

| 项 | 说明 |
|----|------|
| **路径** | `Utils/Stream/` |
| **作用** | Arduino 兼容 **流式 I/O 抽象**（`Stream`、`Print`、`Printable`） |
| **典型用法** | `GPX_Parser` 从流读取；串口/文件类接口统一 |

### Time

| 项 | 说明 |
|----|------|
| **路径** | `Utils/Time/` |
| **作用** | Arduino **TimeLib**：`setTime`、`hour()`、`adjustTime` 时区偏移等 |
| **典型用法** | `DP_Clock` GPS 校时、`DP_Recorder` 时间戳 |

### CommonMacro

| 项 | 说明 |
|----|------|
| **路径** | `Utils/CommonMacro/` |
| **作用** | 通用 **C 宏工具库**：数组大小、限幅、超时执行、一次性执行、事件监视等（`CM_*`） |
| **典型用法** | `DP_Backlight` 等模块 |

---

## 7. 示例目录（可忽略）

| 路径 | 说明 |
|------|------|
| `Time/examples/` | Arduino 官方 Time 库示例 `.ino`，**产品构建不包含** |
| `GPX/Examples/` | GPX 库 Processing 示例，**仅供参考** |

---

## 8. 组件依赖关系简图

```
                    ┌─────────────┐
                    │  MapView    │
                    └──────┬──────┘
           ┌───────────────┼───────────────┐
           ▼               ▼               ▼
      TileView         MapConv          Geo
           │               │
           └─────── TileSystem

      LiveMapView ──→ VectorTileCache / VMapCacheArena
      LiveMapView ──→ MapConv（投影与缩放 scale）

      TrackView ──→ TrackPointFilter / TrackLineFilter
                 └── lv_poly_line

      DP_Recorder ──→ GPX ──→ WString
      DP_Storage  ──→ StorageService (cJSON)

      DashboardView ──→ lv_msg / lv_ext / lv_anim_timeline_wrapper
      StatusBar     ──→ lv_anim_label

      DP_SportStatus ──→ Geo
      DP_TrackFilter ──→ PointContainer / MapConv / TrackPointFilter
      DP_Backlight   ──→ easing / CommonMacro
      DP_SunRise      ──→ SunRiseCalc
      DP_Clock         ──→ Time

      DashboardModel ──→ Binding
```

---

## 快速索引表

| 目录 | 一句话 |
|------|--------|
| Binding | MVP 双向绑定 |
| CommonMacro | 通用 C 宏 |
| easing | 缓动曲线 |
| FifoQueue | 环形 FIFO |
| Filters | 低通/中值/滞后等滤波器 |
| Geo | 距离、方位角 |
| GPX | 生成 GPX XML |
| GPX_Parser | 解析 GPX |
| lv_anim_label | 滚动文字控件 |
| lv_ext | LVGL 动画/输入扩展 |
| lv_msg | View 内消息 |
| lv_poly_line | LVGL 折线 |
| lv_toast | Toast UI |
| MapConv | 经纬度↔瓦片 |
| MapView | 地图控件 |
| VectorMapView | 矢量瓦片栅格底图（LiveMap） |
| PointContainer | 2D 点容器 |
| StorageService | JSON 存盘 |
| Stream | 流抽象 |
| SunRiseCalc | 日出日落 |
| TileView | 瓦片拼接 |
| Time | 系统时间库 |
| TrackLineFilter | 轨迹线简化 |
| TrackPointFilter | 轨迹点滤波 |
| TrackView | 轨迹绘制控件 |
| WString | Arduino String |

---

## 参考

- [data_and_mvp.md](data_and_mvp.md) — Binding、StorageService 在架构中的位置  
- [dp_recorder_guide.md](dp_recorder_guide.md) — GPX、Storage 使用示例

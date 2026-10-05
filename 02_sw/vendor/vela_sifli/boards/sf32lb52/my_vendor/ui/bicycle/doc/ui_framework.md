# bicycle UI 设计与实现说明

本文档描述 **SF32LB52 bicycle C UI** 的产品设计、页面架构、MTP 交互、主题与转场动画约定，以及开发时须遵守的注意事项。

> **当前主推栈**：`src/lvgl_page/` + `src/framework/lv_pm/` + `src/vmap/`  
> **开机 / 关机 splash**：[`splash.md`](splash.md)  
> **框架 API 细节**：见 [`src/framework/lv_pm/README.md`](../src/framework/lv_pm/README.md)  
> **遗留 C++ PageManager 栈**：见 [app_cpp_guide.md](app_cpp_guide.md)（非当前主路径）

---

## 目录

1. [架构总览](#1-架构总览)
2. [设计原则](#2-设计原则)
3. [页面一览](#3-页面一览)
4. [LiveMap 常驻主页](#4-livemap-常驻主页)
5. [MTP 与 UsbTransfer](#5-mtp-与-usbtransfer)
6. [页面导航与生命周期](#6-页面导航与生命周期)
7. [转场动画（成对原则）](#7-转场动画成对原则)
8. [主题系统](#8-主题系统)
9. [开机流程](#9-开机流程)
10. [地图与存储并发](#10-地图与存储并发)
11. [事件总线](#11-事件总线)
12. [禁止事项与注意事项](#12-禁止事项与注意事项)
13. [关键文件索引](#13-关键文件索引)
14. [新增页面 checklist](#14-新增页面-checklist)

---

## 1. 架构总览

```
bicycle_c_main.c
  └─ lvgl_page_init()           注册页面、默认主题
  └─ lvgl_page_open_boot_sequence() / open_home()
  └─ bicycle_mtp_ui_run()       轮询 MTP 状态 → push/pop UsbTransfer
  └─ bicycle_runtime            页面/MTP 事件广播

lv_pm（页面栈）
  ├─ LiveMap      cache=true，主页常驻
  ├─ UsbTransfer  cache=false，MTP 临时顶层
  └─ Startup      cache=false，开机 splash

vmap（矢量地图）
  └─ PSRAM canvas + tile cache + track overlay
```

| 模块 | 路径 | 职责 |
|------|------|------|
| **lv_pm** | `src/framework/lv_pm/` | 页面栈、生命周期、转场动画、主题 shell |
| **lvgl_page** | `src/lvgl_page/` | 各页注册、boot/MTP 导航封装 |
| **vmap** | `src/vmap/` | 矢量地图渲染、tile 缓存、样式 |
| **bicycle_mtp_ui** | `src/bicycle_mtp_ui.c` | USB 插拔防抖、push/pop 传输页 |
| **bicycle_runtime** | `src/bicycle_runtime.*` | 页面/MTP 等 subject 事件 |
| **bicycle_pm_theme** | `src/bicycle_pm_theme.c` | 主题切换 → vmap 样式联动 |

---

## 2. 设计原则

| 原则 | 说明 |
|------|------|
| **LiveMap 是长期主页** | 默认工作界面；MTP 只是临时盖住它，**不销毁、不重建**地图 |
| **overlay 用 push/pop** | UsbTransfer 作为顶层页 `push` 进入，退出 `pop`；**禁止**用 `RESET`/`replace` 换掉 LiveMap |
| **资源 pause，不 recycle** | LiveMap 被盖住时暂停渲染/按键/LFS 读 tile，**保留** canvas、tile cache、缩放/中心状态 |
| **动画成对** | 一次注册的 `lv_pm_anima_t` 同时定义 ENTER + EXIT，逻辑互逆（见 §7） |
| **主题双层** | `lv_pm_theme_def_t` 结构体颜色 + LVGL `lv_theme_t`/`apply_cb` 叠加，切换时 **不** `remove_style_all` |
| **MTP worker 与应用解耦** | UI 只读状态、切页面；**不在**插拔事件里调 `transfer_begin/end` |
| **busy 时 drain** | 转场未完成时新导航会先 `lv_pm_nav_drain()`，再执行 push/pop |

---

## 3. 页面一览

ID 定义：`src/lvgl_page/bicycle_page_ids.h`  
转场 / cache 配置：`src/lvgl_page/bicycle_page_anima.c`

| ID | 名称 | 文件 | 动画 | cache | 角色 |
|----|------|------|------|-------|------|
| 0 | LiveMap | `live_map/map_page.c` | `gpu_depth` 350ms | **yes** | **主页**；boot 预挂载用 `no_anima` 空壳 |
| 1 | UsbTransfer | `usb_transfer/usb_transfer_page.c` | `gpu_zoom` 350ms | no | MTP 临时 overlay |
| 2 | Startup | `startup/startup_page.c` | `gpu_zoom` 400ms | no | 开机 splash |

**LiveMap 布局（240×320）**

- 顶部：**lv_pm 下拉状态栏**（`top_bar_en`）— 收起 28px 条 **恒黑**；下拉展开区用 `theme.status_bg`
- 状态栏内容（LiveMap / Lap / REC）挂在 `lv_pm_status_bar_cont()`
- 中间：矢量底图 + 轨迹/导航箭头
- 底部 48px：骑行数据面板

**UsbTransfer**

- 全屏：USB 图标、文案、MTP 进度（upload/download）
- **不写死颜色**；布局-only，颜色/字体走 lv_pm LVGL 主题（classic 白底适合 MTP）

---

## 4. LiveMap 常驻主页

### 4.1 cache 语义

```c
lv_pm_set_cache_enable(page, true);   /* bicycle_page_anima_apply */
```

- **pop 时**：`close_cb` 仍调用，但 `cache_enable` 为 true 时 **不 destroy** `lv_obj` / `user_data`，仅 `HIDDEN`
- **再次 open**：复用同一 `map_page_t`、同一 vmap canvas 与 tile cache
- **`map_page_on_unload`**：若 `cache_enable`，直接 return，不 `map_page_destroy`

### 4.2 被 UsbTransfer 盖住时

| 动作 | 保留 | 暂停 |
|------|------|------|
| push UsbTransfer | VectorMap 画布、tile cache、Model/View、缩放/中心、`_mapReady` | 渲染 timer、平移动画、硬件键回调、LFS 读 tile |

实现钩子：

| 回调 / API | 行为 |
|------------|------|
| `map_will_disappear` | `map_page_pause_for_cover()` — 设 `covered`、清按键回调 |
| `map_will_appear` | `map_page_resume_after_cover()` — 清 `covered`；`rev>0` 只 invalidate，不开整帧 |
| `vmap_view.c` / tile 路径 | `myvendor_mtp_lfs_quiesce()` 为 true 时不读 LFS |

回到 LiveMap：`pop` 恢复**同一实例**，`resume` + 重绘；quiesce 解除后可继续 render（必要时轮询直到能画）。

### 4.3 boot 预挂载空壳

开机时 LiveMap **先进栈**，但 vmap 在 Logo 之后才加载：

```
push LiveMap（no_anima，黑屏空壳，cache）
push Startup（logo）
Startup 结束 → pop Startup
→ 同一 LiveMap 实例 depth 恢复动画 + vmap 已就绪
```

详见 §9。

---

## 5. MTP 与 UsbTransfer

### 5.1 总体目标

插 USB → 自动进入传输界面；拔线 → 回到地图。LiveMap 始终在栈底，MTP 只是盖住它。

### 5.2 MTP 服务架构

| 项目 | 要求 |
|------|------|
| Worker | `mtp_simple` 由 App 启动时 `init` + `transfer_begin` **一次**，之后 USB/MTP 协议自主运行 |
| 板级 autostart | **关闭** `CONFIG_MYVENDOR_MTP_SIMPLE_AUTOSTART`，由 bicycle **独占** worker |
| UI 职责 | **不在**插拔事件里调 `transfer_begin/end`；只根据状态 **切页面、发事件** |
| 状态来源 | `myvendor_mtp_get_status()` / `get_activity()` 读 worker 状态 |

### 5.3 UI 导航流程

```
LiveMap (cached, 运行中)
  → lvgl_page_push_usb_transfer()
       LiveMap: gpu_depth EXIT + will_disappear → pause
       UsbTransfer: gpu_zoom ENTER
  → lvgl_page_pop_usb_transfer()
       UsbTransfer: gpu_zoom EXIT → destroy
       LiveMap: gpu_depth ENTER (is_back=1) + will_appear → resume
```

入口：`src/bicycle_mtp_ui.c`（800ms 插拔防抖）+ `lvgl_page_push/pop_usb_transfer()`。

**例外**：开机栈里只有 Startup、尚无 LiveMap 时（极早插 USB），可先 **push 静默 LiveMap**，勿对已 cache 的 LiveMap 做 RESET。

### 5.4 与动画的关系

LiveMap ↔ UsbTransfer 使用 **不同** GPU 动画（depth / zoom），但各自 **ENTER/EXIT 成对**：

- 进 MTP：LiveMap **自己的** depth EXIT + UsbTransfer **自己的** zoom ENTER
- 出 MTP：UsbTransfer zoom EXIT + LiveMap depth ENTER（恢复分支）

若 transform 与大地图 canvas 并发导致 LVGL 崩溃，可临时在 `bicycle_page_anima.c` 改为 `lv_pm_no_anima` 定位问题。

### 5.5 LiveMap 导航提示（notify / bottom）

全局 overlay 由 `lv_pm_overlay.c` 绘制，与页面栈独立。**notify**（顶栏卡片）与 **bottom**（底部短条）分工不同：

| API | 位置 | 导航用途 |
|-----|------|----------|
| `lv_pm_notify_show_hold(title, text)` | 顶部 | 规划进行中：`规划路线中…`（hold 至 `lv_pm_notify_dismiss`） |
| `lv_pm_notify_show(title, text, ms)` | 顶部 | 规划结果、路口播报（通常 4000 ms） |
| `lv_pm_bottom_show(text, ms)` | 底部居中 | 偏航瞬间：`逆行偏航` / `路口偏航` / `严重偏航`；到达 `导航已结束` |

偏航流程（`map_page.c` GPX timer）：先 **bottom** 显示原因 → `capture_ridden_history` → **notify hold** 重规划 → 成功则 **notify 4s** `路线已更新 · …`。

`lv_pm_notify_show_hold` 会刷新 overlay chrome；bottom 卡片须 `LV_OBJ_FLAG_FLOATING` + 底部对齐（`overlay_bottom_layout_card`），否则 notify 出现后 bottom 可能错位到左上角。

完整文案表与偏航算法见 [docs/osm/ROUTING.md](../../../../../docs/osm/ROUTING.md) §界面提示、§偏航检测。

---

## 6. 页面导航与生命周期

### 6.1 栈模型

```
open A  →  [A]
open B  →  [A, B]     B 顶层；A 仍 is_open，可能被盖住
close   →  [A]         pop B
```

应用 API（`lvgl_page.h`）：

| API | 作用 |
|-----|------|
| `lvgl_page_open_home()` | push LiveMap |
| `lvgl_page_push_usb_transfer()` | push UsbTransfer |
| `lvgl_page_pop_usb_transfer()` | pop UsbTransfer |
| `lvgl_page_open_boot_sequence()` | boot 序列（§9） |

### 6.2 生命周期回调

| 回调 | 时机 | LiveMap 典型用途 |
|------|------|------------------|
| `open_cb` | 首次创建或 cache 复用 | `map_page_create` / boot 空壳 |
| `will_appear` | 动画开始前 | `map_page_resume_after_cover` |
| `dis_appear` | 进入动画结束 | 注册按键、启动轮询 |
| `will_disappear` | 即将被盖住或 pop | `map_page_pause_for_cover` |
| `dis_disappear` | 离开动画结束 | 少见 |
| `close_cb` | 从栈移除且非 cache | `map_page_destroy`（cache 页直接 return） |

### 6.3 push / pop 顺序（与 lv_pm 对称）

**push**

```
prev.will_disappear → prev EXIT 动画
new.open_cb → new.will_appear → new ENTER 动画 → dis_appear
```

**pop**

```
current.will_disappear → current EXIT 动画
prev.will_appear → prev ENTER 动画 (is_back=1) → dis_appear
current 动画完成 → close_cb → delete（或 cache 隐藏）
```

### 6.4 `lv_pm_target` 路由

| 值 | 用途 | LiveMap |
|----|------|---------|
| `LV_PM_TARGET_NEW` | 默认 **push** | ✅ boot、MTP |
| `LV_PM_TARGET_SELF` | 换栈顶，被盖页 deep 删除 | ❌ |
| `LV_PM_TARGET_RESET` | 整栈清空再 open | ❌ **禁止**（会拆掉 cache 与 vmap） |

---

## 7. 转场动画（成对原则）

### 7.1 一体注册

```c
lv_pm_set_open_options(page, &lv_pm_gpu_depth_anima,
                       LV_PM_TARGET_NEW, LV_PM_GPU_DIR_CENTER, 350);
```

一次注册 = **一对**动画：

| 相位 | API | 回调 |
|------|-----|------|
| **ENTER** | `lv_pm_anima_play(page, LV_PM_ANIMA_ENTER, ...)` | `lv_pm_appear` |
| **EXIT** | `lv_pm_anima_play(page, LV_PM_ANIMA_EXIT, ...)` | `lv_pm_dis_appear` |

**规则**

- 有 ENTER 就必须有 EXIT，来自 **同一** `lv_pm_anima_t`，逻辑互逆
- 动画类型读 **当前页** 自己的 `open_options.lv_pm_anima_cb`
- `route_opts` 仅传给完成回调（如 push 时被盖页的 `target`），**不参与**选动画
- `appear` / `dis_appear` 缺失时回退 `lv_pm_no_anima`

### 7.2 bicycle 默认配置

| 页面 | anima | 说明 |
|------|-------|------|
| LiveMap | `gpu_depth` | 被盖：缩小淡出；恢复：从 `GPU_SCALE_LEAVE` zoom 回来 |
| UsbTransfer | `gpu_zoom` | 进入：zoom in；退出：zoom out |
| Startup | `gpu_zoom` | splash |
| boot 空壳 LiveMap | `no_anima` | 仅 `bicycle_page_anima_apply_silent()` |

配置表唯一入口：`src/lvgl_page/bicycle_page_anima.c`。

---

## 8. 主题系统

### 8.1 内置主题

| 名称 | 文件 | 风格 | 用途 |
|------|------|------|------|
| `classic` | `lv_pm_theme_classic.c` | **亮色**（白底 `#FFFFFF`） | 默认；MTP 传输页 |
| `outdoor` | `lv_pm_theme_outdoor.c` | **暗色** | 户外骑行 |

NSH：`bicycle_nsh style classic|outdoor` → `lvgl_page_theme_set_by_name()`。

### 8.2 双层机制

1. **`lv_pm_theme_def_t`**：`colors` / `fonts` / `on_activate`（每主题一个 `.c` 文件）
2. **LVGL 原生主题**：`lv_pm_theme_lvgl.c` 绑定 `lv_theme_t` + `apply_cb`，新建对象时 `add_style` 叠加

切换时：

- 更新共享 `lv_style_t` + `lv_obj_report_style_change`
- **不** 对已有对象 `remove_style_all` / `lv_theme_apply()` 全量重刷
- `on_activate` → `bicycle_pm_theme_on_activate` → `vmap_style_set_id`
- `lv_pm_theme_apply_page_shell` 刷新各页 shell
- LiveMap 注册 `theme_changed_cb` → chrome + 箭头；画布已画过且主题未变则 **跳过**
  整帧 `vmap_view_render`（算法见 `docs/map/RUNTIME.md` §6）

### 8.3 页面分工

| 页面 | 主题应用 |
|------|----------|
| **LiveMap** | `lv_pm_theme` 颜色 + **`vmap_style`** 地图 canvas；`map_page_apply_style()` 读 `lv_pm_theme_current()->colors` |
| **UsbTransfer** | 纯 LVGL 主题（generic shell）；**不写死**黑白配色 |
| **Startup** | `lv_theme_get_font_normal` 等 |

### 8.4 新增主题

1. 复制 `lv_pm_theme_classic.c`，改 `name` / `colors`
2. 加入 `themes/lv_pm_themes.c` 的 `lv_pm_builtin_themes[]`
3. 若需地图联动，在 `bicycle_pm_theme.c` 扩展 `pm_theme_to_vmap`

### 8.5 国际化（lv_pm i18n）

- 内置 locale：`zh_CN`（默认）、`en`
- API：`lv_pm_tr(LV_PM_I18N_KEY_*)`、`lv_pm_i18n_set_by_name("en")`
- 页面 `lv_pm_set_i18n_changed` 在切换时刷新 label（UsbTransfer、Startup 已接）
- NSH：`bicycle_nsh lang en`（经 ctl 在 LVGL 线程应用）
- **空间**：字符串表约 2KB；字体仍用同一份 LFS subset TTF，不因中英切换增大 Flash

---

## 9. 开机流程

```
lvgl_page_open_boot_sequence()
  1. map_page_set_boot_deferred(true)
  2. push LiveMap（no_anima，空壳，黑屏，不创建 vmap，cache）
  3. push Startup（与 2SFBL 同一套 logo）
  4. Startup：黑→白后事项滚入（与关机同款「左 · 右」）
       骑行记录 · 恢复未保存 GPX
       系统字体 · TTF 进 PSRAM
       地图加载 · begin_load + prepare catalog/region 文件
  5. fade → pop Startup
  6. 露出 LiveMap；淡入完成后再启动首帧 pump
```

**约定**

- LiveMap **必须先**进栈；Startup 只做临时顶层
- 结束用 **pop**，不用 `open LiveMap + RESET`
- 事项行、进度条、首帧画布约束见 [splash.md](splash.md)
- 字体在事项里分块加载；`helm_font_sys()` 不得写 TTF `fallback`
- vmap 在白屏事项「地图加载」里创建；splash 盖着时只走 `boot_prepare`
- canvas 从创建起清零并隐藏；LiveMap `dis_appear` 后才 render/pump
- 还没进过地图页时，禁止 GNSS 隐藏跟车清画布（否则立刻翻地图会看到黑噪点）

---

## 10. 地图与存储并发

MTP ACTIVE / transfer 期间：

| 层 | 行为 |
|----|------|
| `myvendor_mtp_lfs_quiesce()` | true 时 vmap **不读 LFS**（tile 加载、索引 defer） |
| LiveMap pause | 不释放 PSRAM 画布与 tile cache |
| LiveMap resume | quiesce 解除后继续 render；MTP 结束后可 `map_page_reload_storage()` |

实现锚点：

- `src/vmap/vmap_view.c`、`vmap_tile_cache.c`、`vmap_tile_index.c`
- `src/myvendor_mtp.c`、`mtp_simple/`

---

## 11. 事件总线

页面/MTP 广播走 `bicycle_runtime`：

```c
bicycle_runtime_evt_observe(BICYCLE_SUBJ_EVT_PAGE, on_page_evt, obj, NULL);
bicycle_runtime_evt_observe(BICYCLE_SUBJ_EVT_MTP, on_mtp_evt, obj, NULL);
```

`lv_pm_set_nav_notify_cb` 在 appear/disappear 动画完成时写入 PAGE 事件；`bicycle_mtp_ui` 写入 MTP 事件。原 `lv_pm_msg` 已移除。

---

## 12. 禁止事项与注意事项

### 禁止

| 场景 | 禁止操作 | 原因 |
|------|----------|------|
| MTP 进传输 | `replace` / `RESET` 换掉 LiveMap | 销毁 canvas、tile cache |
| LiveMap 被盖 | `state unload` / 析构 vmap | 违反常驻主页设计 |
| 插拔 USB | UI 里调 `transfer_begin/end` | worker 自主运行 |
| 正常 boot | 对 LiveMap 用 RESET | 拆掉应常驻的资源 |
| 开机 splash | 未 `render_finish` 就显示 canvas / GNSS 隐藏 rebase | 半帧黑噪点 |
| 主题切换 | `remove_style_all` + 全量 `lv_theme_apply` | 导致布局/尺寸异常 |

### 注意事项

1. **单动画通道**：全局一组 appear/disappear；重叠时 drain 上一段
2. **`msg_data`**：框架只存指针、不 free；`close_cb` 后失效
3. **栈顶才能 pop**：`close_page_msg` 要求 `history_count > 1`
4. **GPU 动画**：依赖 `LV_USE_SIFLI_EPIC` + transform layer
5. **LiveMap ↔ UsbTransfer 动画不同**：各页用自己的 anima，但各自 ENTER/EXIT 成对
6. **MTP 传输 assert**：USB 栈回调勿在 spinlock 内嵌套 class callback（见 `sf32lb_usbdev.c`）

---

## 13. 关键文件索引

### 应用层

| 文件 | 说明 |
|------|------|
| `src/lvgl_page/lvgl_page.c` | init、boot、MTP 导航封装 |
| `src/lvgl_page/bicycle_page_anima.c` | 转场/cache 配置表 |
| `src/lvgl_page/live_map/map_page.c` | LiveMap 生命周期、主题、pause/resume |
| `src/lvgl_page/usb_transfer/usb_transfer_page.c` | MTP overlay UI |
| `src/lvgl_page/startup/startup_page.c` | 开机 splash |
| `src/lvgl_page/helm_splash.c` | 开机/关机共用 logo 与事项行 |
| `src/lvgl_page/helm_pwr.c` | 亮度、关机确认与关机 splash |
| `src/bicycle_mtp_ui.c` | MTP 插拔防抖、push/pop |
| `src/bicycle_pm_theme.c` | 主题 → vmap 样式 |
| `src/vmap/` | 矢量地图、tile、样式 |

### 框架层

| 文件 | 说明 |
|------|------|
| `src/framework/lv_pm/core/lv_pm_core.c` | 路由栈、push/pop |
| `src/framework/lv_pm/core/lv_pm_anima.c` | `lv_pm_anima_play` 调度 |
| `src/framework/lv_pm/core/lv_pm_theme*.c` | 主题 struct + LVGL 绑定 |
| `src/framework/lv_pm/themes/` | classic / outdoor 预设 |
| `src/framework/lv_pm/anima/lv_pm_gpu_anima.c` | GPU depth/zoom/flip 等 |

---

## 14. 新增页面 checklist

1. `bicycle_page_ids.h` 增加 ID，`BICYCLE_PM_ID_COUNT++`
2. 新建 `src/lvgl_page/<name>/<name>_page.c`，实现 `*_page_register()`
3. `lvgl_page_init()` 里调用 register
4. `bicycle_page_anima.c` 增加一行（anima + cache + time）
5. `lv_pm_create_page` + 六个生命周期 + `bicycle_page_anima_apply(page, id)`
6. CMakeLists 增加源文件
7. 更新本文档 §3 表格与 [`lv_pm/README.md` §8](../src/framework/lv_pm/README.md)

---

## 参考

- 框架 API 与动画细节：[src/framework/lv_pm/README.md](../src/framework/lv_pm/README.md)
- 矢量地图组件：[utils_components.md § VectorMap](utils_components.md)
- 板级开发：[bicycle_guide.md](bicycle_guide.md)
- 功能清单：[todolist.md](todolist.md)

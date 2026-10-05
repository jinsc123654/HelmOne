# 开机 / 关机 splash

开机进入与关机退出共用 `helm_splash`：白底、同一套 logo / 品名、细进度条，
事项行格式为 **`左边名称  ·  右边状态`**（中间一个间隔点）。进行中右侧跟
`.` / `..` / `...`，结果行停点。

相关源码：

| 文件 | 职责 |
|------|------|
| `src/lvgl_page/helm_splash.c` | 共用控件与滚入滚出 |
| `src/lvgl_page/startup/startup_page.c` | 开机事项状态机 |
| `src/lvgl_page/helm_pwr.c` | 关机事项状态机 |
| `src/lvgl_page/live_map/map_page.c` | splash 内准备文件，LiveMap 出现后画首帧 |
| `src/vmap/vmap_view.c` | 画布清零、首帧完成前隐藏 |

框架栈约定见 [ui_framework.md §9](ui_framework.md#9-开机流程)、
[lv_pm/README.md §8.1](../src/framework/lv_pm/README.md#81-开机流程已确定)。
地图首帧约束见 [LIVEMAP.md](../../../../../../docs/map/LIVEMAP.md) §4.4、
[RUNTIME.md](../../../../../../docs/map/RUNTIME.md) §6.3。

---

## 1. 事项行

同一事项内只改右侧，不滚动：

```
骑行记录  ·  恢复中...
骑行记录  ·  已恢复
```

换事项才滚出再滚入。不要用 `helm_splash_show("启动中  字体")` 那种整句替换。

---

## 2. 开机

`lvgl_page_open_boot_sequence()`：先静默 push LiveMap 空壳，再 push Startup。
Startup 先黑屏播开机提示音；音真正播完再黑→白（旋律本身只有一小会），白屏后
按下列事项滚入；每项 **滚入 → 干活 → 出结果 → 停一下 → 滚出**。声音策略关闭
则立刻进白屏。

| 左侧 | 进行中 | 结果 |
|------|--------|------|
| 骑行记录 | 恢复中 | 已恢复 / 已检查 / 无记录 |
| 系统字体 | 加载中 | 已加载 / 未找到（工厂固件跳过此项） |
| 地图加载 | 加载中 → 文件中 | 已加载 / 失败 |

约束：

- 黑屏期间只播开机提示音；白屏出现后才开始 GPX 恢复、字体、地图文件准备。
- 字体加载完成前事项行用内置点阵；TTF 进 PSRAM 并 `lv_bind` 后再切系统字。
  `helm_font_sys()` **不得**把 `fallback` 写进共享 TTF 缓存槽。
- 地图 `begin_load` 创建清零且隐藏的 canvas；`map_page_boot_prepare()` 只加载
  catalog 和启动坐标 region pack，**不**解码或绘制瓦片。
- 总超时 12 s **不打断 GPX 恢复**；其它事项超时则跳到「地图加载」完成文件准备。
- 主机已 ENUM MTP：跳过地图文件准备，直接 fade 退出，避免与 MTP 争用
  LittleFS；拔线后由 LiveMap 正常加载。
- Startup pop 后先完成 LiveMap 的 220 ms 淡入；`dis_appear` 回调再启动首帧
  render/pump，避免地图 I/O 抢占转场动画。

进度条按事项目标 18 / 40 / 62 / 85 / 100 线性跟上，不要在事项边界硬跳。

---

## 3. 关机

长按 PWR 确认或菜单关机后，同一套白底 splash。三项：

| 左侧 | 进行中 | 结果举例 |
|------|--------|----------|
| 骑行记录 | 保存中 | 已保存 / 无记录 / 未完成 |
| 卫星星历 | 保存中 | 已保存 / 无定位 / 同步已关 … |
| 文件存储 | 关闭中 | 已关闭 |

进度条从 0 线性走到 100，覆盖进入、三项滚入滚出和刷黑整段，**不要**按 20/50/80
跳变。事项结束后：隐藏品牌 → 全屏黑幕缓入（约 800 ms）→ `lv_refr_now` → 断电。
最终黑屏不再画反相 logo。半透半反屏关背光仍有残影，必须先把帧刷黑。

关机过程不再响应按键。

---

## 4. LiveMap 首帧（延迟渲染并避免噪点）

Startup 只负责文件加载，不再在进度页下计算地图。若未初始化的 PSRAM canvas
在第一帧完成前送到 LCD，就会看到黑色噪点；若淡入期间启动瓦片泵，又会让同步
VPK I/O 抢占动画。

现行约定是：

| 点 | 行为 |
|----|------|
| `vmap_view_create` | `memset` 画布；canvas **HIDDEN**，直到第一帧 `render_finish` |
| `map_page_create` + `boot_deferred` | 创建资源但保持所有 LiveMap timer 暂停 |
| splash 覆盖期间 | `map_page_boot_prepare` 只设置中心并打开文件，不 render/pump |
| LiveMap `will_appear` | 只显示稳定背景并绑定按键，不恢复 render timer |
| LiveMap `dis_appear` | 淡入完成后清 `covered`，再启动首帧和 render timer |
| 还没进过地图页（`map_ui_seen==false`） | 隐藏页 **禁止** `set_center` 全图 rebase |
| 第一帧完成 | `render_finish` 才解除 canvas HIDDEN，一次显示完整地图 |

屏幕是 **LCDC QSPI**，不是 SPI LCD；不要把 DMA1 CH3（SDMMC）和旧 SPI1 刷屏当成冲突。

---

## 5. 禁止

- 事项进行中把 `busy=false` 的结果行再改回带点的「启动中 xxx」。
- splash 未完成时让 GNSS 隐藏跟车清画布。
- 第一帧未 `render_finish` 就 `clear_flag(HIDDEN)` 显示 canvas。
- 关机刷黑时把 splash 整体 opa 淡到 0（会露出底下未刷黑的帧）。

---

## 6. API

头文件是 Doxygen 源。事项切换用 `set_row` + `slide_in`/`slide_out`，同一事项内
只改右侧、不滚动。

| 函数 | 作用 |
|------|------|
| `helm_splash_build` | 创建 logo / 品名 / 事项行 / 进度条 |
| `helm_splash_set_row(s, key, val, busy)` | 立刻写成 `key  ·  val` |
| `helm_splash_pump` | 进行中追加 `.` / `..` / `...` |
| `helm_splash_slide_in` / `slide_out` | 换事项时滚入滚出 |
| `helm_splash_bar_linear` | 进度条线性走，不要在事项边界硬跳 |
| `helm_splash_hide_brand` | 关机刷黑前去掉 logo |
| `startup_page_register` / `finish` / `sync_boot` | 开机页注册、pop、主题后锁黑 |
| `lvgl_page_open_boot_sequence` | LiveMap 空壳 + Startup |
| `map_page_set_boot_deferred` / `boot_begin_load` / `boot_prepare` | 空壳、创建资源、加载地图文件 |
| `map_page_set_map_ui_visible` | 第一次露出才允许隐藏跟车 rebase |
| `bicycle_ride_gpx_salvage_boot_pump` / `scanned` / `fixed` | 白屏恢复 GPX |
| `helm_pwr_exec` / `exec_as` | 关机 splash 后断电 |

---

## 7. 相关文档

| 文档 | 内容 |
|------|------|
| [ui_framework.md §9](ui_framework.md#9-开机流程) | 页面栈：LiveMap 在底、Startup 只 pop |
| [lv_pm/README.md §8.1](../src/framework/lv_pm/README.md#81-开机流程已确定) | 框架侧 boot / RESET 禁令 |
| [LIVEMAP.md §4.4](../../../../../../docs/map/LIVEMAP.md) | retained canvas 开机首帧 |
| [RUNTIME.md §6.3](../../../../../../docs/map/RUNTIME.md) | warmup / pump / `map_ui_seen` |
| [bicycle_guide.md](bicycle_guide.md) | 模拟器由来；真机 splash 以本文为准 |

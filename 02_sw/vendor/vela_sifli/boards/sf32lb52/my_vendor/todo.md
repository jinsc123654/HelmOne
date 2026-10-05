# my_vendor 待办与已知问题

## 产品需求 · MTP + LiveMap（权威）

### 1. 总体目标

在 bicycle 码表 UI 里接入 MTP 文件传输：插 USB 自动进入传输界面，拔线回到地图；**LiveMap 是长期主页**，MTP 只是临时盖住它，**不销毁、不重建**地图。

### 2. MTP 服务架构

| 项目 | 要求 |
|------|------|
| Worker | `mtp_simple` 由 App 启动时 init + transfer_begin **一次**，之后 USB/MTP 协议 **自主运行** |
| 板级 autostart | **关闭** `CONFIG_MYVENDOR_MTP_SIMPLE_AUTOSTART`，由 bicycle **独占** worker |
| UI 职责 | **不在**插拔事件里调 transfer_begin/end；只根据状态 **切页面、改 Env** |
| 状态来源 | `myvendor_mtp_get_status()` / `get_activity()` 读 worker 状态 |

### 3. 页面与导航（核心）

**LiveMap（主页）**

- 默认工作界面；**资源只做暂停，不做回收**（与其他页面不同）。
- 被 UsbTransfer 盖住时：**保留**画布、tile cache、Model/View、`_mapReady`、缩放/中心；**暂停**渲染 timer、平移动画、GNSS、硬件键、LFS tile 读（`lfs_quiesce`）。
- **禁止**进 MTP 时 state unload / 析构（`setCacheEnable(true)` + **push**，不用 **replace** 换掉 LiveMap）。
- 回到 LiveMap：**pop** 同一实例，**resume** + 必要时重绘。

**UsbTransfer（传输页 · 特殊顶层页）**

- 独立全屏：USB 图标、文案、MTP 进度。
- push 进入 / pop 退出；Startup 直进 UsbTransfer 时退出可 replace LiveMap。

**页面动画**：LiveMap ↔ UsbTransfer 使用 **`PAGE_ANIM::NONE`**。

### 5. 地图与存储并发

- `myvendor_mtp_lfs_quiesce()` 为 true 时地图不读 LFS。
- pause 不释放资源；resume 后 quiesce 解除再 render（可 500ms 轮询）。
- 传输期间不动 LiveMap PSRAM 画布与 tile cache。

持久规则见：`.cursor/rules/mtp-bicycle-requirements.mdc`

---

## 已确认 · EPIC GPU 与 UI 卡死

### 现象

- 多次 USB 插拔后，MTP worker 正常（session open/close），但 **传输界面不出现或 UI 假死**。
- 日志常见：`epic_prepare_start` / `_epic_wait: EPIC wait timeout (500 ms)`。
- EPIC 阻塞 UI 线程时，`App_UpdateTransferUi` 无法执行，页面切换失败。

### 根因

- LVGL 使用 SiFli **EPIC** 硬件合成时，与 LiveMap 渲染、页面 `replace` 动画、MTP catalog 初始化叠加，会在 `_epic_wait` 中同步等待 GPU。
- UI 与 LVGL 同线程；GPU 等待期间主循环停滞，表现为卡死。

### 当前对策（已验证有效）

- **关闭 EPIC GPU**，改用软件渲染：
  - `configs/nsh/defconfig`：`# CONFIG_LV_USE_SIFLI_EPIC is not set`
  - 保留 `CONFIG_LV_USE_DRAW_SW=y`
- 关闭后插拔 USB、MTP 页面切换稳定，不再卡死。
- 代价：部分 LVGL 动画/图片合成略慢；矢量地图本身为 CPU 光栅化，影响较小。

### 后续若重新启用 GPU

- [ ] MTP ACTIVE 或 `myvendor_mtp_lfs_quiesce()` 为 true 时，暂停 LiveMap `VectorMapView::render()` / EPIC 绘制（`render()` 入口目前未检查 quiesce，仅 tile 加载有检查）。
- [ ] UsbTransfer ↔ LiveMap 切换使用无动画或 `PAGE_ANIM::NONE`，减轻 EPIC 与页面切换叠加。
- [ ] 评估 `LV_USE_SIFLI_EPIC_DRAW_THREAD`（独立绘制线程），避免阻塞 UI 线程。
- [ ] 在 bench 环境对比 EPIC 开/关下的帧率与 MTP 插拔稳定性后再合入 defconfig。

---

## MTP + Bicycle UI 架构（当前实现）

| 组件 | 职责 |
|------|------|
| `myvendor_mtp_init()` + `transfer_begin()` 各一次 | Worker 自主跑 USB/MTP 协议 |
| `App.cpp` | plug 状态 → **push(UsbTransfer)** / **pop()**；不 `transfer_end` |
| LiveMap | `setCacheEnable(true)`；`pauseForCover` / `resumeAfterAppear` |
| `VectorMapView::pause/resume` | 停 timer/动画，保留 canvas + cache |
| `myvendor_mtp_lfs_quiesce()` | MTP ACTIVE 时 defer LFS I/O |
| `UsbTransfer` | 轮询 `get_activity()` 显示进度；pop 后 unload |

相关文件：

- `include/myvendor_mtp.h`、`src/myvendor_mtp.c`
- `mtp_simple/mtp_simple.c`
- `bicycle/src/App/App.cpp`
- `bicycle/src/App/UI/UsbTransfer/`

---

## 其他待办

- [ ] `CONFIG_MYVENDOR_MTP_SIMPLE_AUTOSTART` 保持关闭（bicycle 侧 `myvendor_mtp_init()` 负责 worker，避免双实例）。
- [ ] GPX 模拟时校时日志已静默（`CONFIG_MYVENDOR_BICYCLE_GNSS_GPX_SIM` + `HAL_Clock.cpp`）。
- [ ] 首次启动无 `SystemSave.json` 时的 storage load 失败为预期，可考虑默认空配置或降低 log 级别。

---

## 变更记录

| 日期 | 说明 |
|------|------|
| 2026-06-21 | 需求定稿：LiveMap 只 pause 不 unload；push/pop；PAGE_ANIM::NONE |
| 2026-06-21 | 关闭 `CONFIG_LV_USE_SIFLI_EPIC`，确认 UI 不再因 GPU 等待卡死 |
| 2026-06-21 | MTP 改为 init/transfer_begin 一次；UI 只负责页面与状态展示 |

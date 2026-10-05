# lvgl_bench

LVGL 官方 **benchmark** 性能测试（`lv_demo_benchmark`），在 SF32LB52 + `/dev/lcd0`（myvendor_lcd_disp 双 strip 缓冲，240×320 RGB565）上运行。

## 用法

```bash
# 先确保 bicycle 等 LVGL 应用未占用（同一进程只能 lv_init 一次）
lvgl_bench
# 或后台
lvgl_bench &
```

运行过程中屏幕顶部显示 FPS / CPU / 渲染耗时；全部场景结束后串口输出 **CSV 汇总**（`Benchmark Summary`）。

无需触屏，自动跑完全部场景（约 90 秒）。运行中**不**刷 sysmon 实时 log；结束后串口搜索 **`Benchmark Summary`** 查看 CSV（需 `LV_USE_PERF_MONITOR=y`，**不要**开 `LOG_MODE`）。

BIN/PNG 文件场景已改为**内嵌 cogwheel 图片**（不依赖 `A:` 文件系统）。

## 对比 EPIC 开关

1. `menuconfig` → `LV_USE_SIFLI_EPIC` 开/关，重新编译烧录  
2. 各跑一遍 `lvgl_bench`，对比串口 CSV 中的 **Avg. FPS**、**render time**

## 配置

| Kconfig | 说明 |
|---------|------|
| `MYVENDOR_LVGL_BENCH` | 编译 NSH 命令 `lvgl_bench` |
| `LV_USE_DEMO_BENCHMARK` | 编入 liblvgl 的 benchmark demo |
| `LV_USE_DEMO_WIDGETS` | benchmark 依赖 |
| `LV_USE_PERF_MONITOR` | 屏幕/日志性能数据 |
| `LV_USE_SIFLI_EPIC` | GPU 加速（canvas/图片合成；不含矢量地图 CPU 光栅化） |
| `LV_OS_PTHREAD` + `LV_USE_SIFLI_EPIC_DRAW_THREAD` | EPIC 独立绘制线程（须先开 OS，默认 EPIC 单线程） |
| `LV_DRAW_SW_DRAW_UNIT_CNT` | >1 时多 SW 绘制单元（须开 OS） |

默认栈 48 KiB；若 OOM 可在 `MYVENDOR_LVGL_BENCH_STACKSIZE` 增大。

### LiveMap 流畅度（menuconfig 建议）

地图底图是 **CPU 软件光栅化**（`VectorMapView::render`），EPIC 只加速 LVGL 把 canvas 贴到屏幕。
若放大后仍觉卡顿，可在 `menuconfig → LVGL` 中：

1. **Operating System** → `LV_OS_PTHREAD`（NuttX pthread）
2. **Rendering** → `LV_USE_SIFLI_EPIC_DRAW_THREAD` = y
3. 可选：`LV_DRAW_SW_DRAW_UNIT_CNT` = 2

然后全量编译烧录，再用 `test cpu` 对比 bicycle CPU 占用。

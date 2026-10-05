# 可直接覆盖的上游文件

树形与 **OpenVela 根目录** 对齐。在根目录：

```bash
R=vendor/my_vendor/docs/pitch/replace
cp $R/external/zblue/zblue/subsys/bluetooth/host/id.c   external/zblue/zblue/subsys/bluetooth/host/id.c
cp $R/frameworks/connectivity/bluetooth/service/stacks/zephyr/hci_h4.c \
   frameworks/connectivity/bluetooth/service/stacks/zephyr/hci_h4.c
cp $R/apps/graphics/lvgl/CMakeLists.txt                 apps/graphics/lvgl/CMakeLists.txt
cp $R/apps/graphics/lvgl/Makefile                       apps/graphics/lvgl/Makefile
```

| 本目录文件 | 覆盖到 | 对应补丁 |
|------------|--------|----------|
| `external/zblue/zblue/subsys/bluetooth/host/id.c` | 打过 `-EACCES` 回退的整份 `id.c` | `zblue-id-scan-eacces.patch` |
| `frameworks/connectivity/bluetooth/service/stacks/zephyr/hci_h4.c` | 含 `h4_rx_reopen()` 的整份 `hci_h4.c` | `vela-hci-h4-rx-reopen.patch` |
| `apps/graphics/lvgl/CMakeLists.txt` | 含 `MYVENDOR_LVGL_STACK` 早退 + EPIC 头 | `apps-lvgl-myvendor-stack.patch` |
| `apps/graphics/lvgl/Makefile` | 含 EPIC `CFLAGS` | 同上 |

四个文件都与本机现状**逐字节相同**（2026-09-19 核对）。

> **`scan.c`（panic 修复）不在这里**，只有补丁 `zblue-scan-stop-null-ctx.patch`。
> 它的改动是两处函数内改动，[zblue-scan-stop-null-ctx.md](../zblue-scan-stop-null-ctx.md)
> 里已给出两段函数的完整改前/改后，照抄即可；要整份文件用
> `cp external/zblue/zblue/subsys/bluetooth/host/scan.c $R/...` 从本机自身拷。

这些是功能补丁后的完整文件，不是最小 diff。上游 HEAD 若已离开 [README.md](../README.md) 里记录的版本，先看 [../patches/](../patches/) 或按 [ble.md](../ble.md) / [ble-hci-h4-rx-reopen.md](../ble-hci-h4-rx-reopen.md) / [lvgl.md](../lvgl.md) 手工改。

不要把本目录拷进 `apps/graphics/lvgl/lvgl/`（官方 LVGL 源码树）。

# 工厂固件：分区跳转，不在 2SFBL 里做 MTP

USB 拷文件走 **独立的工厂 NuttX 镜像**，由 2SFBL msh 命令 `factory` 跳转。  
**2SFBL 不再内置 USB gadget / MTP。**

相关：[boot_2sfbl.md](boot_2sfbl.md)、[sd_partition.md](sd_partition.md) §2.7、
[fs_firmware.md](fs_firmware.md)、
[mtp_simple/README.md](../boards/sf32lb52/my_vendor/services/mtp_simple/README.md)、
[tools/vela_my_vendor_tools.md](tools/vela_my_vendor_tools.md)。

日期：2026-08-27。

---

## 1. 为什么换思路

最初想在 2SFBL 里直接 MTP（`boot_usb.c` + `boot_mtp.c`，KV `/boot` 沙箱），这样停在 msh 就能拷文件、不必进 NuttX。实践上这条路不成立：

| 约束 | 结果 |
|------|------|
| 2SFBL 只有 **160 KiB SRAM** | 放不进 NuttX USB 栈 + `mtp_simple` |
| 自写 HAL PCD gadget | 枚举能过，主机再进目录就卡住（gvfs/资源管理器） |
| bulk IN `txcsr` UNDERRUN、UART 打印堵 USB | 应答丢包、会话冻住；计数器/超时只能缓解，不能当产品路径 |
| 只暴露 KV `/boot` | 工厂真正要拷的是 `/mnt/lfs`（地图、资源），不是 persist 卷 |

NuttX 侧 `mtp_simple` **已经能用**：完整 USBMTP 驱动、lazy catalog、`/mnt/lfs`。  
所以不再修 boot MTP，改成：**boot 只负责跳转，MTP 只在工厂固件里跑。**

`boot_mtp.*` / `boot_usb.*` 已从 2SFBL 删除。msh 没有 `mtp` 命令。

---

## 2. 架构

两份 OVNX 镜像，都链接/运行在 PSRAM `0x10000000`，**同一时刻只跑一个**，工厂镜像不必改链接地址，只换 SD 上的源槽。

```
上电 → Mask ROM → 2SFBL
                      │
          默认 autoboot：persist.boot.target，否则 /fw → main → factory
          10× B/b 留 msh；10× F/f 进工厂
                      │
          ┌───────────┼───────────┐
          │ app / boot            │ factory / F-hotkey
          ▼                       ▼
     /fw 否则 main 否则 factory    factory @ 0x62500000
          │                       │
          └───────────┬───────────┘
                      ▼
         OVNX → PSRAM 0x10000000 → run_img()
                      │
                      ▼
         NuttX：产品 UI / 传感器     或     工厂：MTP 自启 /mnt/lfs
                      │
                      └─ 工厂 MTP（KV 卷）/ BLE OTA 把 <version>.bin 写到 /mnt/kv/fw/
```

| 槽 | 来源 | msh | 固件 |
|----|------|-----|------|
| 产品 | `/mnt/kv/fw/<version>.bin`（最新），否则分区 `main` @ `0x62100000`，再否则 `factory` | `app` / `boot` / 默认 autoboot（可被 `persist.boot.target` 钉住） | `configs/nsh` |
| 工厂 | 分区 `factory` @ `0x62500000` | `factory` / 上电 10×`F` / `target factory` | `configs/nsh-factory` |

工厂槽 **不进 ftab**。2SFBL 用编译进去的 `ptab_find_img("factory")` 取地址，复用已有 `boot_ovnx_load_hcpu()` + `run_img()`。`gen_ftab.py` 不用改。

---

## 3. 分区

`ptab.sdmmc.json`（`mem:"sd"`）：在 `main`（1 MiB 起、4 MiB）之后插入 4 MiB `factory`，coredump 起点下移 4 MiB。**KV（256 MiB）和 FS（512 MiB 起）锚点不动**，已有卡上 persist / `/mnt/lfs` 不受影响。

| 卡偏移 | 大小 | tag / img | 用途 |
|--------|------|-----------|------|
| `0x00100000` | 4 MiB | `HCPU_FLASH_CODE` / `main` | 产品固件 |
| `0x00500000` | 4 MiB | `HCPU_FACTORY_CODE` / `factory` | 工厂固件 |
| `0x00900000` | 128 MiB | `COREDUMP_REGION` | 占位，不是 LittleFS |

`build-boot` 会重生成 `ptab_table.c`（`PTAB_ENTRY_COUNT` 含 factory 一行）。布局细节见 [sd_partition.md](sd_partition.md)。

---

## 4. 工厂固件做什么

独立 defconfig：`boards/sf32lb52/my_vendor/configs/nsh-factory/`。  
以产品 `nsh` 为基线，强制：

- `CONFIG_MYVENDOR_FACTORY_MODE=y`（忽略 persist 关闭 USB MTP / 手机 BLE）
- `CONFIG_MYVENDOR_MTP_SIMPLE_AUTOSTART=y`
- `CONFIG_MYVENDOR_MTP_SIMPLE_STORAGE_PATH="/mnt/lfs"`（全盘；产品是 `/mnt/lfs/mtp`）
- `CONFIG_MYVENDOR_MTP_SIMPLE_EXTRA_PATHS="/mnt/kv,/mnt/fat"`（产品 `nsh` 不设此项）
- `CONFIG_MYVENDOR_BLE_COMPANION_AUTOSTART=y`
- `CONFIG_MYVENDOR_BICYCLE_AUTOSTART=y`（工厂页：编译期字模「工厂模式」，不加载 TTF / LiveMap）

输出目录：`cmake_out/my_vendor_nsh-factory/`。以后裁 UI/LVGL 只改这个变体。

`mtp_simple` 按表驱动多盘：`GetStorageIDs` 返回主键 + `EXTRA_PATHS`（上限 `MTP_STORAGE_MAX`，默认 4）。工厂镜像看到 **file**（`/mnt/lfs`）、**kv**（`/mnt/kv`）、**fat**（`/mnt/fat`，读写，根下 `map/` 与 `fonts/`）。产品固件只挂 `/mnt/lfs/mtp`，FAT 只读挂在 `/mnt/fat` 但不进 MTP。不能把 main/factory/coredump 当 MTP 盘。KV / fat 上 FormatStore 会 `Access Denied`。

---

## 5. 构建与烧录

```bash
python3 vendor/my_vendor/build_board.py build-all          # main + factory + boot
python3 vendor/my_vendor/build_board.py pack-sd-img        # SD 镜像含 factory 槽
python3 vendor/my_vendor/build_board.py flash -p /dev/ttyACM0           # ftab + 2SFBL + main（日常）
python3 vendor/my_vendor/build_board.py flash-all -p /dev/ttyACM0       # 含 factory + fs
python3 vendor/my_vendor/build_board.py flash-factory -p /dev/ttyACM0   # 只写 factory@0x62500000
```

日常 **`flash` 不含** factory，避免没编工厂镜像时迭代产品失败。  
`build-all` / `pack-sd-img` / `flash-all` **含** factory。  
脚本不在 PATH 里，必须 `python3 vendor/my_vendor/build_board.py ...`。

上电默认 **不进工厂**：无 `persist.boot.target` 时先 `/fw`，再 `main`，都失败才 `factory`。msh **`check`** 可在不跳转的情况下 CRC 这三处。要工厂 MTP：

```text
2SFBL msh
autoboot: kv persist.boot.target, else /fw then main then factory
10x B/b = msh, 10x F/f = factory
CHG+KEY1+KEY2 = factory (no kv)
```

连发 **10** 个 `F`/`f` 跳工厂；NuttX 起来后 MTP 自启，主机出现 **file**（LFS）、**kv**、**map** 三个盘。地图拷到 **map 卷根**。把产品 wrap 产出的 **`<version>.bin`** 拷到 **kv 卷的 `fw/`**（保留旧版本作回退），下次开机加载版本号最高的一份。要停在 msh：连发 10 个 `B`/`b`。产品 MTP 只有 `/mnt/lfs/mtp`，看不到 `fw/` 和地图。产品 BLE 文件管理器是 `/mnt/lfs`；OTA 用相对路径 `fw/<file>.bin`。

---

## 6. 以后扩展

- 开机进工厂：上电连发 10 个 `F`/`f`，或 msh 敲 `factory`，或 `target factory` 后复位，或**插电充电时按住 KEY1+KEY2**（不写 KV）。无标记时默认 autoboot 是 `/fw` → `main` → `factory`。
- 工厂镜像裁体积：只改 `configs/nsh-factory/defconfig`。
- 不要把 MTP 加回 2SFBL。
- 量产可去掉分区 `main`，只留工厂槽 + `/mnt/kv/fw/<version>.bin`（≥2 份）；见 [fs_firmware.md](fs_firmware.md)。

---

## 7. 文件索引

| 路径 | 说明 |
|------|------|
| `boot_loader/config/nsh/ptab.sdmmc.json` | `factory` 区定义 |
| `boot_loader/.../board/main.c` | `boot_factory_boot()`、prepare 先读 persist.boot.target 再 `/fw`/分区 |
| `boot_loader/.../board/boot_fw.c` | 产品从 KV `/fw/<version>.bin` 加载最新 |
| `boot_loader/.../board/boot_msh.c` | msh `factory` / `fw` / `check` / `target` / `app` |
| `boot_loader/.../board/boot_target.c` | `persist.boot.target` |
| `boards/.../configs/nsh-factory/defconfig` | 工厂变体 |
| `boards/.../ui/bicycle/src/lvgl_page/factory/` | 工厂页（字模「工厂模式」，不加载 TTF） |
| `vela_my_vendor_tools.py` | `build-all`（含 factory）/ `build-factory` / `flash-factory` / `pack-sd-img` |
| `boards/.../mtp_simple/` | 真正的 USB MTP |

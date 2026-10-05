# SF32LB52 SD 分区布局

SD/eMMC 启动时卡上的分区表、两卷 LittleFS + 一卷 FAT，以及 FAT 余量窗口的做法。
**唯一源**是 `boot_loader/config/nsh/ptab.sdmmc.json`。改布局先改这份 JSON，再同步头文件并重新生成 `ptab_table.{c,h}`。

相关：[build_guide.md](build_guide.md)、[boot_2sfbl.md](boot_2sfbl.md)、
[factory_firmware.md](factory_firmware.md)、[fs_firmware.md](fs_firmware.md)、
[sd_recovery.md](sd_recovery.md)、
[tools/vela_my_vendor_tools.md](tools/vela_my_vendor_tools.md)、
[debug/mklfs.md](debug/mklfs.md)、[debug/mtp_upload.md](debug/mtp_upload.md)、
[nuttx_boot_flow.md](nuttx_boot_flow.md)。

日期：2026-09-03（二级 boot 槽 128 KiB → 160 KiB，从 BOOT_RESERVE 再匀 32 KiB）。
2026-09-15：CMD17/写超时后先 CMD12+CMD13 回 TRAN，DATA 态禁止 CMD8，见 §4.5。
2026-09-16：卡"死掉只能断电恢复"的根因找到并修掉 —— 控制器卡在 `CMD_BUSY`，
靠**模块级 RCC 复位**救回（`HAL_RCC_ResetModule(RCC_MOD_SDMMC1)`）。
**这一部分不在本文**，见 [sd_recovery.md](sd_recovery.md)：判死闸门、`sr=/why=` 取证、
硬复位轮询、开机挂载阶梯 rw→ro→重启、FAT-as-oracle 重挂、`ctl sd read|write`。
另：诊断日志落盘到 `/mnt/kv/diag/`（独立于 `/mnt/kv/coredump/`），见
[diag_log.md](diag_log.md)。

---

## 1. 当前卡布局

卡字节偏移 = SBUS 地址 − `0x62000000`。低 **512 MiB** 是杂项带（boot / firmware / coredump / KV）；用户 LittleFS 固定 **512 MiB**（512 MiB–1 GiB）；FAT 从 **1 GiB** 起到卡末。

```
卡偏移
0        1MiB     5MiB     9MiB              137MiB          256MiB          512MiB     1GiB      卡末
|--------|--------|--------|-----------------|---------------|---------------|----------|---------|
 MBR+ftab+ firmware factory   COREDUMP 128MiB   LOW256_RESERVE  KV 256MiB       LFS 512MiB  FAT
 160KiB BL (main 4M) (MTP 4M)  (raw, 占位)        垫到 256MiB     /mnt/kv         /mnt/lfs    /mnt/fat
 +BOOT_RESERVE                                                                               余量(CSD)
```

| 卡偏移 | 大小 | tag | 挂载 / 用途 |
|--------|------|-----|-------------|
| `0x00000000` | 4 KiB | `MBR` | 占位，给主机工具当磁盘头 |
| `0x00001000` | 64 KiB | `FLASH_TABLE` / `ftab` | 二级 boot 查表 |
| `0x00011000` | 160 KiB | `bootloader` | SRAM 拷贝 `@ 0x20020000`（同长，接到 data RAM `0x20048000`） |
| `0x00039000` | `0xC7000` | `BOOT_RESERVE` | 把 boot 带垫到 1 MiB；main 仍在 1 MiB |
| `0x00100000` | 4 MiB | `HCPU_FLASH_CODE` / `main` | 产品固件槽，SBUS `0x62100000` |
| `0x00500000` | 4 MiB | `HCPU_FACTORY_CODE` / `factory` | 工厂固件槽（MTP 自启），SBUS `0x62500000`；2SFBL `factory` 命令跳转，跑在 PSRAM `0x10000000`（与 main 同址，一次一个） |
| `0x00900000` | 128 MiB | `COREDUMP_REGION` | 原始 crash dump 占位，**不是** LittleFS |
| `0x08900000` | `0x07700000` | `LOW256_RESERVE` | 垫到 256 MiB；不要放用户 LFS |
| **`0x10000000`** | **256 MiB** | **`KV_REGION`** | **`/mnt/kv`**（persist + **`fw/` OTA 槽**） |
| **`0x20000000`** | **512 MiB** | **`FS_REGION`** | **`/mnt/lfs`**（MTP / BLE / GPX / 字体；**不含地图**） |
| **`0x40000000`** | **0 = 余量** | **`FAT_REGION`** | **`/mnt/fat`**（FAT；产品只读，工厂读写；根下 `map/` `fonts/`） |

对应宏（`ptab_sdmmc.h`）：

| 宏 | 值 | 含义 |
|----|----|------|
| `FLASH_BOOT_LOADER_OFFSET` / `SIZE` | `0x11000` / `0x28000` | 卡上 160 KiB 槽 |
| `FLASH_BOOT_LOADER_XIP_*` | `0x20020000` / `0x28000` | SRAM 拷贝目的，与槽同长 |
| `KV_REGION_OFFSET` / `SIZE` | `0x10000000` / `0x10000000` | 卡上 256 MiB 起，长 256 MiB |
| `KV_REGION_START_ADDR` | `0x72000000` | SBUS = `0x62000000 + 0x10000000` |
| `FS_REGION_OFFSET` / `SIZE` | `0x20000000` / `0x20000000` | 512 MiB 起，固定 512 MiB |
| `FS_REGION_START_ADDR` | `0x82000000` | SBUS |
| `FAT_REGION_OFFSET` / `SIZE` | `0x40000000` / `0` | 1 GiB 起；`0` 表示运行时用 CSD 余量 |
| `FAT_REGION_PACK_CARD_MIB` | `14832` | 主机打包：16GB 卡 CSD（FAT ≈ 13808 MiB） |
| `FAT_REGION_START_ADDR` | `0xA2000000` | SBUS |

15.5 GB 卡上 `df -h` 预期：

```
Filesystem      Size      Used  Available Mounted on
littlefs        256M        …         … /mnt/kv
littlefs        512M        …         … /mnt/lfs
vfat             14G        …        14G /mnt/fat
```

KV 与用户 LFS 都小于 4 GiB，`df` 乘法不会 32 位溢出；`/mnt/fat` 大于 4 GiB，见第 5 节。

---

## 2. 设计思路

### 2.1 三卷，互不共用元数据

| 卷 | 设备 | 挂载点 | 谁用 | 总线优先级 |
|----|------|--------|------|------------|
| KV | `/dev/sdkv` | `/mnt/kv` | VELA KVDB、蓝牙 persist（`/mnt/kv/db`）、**`fw/` 产品 OTA**、**`db/persist.boot.target` 开机槽**、**`coredump/` 崩溃短记录** | 高：KV 等待时 MTP/BLE 让出 SD 锁 |
| 用户 FS | `/dev/sd0` | `/mnt/lfs` | GPX / 星历 / 图片；产品 MTP 只暴露 `/mnt/lfs/mtp`；工厂 MTP 全盘；BLE 文件管理器沙箱 | 普通 |
| FAT | `/dev/mtdblock0`（FTL←`/dev/sdfat`） | `/mnt/fat` | `map/` 矢量瓦片、`fonts/` TTF。**产品只读**，**工厂读写** | 普通 |

产品 MTP 根是 `/mnt/lfs/mtp`（开机 mkdir）。工厂 MTP 是 `/mnt/lfs` + `/mnt/kv` + `/mnt/fat` 三卷全盘。BLE 文件管理器仍沙箱 `/mnt/lfs`；OTA 专属路径 `fw/` 落到 `/mnt/kv/fw`。开机跳转标记是 **`/mnt/kv/db/persist.boot.target`**（msh `target` / NuttX `setprop`）。固件槽约定见 [fs_firmware.md](fs_firmware.md)。

主机 `mkfs/` 对应三棵树：`kv/`、`lfs/`、`fat/`，`build-fs` 分别打 `kv_root.bin` / `fs_root.bin` / `fat_root.bin`。FAT 卷根是 `map/` + `fonts/`。

### 2.2 用户 LFS 固定 512 MiB，多出来的卡容量给地图

杂项带故意对齐到 **512 MiB**，用户 LFS 永远是随后 **512 MiB**，FAT 永远从 **1 GiB** 起。加大卡只加长 `/mnt/fat`。

- 主机 `pack-sd-img` 按常见 **16GB 卡 CSD 14832 MiB** 打全国地图种子：FAT = `14832 − 1024 = 13808 MiB`（`custom.SD_PACK_CARD_MIB=14832`）。按 15 GiB 卡打 14 GiB FAT 时，这种卡的窗口只有 ~13.48 GiB，`mount` 会 `EINVAL`。
- 真卡更大时 MTD 窗口是 CSD 余量，但 FAT **不会**像 LittleFS 那样改 superblock 扩容。要吃满余量：按目标卡设 `SD_PACK_CARD_MIB` 再打包，或工厂 `mkfatfs /dev/mtdblock0` 后重新拷图。
- 编译进固件的 `ptab_entry.size` 是 `uint32`，大 FAT 不能写死，所以 `max_size` 仍为 `0`（运行时按 CSD）。
- 不要把地图再放进 `/mnt/lfs`：GPX/`lfs_alloc` 会扫整棵树，上千个 `.vpk` 会卡十几秒。

所以：**改 KV 大小/位置，只要仍停在 512 MiB 之前，已烧过的 `/mnt/lfs` 可以保留。** 地图起点钉在 1 GiB，用户 LFS 大小钉死，加大卡只加地图。

### 2.3 锚点用 256 MiB / 512 MiB / 1 GiB，不用「紧挨上一段」

| 锚点 | 为什么钉死 |
|------|------------|
| firmware `@ 1 MiB`，槽 4 MiB | boot 带用 `BOOT_RESERVE` 垫满 1 MiB；boot 以后变长不必挪 main |
| KV `@ 256 MiB`，长 256 MiB | 和 512 MiB LFS 起点对齐；中间用 `LOW256_RESERVE` 填 |
| LFS `@ 512 MiB`，长 512 MiB | 日常 MTP/GPX 树保持小；`lfs_alloc` 不再扫地图 |
| MAP `@ 1 GiB`，`max_size=0` | 任意 SDHC/SDXC；容量来自 CSD |

`factory` 4 MiB 紧跟 `main`（`0x00500000`），是独立的工厂固件槽（MTP 自启，见 2.7）。`COREDUMP` 128 MiB 再往后（`0x00900000`），仍是 raw 占位（完整 ELF 内存转储不往这里写）。ARM 寄存器组写在 **`/mnt/kv/coredump/`**（见 `CONFIG_MYVENDOR_COREDUMP`），不进 pack；**factory 会打进** `pack-sd-img`。

### 2.7 工厂固件槽（factory）

`factory` 与 `main` 是两份独立的 OVNX 固件，都拷到 PSRAM `0x10000000` 运行、**一次只跑一个**，所以工厂镜像无需改链接地址。2SFBL 停在 msh 时：`app` 跳产品固件（`main`），`factory` 跳工厂固件（`factory`）。boot 通过编译进去的 `ptab_find_img("factory")` 拿到槽地址（`0x62500000`），复用 `boot_ovnx_load_hcpu()` 加载校验后 `run_img()`——不进 ftab，`gen_ftab.py` 不用改。

工厂固件是 `configs/nsh-factory` 变体（MTP 暴露 `/mnt/lfs`、`/mnt/kv`、`/mnt/fat` 三卷；产品只暴露 `/mnt/lfs/mtp`），构建/烧录：

```bash
python3 vendor/my_vendor/build_board.py build-all     # main + factory + boot（+ fs）
python3 vendor/my_vendor/build_board.py pack-sd-img   # 含 factory 槽 @ 卡偏移 5 MiB
python3 vendor/my_vendor/build_board.py flash-factory # 也可只写 factory@0x62500000
```

改工厂固件行为只需改 `configs/nsh-factory/defconfig` 再 `build-factory`（条件编译产出）。设计理由与 boot 不再做 MTP 见 [factory_firmware.md](factory_firmware.md)。

### 2.4 不把 KV 做成 256 MiB 密集体

LittleFS format **只写 superblock（块 0/1）**，不会把 256 MiB 写成 0xFF。空白窗口第一次 `mount(..., "autoformat")` 即可。主机不打 KV 镜像，`burn-sd` 也不写 256–512 MiB。

若以后打包了一个很小的 KV 种子，挂载前走和第 4 节相同的 grow，把 `block_count` 扩到 65536（`256 MiB / 4 KiB`）。

### 2.5 二级 bootloader 槽 160 KiB

`ftab` 的拷贝长度跟 `bootloader.max_size`。镜像拷到 SRAM `0x20020000`，槽长 **160 KiB**，接到 `BOOTLOADER_RAM_DATA`（`0x20048000`，**200 KiB**，到 `0x2007A000`）。多出来的 32 KiB 从 `BOOT_RESERVE` 匀出，boot 带仍垫到 1 MiB，**不必挪 main**。

再往上加（超过 160 KiB）会与 data RAM 重叠：要么把 `BOOTLOADER_RAM_DATA` 再往后挪，要么把更大的 boot 放到 PSRAM。不要只改卡上 `max_size` 而忘了 `hpsys_ram` 里的 `FLASH_BOOT_LOADER`。

### 2.6 不改 `nuttx/fs/littlefs`

NuttX 自带 littlefs 要求 **磁盘 `block_count` == MTD `neraseblocks`**，没有 `lfs_fs_grow`。扩窗口放在 vendor：

- `chips/sf32lb52/sf32lb_lfs_super.c` — 改 **当前更高 rev** 那一侧 metadata pair 的 `block_count` 并重算 CRC
- 板级 CMake 抽换 `fs_procfs_mount.c` 做 `df -h` 的 64 位乘法（第 5 节）

禁止直接改 `nuttx/fs/littlefs/lfs.c` / `lfs_vfs.c`。

---

## 3. 本次改动（256 KiB → 256 MiB KV）

此前 KV 紧跟 firmware：`offset=0x00500000`，`max_size=0x00040000`（**256 KiB**）。`df -h` 打出 `256K` 就是这张表，不是驱动算错。

目标：**容量 256 MiB，并且从卡上 256 MiB 开始**，LFS 仍从 512 MiB 起。

| | 旧 | 新 |
|--|----|----|
| KV 偏移 | `0x00500000`（5 MiB） | `0x10000000`（256 MiB） |
| KV 大小 | `0x00040000`（256 KiB） | `0x10000000`（256 MiB） |
| COREDUMP | KV 之后 `0x00540000` | firmware 之后 `0x00500000` |
| 垫片 | `LOW512_RESERVE` 垫到 512 MiB | `LOW256_RESERVE` 垫到 256 MiB |
| FS_REGION | `0x20000000`，size 0 | **不变** |

改过的文件：

- `boot_loader/config/nsh/ptab.sdmmc.json`
- `boards/sf32lb52/my_vendor/include/ptab_sdmmc.h`
- `boot_loader/include/ptab_sdmmc.h`
- 生成：`boards/.../bsp/ptab_table.c`、`include/ptab_table.h`（app）
- 生成：`boot_loader/project/butterflmicro/board/ptab_table.{c,h}`（二级 boot）
- `chips/sf32lb52/sf32lb_sdio.c` — KV 挂载前同样 `sf32lb_lfs_grow_super`
- `scripts/pack_sd_img.py` — 注释与布局一致

运行时 `sifli_ap.c` 用 `ptab_find_tag("KV_REGION")`，固件里的表即窗口。UART 只烧新 `nuttx.flash.bin` 就会改 KV 位置；**不必**为这次改动重打 `/mnt/lfs`。

旧卡上 5 MiB 处那 256 KiB persist（蓝牙配对等）**不会**搬到新窗口。第一次启动会在 256 MiB 处 `autoformat` 一块空的 `/mnt/kv`。

---

## 4. 运行时：余量窗口

### 4.1 `FAT_REGION max_size = 0`

`sf32lb_sd_clamp_window()`：size 为 `0` 或 `0xffffffff` 时，窗口 = `CSD 容量 − 该区偏移`，再向下对齐到 erase（4 KiB）。15.5 GB 卡大约：

```
off=0x40000000  size=13958643712  (= FAT 从 1 GiB 到卡末)
```

KV 窗口写死 256 MiB，用户 LFS 写死 512 MiB，都不走余量。

用户 LFS 窗口已钉死 512 MiB。磁盘 `block_count` 若仍是旧余量（约 14 GiB / 3665920 块），开机把 superblock **缩回** 512 MiB，不 format。LFS 挂失败不再中断 KV/FAT。

FAT 没有 LittleFS 那种 cheap `block_count` 补丁。产品只读挂载已有 FAT；空白窗口请在工厂 `mkfatfs /dev/mtdblock0` 或按目标卡 `SD_PACK_CARD_MIB` 打包。不要在产品固件里自动 mkfatfs（会把地图抹掉）。

KV 空白切片仍 `autoformat`。用户 LFS **从不** autoformat。

### 4.3 几何

SD 对 LittleFS 报 **page 512 / erase 4096**（不是 NAND 的 2048/128KiB）。原因见 [debug/mklfs.md](debug/mklfs.md)。FAT 经 FTL 看到同样的 512 B 扇区。

### 4.4 新镜像第一次 `mkdir` 与 IWDT

挂载只扫 metadata tail（日志 `lfs_mount took ~2 s`），还没分配新块。
`bootinfo` 之后 `bringup_gpx_dirs()` 会 `mkdir /mnt/lfs/mtp`。

**第一次**给该卷分配目录块时，`lfs_alloc()` 从 0 滑动 lookahead，每窗都
`lfs_fs_traverse` 已有文件。镜像里文件越多（字体、地图 cell）越慢；init 连续
bread 不让出 CPU → idle 心跳过期 → `iwdg_feed` 停喂 → 约 15 s 复位。日志停在
`bootinfo:` 下一行没有 `INFO: mkdir /mnt/lfs/mtp`。

处理：

- SD MTD `bread`/`bwrite`/`erase` 结束后只打 `work_beat`（时间戳，**不** `usleep`）。
  第一次 `lfs_alloc` 扫树时 IWDT 仍喂；平时读地图/MTP 不会每秒卡 10 ms。
  `busy_pump` 只留在 bringup 步进和 UI 分块加载。
- `bringup_mkdir` 打 `INFO: mkdir PATH ...` 和完成耗时。
- 下次 `pack-sd-img` 把 `mkfs/mtp/import`、`mkfs/mtp/record` 打进种子，开机
  `mkdir` 走 `EEXIST`。MTP 之后新建文件夹仍会 alloc，靠 SD 进度心跳撑过。

不要在加载路径里伪造 **UI** 心跳。

### 4.5 读/写超时后的恢复

LittleFS / FAT 一次 `CMD17` 超时，卡往往还停在 **DATA**。旧逻辑立刻 `CMD0`/`CMD8`
全量识别：CMD8 在数据相非法，控制器和卡对不上，日志变成 1 Hz `re-identify`，
`/mnt/lfs` 整卷僵死。

现行 `chips/sf32lb52/sf32lb_sdio.c`：

1. 停 DMA，清 `DCR`/`IER`，等 DAT0。
2. **CMD12** `STOP_TRANSMISSION`（CMD17 超时也要发，不只是多块）。
3. **CMD13** `SEND_STATUS`。回到 **TRAN** 就重试，不要重新 identify。
4. 仍在 DATA/RCV：再 CMD12，**不要**发 CMD8。
5. 只有 R1 已经是 **IDLE**（`-ENOTCONN`）才允许 CMD0/CMD8。

`sd1_reinit_card()` 1 秒内最多尝试一次全量识别。细节与日志见
[debug/mklfs.md](debug/mklfs.md) 2026-09-15 补记。

---

## 5. `df -h` 与 4 GiB 溢出

NuttX 上游 `fs_procfs_mount.c` 用 `uint32_t` 做 `f_bsize * f_blocks`。  
`3665920 × 4096 = 15015608320`，模 `2^32` = `2130706432` = **2032 MiB**，所以大卡曾显示 `2032M` 而不是 `14G`。溢出从窗口 ≥ 4 GiB 开始。

处理方式与 `vela_override/bluetooth/` 相同：CMake 从 lib `fs` 去掉上游 `.c`，改用板级文件：

- `boards/sf32lb52/my_vendor/vela_override/fs/fs_procfs_mount.c` — `uint64` 乘法
- `boards/sf32lb52/my_vendor/vela_override/CMakeLists.txt` — `myvendor_patch_target_by_name(fs, …)`

配置日志应有：`my_vendor: fs uses board fs_procfs_mount.c`。

不要改 `nuttx/fs/mount/fs_procfs_mount.c`。

`pack_fat_root.py` 按树自动选簇：能装下就尽量用大簇（FAT 表更小）；全国图几十万个小 `.vpk` 时 32 KiB 簇会把 14 GiB 卷撑爆（每文件至少一簇），会落到 8/4 KiB。旧的 1 GiB / 32 KiB 种子簇数约 32758，曾落在 NuttX 上游的 FAT16 区间。上游会按 2 字节 FAT 项走链，`x3443_y858.vpk` 这类跨簇文件 `read()` 报 `errno=5`。板级抽换 `vela_override/fs/fs_fat32util.c`：BPB `FatSz16==0` 一律 FAT32。**只刷固件，不用重烧卡。** 配置日志应有：`my_vendor: fs uses board fs_fat32util.c`。不要改 `nuttx/fs/fat/fs_fat32util.c`。

---

## 6. 主机打包与烧卡

```bash
python3 vendor/my_vendor/build_board.py pack-sd-img
python3 vendor/my_vendor/build_board.py burn-sd --sd /dev/sdX
```

| 步骤 | 行为 |
|------|------|
| pack | 前 **9 MiB** 填 `0xFF`，再叠 ftab / bootloader / main / factory（避免 GNU `dd conv=sparse` 跳过文件里的 NUL） |
| pack LFS | 从 512 MiB 起写入固定 512 MiB LittleFS 种子；镜像在最后一个有数据的字节处截断（稀疏） |
| pack KV | **不写**。256–512 MiB 在镜像里是洞 |
| burn 1 | 密写前 9 MiB（`conv=fsync`，不用 sparse） |
| burn 2 | 从 512 MiB `skip/seek` sparse 写 LFS |

只改固件、KV 位置变了：UART `flash` 新 `nuttx.flash.bin` 即可。只改 `/mnt/lfs` 内容才需要 `pack-sd-img` / `burn-sd` 的第二段。

LittleFS 主机工具版本：**v2.5.1**（与设备端一致）。

---

## 7. 改表时要动哪些文件

1. 编辑 `boot_loader/config/nsh/ptab.sdmmc.json`。
2. 同步两份 `ptab_sdmmc.h`（app + boot_loader），宏与 JSON 一致。
3. 生成表：

```bash
# app
python3 vendor/my_vendor/boot_loader/scripts/gen_ptab_table.py \
  --ptab vendor/my_vendor/boot_loader/config/nsh/ptab.sdmmc.json \
  --storage sd \
  --out-c vendor/my_vendor/boards/sf32lb52/my_vendor/bsp/ptab_table.c \
  --out-h vendor/my_vendor/boards/sf32lb52/my_vendor/include/ptab_table.h

# 二级 boot（build-boot 也会跑）
python3 vendor/my_vendor/boot_loader/scripts/gen_ptab_table.py \
  --ptab vendor/my_vendor/boot_loader/config/nsh/ptab.sdmmc.json \
  --storage sd \
  --out-c vendor/my_vendor/boot_loader/project/butterflmicro/board/ptab_table.c \
  --out-h vendor/my_vendor/boot_loader/project/butterflmicro/board/ptab_table.h
```

`build` 会经 `sync_ptab_table()` 刷新 app 侧；`boot_loader/build.sh` 刷新 boot 侧。NSH `ptab` 打印的是这张编译进去的表。

介质由 `boot_loader/storage.conf` 的 `BOOT_STORAGE=sd` 选择；`ptab.h` 据此包含 `ptab_sdmmc.h` 或 `ptab_nand.h`。

---

## 8. 不要做的事

- 不要只加大卡上 bootloader `max_size` 而不改 SRAM `FLASH_BOOT_LOADER`（二者必须同长）。超过 160 KiB 会碰到 `BOOTLOADER_RAM_DATA`。
- 不要把用户 LFS 放进 512 MiB 以下（那是杂项带）。
- 不要把 KV 和 MTP/BLE 文件放进同一个 LittleFS。
- 不要打一份 256 MiB 的密集体 KV，也不要 `dd` 整段 KV。
- 不要为了「卡变大」去改 NuttX littlefs；用 vendor grow。
- 不要 shrink：磁盘 `block_count` 大于 MTD 窗口会拒绝挂载。
- 不要在未确认的情况下把 `FS_REGION` 移出 512 MiB——已烧的 LFS 种子会全部对不上。
- 不要在卡仍处于 DATA/RCV 时发 CMD8 做全量识别；先 CMD12+CMD13 回到 TRAN。

---

## 9. 源文件索引

| 路径 | 职责 |
|------|------|
| `boot_loader/config/nsh/ptab.sdmmc.json` | 布局源 |
| `boards/.../include/ptab_sdmmc.h`、`boot_loader/include/ptab_sdmmc.h` | 编译期偏移/大小宏 |
| `boards/.../bsp/ptab_table.c`、boot 侧同名 | 运行时 `ptab_find_tag` |
| `boards/.../bsp/bringup.c` | 按 tag 挂 `/mnt/lfs`、`/mnt/kv`，mkdir `/mnt/kv/fw` |
| `boot_loader/.../board/boot_fw.c` | 2SFBL 从 KV `/fw/<version>.bin` 加载最新 OVNX |
| `chips/sf32lb52/sf32lb_sdio.c` | SD MTD 窗口、mount、KV 优先锁；CMD12+CMD13 恢复，DATA 态禁 CMD8 |
| `chips/sf32lb52/sf32lb_lfs_super.c` | 挂载前改 `block_count` |
| `boards/.../vela_override/fs/fs_procfs_mount.c` | `df -h` uint64 |
| `scripts/pack_sd_img.py` | 整盘 `.img` |
| `docs/tools/vela_my_vendor_tools.py` | `pack-sd-img` / `burn-sd` |

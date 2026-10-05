# mtp_simple

SF32LB52 板载 **轻量 USB MTP 应答器**：将配置路径暴露给 Windows 资源管理器。产品固件默认只挂 `/mnt/lfs/mtp`；工厂固件挂 `/mnt/lfs`、`/mnt/kv`、`/mnt/fat` 三卷。单线程 poll 循环，无 GLib / SysV 消息队列。

替代 `external/mtp-responder` 全量方案，Flash 与 RAM 占用更小。

## 架构

```
Windows Explorer / libmtp
        │  USB bulk + interrupt + EP0 (PTP class)
        ▼
/dev/mtp/ep0 | ep1(IN) | ep2(OUT) | ep3(INT)     ← NuttX USBMTP 驱动
        │
        ▼
mtp_simple_main()
  └─ poll 循环
       └─ dispatch_command()
            ├─ PTP 数据集打包 (GetDeviceInfo / ObjectInfo …)
            ├─ catalog（LFS 扫描 ↔ object handle）
            └─ 可选 opcode（见 mtp_features.h）
        ▼
LittleFS volume(s): STORAGE_PATH + EXTRA_PATHS (mtp_storage.c)
```

`mtp_simple.c` 内按 `§1`–`§15` 分区，可用编辑器搜索 `§` 快速跳转。

## 源文件

| 文件 | 作用 |
|------|------|
| `mtp_simple.c` | 主逻辑：USB 传输、catalog、命令分发 |
| `mtp_simple.h` | PTP/MTP 常量、容器结构 |
| `mtp_features.h` | **可选功能开关**（`#define 0/1`），含资源占用表 |
| `mtp_scratch.h` | PSRAM scratch 池 + 目录遍历 walk 栈 |
| `mtp_psram.c/h` | PSRAM 尾部 bump 分配器 |
| `mtp_names.c/h` | GetDeviceInfo / GetStorageInfo 字符串（主盘默认文案） |
| `mtp_storage.c/h` | 表驱动多盘：主键 + `EXTRA_PATHS`，友好名 / MTD 节点在 `g_known[]` |
| `CMakeLists.txt` | NuttX application 注册 |

## 启用与编译

Kconfig 路径：`MYVENDOR_MTP_BACKEND` → **Simple MTP (mtp_simple)**。

典型 `defconfig` 项：

```kconfig
CONFIG_USBMTP=y
CONFIG_MYVENDOR_MTP_SIMPLE=y
CONFIG_MYVENDOR_MTP_SIMPLE_AUTOSTART=y          # 开机自动起任务
CONFIG_MYVENDOR_PRODUCT_NAME="Helm One"
CONFIG_MYVENDOR_PRODUCT_VERSION="1.0.0"
CONFIG_MYVENDOR_MANUFACTURER="Helm"
CONFIG_USBMTP_PRODUCTSTR="Helm One"            # 须与 PRODUCT_NAME 同步
CONFIG_USBMTP_VENDORSTR="Helm"                 # 须与 MANUFACTURER 同步
CONFIG_MYVENDOR_MTP_SIMPLE_STORAGE_PATH="/mnt/lfs/mtp"
CONFIG_MYVENDOR_MTP_SIMPLE_STORAGE_DESC="Helm One"
CONFIG_MYVENDOR_MTP_SIMPLE_VOLUME_LABEL="Helm One"
# 产品固件不要设 EXTRA_PATHS，persist 与 map/fonts 对主机不可见。
# 工厂固件：STORAGE_PATH="/mnt/lfs" 且 EXTRA_PATHS="/mnt/kv,/mnt/fat"（三卷）
CONFIG_MYVENDOR_MTP_SIMPLE_STACKSIZE=4096
CONFIG_MYVENDOR_MTP_PSRAM_RESERVE_KB=192
```

产品名 / 版本 / 厂商只改 Kconfig `CONFIG_MYVENDOR_PRODUCT_*`。USB iProduct、MTP 型号和产品盘卷标在开机后都会变成 **`Helm One-XXXX`**（与 BLE 广播名相同，XXXX 为 MAC 后两字节）。工厂固件三个盘在主机上叫 **file**（`/mnt/lfs`）、**kv**（`/mnt/kv`）和 **fat**（`/mnt/fat`）。

工厂拷文件：**不要**在 2SFBL 里开 MTP。msh `factory`（调试 boot 会自动跳）进 `nsh-factory`：MTP 挂 **全盘** `/mnt/lfs`、`/mnt/kv` 和 `/mnt/fat`。地图拷到 **fat 卷的 `map/`**（`lon*/…`），字体拷到 **`fonts/`**。把产品 `<version>.bin` 放到 **KV 卷的 `fw/`**（见 [fs_firmware.md](../../../../docs/fs_firmware.md)）。产品 `nsh` 只挂 `/mnt/lfs/mtp`，**看不到** 固件槽和地图。再加盘：Kconfig `EXTRA_PATHS` 追加路径，必要时在 `mtp_storage.c` `g_known[]` 补一行。思路见 [docs/factory_firmware.md](../../../../docs/factory_firmware.md)。

## 配置分工

| 改什么 | 改哪里 |
|--------|--------|
| 栈大小、优先级、主盘路径、额外盘（`EXTRA_PATHS`）、产品名/版本、PSRAM 预留、是否 autostart | **Kconfig / defconfig** |
| 新挂载点的盘符名 / FormatStore 是否允许 / MTD 容量节点 | **`mtp_storage.c` `g_known[]`** |
| Move/Copy/文件夹/FormatStore 等 opcode | **`mtp_features.h`** |
| 目录浏览模式（lazy vs 全树扫描）、可选隐藏目录 | **`mtp_features.h`** → `MTP_FEAT_LAZY_CATALOG`、`MTP_CATALOG_SKIP_ENABLE` |
| 目录最大嵌套层数 | **`mtp_scratch.h`** → `MTP_WALK_MAX_DEPTH` |
| 最大对象数、路径长度 | **`mtp_simple.h`** → `mtp_max_objects()`（由 PSRAM 尾预留自动推算）等 |

> 可选 MTP 功能不再走 Kconfig，只编辑 `mtp_features.h` 后重新编译即可。

## 内存模型

### SRAM（任务栈 + 少量 .bss）

- 任务栈默认 **4096 B**（`MYVENDOR_MTP_SIMPLE_STACKSIZE`）。
- 大缓冲已迁到 PSRAM；handler 栈峰值约数百字节。
- 目录 delete/copy/rescan 使用 **PSRAM walk 栈**，深度不再消耗任务栈。

### PSRAM（尾部保留区）

由 `mtp_psram.c` 在 `PSRAM_DATA` 末尾 bump 分配（与 `sifli_allocateheap.c` 中扣减的 `MYVENDOR_MTP_PSRAM_RESERVE_KB` 一致，默认 192 KiB）：

| 缓冲 | 约大小 | 说明 |
|------|--------|------|
| `catalog[]` | 按 arena 自动 | 内存中对象表（`mtp_max_objects()` 槽位） |
| `g_catalog_ph_snap[]` | rescan 快照 | 保持 handle 稳定 |
| `g_io_buf` | 4096 B | 上传/下载/PartialObject 流式 I/O |
| `g_handles_buf` | ~400 B | GetObjectHandles 列表 |
| `g_mtp_scratch` | ~16 KiB | 路径 scratch、PTP 数据集、UTF-16、walk 栈 |

`mtp_scratch.h` 槽位：

- `path[0]` → 进行中上传路径（`MTP_UPLOAD_PATH`）
- `path[1..3]` → Move/Copy/Rename 临时路径
- `data512/256/128/64` → GetDeviceInfo 等数据集
- `walk[24]` → 迭代式删目录 / 复制树 / catalog 扫描

## 功能裁剪

编辑 `mtp_features.h` 中 `MTP_FEAT_*` 宏。文件顶部表格列出各开关对 Flash / SRAM / PSRAM 的大致影响。

当前默认（可按需改）：

- 文件夹新建/删、Move、Copy、Rename、FormatStore
- ObjectPropList（文件名 + 大小；不报日期）
- GetPartialObject（Windows 就地打开）
- GetThumb（空缩略图容器）

关闭示例：若只需浏览 + 上传/下载，可将 `MTP_FEAT_COPY_OBJECT`、`MTP_FEAT_FORMAT_STORE` 等设为 `0` 以减 Flash。

## 能力与限制

| 项 | 值 |
|----|-----|
| 最大 catalog 对象数 | 由 `MYVENDOR_MTP_PSRAM_RESERVE_KB` 自动推算（256 KiB 约 350+）；lazy 模式下仅缓存已浏览目录 |
| 路径 / 文件名 | 256 / 128 字符（受 NuttX `CONFIG_NAME_MAX` 约束，板级默认 128） |
| 单文件大小 | 理论约 4 GiB（流式 4 KiB，无断点续传） |
| 目录嵌套 | ≤ `MTP_WALK_MAX_DEPTH`（默认 24），超限返回错误 |
| 并发 | 单线程；同时只处理一条 host 命令 |

### 大量小文件（如 vector map 瓦片）

默认 **`MTP_FEAT_LAZY_CATALOG=1`**：OpenSession 只列出存储根目录一层；Explorer **展开哪一层才扫描哪一层**。**返回父目录**（如 `test/sub1` → `test/`）时，会释放子文件夹在 catalog 里占用的槽位，父目录可重新加载更多条目。

`/mnt/fat` 只在 **工厂固件** 里作为独立 MTP 盘对主机可见（读写）。产品固件 MTP 根是 `/mnt/lfs/mtp`，FAT 只读挂在 `/mnt/fat`，不出现在资源管理器。需要再隐藏某子目录时设 `MTP_CATALOG_SKIP_ENABLE=1`。

瓦片目录建议保持 **`map/z/x/*.vmap`** 分层（每层条目数少），避免单个文件夹内条目数接近 catalog 上限（见启动日志 `catalog cap`）。

若需恢复 OpenSession 全树扫描，设 `MTP_FEAT_FULL_CATALOG_SCAN=1` 并关闭 `MTP_FEAT_LAZY_CATALOG`。

## Windows 兼容性说明

- **UTF-8 ↔ UTF-16LE** 文件名转换；ObjectInfo 支持 Win64 偏移 52/56。
- **新建文件夹**：`SendObjectInfo` 后立即 `mkdir` 并发布 catalog（Windows 常不发空 `SendObject`）。
- **新建空文件**（如文本文档）：同样在 `SendObjectInfo` 后发 `ObjectAdded`；若主机仍发空 `SendObject` 则可选 drain。
- **GetPartialObject**：大文件就地打开依赖 `MTP_FEAT_GET_PARTIAL_OBJECT`。
- 不报文件日期。LittleFS 没有可靠 POSIX mtime；ObjectInfo 日期串为空，PropList 不含 DateCreated/DateModified。

## Ubuntu / gvfs 兼容性说明

Nautilus（gvfs + libmtp）和资源管理器走的协议路径不同，多选拷贝更容易踩这些点：

- **ObjectSize**：gvfs 用 `GetObjectPropList` / `GetObjectPropValue`，不用 `GetObjectInfo`。懒 catalog 列举时 size=0，会被当成空文件而跳过 `GetObject`。应答器在属性查询时补 `stat()`。文件夹报 `0xFFFFFFFF`（MTP「未定义」）时，Nautilus 会当成真实体积：每个目录约 4 GiB，工厂两盘合计约 **8.6 GB**，进度条瞬间跑完。文件夹改为报 0。
- **文件名缓冲**：属性值按 UTF-16 打包，超过约 23 字符的名字原先会 `GEN_ERROR`，多选里只要有一个长文件名整次拷贝失败。
- **GetObjectPropList(0xFFFFFFFF)**：libmtp 启动时会拉全部对象属性。只回 `INVALID_OBJ_HANDLE`（无 DATA）会把 bulk 会话打乱，随后「无法多选 / 无法列出」。现在始终带 DATA 阶段。
- **ObjectAdded**：`SendObject` 成功后不再发中断事件。gvfs 的事件线程和传输线程共用 libmtp，中断 IN 会和下一个文件的 bulk 命令打架。Windows「新建空文件/文件夹」仍在 `SendObjectInfo` 后发 `ObjectAdded`。
- **CopyObject**：跨目录复制保留原名（仅重名时加 `_copy`），响应带新 handle。
- **上传（2026-09-07）**：USB 先收完再回 `0x100d`（≤2 MiB）；LittleFS 后台写，列目录不要去 `opendir` 抢锁。详见 [docs/debug/mtp_upload.md](../../../../docs/debug/mtp_upload.md)。

## 已知异常（先查镜像和 SD，再查 MTP）

MTP 能枚举、主机能看见设备，**不等于**协议或 USB 有问题。2026-08-23 实测：
文件夹/文件「打不开」是 **SD 上的 LittleFS 仍是旧几何镜像**，重打包刻录后即恢复。
镜像几何与打包命令见 [docs/debug/mklfs.md](../../../../docs/debug/mklfs.md) 第 8 节。

### 设备可见但文件夹打不开 / 极慢

优先核对整盘镜像，不要先改 MTP：

| 项 | 错误（旧） | 正确（当前 SD） |
|----|------------|-----------------|
| `my_vendor_sd.img` | ~33–34 MiB（2026-08-11） | ~9.7–10 MiB |
| LittleFS 几何 | read/prog=2048，block=131072（NAND） | **512 / 4096**，blocks=1047040 |
| 典型内容 | 同一棵 `mkfs/`（约 610 个小文件） | 同左，元数据不再按 128 KiB 块放大 |

`mkfs/vmap` 这种几百个小瓦片在 128 KiB 块上每个文件独占一块，MTP 展开目录等于扫
放大后的日志，看起来像协议卡死。用当前配置：

```bash
./vela_my_vendor_tools.py build-fs
./vela_my_vendor_tools.py pack-sd-img
./vela_my_vendor_tools.py burn-sd --sd /dev/sdX --yes
```

验证时 **断开 MTP**，在 NSH：

```text
nsh> test layers
```

不要跑 MTP，不要对 `/mnt/lfs` 做 `statfs`。挂载 EINVAL：`test lfs format -y`（清卷）。

### GetStorageInfo 禁止 statvfs

LittleFS 的 `statfs`/`statvfs` 会走 `lfs_fs_size()`，把**每个已用块**走一遍。
空盘还行，产品树（瓦片/字体）上会卡数秒到挂死，EP0 `GET_DEVICE_STATUS` 超时，
主机以为设备掉了。`GetStorageInfo` 容量来自 `MTDIOC_GEOMETRY`，空闲按窗口报满
（乐观值）。不要改回去。

### 上传慢、下载还可以

分层看，不要只改 `g_io_buf`。完整记录：[docs/debug/mtp_upload.md](../../../../docs/debug/mtp_upload.md)。

- **读 / 下载**：CMD17/CMD18 + DMA（DMAC1 CH3 / REQUEST_57）。裸读约 3 MB/s；经
  LittleFS 约 1 MB/s。USB FS 实务大约 1 MB/s，下载往往先碰到 USB。
- **写 / 上传**：对齐官方 `drv_sdio.c`——DMA CMD24/CMD25，**CMD 应答后再
  `DATA_START`**。失败回退 PIO CMD24。成功后保持/恢复 **24 MHz**，不要 stay 在 6 MHz。
- **MTP**：≤2 MiB 先收完 USB 再回 PTP，LFS 后台刷；刷盘中 catalog 报 ObjectInfo
  大小，且不要 `opendir`（LittleFS 一把锁）。>2 MiB 仍 USB+LFS 分块，`0x100d` 等写完。
- **仍慢的部分**：packed 14 GiB 上**新建**文件第一刀 `lfs_alloc` 可达 ~24 s
  （`test lfs` 写已有文件很快，不能对照）。刷盘未完成不要拔线。

POSIX/LFS 卡住时 MTP 同样答不了 EP0——这是存储路径堵住协议，不是 catalog 算法。

## 调试

- 常规 INFO 默认关：`MTP_FEAT_INFO`（`mtp_features.h`）为 `0`，调用点保留。
  调试上传时改为 `1`（`mtp_info`、`myvendor_mtp:` INFO、PSRAM arena 行）。
- ERR/WARNING 恒开：`mtp_err` / `LOG_ERR`。
- 详细 trace：将 `MTP_FEAT_VERBOSE` 设为 `1`（会镜像到 `/dev/console`，可能与 NSH 串口混用）。
- SD 计数：`SDDIAG` / `test sdio`（`cmd18_ok` / `cmd25_ok` 上传时应增加）。
- 上传时序与 gvfs 超时：[docs/debug/mtp_upload.md](../../../../docs/debug/mtp_upload.md)。

## 与 full mtp-responder 对比

| | mtp_simple | mtp-responder (full) |
|--|------------|----------------------|
| 依赖 | NuttX + LFS | GLib、较大中间层 |
| 线程 | 单 poll 线程 | 多组件 |
| RAM | PSRAM 大缓冲 + 4K 栈 | 显著更大 |
| 适用 | 手表 / 嵌入式 LFS | 功能完整性优先 |

Kconfig 选 `MYVENDOR_MTP_RESPONDER_FULL` 可切回 legacy 方案。

## 相关代码位置（板级）

- USB 设备节点：板级抽换 `vela_override/usbdev/mtp.c`（上游 `nuttx/drivers/usbdev/mtp.c` 不改）
- 板级 USB 注册：`vendor/.../sf32lb52/sf32lb_usbdev.c`
- 任务启动：`vendor/.../my_vendor/bsp/bringup.c`（`CONFIG_MYVENDOR_MTP_SIMPLE_AUTOSTART`）

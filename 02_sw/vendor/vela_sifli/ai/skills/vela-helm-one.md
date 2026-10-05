# Helm One 板级助手

在 openvela + SF32LB52 的 Helm One 骑行码表上做板级开发、地图工程、双端协议或现场排障时的板级知识库：给出分区/引脚/阈值等权威值、必须遵守的约束，以及常见故障（SD 假死、开机复位、静止假速度、蓝牙双角色）的根因与处置步骤。

## When to use

- 用户提到 Helm One / 骑行码表 / my_vendor，或要在这块板子上改驱动、改地图、改协议。
- 出现这些现象时：开机约 15 秒反复复位、SD 卡整卷假死或 1 Hz 反复识别、GNSS 静默无定位、蓝牙连上就断或开机自启 hardfault、界面卡顿、看门狗误复位。
- 要改这些数值时：坐标吸附半径、偏航/到达距离、自动暂停秒数、轨迹折线点数、地图网格与缩放级别。

## How to use

1. **先查权威值，再动手。** 阈值/上限/偏移只在两处为准：设备上的 `docs/system_states.md`（状态、阈值、文案口径）与 `docs/sd_partition.md`（卡分区）。不要凭常识写数字。
2. **用 run_shell 取证，不要猜。** 常用命令：`ptab`（打印编译进固件的分区表）、`bootinfo`、`df -h`（分区容量）、`free`、`ps`。SD 相关再看日志里的 `sr=` / `why=` 字段。
3. **按配方处置故障。** 见下方「故障速查」；配方之外的情况，先 `read_file` 对应文档再动手。
4. **改一处必须同步一处。** 改阈值先改 `docs/system_states.md`；改分区先改 `boot_loader/config/nsh/ptab.sdmmc.json` 并重新生成两份 `ptab_table.{c,h}`；改双端协议要同时改固件 `companion_proto.h` 与 App 的 `companion_proto.dart`。
5. **不许绕过既有抽象。** 不改上游 `nuttx/`（板级差异走 `boards/sf32lb52/my_vendor/vela_override/`），不改生成产物，不改 `logs/` 下的 AI Coding 日志。
6. **收尾留痕。** 解决一个非平凡问题后，把根因与处置写进对应 `docs/*.md`。
7. **串口 / monitor 保活（硬约束）。** 用户常在另一终端跑
   `./vela_my_vendor_tools.py monitor -p /dev/ttyACM0`（或 `--follow`）。
   烧录只用工具自带的 `release`/`acquire`；**禁止** `pkill`/`kill` 任何
   `monitor`/`serial_hub`，**禁止** `monitor stop` / `{"op":"shutdown"}` /
   删或抢断 `/tmp/vela-serial-*.lease`。
   Agent 抓日志用 `monitor --follow --no-stdin`，不要另起会抢归属的直连
   （`--no-broker`）进程。

## 权威值（速查）

- 卡分区（唯一源 `ptab.sdmmc.json`）：boot 160 KiB @0x11000；main 4 MiB @1 MiB；factory 4 MiB @5 MiB；KV 256 MiB @256 MiB（`/mnt/kv`，含 OTA 槽 `fw/`）；用户 LittleFS 512 MiB @512 MiB（`/mnt/lfs`）；FAT 从 1 GiB 到卡末（`/mnt/fat`，**地图与字体在这里，产品只读**）。
- 屏幕：2.4 英寸 NV3031A 240×320 RGB565，**无触摸**，只有 KEY1/KEY2（另有电源键）。
- 接口：console `/dev/console`（UART1 PA18/19）；日志 `/dev/ttyS0`（UART2 PA20/27）；I2C1 PA30/33（`/dev/i2c0`）；SDIO PA12–PA17（与 MPI2 互斥）；USB DP/DM PA35/36。
- 阈值：自动暂停 4 s / 恢复 1.5 s；停表 1.8 km/h / 恢复 2.5 km/h；偏航 35 m 重规划、45 m 判到达；导航折线上限 2048 点（**超了整条路线被拒，不截断**）；屏幕轨迹 384 顶点；自动计圈 ≥2 km 且回起点 50 m 内。
- 地图：3 km 固定网格，`lon<ix/4>/lat<iy/4>/x<ix>_y<iy>.vpk`，无顶层 `map.idx`；跨区读 vpk 尾部 VPOR；缩放 11–15。
- 蓝牙：对手机 GATT Server（自定义服务 0xFF10，12 条特征 0xFF11–0xFF1A，MTU 193），对传感器 GATT Client；文件传输约 15 KB/s，大文件走 USB MTP。

## 故障速查

| 现象 | 根因 | 处置 |
|---|---|---|
| 整卡假死、1 Hz 反复识别 | 控制器卡在 `CMD_BUSY` | 模块级 RCC 复位 `HAL_RCC_ResetModule(RCC_MOD_SDMMC1)` 后重跑 `sd1_init()`；日志会有 `SD: RECOVERED by hard reset #1` |
| 一次读超时后整卷僵死 | 卡还在 DATA 态就发 CMD8 全量识别 | 停 DMA → CMD12 → CMD13 回 TRAN 就重试；**DATA 态禁止 CMD8**；只有 R1 已 IDLE 才允许 CMD0/CMD8 |
| 开机约 15 s 必复位 | `mkdir /mnt/lfs/mtp` 触发首次 `lfs_alloc` 扫树，init 连续读不让出 CPU，看门狗停喂 | 让 SD MTD 读写结束只打心跳（不 `usleep`）；把 `mkfs/mtp/*` 打进种子走 `EEXIST` |
| 静止不动却显示几 km/h | 位移窗缺静止门，位置噪声累进里程 | 补静止门（3 m 进 / 8 m 出 / 保持 4 s）；速度源切 UBX-NAV-PVT 的 `gSpeed`，`sAcc` 差就降权，两源都不可信时冻结上一次输出 |
| 蓝牙已有连接时设随机地址返回 `-EACCES` | zblue LCPU 限制，导致无法「连手机同时扫传感器」 | 按上游补丁调整地址设置路径（改 zblue，不在应用层绕） |
| 蓝牙开机自启 hardfault | companion 注册时机与 LCD 初始化并发 | 改挂到 LCD 初始化完成之后；手动 `ble_companion &` 不复现即为此因 |
| HCI 数据不动但蓝牙还活着 | HCI 接收环满 | 用 diag `ble` 槽复位 LCPU/投递 cycle；注意 IWDT 只管整机死锁，不管 HCI 环满 |
| 地图页卡顿十几秒 | 地图被放进 `/mnt/lfs`，`lfs_alloc` 扫树 | 地图只放 `/mnt/fat`；渲染走 20 ms 分片（单拍 2 块 / 8 ms） |
| `df -h` 显示 2032M 而非 14G | 上游 `uint32` 容量乘法溢出 | 板级 `vela_override/fs/fs_procfs_mount.c` 用 uint64；改完确认日志有 `fs uses board fs_procfs_mount.c` |
| 跨簇 `.vpk` 读取 `errno=5` | FAT16/FAT32 判定错，按 2 字节项走链 | 板级 `vela_override/fs/fs_fat32util.c`：`FatSz16==0` 一律 FAT32 |

## Example

用户：「码表开机大概十几秒就自己重启了，怎么回事？」

→ `run_shell "df -h"` 看三卷是否齐全，`run_shell "free"` 看堆
→ 查串口日志：停在 `bootinfo:` 下一行、没有 `INFO: mkdir /mnt/lfs/mtp`
→ 判定：首次 `lfs_alloc` 扫树导致看门狗停喂（不是卡坏、不是栈溢出）
→ 处置：确认 SD MTD 读写在搬数据期间只打心跳不 `usleep`；确认 `mkfs/mtp/import`、`mkfs/mtp/record` 已打进 SD 种子（下次开机走 `EEXIST`，不再 alloc）
→ 若仍未解决，再确认 IWDT 是否被其它路径（如加载期）停喂，**不要**去降低 IWDT 灵敏度

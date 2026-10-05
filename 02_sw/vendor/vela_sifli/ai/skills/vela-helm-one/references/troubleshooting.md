# 故障恢复手册（现象 → 根因 → 处置）

> 用法：先按**现象**定位条目，照「处置」执行；改动前读对应源文档全文。
> 新增条目时，同时把根因与处置写进源文档，并在本表末尾登记。

---

## A. 存储 / SD

### A1. 整卡假死，日志 1 Hz 反复 `re-identify`
**根因**：SD 控制器卡在 `CMD_BUSY`，全量识别（CMD0/CMD8）无法救回。
**处置**：模块级 RCC 复位 —— `HAL_RCC_ResetModule(RCC_MOD_SDMMC1)`，重启 `sd1_init()`。
现场首次应看到 `SD: RECOVERED by hard reset #1`。
**源**：`docs/sd_recovery.md` §3

### A2. 一次 `CMD17` 超时后整卷僵死
**根因**：卡仍停在 **DATA** 态就去发 CMD8 全量识别，控制器与卡状态机对不上。
**处置**（`chips/sf32lb52/sf32lb_sdio.c`）：停 DMA → 清 `DCR`/`IER` → 等 DAT0 → **CMD12**（单块超时也要发）→ **CMD13** 回 TRAN 就重试；仍在 DATA/RCV 再 CMD12，**禁止** CMD8；只有 R1 已是 IDLE（`-ENOTCONN`）才允许 CMD0/CMD8。`sd1_reinit_card()` 1 s 内最多一次。
**源**：`docs/sd_partition.md` §4.5

### A3. 开机约 15 s 必复位，日志停在 `bootinfo:` 下一行
**根因**：`mkdir /mnt/lfs/mtp` 触发**第一次** `lfs_alloc()`，从 0 滑动 lookahead 逐窗 `lfs_fs_traverse`；init 态连续 bread 不让出 CPU → idle 心跳过期 → `iwdg_feed` 停喂。
**处置**：SD MTD `bread/bwrite/erase` 结束只打 `work_beat`（时间戳，**不** `usleep`），让第一次 `lfs_alloc` 期间 IWDT 仍被喂；`busy_pump` 只留在 bringup 步进与 UI 分块加载；把 `mkfs/mtp/import`、`mkfs/mtp/record` 打进种子让开机走 `EEXIST`。
**源**：`docs/sd_partition.md` §4.4

### A4. LittleFS 挂载慢（优化后 0.474 s）
**根因**：挂载期实际只读约 **868** 个扇区，缓存/预读参数却按整卷规模配置，白读 **10 904** 个扇区。
**处置**：按实际读取量调缓存与预读参数。**方法**：先统计挂载期真实扇区数，再调参，不要凭感觉拍。
**源**：`docs/debug/mklfs.md`

### A5. `df -h` 显示 `2032M` 而不是 `14G`
**根因**：上游 `fs_procfs_mount.c` 用 `uint32` 算 `f_bsize * f_blocks`，`3665920 × 4096` 溢出。
**处置**：板级抽换 `vela_override/fs/fs_procfs_mount.c`（`uint64` 乘法）+ `CMakeLists.txt` 索引；日志应出现 `my_vendor: fs uses board fs_procfs_mount.c`。**不要**改 `nuttx/fs/mount/fs_procfs_mount.c`。
**源**：`docs/sd_partition.md` §5

### A6. `x3443_y858.vpk` 这类跨簇文件 `read()` 报 `errno=5`
**根因**：BPB `FatSz16 != 0` 时上游按 FAT16 的 2 字节项走链，实际是 FAT32。
**处置**：板级 `vela_override/fs/fs_fat32util.c`：`FatSz16==0` 一律 FAT32。只刷固件，不用重烧卡。
**源**：`docs/sd_partition.md` §5

### A7. 地图页卡顿 / 十几秒无响应
**根因**：地图被放进 `/mnt/lfs`，`lfs_alloc` 扫树；或渲染未分片。
**处置**：地图只放 `/mnt/fat`；渲染走 `render_timer` 20 ms 分片，单拍 `max=2, 8 ms`；**不要**在加载路径伪造 UI 心跳。
**源**：`docs/map/LIVEMAP.md` §5/§11、`docs/sd_partition.md` §2.2

---

## B. 定位 / GNSS

### B1. 静止不动却显示几 km/h；巡航值 17↔25 跳
**根因**：① 位移窗输入缺**静止门**，位置噪声直接累进里程；② 未滤波的 NMEA SOG 当速度源。
**处置**：补 `rt_still_gate()`（`3.0 m` 进 / `8.0 m` 出 / `4000 ms` 保持）；速度源切 UBX-NAV-PVT 的 `gSpeed`；用 `sAcc > max(0.6 m/s, 30% × v)` 做质量门；两源都不可信时冻结上一次输出。
**源**：`docs/gnss_speed_filter.md`

### B2. GNSS 静默（无定位、日志停更）
**处置**：diag `gnss` 槽看读线程心跳（超时 12 s / 冷却 30 s），`restart()` 走 PA43 断电 + 读线程 kick；上电后重新确认 `gnss: cfg ack ... pvt=` 与 `gnss: first pvt len=92`。
**源**：`docs/diag.md` §1、`docs/gnss_ble_diag_log.md`

---

## C. 蓝牙 / USB

### C1. 已有连接时设置随机地址返回 `-EACCES`（双角色冲突）
**根因**：zblue LCPU 在已有连接时禁止设置随机地址，导致无法「连手机的同时扫描传感器」。
**处置**：按上游补丁调整 LCPU 侧地址设置时机/路径（读源码定位，勿在应用层绕）。
**源**：`docs/ble/companion_impl_plan.md`

### C2. 开机自启蓝牙 hardfault（带走 NSH）
**根因**：注册挂在 `board_late_initialize()`，崩溃点精确落在 LCD（ST7789S 62.5 MHz SPI）初始化那一刻 —— 与适配器开启并发。
**处置**：把 companion 启动改挂到 **LCD 初始化完成之后**；手动 `nsh> ble_companion &` 不复现，正是这个时序。
**源**：`docs/ble/companion_bringup_notes.md`

### C3. 蓝牙"还能跑"，但 HCI 环满 / 数据不动
**说明**：IWDT 管整机死锁，**不管** HCI 环满。
**处置**：用 diag `ble` 槽（companion 线程是否推进）→ `restart()` 复位 LCPU / 投递 cycle；真正 disable/enable 仍在 companion。
**源**：`docs/diag.md` §1

### C4. 大文件传输太慢 / 断点续传
**事实**：BLE 约 15 KB/s（CRC32 + 偏移续传）；大文件走 USB MTP（产品固件只暴露 `/mnt/lfs/mtp`，工厂固件暴露 `/mnt/lfs`+`/mnt/kv`+`/mnt/fat`）。
**源**：`docs/sd_partition.md` §2.1、`docs/debug/mtp_upload.md`

---

## D. 看门狗与整机

### D1. 判定「真死」还是「单路失效」
**规则**：单路失效**只降级这一路**（`docs/system_states.md` §6）；只有 idle 心跳过期导致 `iwdg_feed` 停喂才允许整机复位。
**处置**：先看 diag 四槽（`ble` / `gnss` / `dvfs` / `fs`）哪个 `ok()` 失败，再看 IWDT 是否真的停喂。**不要去降低 IWDT 灵敏度来"治"卡顿。**
**注意**：`myvendor_diag_register` 应有 **4 次**调用（历史上曾误删 GNSS 注册）。
**源**：`docs/diag.md` §1

### D2. 崩溃后取证
**处置**：ARM 寄存器组落在 `/mnt/kv/coredump/`（`CONFIG_MYVENDOR_COREDUMP`），诊断日志落 `/mnt/kv/diag/`；`COREDUMP_REGION` 是 raw 占位，别往里写。
**源**：`docs/diag_log.md`、`docs/sd_partition.md` §2.3

---

## E. 索引维护

| 日期 | 新增条目 | 源文档 |
|---|---|---|
| 2026-09-16 | A1 SD `CMD_BUSY` 模块复位 | `sd_recovery.md` |
| 2026-09-15 | A2 DATA 态禁 CMD8 | `sd_partition.md` §4.5 |
| 2026-09-17 | B1 静止门 + PVT `gSpeed`/`sAcc` | `gnss_speed_filter.md` |
| 2026-09-20 | C3 HCI RX stall 与 IWDT 边界 | `diag.md` §1 |

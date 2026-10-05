# vela_my_vendor_tools 使用说明

SF32LB52 **my_vendor** 板级的构建、烧录与串口监视辅助脚本，用法风格类似 ESP-IDF 的 `idf.py` 或 SiFli 的 `sf_sdk_tools.py`。

> **调试现场怎么用**（口径、串口归属、看屏幕、读崩溃、常见坑）：见同目录
> [`debug_workflow.md`](debug_workflow.md)。本文是**全量命令说明**，那份是**实际操作口径**。

| 项 | 路径 |
|----|------|
| 实现 | `vendor/my_vendor/docs/tools/vela_my_vendor_tools.py` |
| 推荐入口 | `vendor/my_vendor/build_board.py`（薄封装，参数相同） |
| 板级配置 | `vendor/my_vendor/boards/sf32lb52/my_vendor/configs/nsh/` |
| NuttX 输出 | `cmake_out/my_vendor_nsh/` |
| Boot 镜像 | `vendor/my_vendor/boot_loader/bin/` |
| 烧录清单 | `vendor/my_vendor/boot_loader/config/nsh/sftool_param.json`（镜像列表） |
| 分区地址 | `vendor/my_vendor/boot_loader/config/nsh/ptab.json`（实时计算） |
| 烧录快照 | `cmake_out/my_vendor_nsh/flasher_args.json`（build 后生成，类似 IDF） |

相关文档：[board_guide.md](../board_guide.md)、[build_guide.md](../build_guide.md)、[boot_2sfbl.md](../boot_2sfbl.md)、[factory_firmware.md](../factory_firmware.md)、[sd_partition.md](../sd_partition.md)、[nuttx_boot_flow.md](../nuttx_boot_flow.md)、[nuttx_ovnx_image.md](../../scripts/nuttx_ovnx_image.md)。

---

## 1. 快速开始

在 **openvela 根目录**（含 `build.sh` 与 `nuttx/`）执行：

```bash
# 日常：编译固件 + 烧录 + 串口监视（不含 NAND 文件系统）
python3 vendor/my_vendor/build_board.py build flash monitor

# 首次或全量（固件 + bicycle 资源分区）
python3 vendor/my_vendor/build_board.py build-all flash-all monitor
```

也可直接调用实现脚本（路径更长，行为一致）：

```bash
python3 vendor/my_vendor/docs/tools/vela_my_vendor_tools.py build flash monitor
```

子命令可**组合**，按书写顺序依次执行，例如 `build flash monitor`、`build-fs flash-fs`。

---

## 2. 命令一览

### 2.1 构建

| 命令 | 作用 |
|------|------|
| `build` | 编译 NuttX（`build.sh --cmake`）；结束后按需自动 `build-boot`、`build-fs`、`wrap` |
| `build-boot` | 仅构建 `ftab.bin` + `bootloader.bin` → `boot_loader/bin/` |
| `build-factory` | 编译工厂变体 `configs/nsh-factory` → `cmake_out/my_vendor_nsh-factory/nuttx.flash.bin` |
| `build-fs` | 仅打包 NAND LittleFS → `boot_loader/bin/fs_root.bin` |
| `build-all` | **main** → **factory** → **始终** `build-boot` → wrap → 若启用 bicycle 则**始终** `build-fs` |
| `pack-sd-img` | 打 SD 整盘 `.img`（ftab + bootloader + **main** + **factory** + LFS 种子） |
| `wrap` | 将 `nuttx.bin` 包装为 `nuttx.flash.bin`（OVNX 头 + CRC，烧录用） |
| **`pack-fw`** | 同上，并改名为 `cmake_out/.../fw/1.0.0-Helm-One.bin` 供 OTA（别名 `pack-ota`） |
| `menuconfig` | Kconfig 图形/终端配置 |
| `savedefconfig` | 将当前配置写回 `defconfig` |

### 2.2 烧录

| 命令 | 烧录内容 |
|------|----------|
| `flash` | `ftab.bin` + `bootloader.bin` + `nuttx.flash.bin`（**不含**文件系统 / factory） |
| `flash-factory` | 仅工厂固件 @ ptab `factory`（SD：`0x62500000`）；不动 ftab/bootloader/main/fs |
| `flash-fs` | 仅 `fs_root.bin` @ `0x63800000` |
| `flash-all` | `sftool_param.json` 中的**全部**条目（含 factory 与 `fs_root.bin`） |

### 2.3 其它

| 命令 | 作用 |
|------|------|
| `monitor` | 串口终端（默认 1000000 波特，支持 NSH / bttool） |
| `clean` | Ninja `clean`（保留 `.config`） |
| `distclean` | `build.sh distclean`，删除 CMake 输出目录 |
| `fullclean` | 删除 `cmake_out/my_vendor_nsh/` 及 `nuttx/.config` 等 |
| `complete bash\|zsh` | 输出 shell 补全脚本 |
| `complete install` | 写入 `~/.bashrc` / `~/.zshrc` |
| `help` | 打印帮助 |

---

## 3. 构建流程说明

### 3.1 `build` 自动步骤

`build` 在 `build.sh` 成功后依次检查：

1. **boot 镜像** — `boot_loader/bin/` 缺少 `ftab.bin` 或 `bootloader.bin` 时，自动执行 `build-boot`（与单独跑 `build-boot` 相同）。
2. **文件系统** — `defconfig` 含 `CONFIG_MYVENDOR_BICYCLE=y` 且缺少 `fs_root.bin` 时，自动执行一次 `build-fs`（**仅缺失时**构建，不强制刷新）。
3. **OVNX 包装** — `AUTO_WRAP_NUTTX=True` 时生成或更新 `nuttx.flash.bin`（供二级 boot 校验与烧录），并在同目录 `fw/` 下写出 OTA 文件（`1.0.0-Helm-One.bin`）。单独打包可跑 **`pack-fw`**。
4. **flasher_args** — 写入 `cmake_out/my_vendor_nsh/flasher_args.json`。

若 `nuttx.bin` 比 `ftab.bin` 新，会提示建议重新 `build-boot`（分区体积变化时必须）。

### 3.2 `build-fs` 做什么

1. `boards/.../my_vendor/mkfs/` — 烧录文件系统内容（直接编辑此目录）
2. `scripts/build_fs_root.sh` — 生成 LittleFS 镜像 `boot_loader/bin/fs_root.bin`

运行时挂载点统一为 `/mnt/lfs`（NAND 或 SD/eMMC，见 `sf32lb_nand.c` / `sifli_ap.c`）。大体积字体/图片**不要**打进 ROMFS，走此分区。

### 3.3 `build` 与 `build-all` 的区别

| | `build` | `build-all` |
|--|---------|-------------|
| 产品 NuttX + wrap | ✓ | ✓ |
| 工厂 NuttX + wrap | ✗（单独 `build-factory`） | ✓ **每次** |
| `build-boot` | 仅 boot 镜像**缺失**时自动构建 | **每次**执行（app/factory 编完后再 boot） |
| `fs_root.bin` | 仅**不存在**时自动构建 | bicycle 开启时**每次**重建（boot 之后） |

改动了 `mkfs/` 资源后，用 `build-fs` 或 `build-all`，再 `flash-fs` 或 `flash-all`。

---

## 4. 烧录流程说明

烧录通过 **sftool**。地址**不在** `vela_my_vendor_tools.py` 里写死，而是：

1. **`sftool_param.json`** — 定义烧哪些镜像（`path` + `ptab_img`）
2. **`ptab.json`** — 按 `base + offset` 计算各 `img` 分区地址
3. **`flasher_args.json`** — `build` / `build-boot` / `build-fs` 结束后写入 `cmake_out/my_vendor_nsh/`，`flash*` 优先读此文件

逻辑实现在 `vendor/my_vendor/scripts/flash_args_lib.py`；也可手动生成：

```bash
python3 vendor/my_vendor/scripts/gen_flasher_args.py
```

示例（地址随 `ptab.json` 变化，勿手抄）：

| 镜像 | ptab_img | 来源目录 |
|------|----------|----------|
| `ftab.bin` | `ftab` | `boot_loader/bin/` |
| `bootloader.bin` | `bootloader` | `boot_loader/bin/` |
| `nuttx.flash.bin` | `main` | `cmake_out/my_vendor_nsh/` |
| `nuttx.flash.bin`（工厂） | `factory` | `cmake_out/my_vendor_nsh-factory/`（`build-all` / `pack-sd-img` / `flash-all` / `flash-factory`） |
| `fs_root.bin` | `fs_root` | `boot_loader/bin/` |

若 `flasher_args.json` 不存在，`flash` 会按 ptab + sftool_param **现场计算**（与生成文件内容一致）。

### 4.1 三种 flash 模式

```
flash          →  ftab + bootloader + nuttx          （日常迭代产品固件）
flash-factory  →  factory 槽 only                     （工厂 MTP 固件）
flash-fs       →  fs_root.bin only                    （只更新 /mnt/lfs）
flash-all      →  ftab + bootloader + main + factory + kv + lfs
                 （出厂 / 换板；不含 /mnt/fat）
```

`/mnt/fat`（地图 + 字体）**不能**串口 sftool（sparse 段数会撑爆命令行），用 `pack-sd-img` / `burn-sd`。

- 普通 **`flash` 不要求** `fs_root.bin` 或 factory 镜像存在，也不写 kv/lfs/fat。  
- **`flash-all`** / **`pack-sd-img`** 缺少 factory 或清单内文件会报错并提示 `build-all`。  
- **`flash-fs`** 仅需 `fs_root.bin`；会先 `erase_region` 擦除 FS 分区，再 sparse 写入（与 `mkfs/` 完全一致）。

烧录前建议**复位开发板**，复位后约 2 秒内开始下载。

### 4.2 整片擦除

```bash
python3 vendor/my_vendor/build_board.py flash --force
```

`--force` 向 sftool 传递 `--erase-all`，先擦整片 NAND 再写入（避免旧 ftab 导致 skip）。

---

## 5. 全局选项

| 选项 | 说明 |
|------|------|
| `-p` / `--port` | 串口（如 `/dev/ttyUSB0`、`COM19`）；默认用脚本顶部 `PORT` 或自动检测 |
| `-j` / `--jobs` | 并行编译任务数；默认 CPU 核心数 |
| `-b` / `-B` / `--baud` | `monitor` 波特率（默认 `1000000`） |
| `--force` | `flash*` 时 NAND 整片擦除 |
| `--dtr {0,1}` | monitor DTR（CH340 接 RESET 时无输出可试 `--dtr 0`） |
| `--rts {0,1}` | monitor RTS |
| `--no-reset` | monitor 打开时不发硬件复位 |
| `--no-smart-bttool` | 禁用 bttool 本地回显 |

---

## 6. 脚本顶部配置

切换板子或环境时，编辑 `vela_my_vendor_tools.py` 顶部常量（无 `-b` 板名参数）：

| 常量 | 含义 |
|------|------|
| `BOARD_CONFIG` | 板级 `configs/nsh` 相对 openvela 根的路径 |
| `BOOT_CONFIG` | NAND 分区与 sftool 配置目录 |
| `BOOT_BIN_DIR` | `ftab.bin` / `bootloader.bin` / `fs_root.bin` 输出目录 |
| `SIFLI_SDK` | SiFli SDK 路径（`build-boot` 用）；`None` 则自动探测 |
| `PORT` | 默认串口；`None` 为自动检测 |
| `MONITOR_BAUD` | 监视波特率 |
| `AUTO_WRAP_NUTTX` | build/flash 前自动 wrap |
| `JOBS` | 默认 `-j`（`0` = 全部核心） |

**输出目录**不由脚本指定，遵循 openvela 规则：`cmake_out/<板目录名>_<配置名>/`，当前为 `cmake_out/my_vendor_nsh/`。

---

## 7. 常用工作流

### 7.1 只改应用 / NuttX 代码

```bash
python3 vendor/my_vendor/build_board.py build flash monitor
```

### 7.2 改了分区表或 bootloader

```bash
python3 vendor/my_vendor/build_board.py build-boot flash
# 或 build 会在缺 boot 时自动 build-boot
```

### 7.3 改了 bicycle 图片 / 轨迹等资源

```bash
python3 vendor/my_vendor/build_board.py build-fs flash-fs
```

`flash-fs` 会先擦除 FS 分区再写入 sparse 段，设备 `/mnt/lfs` 与 `mkfs/` 一致。

### 7.4 新板或出厂镜像

```bash
python3 vendor/my_vendor/build_board.py build-all flash-all --force
```

### 7.5 改 Kconfig

```bash
python3 vendor/my_vendor/build_board.py menuconfig
python3 vendor/my_vendor/build_board.py savedefconfig
python3 vendor/my_vendor/build_board.py build
```

改 `defconfig` 后若 CMake 未刷新，可在 `cmake_out/my_vendor_nsh` 执行 `cmake --build . --target resetconfig` 再 `build`。

---

## 8. 串口监视（monitor）

- 依赖 Python `pyserial`；优先使用 `serial.tools.miniterm`。
- NSH：Enter 发送 LF（`\n`），与 NuttX readline 一致。
- 默认打开串口时**硬件复位**，便于抓取 SFBL 起的启动日志；不需要时用 `--no-reset`。
- NSH Tab 补全需固件含 `CONFIG_READLINE_TABCOMPLETION=y` 并已重新 `build`。
- **行距 / `^[[A`（2026-09-21）**：`monitor-ctl` 第二终端默认改为 **raw 按键转发**
  （不再按行+本地回显）；显示侧收掉 `\r\r\n`（设备 `SYSLOG_CRLF` + 主机 `ONLCR`
  叠出来的双 CR）。脚本若仍要整行再发，加 `--line-stdin`。
- **Ctrl-S 暂停显示 / Ctrl-Q 恢复（本地行为，不下发给板子）**：monitor 自己在
  `console.getkey` 上拦这两个键（并把控制台 tty 的 `IXON` 关掉 —— pyserial 的
  `Console.setup()` 只清 ICANON/ECHO/ISIG，不清 IXON，于是 Ctrl-S 会被终端线路
  规程当成 XOFF：只停屏幕输出，monitor 的 write 阻塞 → 读线程停 → 串口内核缓冲
  填满 → **板子那几 KB 丢掉**，Ctrl-Q 再把积压一次喷出来）。现在的语义是：
  暂停期间**串口照读不丢**，内容进内存缓冲；**Ctrl-Q 把缓冲按原顺序补上屏**，
  然后接着实时打印。没有提示行，也不落盘。
  缓冲上限 `MONITOR_PAUSE_MAX`（1 MiB），超出丢**最旧**的（保住与实时流相接的
  那一段）；补屏按 `MONITOR_PAUSE_CHUNK`（4 KiB）分块写，边补边继续收串口
  （一次倒完会让内核串口缓冲溢出丢字节）；`MONITOR_READ_TIMEOUT_S`（0.2 s）
  是补屏的响应上限 —— 读线程要靠 `read()` 返回才回到循环顶做补屏。
  注意：`--no-smart-bttool` 走的是 pyserial CLI 子进程，没有这套拦截。

### 8.1 一个串口，多个终端：`/tmp` 只放信物

**2026-09-22**：共享模型改成 **lease 信物**，不再用 `/tmp/*.sock` 当枢纽。

| 角色 | 路径 |
|------|------|
| **信物（唯一 /tmp 落盘）** | `/tmp/vela-serial-<串口名>-<hash8>.lease`（hash=`sha1(hub路径+串口路径)`） |
| **多路复用** | Linux abstract `@vela-serial-<串口名>-<hash8>`（不落盘，写在 lease 里） |
| **落盘日志** | `~/.cache/vela/serial/<串口名>-<hash8>.log` |

- 谁先抢到 lease 谁当 **`serial_hub` 持有者**（独占 TTY + 起 abstract mux）。
- **统一入口 `monitor`**：交互主窗口、第二终端、查状态、发 NSH 都用它（自动挂 hub）。
- **都走了它就消失**：最后一个流式客户端退出后约 300 s（`MONITOR_BROKER_IDLE_S`），
  hub 收工并释放信物；`release`（烧录让出）期间不算 idle。
- `VELA_SERIAL_HUB_LEASE` 可覆盖信物路径；旧 `VELA_MONITOR_SOCK` 若仍指向 `.sock` 会改写成 `.lease`。

```bash
# 终端 1：操作者（交互）
./vela_my_vendor_tools.py build flash monitor -p /dev/ttyACM0

# 终端 2 / 脚本 / AI（同一 hub）
./vela_my_vendor_tools.py monitor --follow -p /dev/ttyACM0
./vela_my_vendor_tools.py monitor --status -p /dev/ttyACM0
./vela_my_vendor_tools.py monitor sys -p /dev/ttyACM0
```

- 终端 2 也可以**第一个跑**：内部拉起 `serial_hub` 抢 lease；之后交互 `monitor` 只挂客户端。
- flash：`release`（关 TTY、`state=yielded`）→ sftool → `acquire`+`pulse`（`gen++`）。
- 旧名 `monitor-ctl` / `ctl` 仍是别名，不必再记。

### 8.2 持有者程序：`serial_hub.py`

```bash
python3 vendor/my_vendor/docs/tools/serial_hub.py /dev/ttyACM0 -b 1000000
python3 vendor/my_vendor/docs/tools/serial_hub.py /dev/ttyACM0 --reset
python3 vendor/my_vendor/docs/tools/serial_hub.py /dev/ttyACM0 --no-idle-exit
```

- 抢 `/tmp/vela-serial-<dev>-<hash8>.lease`；失败则说明已有持有者（看 lease 里的 pid/sock）。
  `VELA_SERIAL_HUB_ROOT` 可覆盖参与 hash 的“运行路径”。
- 协议仍是逐行 JSON：`status` / `rx` / `send` / `tx` / `attach` / `release` /
  `acquire` / `pulse` / `shutdown`。
- 本工具需要持有者时会 **exec `serial_hub.py`**（`ps` 里能直接认出来）。

### 8.4 卡死与自愈（2026-09-20 现场）

「hub 卡死」在串口这条线上其实是**三个不同的故障**，现象像、修法不同：

**(1) 读线程死了 —— mux 还活着，板子却"哑"了。**
判据：`monitor --status` 回 `ok:true`、往口里写命令也不报错，但屏幕 / `--rx` 不再出数据。
真因：monitor 自己的读线程在让出/收回（烧录）的竞态里吃到一次 `SerialException` 就
`alive=false` 直接退出，`miniterm.join()` 之后没人重启它 —— 串口再没人排空，板子的输出
全堆在内核 tty 缓冲里。取证：

```bash
kill -USR1 <monitor_pid>     # faulthandler 已注册；栈会打进 monitor 的日志
# 修复前：进程里没有一条在读串口的线程（只剩 socket 服务 + 键盘线程）
```

修好的行为：`SifliMiniterm.reader()` 遇断线不再退出，而是**关 fd → 等串口回来 → 重开 → 接着读**，
并留一行 `*** monitor: 串口断开，等它回来（读线程不再退出） ***`。

**(2) 客户端不读，把读线程钉死（已改成异步扇出，2026-09-21）。**
以前 `push()` 在读线程里对每个流式客户端 `sendall`：某个"只连不读"的客户端（终端被
暂停、跟随进程僵住）会把读线程钉住，串口随之中断排空。现在读路径只往每客户端队列
塞一帧（队列满 / 发送超时就摘掉慢客户端），独立 `hub-fanout` 线程做真正的 socket 写 ——
慢客户端拖不死串口排空。`status` 多了 `rx_age_ms` / `rx_bytes`，方便区分"板子哑了"
还是"hub 卡了"。`ser.write_timeout` 仍是 1 s，写不动就报错给请求方而不是无限等。

**(3) 谁在持有串口 / 怎么起一个靠谱的持有者。**
`monitor` 在"没有常驻 broker"时会把 broker 跑在**自己进程内**（legacy 路径），一旦它自己的
读线程出问题，整个口就没人排空。长期挂机建议显式起独立 hub：

```bash
# 一般不必手起；`monitor` 会自动拉。长期挂机可：
nohup setsid python3 vendor/my_vendor/docs/tools/serial_hub.py /dev/ttyACM0 -b 1000000 \
      --no-idle-exit >>/tmp/vela-hub-fixed.log 2>&1 &
```

端点靠 lease / abstract，`monitor` 会自己接回来（broker 模式下断开即 2 s 重试）。
`serial_hub.py` 的读循环自带"断线 → 等回来 → 重开"，比 in-process broker 更抗造。

### 8.5 常驻 hub（默认，不必再记 broker）

`monitor` **默认**挂 `serial_hub`：串口、64 KiB RX 环、落盘 spool、abstract mux 都在 hub 里；
交互窗口只是流式客户端。`--no-broker` 才退回直连（不推荐）。

```bash
python3 vendor/my_vendor/docs/tools/vela_my_vendor_tools.py monitor -p /dev/ttyACM0
python3 vendor/my_vendor/docs/tools/vela_my_vendor_tools.py monitor --rx -p /dev/ttyACM0
python3 vendor/my_vendor/docs/tools/vela_my_vendor_tools.py monitor stop -p /dev/ttyACM0  # 真收工
```

- 客户端**随时退出/重开**：串口不断；重开时按 `replay` 把错过的一段补回屏幕。
- 落盘：`~/.cache/vela/serial/<dev>.log`。
- `flash` 可在 monitor 开着时借用：`release` → sftool → `acquire`+`pulse`。
- **停 hub 用 `monitor stop`，不要按命令行 `pkill`**（容易误杀别的 python）。

旧名 `monitor-broker` / `monitor --broker` 仍兼容，日常不必用。
### 8.4 后台跑 monitor：`docs/tools/pty_run.py`

`monitor` 走 pyserial `miniterm`，而它的 `Console` 需要**真 tty**：`nohup ... monitor &`
或 `... > log &` 会直接抛 `termios.error: (25, 'Inappropriate ioctl for device')`。
`pty_run.py` 给子进程一个 pty 并把输出写进日志（追加）：

```bash
python3 vendor/my_vendor/docs/tools/pty_run.py /tmp/mon.log \
    python3 vendor/my_vendor/docs/tools/vela_my_vendor_tools.py -p /dev/ttyACM0 monitor --no-decode &
```

`--no-decode` 是避坑：崩溃地址解码在某些输入上会让**客户端**读线程停住（带 broker 时
丢的只是屏幕显示，数据仍在 broker 缓冲里，`--rx` 可补看）。

---

## 9. Tab 补全

```bash
eval "$(python3 vendor/my_vendor/docs/tools/vela_my_vendor_tools.py complete bash)"
# 或持久安装
python3 vendor/my_vendor/docs/tools/vela_my_vendor_tools.py complete install
source ~/.bashrc
```

**补全覆盖到哪一层（2026-09-19 重写）**：生成的是**纯 bash 词表**，按 Tab 不启动 Python（快），
而且不再只有第一层：

| 位置 | 候选 |
|---|---|
| 第一个词 | 全部子命令（按前缀过滤） |
| 链式位置 | `build flash mo<Tab>` → 还能再跟的子命令（`monitor`/`monitor-broker`/`monitor-ctl`） |
| 子命令之后 | 该子命令自己的选项（`monitor-ctl --` → `--status --rx --follow --no-stdin …`；`flash --fl` → `--flash-medium --flash-baud`） |
| `-p` / `--port` 之后 | **真实串口**（`/dev/ttyACM*`、`/dev/ttyUSB*`、`/dev/serial/by-id/*`；没有则退回文件名） |
| `-M` / `-b` / `-fb` / `-j` 之后 | `nand sd emmc nor` / 常用波特率 / 常用并行数 |
| `--sock` `--elf` `-i` `--image` `--sd` 之后 | 文件路径 |
| `monitor-broker` 之后 | `status stop` |
| `monitor-ctl ctl …` | 板级命令：`radio sensor bl sd wt watch mtp notif log dvfs gnss`，再往下还有取值（`ctl radio on|off`、`ctl dvfs auto|off|low|high|48…240`、`ctl log err|warn|…`） |

改完当前 shell 生效一次就够：`source <(python3 vendor/my_vendor/docs/tools/vela_my_vendor_tools.py complete bash)`。

### 9.1 工作区环境怎么加载（`*ubuntu_get_env.sh`，2026-09-19 改写）

启动脚本里有个 `cd()` 钩子：**每次 cd 进含 `*ubuntu_get_env.sh` 的目录就 source 它**（openvela
根目录就有一份）。原来那份有三个问题，已改：

| 问题（实测） | 现在 |
|---|---|
| 每 cd 一次起 3 个 Python 跑补全生成，**0.25 s/次** | 补全生成成静态文件放 `~/.cache/vela-completions/*.bash`，之后只 `source`（纯 bash）→ **0.01 s/次**；工具文件更新会自动重生成 |
| PATH 不幂等：22→23→24→25，`bloaty` 重复 3 份 | `path_add_once`：只加一次，稳定 23 条 |
| 补全抄在这份文件里，和工具自己的 `complete install` 两处来源、会漂移 | 不再手抄，只调用工具生成 |

- 真源放在仓库：`docs/tools/vela_ubuntu_get_env.sh`（`re.sh` / `repo sync` 会清掉根目录，根目录那份是薄壳）；
  原文备份 `vela_ubuntu_get_env.sh.orig-20260919`。
- 想让"**离开工作区也还原**"（PATH 不带去别的项目）：`docs/tools/vela_ubuntu_env_hook.sh` —— 一个最小
  号的 direnv（进：存 PATH 再加载；出：还原）。用法二选一：新 shell 里 source 它一次，或把启动脚本里
  那段 `_load_ubuntu_env` / `cd` 换成 source 它。

---

## 10. 与 build.sh 的关系

`build` / `menuconfig` / `savedefconfig` / `distclean` 内部调用：

```bash
./build.sh vendor/my_vendor/boards/sf32lb52/my_vendor/configs/nsh/ --cmake ...
```

不用封装脚本时可直接 `build.sh`，产物路径相同。`vela_my_vendor_tools` 额外提供：boot/fs 自动构建、OVNX wrap、按模式拆分的 flash、monitor、补全。

---

## 11. 故障排查

| 现象 | 处理 |
|------|------|
| `未找到 SiFli SDK` | 设置脚本顶部 `SIFLI_SDK` 或导出环境变量 |
| `flash` 报缺 ftab/bootloader | `build` 或 `build-boot` |
| `flash-all` 报缺 fs_root.bin | `build-fs` 或 `build-all` |
| `flash-all` / `pack-sd-img` 报缺 factory | `build-factory` 或 `build-all` |
| bicycle 无资源 / 字体 | 确认已 `flash-fs` 或 `flash-all`，且 `/mnt/lfs` 已挂载 |
| `module resource has no attribute getpagesize` | 勿让 SiFli SDK 的 `PYTHONPATH` 污染 NuttX 构建；脚本已过滤，避免在 build 前 `source export.sh` 后不再清理 |
| monitor 无输出 | 试 `--dtr 0 --rts 0`；确认波特率与 `CONFIG_UART_BAUD` 一致 |
| monitor 打字无实时回显、Tab 才出字 | 默认崩溃解码按行缓冲导致；已修复 `vela_elf_resolve.py`；临时可加 `--no-decode` |
| 改过 defconfig 未生效 | `resetconfig` 后重新 `build` |
| msh 出现 `unknown: ~ATSF32!`、flash 连不上 | 不要在 2SFBL 里 `reboot`；用工具自带 RTS 脉冲或 msh `download`。见 [boot_2sfbl.md](../boot_2sfbl.md) 第 7 节 |
| `Failed to download stub: Timeout` | 确认 stub 阶段 `--compat true`（工具已两阶段烧录）；关掉占用 ACM 的 monitor |
| 烧录只有约 20 KB/s | 整段 `--compat true` 会把 payload 拆成 256 B+10 ms；工具已改为 compat 只灌 stub |

Boot / OVNX 细节见 [nuttx_ovnx_image.md](../../scripts/nuttx_ovnx_image.md)；2SFBL msh 与烧录窗口见 [boot_2sfbl.md](../boot_2sfbl.md)；NAND 分区说明可执行 `boot_loader/scripts/print_nand_boot_help.sh`。

---

## 12. 命令速查

```bash
# 构建
build | build-boot | build-factory | build-fs | build-all | wrap | pack-fw | menuconfig | savedefconfig

# 烧录
flash | flash-factory | flash-fs | flash-all          # 可加 -p PORT、--force

# 维护
monitor | clean | distclean | fullclean | complete | help
```

# 二级 Boot（2SFBL）开发说明

本文记录 **my_vendor 二级 bootloader** 的开发约定：从 Mask ROM 拷到 SRAM、自定义 msh、KV LittleFS、OVNX 跳转 NuttX，以及 USB-CDC 上用 sftool 烧录时踩过的坑。

NuttX 跳转之后的 `rcS` / builtin / `board_late` 见 [nuttx_boot_flow.md](nuttx_boot_flow.md)。  
卡上分区见 [sd_partition.md](sd_partition.md)。  
OVNX 头与 CRC 见 [nuttx_ovnx_image.md](../scripts/nuttx_ovnx_image.md)。  
主机烧录命令见 [tools/vela_my_vendor_tools.md](tools/vela_my_vendor_tools.md)。  
工厂固件分区 + 不在 boot 里做 MTP：[factory_firmware.md](factory_firmware.md)。  
产品固件 A/B 在 KV `/fw`（NuttX `/mnt/kv/fw`）：[fs_firmware.md](fs_firmware.md)。

日期：2026-09-02（2026-09-29 更正 pwr 语义与横幅读数）。当前默认介质：`boot_loader/storage.conf` → **`BOOT_STORAGE=sd`**。版本 **1.2.0**。

---

## 1. 启动链

```
上电 / 硬件复位
    │
    ▼
[Mask ROM]          约 1 s UART Debug-IP 窗口（仅 pin 复位，且未置 PMUC_CR_REBOOT）
    │               从卡上 FLASH_TABLE 拷 2SFBL → SRAM 0x20020000
    ▼
[2SFBL]             vendor/my_vendor/boot_loader/project/butterflmicro/
    │               片内 HRC48（不等 48 MHz 晶振）；dfu_flash_init() → boot_msh_start
    │               挂 KV（可写 persist.boot.target）；autoboot：KV target，否则 /fw → main → factory
    │               persist.boot.pwr 读到 1/on/true（nsh `ctl pwr on`）：不判 PWR 键、不进充电等待页，直接跳转
    │               连续 10 次 B/b 留 msh（中止加载，不再复位）；连续 10 次 F/f 进工厂
    │               充电(STAT 低)+KEY1+KEY2：立刻工厂，不写 persist.boot.running
    │               PA40 蜂鸣片拉低；NV3031A + SGL 显示加载进度
    ▼
[OVNX payload]      nuttx.flash.bin：跳过 1 KiB 头，CRC 校验后拷到 PSRAM
    ▼
[NuttX __start]     chips/sf32lb52/sifli_start.c
```

| 阶段 | 谁在跑 | 串口上你会看到 |
|------|--------|----------------|
| ROM | 片内 Mask ROM | 无 msh；sftool 在此用 `0x7E 0x79` + `ATSF32` |
| 2SFBL | SRAM `@ 0x20020000` | `2SFBL msh 1.2.0`，提示符 `msh />`；`ver` 打印版本 |
| NuttX | PSRAM | `nsh>` |

2SFBL **不打开 48 MHz 晶振**：系统钟保持 ROM 的片内 **HRC48**。`HAL_HPAON_EnableXT48()` 在没焊晶振时会空等。DLL / HCLK 144 MHz 也依赖那颗钟，boot 里 USB 已拿掉，不再提速。NuttX `BSP_Board_PreInit` 再切 HXT48（USB / BLE 需要）。

sftool **只跟 Mask ROM 说话**，不认识 2SFBL msh。已经进了 `msh />` 再跑 sftool，必须先让芯片回到 ROM 下载窗（见第 7 节）。

---

## 2. 源码与产物

```
vendor/my_vendor/boot_loader/
├── storage.conf                 # 唯一介质选择：nand | sd | emmc | nor
├── build.sh                     # scons 编 boot + gen_ftab / gen_ptab_table
├── bin/                         # ftab.bin、bootloader.bin
├── config/nsh/
│   ├── ptab.sdmmc.json          # SD/eMMC 分区（当前）
│   ├── ptab.nand.json
│   ├── ptab.nor.json
│   ├── boot.json                # SDK board / 工程路径
│   └── sftool_param.json        # 烧哪些镜像（地址来自 ptab）
├── include/                     # ptab.h → ptab_sdmmc.h 等
├── third_party/sgl/             # SGL 快照（见 BOOT_PORT.md）
│   └── SConscript               # 裁剪编译，产物在 ram_v2/build_*/sgl/
└── project/butterflmicro/board/
    ├── main.c                   # dfu_flash_init 后进 msh
    ├── boot_msh.c / .h          # 自定义 UART msh（不是 RT-Thread FinSH）
    ├── boot_hw.c / .h           # PA40 蜂鸣片、充电+KEY1+KEY2
    ├── boot_lcd.c / .h          # NV3031A QAD-SPI（轮询 LCDC）
    ├── boot_sgl.c / .h          # SGL 启动条
    ├── sgl_cfg/sgl_config.h     # boot 裁剪后的 SGL 配置
    ├── boot_lfs.c / .h          # KV LittleFS（浏览 + 写 persist.boot.target）
    ├── boot_target.c / .h       # persist.boot.target 读写
    ├── boot_ovnx.c / .h         # 解析 nuttx.flash.bin
    ├── boot_flash.c             # SD/NAND 读
    ├── SConscript               # 拷 NuttX littlefs 进 .cache
    └── ptab_table.c / .h        # 由 JSON 生成，勿手改
```

构建产物：

| 文件 | 卡上位置（SD，`ptab.sdmmc.json`） | SRAM / 用途 |
|------|-----------------------------------|-------------|
| `ftab.bin` | `0x62001000`（卡偏移 `0x1000`，64 KiB） | ROM / 2SFBL 查表 |
| `bootloader.bin` | `0x62011000`（卡偏移 `0x11000`，**160 KiB**） | 拷到 `FLASH_BOOT_LOADER` `@ 0x20020000` |
| `nuttx.flash.bin` | `0x62100000`（卡偏移 `0x100000`，4 MiB 槽） | OVNX 包装后的 NuttX |

`ftab` 拷贝长度跟 `bootloader.max_size`。槽现为 160 KiB，SRAM 区接到 `BOOTLOADER_RAM_DATA` `@ 0x20048000`（**200 KiB**）。再加大代码槽必须再挪 data RAM 或改放到 PSRAM，不能只改卡上 `max_size`。

改分区：先改 JSON，再 `build-boot`（`gen_ptab_table.py` 重写 `ptab_table.{c,h}`）。

---

## 3. msh（`boot_msh.c`）

这是 **NO_OS 自写的 UART shell**，提示符学 FinSH（`msh />`），**不是** RT-Thread FinSH，也不是 NuttX NSH。默认浏览 KV；`fw` 列 KV 上的固件槽；`target` 改开机跳转标记。

### 3.1 命令

| 命令 | 作用 |
|------|------|
| `help` / `?` | 命令列表 |
| `ver` | 打印 2SFBL 版本（`BOOT_LOADER_VERSION`） |
| `ls [path]` | 列 KV LittleFS |
| `cat <file>` | 打印文件（**最多 4 KiB**） |
| `cd [path]` | 改当前目录 |
| `pwd` | 打印 cwd |
| `df` | 打印当前挂载卷信息（msh 常态是 KV） |
| **`fw`** | 列 KV `/fw/*.bin`（最新在前）；msh 本来就挂着 KV |
| **`check`** / `verify` | CRC 校验 `/fw`、分区 `main`、分区 `factory` 是否完整、复位向量是否可跳；**不跳转**。末行打印 autoboot 会选哪一个（已计 KV target） |
| **`target [slot]`** | 读/写 `persist.boot.target`：`fw` / `main` / `factory` / `boot`（留 msh）；`off` 删掉，恢复默认链。无参数打印当前值 |
| `app` / `boot` | 立刻加载并跳转：**先** `/fw/<版本>.bin`（最高版本），**再** 分区 `main`，再失败则 **工厂** 分区（**不读** KV target） |
| **`factory`** | 立刻跳转 **工厂固件**（`factory` 槽，NuttX 里 MTP 自启挂 `/mnt/lfs`）；见 3.4 |
| `reboot` | `HAL_PMU_Reboot()`，置 `PMUC_CR_REBOOT` |
| `download` | WDT 整片复位，**清掉** `PMUC_CR_REBOOT`，ROM 仍会等 ~1 s UART 下载 |
| Tab | 补全命令或 KV 路径 |

路径可写 `/mnt/kv/...`，msh 会剥掉前缀，与 NuttX persist 文件名对齐。

### 3.2 停留还是自动跳转

上电先读 KV **`persist.boot.target`**（NuttX 路径 `/mnt/kv/db/persist.boot.target`，与 KVDB file 后端同文件）。有合法值就只跳那个槽（粘性，不会用完删除）；读不到或非法则默认链：

1. LittleFS `/fw/*.bin`（版本号最高且 CRC 通过）
2. 分区 `main`
3. 分区 `factory`（静默回退，不弹「固件异常」）
4. `factory` CRC 也失败：屏上红字 `CHIP DAMAGED`（UART：`芯片损坏`），留 msh。不再 `boot_error` 复位空转。

**主槽**指 KV `persist.boot.target` 钉住的槽（`fw` 或 `main`），不是分区名 `main`。该槽 CRC/头失败：屏上黄字 `FW error, factory`（UART：`固件异常，进入 factory`），再加载 `factory`。屏上只用 ASCII（consolas14）；中文在 UART。

| KV 值 | 开机 |
|-------|------|
| （无文件 / 空 / 非法） | 上面默认链 |
| `fw` | 只试 `/fw`；CRC 失败提示并进 factory |
| `main` | 只加载分区 `main`；CRC 失败提示并进 factory |
| `factory` | 只加载分区 `factory`；CRC 失败提示芯片损坏并留 msh |
| `boot`（或 `msh`） | 留在 2SFBL msh，不加载 |

msh 改标记：

```text
msh /> target              # 打印当前（default 表示走默认链）
msh /> target factory      # 下次开机进工厂
msh /> target fw
msh /> target main
msh /> target boot         # 下次开机停 msh
msh /> target off          # 删文件，恢复默认链
```

NuttX 侧同一文件：

```text
nsh> setprop persist.boot.target factory
nsh> getprop persist.boot.target
nsh> rm /mnt/kv/db/persist.boot.target    # 清掉，恢复默认链
```

调试反复烧录时不想每次长按开机：`nsh> ctl pwr on` 写 **`persist.boot.pwr`**。2SFBL 在判 PWR 键 / 充电等待页之前读这个文件，**只有读到明确的 on（`1` / `on` / `true`）才拉住 PA29 直接 autoboot**；缺文件、空文件、`0` / `off`，以及任何读不出来的值（坏文件）都算关，照常走按键 / 充电页。`ctl pwr off` 恢复正常按键开机。msh 里 `pwr on|off` 等价。量产不要开。

开机横幅最后一行就是这次读到的值：`persist.boot.pwr=on: skip key, autoboot` 或 `persist.boot.pwr=off: pwr key boots, plug-in charge page` —— 它是**实时读数**，不要跟紧随其后的**路径行**（`pwr: boot` / `pwr: persist.boot.pwr, skip key` / `charging: any key 1s boot`）混起来看。

UART 热键（连续计数，其它字节清零；与加载窗口重叠，共约 2 s）**优先于** KV target：

| 热键 | 次数 | 行为 |
|------|------|------|
| `B` / `b` | **10** | 立刻停加载，留在 msh；**不再走 ftab / `boot_error` 复位** |
| `F` / `f` | **10** | 跳过产品，进工厂固件（仍写 `persist.boot.running`） |

上电组合键（LCD 复位之前采样，插电即开机时用）：

| 条件 | 行为 |
|------|------|
| 充电中（PA25 STAT 低）且 KEY1（PA30）+ KEY2（PA33）按下 | 立刻工厂固件，**不写** `persist.boot.running` / `persist.boot.target` |

PA40 无源蜂鸣片在 `hw_preinit0` / `entry` 里切 GPIO 并拉低，boot 期间不应再响。

屏：NV3031A 240×320 QAD-SPI + 裁剪版 [SGL](https://gitee.com/sgl-org/sgl)（label + progress）。LCD/SGL 失败不挡 UART 启动。

msh 里 `app`/`boot` 走默认链 `/fw` → `main` → `factory`，**不读** KV target；`factory` 仍直接跳工厂槽。

`main.c` 在 `dfu_flash_init()` 之后调用：

```c
boot_msh_start(boot_images_prepare, boot_images_go);
```

`prepare` 若 KV target 有值则只加载该槽；否则先尝试 KV `/fw/*.bin`（文件名 `X.Y.Z` 从高到低），失败再按 ftab 把分区 `main` 拷进 PSRAM，再失败加载分区 `factory`；`go` 跳向量表。msh 里 `app`/`boot` **每次重新加载默认链**（工厂跳转之后 PSRAM 里是工厂镜像）。连发 10 个 `F` 会丢掉已加载的产品镜像、改灌工厂。USB MTP **不在 2SFBL 里**：要传文件请 `factory` 或上电连发 `F` 进工厂 NuttX（`mtp_simple`，KV 卷的 `fw/`），或 BLE OTA 写 `fw/<version>.bin`（落到 `/mnt/kv/fw`）。详见 [fs_firmware.md](fs_firmware.md)。

### 3.3 与 sftool 帧的关系

所有 Debug-IP 帧都以 `0x7E 0x79` 开头（Enter **和** stub 的 MEMRead/Write）。msh 只吞掉这两个字节，避免把 `0x7E` 当成 `~` 回显。

**不要**在看到 `0x7E 0x79` 时 WDT 复位：Connected 之后 stub 下载用的是同一 START_WORD，复位会杀掉这次烧录。需要进 ROM 下载窗时，用主机 RTS 脉冲，或在 msh 里敲 **`download`**。

### 3.4 工厂固件跳转（`factory`）

`factory` 与产品固件 `main` 是**两份独立的 NuttX OVNX 镜像**，分别烧在 SD 的 `main`（`0x62100000`）和 `factory`（`0x62500000`）槽。二者都拷到 PSRAM `0x10000000` 运行、**一次只跑一个**，所以工厂镜像无需改链接地址。

- msh `app` → **先** `/mnt/kv/fw/<version>.bin` 最新，**再** 分区 `main`，再失败则工厂槽；msh `factory` / 上电 10×`F` → 工厂分区槽。
- boot 侧 `boot_factory_boot()`（`main.c`）用编译进去的 `ptab_find_img("factory")` 取槽地址，复用 `boot_ovnx_load_hcpu()` 校验 CRC 后 `run_img()`——**不进 ftab**，`gen_ftab.py` 无需改。
- 工厂固件是 `configs/nsh-factory` 变体：`CONFIG_MYVENDOR_MTP_SIMPLE_AUTOSTART=y`，NuttX 起来后 MTP 自动服务 **`/mnt/lfs`**。2SFBL 不再内置 USB gadget。

条件编译产出与烧录：

```bash
python3 vendor/my_vendor/build_board.py build-all      # main + factory + boot
python3 vendor/my_vendor/build_board.py pack-sd-img    # 卡镜像含 factory@0x62500000
python3 vendor/my_vendor/build_board.py flash-factory  # 也可只写 factory 槽
```

改工厂行为只改 `configs/nsh-factory/defconfig` 再 `build-factory`。分区细节见 [sd_partition.md](sd_partition.md) 2.7。

---

## 4. Boot 里的 LittleFS

2SFBL **同一时刻只挂一卷**（SRAM 里一份 cache）。msh 默认挂 **`KV_REGION`**（SD：卡偏移 `0x10000000`，256 MiB，NuttX `/mnt/kv`）。产品 `/fw` 就在这卷上，加载时不必改挂 `FS_REGION`。

| 项 | 值 | 原因 |
|----|----|------|
| 源码 | `nuttx/fs/littlefs/littlefs/`（v2.7，磁盘 2.1） | 与 NuttX 同树 |
| 构建拷贝 | `vendor/my_vendor/.cache/littlefs-nuttx/` | scons 不往 NuttX 树丢 `.o` |
| `read_size` / `prog_size` | 512 | 与 `sf32lb_sdio.c` 一致 |
| `block_size` | 4096 | 同上 |
| `LFS_NAME_MAX` | 128 | 同上 |
| 空 KV | **不在 boot 里 format** | 空白区由 NuttX `autoformat` |

禁止改用仓库里的 **littlefs 2.5.1（磁盘 2.0）**：NuttX 再挂 KV 会 `LFS_ERR_CORRUPT`（-84）。

`SConscript` 里 **未** 开 `LFS_READONLY`：NuttX 的 getpath 补丁需要 `lfs_fs_parent`。boot 侧 msh 浏览 KV，并用 `target` 写 `persist.boot.target`。产品加载只读 `/fw/*.bin`。不在 boot 里 format。

FS_REGION `max_size=0`：先按 CSD 余量挂（与 NuttX grow 后一致）；失败再试 pack-sd-img 的 2 GiB 种子几何。地址用 `uint64`，否则 512 MiB 之后的 14 GB 窗口会截断。

SD 读成功以驱动返回 **512** 为准，不要用错误的 `TEST_PASS` 判据。

---

## 5. 编译

SiFli SDK 编 scons 工程（默认探测，或显式指定）：

```bash
# 仅 boot（改 msh / lfs / ptab 后）
SIFLI_SDK=/home/jinsc/SDK/SiFli/SDK/2.4 \
  vendor/my_vendor/boot_loader/build.sh --bootloader-only --no-prompt

# 或统一入口（会按 storage.conf 选 ptab）
python3 vendor/my_vendor/build_board.py build-boot
```

检查 `boot_loader/bin/bootloader.bin` 体积：**必须 < 160 KiB**（`0x28000`）。当前带 msh + LittleFS + ADC 大约 130 KiB。

改 `storage.conf` 后必须重编 **boot 和 NuttX**（板级 `my_vendor_boot_storage.h` 也跟这个值）。

---

## 6. 烧录（sftool + USB-CDC）

日常：

```bash
# 先关掉占用 /dev/ttyACM0 的 monitor
python3 vendor/my_vendor/build_board.py flash -p /dev/ttyACM0
# 或仓库根目录的软链
./vela_my_vendor_tools.py flash -p /dev/ttyACM0
```

`flash` 写 **ftab + bootloader + nuttx.flash.bin**。只改了 2SFBL 也建议 **ftab 与 bootloader 一起烧**（拷贝长度跟 ftab）。

工具在调 sftool 之前会做与 **monitor 相同的 DTR+RTS 脉冲**，然后分两步（ACM 上必须如此）：

```text
1) --compat true  --before no_reset --after no_reset
     erase_region  <BOOT_RESERVE 末 512B>
2) --compat false --before no_reset_no_sync
     write_flash   ftab / bootloader / nuttx.flash.bin
```

| 参数 | 为什么 |
|------|--------|
| `--before no_reset` | ACM 上 sftool 自己的 RTS-only 常常 **拉不了芯片复位** |
| `--compat true`（仅灌 stub） | stub 按 **256 字节** 下 RAM（默认 64 KiB 块在 ACM 上会 `Timeout("receiving UART frame")`） |
| `--compat false`（写镜像） | 整包 128 KiB；`--compat true` 会把 payload 也拆成 256 B + 10 ms，只剩约 **20 KB/s** |
| `--before no_reset_no_sync` | 第二步不再灌 stub，沿用第一步已在跑的 ram stub（需要 sftool 0.1.16+） |
| `-m sd` | 与 `storage.conf` 一致；stub 起来后会等 `sd0 OPEN success` |

波特率与芯片见 `vela_my_vendor_tools.py`；端口占用会直接导致复位/下载失败。

---

## 7. 进 ROM 下载窗：能用 / 不能用

SF32LB52 **没有 BOOT 脚**。Mask ROM 只在 **pin 复位** 且 **未置 `PMUC_CR_REBOOT`** 时等待 ~1 s UART 下载。

| 做法 | 结果 |
|------|------|
| 主机 `flash`（内部 `_prepare_rom_download`，与 monitor 同脉冲） | 正确 |
| msh **`download`**（WDT，清 `PMUC_CR_REBOOT`） | 正确（随后立刻跑 sftool） |
| msh **`reboot`**（`HAL_PMU_Reboot()`） | **错误**：A4+ ROM **跳过** 下载窗，2SFBL 立刻起来；sftool 的 `~ATSF32!` 被 msh 当命令吃掉（`0x7E`=`~`，头里的 `0x08` 当退格） |
| 看到 `0x7E 0x79` 就在 msh 里 WDT | **错误**：会打断 stub 下载 |

烧录前关掉 monitor。ACM 被占用时复位脉冲发不出去。

---

## 8. 开发检查清单

| 症状 | 优先查 |
|------|--------|
| `msh />` 不自动进 NuttX | 上电连发了 10 个 `B`/`b`；或 `target boot`；或指定槽/三处固件都加载失败 |
| `app`/`boot` 打印 `load failed` | `/fw`、`main`、`factory` 都无效；`factory` 也坏时屏上 `CHIP DAMAGED`。黄字 `FW error, factory` 只在 KV `target` 钉住的槽 CRC 失败时出现 |
| boot 能 `ls` KV，NuttX 挂 `/mnt/kv` 报 -84 | boot 误链了 littlefs 2.5；或几何不是 512/4096 |
| `cat` 内容被截断 | 超过 4 KiB（`BOOT_LFS_CAT_MAX`） |
| `bootloader.bin` 链接失败 / 跑飞 | 超过 160 KiB，踩到 `BOOTLOADER_RAM_DATA` |
| `Failed to download stub: Timeout` | 第一步未 `--compat true`；或仍开着 monitor |
| `Connected success!` 后失败 | 不要对 `0x7E 0x79` 复位；确认 `--before no_reset` |
| 写入只有约 20 KB/s | 整段带了 `--compat true`（payload 256 B + 10 ms）；应用两阶段：compat 只灌 stub |
| msh 出现 `unknown: ~ATSF32!` | 用了 `reboot` 或没 pin 复位，ROM 窗已过 |
| SD stub 卡住 | 等 `sd0 OPEN success`；`-m` 须为 `sd` |
| 需要 USB 拷文件 | 上电连发 10 个 `F` 进工厂 NuttX 的 `mtp_simple`；或 msh 敲 `factory` |

串口确认 2SFBL 已起来：

```text
2SFBL msh 1.0.4
autoboot: kv persist.boot.target, else /fw then main then factory
10x B/b = msh, 10x F/f = factory
CHG+KEY1+KEY2 = factory (no kv)
boot=1.0.4
kv target: default
```

无 KV 标记时走产品链（`/fw` → `main` → `factory`）。要停 msh：上电连发 10 个 `B`，或事先 `target boot`。要工厂 MTP：连发 10 个 `F`，或插电时按住 KEY1+KEY2（充电中），或 `target factory` 后复位。连发 10 个 `B` **不会再整片复位**。combo / 10×F 只中止**产品**加载；已经在拷 factory 时不再把自己当成 abort。

---

## 9. 关键文件索引

| 路径 | 说明 |
|------|------|
| `boot_loader/project/butterflmicro/board/boot_msh.c` | msh、autoboot（KV target 否则 `/fw`→`main`→`factory`）、`target`、`check`、`B`/`F` 热键、充电+双键工厂、`download`/`reboot`、`factory` |
| `boot_loader/project/butterflmicro/board/boot_hw.c` | PA40 蜂鸣片、PA25/PA30/PA33 工厂组合键 |
| `boot_loader/project/butterflmicro/board/boot_lcd.c` / `boot_sgl.c` | NV3031A + SGL 启动条 |
| `boot_loader/third_party/sgl/` | SGL 快照（`BOOT_PORT.md`） |
| `boot_loader/project/butterflmicro/board/boot_version.h` | `BOOT_LOADER_VERSION`；写入 `persist.boot.version` |
| `boot_loader/project/butterflmicro/board/boot_target.c` | `persist.boot.target` / `running` / `version` / `pwr` 读写 |
| `boot_loader/project/butterflmicro/board/boot_lfs.c` | KV LittleFS（浏览 + 写 persist 标记） |
| `boot_loader/project/butterflmicro/board/boot_ovnx.c` | OVNX 加载 / 流式 CRC 校验（不占 PSRAM） |
| `boot_loader/project/butterflmicro/board/boot_flash.c` | SD 读；2SFBL 保持 HRC48，不等 HXT48 |
| `boot_loader/project/butterflmicro/board/SConscript` | littlefs 拷贝与宏 |
| `boot_loader/config/nsh/ptab.sdmmc.json` | SD 分区源 |
| `docs/tools/vela_my_vendor_tools.py` | `_prepare_rom_download`、两阶段 `--compat` stub / 快写 |

---

## 10. 相关文档

- NuttX 启动与隐性注册：[nuttx_boot_flow.md](nuttx_boot_flow.md)
- SD 分区 / 160 KiB boot 槽：[sd_partition.md](sd_partition.md)
- 工厂固件分区与跳转：[factory_firmware.md](factory_firmware.md)
- OVNX 镜像：[nuttx_ovnx_image.md](../scripts/nuttx_ovnx_image.md)
- 编译烧录命令：[build_guide.md](build_guide.md)、[tools/vela_my_vendor_tools.md](tools/vela_my_vendor_tools.md)
- SD LittleFS 几何踩坑：[debug/mklfs.md](debug/mklfs.md)

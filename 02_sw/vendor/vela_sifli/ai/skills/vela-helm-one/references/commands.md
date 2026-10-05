# 构建、烧录、监视与机上探针

## 1. 构建（`vendor/my_vendor/build_board.py`，或其 shell 包装 `vela` 工具）

```bash
# 日常（应用 / NuttX / 驱动改动）
python3 vendor/my_vendor/build_board.py build

# 全量：main + factory + boot（+ fs）
python3 vendor/my_vendor/build_board.py build-all

# 单件
build-boot      # ftab.bin + bootloader.bin → boot_loader/bin/
build-factory   # 工厂变体 configs/nsh-factory
build-fs        # 打包 LittleFS → boot_loader/bin/fs_root.bin
wrap            # nuttx.bin → nuttx.flash.bin（OVNX 头 + CRC）
pack-fw         # 同上，并产出 fw/1.0.0-Helm-One.bin 供 OTA（别名 pack-ota）
pack-sd-img     # SD 整盘 .img（ftab + bootloader + main + factory + LFS 种子）
menuconfig / savedefconfig
```

`build` 会自动：缺 boot 镜像则 `build-boot`；`CONFIG_MYVENDOR_BICYCLE=y` 且缺 `fs_root.bin` 则 `build-fs`（**仅缺失时**）；`AUTO_WRAP_NUTTX=True` 则 `wrap` 并写 OTA 文件；最后写 `flasher_args.json`。

## 2. 烧录

| 命令 | 内容 |
|---|---|
| `flash` | `ftab.bin` + `bootloader.bin` + `nuttx.flash.bin`（**不含** fs / factory） |
| `flash-factory` | 仅工厂固件（SD `0x62500000`） |
| `flash-fs` | 仅 `fs_root.bin`（`@ 0x63800000`） |
| `flash-all` | `sftool_param.json` 全部条目 |
| `burn-sd --sd /dev/sdX` | 整盘 SD 镜像（前 9 MiB 密写，LFS 段 sparse） |

选择原则：**只改代码/固件 → `build` + `flash`**；**改了分区表或 boot → `build-boot`（或 `build`）再 `flash`**；**改了文件系统内容 → `pack-sd-img` + `burn-sd`**。
可加 `-p PORT`、`--force`。

## 3. 串口监视（AI 可读）

```bash
# 终端 1：操作者 —— 原命令即可
python3 vendor/my_vendor/docs/tools/vela_my_vendor_tools.py monitor

# 终端 2：AI / 脚本 —— 不带命令就是监听
python3 vendor/my_vendor/docs/tools/serial_hub.py        # 转发枢纽（/tmp 下的"文件"其实是个程序）
python3 vendor/my_vendor/docs/tools/pty_run.py           # 后台跑 monitor
python3 vendor/my_vendor/docs/tools/probe_boot.py        # 启动链路探针
```

默认 **1 000 000** 波特，支持 NSH 与 bttool。若出现「进程里没有读串口的线程」，见 `docs/tools/vela_my_vendor_tools.md` §8.4 的现场处置。

## 4. 机上探针（nsh）

原则：**每个改动都要有一个能在板上直接读出状态的手段**，不要只靠日志推断。

| 类别 | 探针 |
|---|---|
| 分区 / 启动 | `ptab`（打印编译进固件的表）、`bootinfo`、`df -h`（板级 uint64 版本） |
| SD / 文件系统 | `ctl sd read\|write`、`sr=` / `why=` 取证字段、`mount` 重挂阶梯（rw → ro → 重启） |
| 电源（调试） | `ctl pwr on\|off`（写 `persist.boot.pwr`，2SFBL 据此 autoboot；**量产不要开**） |
| 传感器 / I2C | `i2c` 工具（`apps/system/i2c`）、BMI270 / BMP388 / MMC5983 各自 reopen |
| 蓝牙 | diag `ble` 槽 + companion 日志；手工 `nsh> ble_companion &` 只用于对比时序 |
| GNSS | `gnss: cfg ack ... pvt=`、`gnss: first pvt len=92`（92 字节是 M10 PVT 定长） |
| 看门狗 / 诊断 | diag 四槽 `ble`/`gnss`/`dvfs`/`fs`，`myvendor_diag_register` 应有 **4 次**调用 |

完整探针清单以 `docs/diag.md`、`docs/sd_recovery.md` §7、`docs/ble/*` 为准。

## 5. PC 端地图工具链（`docs/osm/`）

```bash
docs/osm/make_china_map.sh        # 全国
docs/osm/make_cities_map.py       # 城市合包（见 CITIES_MAP.md）
docs/osm/build_vmap.py            # 切 3 km 网格 .vpk
docs/osm/build_vgraph.py          # 路由图 / portal（--graph、--rebuild-portals）
docs/osm/fetch_dem_china.py       # DEM 高程
docs/osm/subset_font.py           # 地图字体裁剪
```

产物布局与命名规则必须与固件侧 `vmap_grid.c` 一致（3 km、原点 `(0°,0°)`、`lon<ix/4>/lat<iy/4>/x<ix>_y<iy>.vpk`）。改一端等于改两端。

## 6. 提交前

```bash
# 钩子式代码审查（mimosa），报告落 .mimosa/reports/
# 确保 logs/ 下的 AI Coding 日志完整、未被手工改动
```

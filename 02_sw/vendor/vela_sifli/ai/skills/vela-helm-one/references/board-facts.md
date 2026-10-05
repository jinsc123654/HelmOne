# 板级事实表（权威值）

> 这些值是**结论**，不是建议。改任何一项前先回到「唯一源」文档确认，并同步所有副本。
>
> **开发主树：`vendor/my_vendor`。** `vendor/HelmOne` 仅打包用快照，**非特殊声明不要改**
> （见该目录 `README.md`）。

## 1. 硬件与软件基线

| 项目 | 值 |
|---|---|
| SoC | SF32LB52，双核（HCPU + LCPU BT ROM），外部 PSRAM（APS，MPI1），16 MB 级 |
| 显示 | 2.4 英寸 **NV3031A 240×320**，RGB565，**无触摸**，半透半反（MIP） |
| 定位 | u-blox **MAX-M10S** + 外置天线，UBX-NAV-PVT 定长 **92 字节** |
| 运动传感器 | **BMI270**（IMU）、**BMP388**（气压）、**MMC5983**（磁力）、VBATS ADC |
| 存储 | microSD（SDIO/SPI），三卷：KV 256 MiB + 用户 LittleFS 512 MiB + FAT（CSD 余量） |
| 系统 | openvela（NuttX 内核）+ LVGL 9 + FreeType |
| 固件槽 | `main`（产品）与 `factory`（工厂）两份独立 OVNX，一次只跑一个 |
| 二级 boot | 2SFBL，卡上槽 **160 KiB**，SRAM 拷贝 `@ 0x20020000`，data RAM `@ 0x20048000`（200 KiB） |

## 2. SD 卡分区（唯一源 `boot_loader/config/nsh/ptab.sdmmc.json`）

卡字节偏移 = SBUS 地址 − `0x62000000`。

| 卡偏移 | 大小 | tag | 用途 |
|---|---|---|---|
| `0x00000000` | 4 KiB | `MBR` | 磁盘头占位 |
| `0x00001000` | 64 KiB | `ftab` | 二级 boot 查表 |
| `0x00011000` | 160 KiB | `bootloader` | 2SFBL 镜像 |
| `0x00039000` | `0xC7000` | `BOOT_RESERVE` | 把 boot 带垫到 1 MiB |
| `0x00100000` | 4 MiB | `main` | 产品固件槽，SBUS `0x62100000` |
| `0x00500000` | 4 MiB | `factory` | 工厂固件槽，SBUS `0x62500000` |
| `0x00900000` | 128 MiB | `COREDUMP` | raw 占位，**不是** LittleFS |
| `0x10000000` | 256 MiB | `KV_REGION` | `/mnt/kv`（persist + `fw/` OTA 槽 + `db/persist.boot.target`） |
| `0x20000000` | 512 MiB | `FS_REGION` | `/mnt/lfs`（MTP / GPX / 星历 / 字体；**不含地图**） |
| `0x40000000` | `0`（余量） | `FAT_REGION` | `/mnt/fat`，根下 `map/` 与 `fonts/`；产品只读、工厂读写 |

锚点规则：**1 MiB / 256 MiB / 512 MiB / 1 GiB 钉死**，加大卡只加长 FAT。
MDT 几何：LittleFS 看 page 512 / erase 4096。

改表必动 5 处：`ptab.sdmmc.json` → 两份 `ptab_sdmmc.h` → 两份 `ptab_table.{c,h}`（`gen_ptab_table.py`，`build` 与 `boot_loader/build.sh` 也会自动刷）。

> **已知文档不一致（待统一）**：`docs/map/RUNTIME.md` 写 `/mnt/lfs/map/...`，而
> `docs/sd_partition.md` 明确地图在 `/mnt/fat`、且「不要把地图再放进 `/mnt/lfs`」
> （`lfs_alloc` 扫树会卡十几秒）。**以 `sd_partition.md` 为准**。

## 3. 引脚（源 `docs/gpio_pinmux_guide.md`）

| 外设 | 引脚 | 信号 |
|---|---|---|
| UART1（console） | PA18 RX / PA19 TX | `/dev/console` |
| UART2（log） | PA20 RX / PA27 TX | `/dev/ttyS0` |
| I2C1（传感器/触摸） | PA30 SCL / PA33 SDA | `/dev/i2c0` |
| USB Device | PA35 / PA36 | USB_DP/DM |
| SDIO | PA12～PA17 | `SD1_DIO2/3/CLK/CMD/DIO0/1`（与 MPI2 互斥） |
| SPI1（TF 卡备用） | PA24/25/28/29 | `SPI1_DIO/DI/CLK/CS` |
| LCD QADSPI | PA01 背光 PWM；PA02～PA08 LCDC1_SPI_*；PA00/PA10 复位/电源 | |
| 按键 Key2 | PA11（下拉） | |
| 电源键 Key1 | PA34（注释掉，保留下拉以免影响 UART 烧录） | |
| LED / RGB | PA32 | `GPTIM2_CH1` |
| VBUS 检测 | PA44 | |

三处不要搞混：`bf0_pin_const.c`（脚**能不能**接某外设）、`bf0_hal_pinmux.c`（`HAL_PIN_Set` 如何路由到实例）、`bsp_pinmux.c`（**本板**实际接在哪）。

时钟：LCD SPI 期望 ≥62.5 MHz 时驱动 clamp 到 **48 MHz**；SD 外部 48 MHz 不可用（见 `sd_recovery.md` §8 时钟拓扑）。

## 4. 阈值与上限（源 `docs/system_states.md` §4，改这里必须同步该文档）

**上屏**

| 项 | 值 | 符号 |
|---|---|---|
| 屏幕轨迹折线 | 384 顶点 | `BICYCLE_TRACK_DRAW_CAP` |
| 公里标记 | 150 槽 | — |
| 导航折线 | 2048 点（**超了整条路线被拒**，不截断） | `VMAP_ROUTE_MAX_PTS` |
| 转向提示 | 64 条（超了静默截断，导航照走） | `VMAP_ROUTE_MAX_MANEUVERS` |
| 爬升剖面 | 60 点 / 近 30 分钟（30 s 一点） | `HELM_CLIMB_REC_MS` |
| 数据走势 | 约 60 s；均速窗口约 30 s | — |
| 地图缩放 | 级别 11–15 | — |

**存储**

| 项 | 值 | 符号 |
|---|---|---|
| 屏幕轨迹环 | 约 5 万点、点距 3 m（约 150 km/圈） | `BICYCLE_TRACK_POINT_MIN_DIST_M` |
| GPX 记录采样 | 约 1 s 一点（不受 384 点限制） | — |
| GPX 导航窗口 | 每次约 8 km，余约 2.5 km 续载 | — |

**交互与电源**

| 项 | 值 | 符号 |
|---|---|---|
| 自动计圈 | ≥2 km + 回到起点 50 m 内 + 航向偏差 ≤90° | `VMAP_LAP_MIN_DISTANCE_M` / `_CLOSE_RADIUS_M` / `_MAX_COURSE_DIFF_DEG` |
| 自动暂停 / 恢复 | 静止 4 s / 恢复 1.5 s | `HELM_AUTOPAUSE_MS` / `HELM_AUTORESUME_MS` |
| 暂停抑制 | 手动继续后 1 min 内不再自动暂停 | — |
| 停表 / 恢复速度 | 1.8 / 2.5 km/h（0.7 迟滞） | `BICYCLE_RIDE_MOVE_KPH` / `RESUME_KPH` |
| 累计爬升迟滞 | 3 m | — |
| 偏航 / 到达 | 35 m 重规划 / 45 m 到达 | `VMAP_ROUTE_OFF_M` / `VMAP_ROUTE_ARRIVE_M` |
| 通知收件箱 | 8 条 | — |
| 途经点 / 常用点 | 各 32 | — |

**界面口径**（中文 / 顶栏 / 代码必须一致）：记录中=REC、导航中=NAV、暂停=PAUSE、待机、途经点（**不是**「途径点」）、常用点、返航、偏航、到达。

**降级策略**：一路失效只降一路 —— 板载定位失效可用手机位置临时顶替（锁星后以板载为准）；卫星质量差抑制飞点、短时失锁保持上一拍时速（**不跳 0**）；外设失联保留上次有效值；手机断连不影响记录/离线导航/返航；整机真卡死才看门狗复位。

## 5. 地图与路由

| 项 | 值 |
|---|---|
| 网格 | **3 km 固定网格**，原点 `(0°,0°)`，文件名 `x<ix>_y<iy>.vpk`，目录 `lon<ix/4>/lat<iy/4>/` |
| 索引 | 网格模式**无顶层 `map.idx`**（新包不要写） |
| 跨区 | 读每个 vpk 尾部 **VPOR**（忽略顶层 `route.port`）+ 邻区图 BFS 拼接；依赖制作时 `--graph` 与 `--rebuild-portals` |
| 可视范围 | 240×320 屏、scale 0.25× 时约 13 km，常跨多个 cell |
| 缩放与瓦片 | z14 时一个 cell 约 2×2～3×3 块瓦片 |
| 读盘代价 | 单个 cell `.vpk` 冷读约 50–150 ms；一次 `fread` 不可被预算中断 |
| 加载/渲染 | 跟车只平移画布（不读盘）；越界/换缩放走 `render_timer` 20 ms 分片，单拍 `max=2, 8 ms` |

三城离线样本：5 925 个分片、约 314 MB。路线：堆式 Dijkstra + 跨网格 portal 拼接。

## 6. 双端协议（Companion）

- 固件侧 `companion_proto.h` ↔ App 侧 `companion_proto.dart`：**版本号与特征号必须双端同步**，改一端必改另一端。
- 单射频同时双角色：GATT Server（对手机）+ GATT Central（心率/踏频/功率）。
- 已知限制：传感器绑定未用 IRK，随机地址配件需固定地址才能稳定回连。
- **手机定向 ADV 不支持（2026-09-22 实测）**：`ADV_DIRECT_IND` + `DIR_ADDR_RPA` 在 SF32
  上稳定 `adv start failed status=2`；产品路径用可连接无名 ADV + 准入/ENCRYPT。
  勿再打开定向广播。
- **配对窗口内射频只给手机 ADV（2026-09-23）**：开窗上升沿停传感器扫/拆链
  （保留 `want_link`，关窗后回连），禁止 `hold phone adv`；窗内不建传感器连接。
  修现场「window open 后整段 adv held by sensor / busy=1、手机扫不到」。
- **手机绑定最多 3 台（IRK/CONFIG_BT_MAX_PAIRED=3）**：`bt_phone.tsv` 多行名单；
  码表「手机蓝牙」页列出手机1..3，可再配对至满，长按单台解绑。
  **注意**：改 `defconfig` 后必须让 `cmake_out/.../include/nuttx/config.h` 重生
  （删 config.h 再 build）；`.config=3` 而 config.h 仍为 1 时运行态仍是
  `phones 1/1`、密钥池只 1 槽，第二台手机会报 PIN 错且无配对弹窗。
- **开配对窗口清拒连冷却**：避免 grace 踢过后 60 s cooldown 挡掉窗内再配。
- **RPA 宽限**：名单外地址 3 s 等 L2 + 60 s 拒连冷却（防陌生人空连占 ADV）。
- BLE 文件传输约 15 KB/s（带 CRC32，支持偏移续传），大文件走 USB MTP。
- 星历：App 取当天广播星历 → 转 UBX-MGA → 注入模组，连接期间每 2 h 同步。
- OTA：SHA-256 + 长度校验，版本号 + 硬件型号双重校验，槽位 `/fw`（最多 12 个版本），回退 `main → factory`。

## 7. GNSS 速度口径

- 默认 `g_gnss_rmc = 1`：信模组多普勒 SOG；`0` 走 UI 自定义位移窗滤波（KV 键 `MYVENDOR_DEVCTL_GNSS_RMC_KEY`）。
- PVT 可用时用 `gSpeed`（模组滤波）；`sAcc > max(0.6 m/s, 30% × v)` 判最低档，只降不升。
- 静止门：`RT_STILL_ENTER_M 3.0` / `RT_STILL_EXIT_M 8.0` / `RT_STILL_HOLD_MS 4000`。
- 两个源都不可信时**冻结**上一次输出（`hAcc > RT_POS_BAD_M`）。

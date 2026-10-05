# 强制约束与禁止事项

违反下面任何一条的改动，即使「看起来能跑」，也要打回。

## 1. 单一事实源（Single Source of Truth）

| 主题 | 唯一源 | 同步目标 |
|---|---|---|
| SD 分区布局 | `boot_loader/config/nsh/ptab.sdmmc.json` | 两份 `ptab_sdmmc.h` + 两份生成的 `ptab_table.{c,h}` |
| 状态 / 阈值 / 文案 | `docs/system_states.md` | UI 代码常量、本 Skill `board-facts.md`、报告口径 |
| 板级引脚 | `docs/gpio_pinmux_guide.md` + `bsp_pinmux.c` | — |
| 地图格式与打包 | `docs/map/RUNTIME.md`、`docs/osm/` | `pack_map.py` 与 `vmap_grid.c` 必须同源同规则 |
| 双端协议 | `companion_proto.h` ↔ `companion_proto.dart` | 两端同版本、同特征号 |

**规则**：改阈值先改 `system_states.md`，再改代码；改分区先改 JSON，再生成表。不允许「只改代码、文档回头补」。

## 2. 不许动的文件

- `nuttx/fs/littlefs/lfs.c`、`lfs_vfs.c` —— LittleFS 的窗口扩展走板级 `chips/sf32lb52/sf32lb_lfs_super.c`（`lfs_fs_grow`）。
- `nuttx/fs/mount/fs_procfs_mount.c` —— `df -h` 的 64 位乘法走 `vela_override/fs/fs_procfs_mount.c`。
- `nuttx/fs/fat/fs_fat32util.c` —— FAT32 判定走 `vela_override/fs/fs_fat32util.c`。
- 生成产物（`ptab_table.*` 等）手工编辑 —— 必须改源再生成。
- `logs/` 下的 AI Coding 日志 —— 由插件自动写入，**不得手工删改**。

改 `vela_override/` 后，配置日志里必须能看到对应的抽换记录（例如
`my_vendor: fs uses board fs_procfs_mount.c`），否则抽换没生效。

## 3. 上板与验证

- 任何改动都要在真机验证：**nsh 探针 → 串口结构化日志 → diag 巡检**，三者取证据。
- 不要用「模拟/静态推理」替代真机结论；不要把未验证的 AI 建议直接写进文档当事实。
- 烧录与调试必须走指定工具链（`build_board.py` / `vela_my_vendor_tools.py` / `serial_hub.py`），不要手写 `dd` 或第三方烧录器。
- 只改固件、分区位置变了：UART 烧新 `nuttx.flash.bin` 即可；只有动了 `/mnt/lfs` 内容才需要 `pack-sd-img` / `burn-sd`。

## 4. 存储与地图

- 不要把地图放进 `/mnt/lfs`（`lfs_alloc` 会扫整棵树，上千 `.vpk` 卡十几秒）。
- 不要在卡仍处于 DATA/RCV 态发 CMD8 做全量识别；先 CMD12 + CMD13 回 TRAN。
- 不要在窗口 < 4 GiB 之外的地方做 `uint32` 的容量乘法。
- 不要在 LittleFS 上 silent shrink / 打密集体 KV / `dd` 整段 KV。
- 不要在产品固件里自动 `mkfatfs`（会抹掉地图）。
- 不要把用户 LFS 放到 512 MiB 以下（那是杂项带）。

## 5. 交互与状态

- 整机只有 KEY1 / KEY2（另有电源键），**无触摸**：新增交互必须在双键矩阵内表达，且不得在菜单打开时误触发记录。
- 一路失效只降级这一路，**不整机复位**；看门狗只在真死时动手。
- 界面文案严格按 `system_states.md` §5 的中/英/代码三列口径，新增文案同表登记。
- 改 UI 刷新时，不要在加载路径里伪造 UI 心跳（会掩盖真实卡顿）。

## 6. AI / 流程

- 提交前跑钩子式代码审查（mimosa），把「发现 → 根因 → 处置」记录进仓库文档，形成可复用记忆。
- 报告 / 文档里的数字必须能追溯到代码常量或实测日志，不允许估算值当成实测值。
- 每个非平凡问题的收尾动作：写进对应 `docs/*.md`，并在本 Skill 的 `troubleshooting.md` 增一行索引。

# 文件系统固件（按版本号）

产品固件优先从 **`/mnt/kv/fw/<版本>.bin`** 加载进 PSRAM，SD 分区 `main` 只作回退。  
2SFBL 默认就挂着 KV，不必再切到用户 FS。

上传走 **BLE OTA 专属路径** `fw/`（落到 `/mnt/kv/fw`），或 **工厂 MTP** 的 KV 卷。  
产品 BLE 文件管理器仍沙箱 `/mnt/lfs`（地图 / 轨迹 / 图标），看不到固件槽。  
产品 MTP 只暴露 `/mnt/lfs/mtp`。

相关：[boot_2sfbl.md](boot_2sfbl.md)、[factory_firmware.md](factory_firmware.md)、[sd_partition.md](sd_partition.md)。

日期：2026-08-27。

---

## 1. 命名

NuttX 开机 `mkdir /mnt/kv/fw`。KV 卷根对应 2SFBL 的 `/`，所以 boot 侧路径是 **`/fw`**。

`wrap` / **`pack-fw`** 之后会在输出目录生成烧录镜像，并在 `fw/` 下改名为 OTA 上传名（必须以 `X.Y.Z` 开头，2SFBL 按文件名排版本）：

```
cmake_out/my_vendor_nsh/nuttx.flash.bin          ← sftool 烧分区 main
cmake_out/my_vendor_nsh/fw/1.0.0-Helm-One.bin    ← 拷到设备 /mnt/kv/fw/
cmake_out/my_vendor_nsh/fw/1.0.0-Helm-One.txt    ← 版本 / CRC / 大小
```

```bash
python3 vela_my_vendor_tools.py pack-fw
```

版本来自 Kconfig `CONFIG_MYVENDOR_PRODUCT_VERSION`（`chips/sf32lb52/Kconfig`，产品 `configs/nsh/defconfig`），wrap 再拼 `-g` + git short hash。改产品版本号就改那个宏，不要改 `app_version.txt`（仅作缺省回退）。

| 路径（NuttX） | 说明 |
|---------------|------|
| `/mnt/kv/fw/1.0.1.bin` | 纯版本号也可以 |
| `/mnt/kv/fw/1.0.0-Helm-One.bin` | **`pack-fw` 默认产出**（产品名来自 `CONFIG_MYVENDOR_PRODUCT_NAME`） |
| `/mnt/kv/fw/*.bin` | 任意以 `.bin` 结尾的 OVNX；`a.bin` 仍可用（当成 0.0.0） |

至少留两份不同版本：新镜像 CRC 失败时 2SFBL 会试次新的，再回落到分区 `main`。KV 一共 256 MiB（还有 persist / 蓝牙），不要把地图丢进这里。

量产可以不再烧 `main`：没有合法 `/fw/*.bin` 时再试工厂分区。工厂固件仍在独立槽，`factory` / 10×`F` 不变。

旧设备若还把文件放在 `/mnt/lfs/fw`，boot **不会**再读；请拷到 `/mnt/kv/fw`。BLE 若仍写相对路径 `fw/…` 或绝对路径 `/mnt/lfs/fw/…`，固件会改写到 KV。

---

## 2. 谁算「最新」

扫描 KV `/fw` 下全部 `*.bin`（最多 12 个，多了丢掉版本最低的）：

1. 从**文件名**解析开头的 `X.Y.Z`（可带 `v` 前缀；`1.0.0-gabc.bin` → 1.0.0）
2. 同版本再用 OVNX 头里的 **`build_unix`**（后编的赢）
3. 按这个顺序做 CRC，第一份成功的拷进 PSRAM

不要靠 `active` 文件。新版本用新文件名，旧文件留着当回退。

---

## 3. 启动顺序（产品 `app` / `boot` / 发布 autoboot）

上电先读 KV **`persist.boot.target`**（boot 路径 `/db/persist.boot.target`，NuttX `/mnt/kv/db/persist.boot.target`）。这是 Vela KVDB file 后端的普通 persist 键。

| 值 | 行为 |
|----|------|
| 读不到 / 空 / 非法 | 下面默认链（`/fw` → `main` → `factory`，不弹「固件异常」） |
| `fw` / `main` | **只**加载该槽（主槽）；CRC 失败提示固件异常并进 factory |
| `factory` | **只**加载 factory；CRC 失败提示芯片损坏并留 msh |
| `boot` | 留在 2SFBL msh |

无标记时：

```
2SFBL 已挂 KV_REGION
        │
        ├─ 列 /fw/*.bin，按版本从高到低
        ├─ 试最新 OVNX → CRC OK → PSRAM 0x10000000 → run_img
        ├─ 失败再试次新……
        ├─ 都失败 → 分区 main
        └─ main 也失败 → 分区 factory
```

- 上电 **10×`F`/`f`** 或 msh **`factory`**：只走分区 `factory`，不读 `/fw`（热键压过 KV target）。
- 上电 **10×`B`/`b`**：留在 msh，不跳转。
- msh **`target [fw\|main\|factory\|boot\|off]`**：改/清 persist 标记；下次复位生效。NuttX 里 `setprop persist.boot.target factory` 等价。
- msh **`boot`/`app`**：立刻走默认链，不读 KV target。
- msh **`fw`**：列出将要尝试的顺序（最上为最新）；`ls /fw` 也可。
- msh **`check`**：CRC 校验 `/fw`、`main`、`factory` 是否完整且复位向量可跳，不跳转。
- 2SFBL **不 format** KV。

---

## 4. 怎么上传

把 **`pack-fw`** 产出的 **`fw/<X.Y.Z-Product>.bin`** 拷到设备 **`/mnt/kv/fw/`**，**不要覆盖**还想留着回退的旧版本。改产品版本号就改 `CONFIG_MYVENDOR_PRODUCT_VERSION`。

| 通道 | 可见范围 | 做法 |
|------|----------|------|
| BLE OTA | 专属沙箱 `fw/` → `/mnt/kv/fw` | 相对路径 `fw/1.0.1.bin`（或绝对 `/mnt/kv/fw/…`） |
| BLE 文件管理器 | `/mnt/lfs` | 轨迹 / 图标；**列根目录看不到** `fw/`；地图在 `/mnt/fat/map` |
| 工厂 MTP | `/mnt/lfs` + `/mnt/kv` + `/mnt/fat` 三卷 | 在 **KV 卷** 建 `fw/`；地图拷到 **fat 卷的 `map/`** |
| 产品 MTP | 只有 `/mnt/lfs/mtp` | **不能** 改固件槽；看不到地图 |

---

## 5. 与分区固件的关系

```
boot (2SFBL) + factory 分区 + main 分区 + KV /fw/<ver>.bin（≥2 份）
```

| 镜像 | 从哪来 | 谁跳 |
|------|--------|------|
| 2SFBL | SD `bootloader` | Mask ROM |
| 工厂 NuttX | SD `factory` | msh `factory` / 上电 10×`F` / `target factory` / `/fw` 与 `main` 都失败时的 autoboot |
| 产品 NuttX | 最新合法 KV `/fw/*.bin`，否则 `main` | `app` / 默认 autoboot / `target fw` 或 `target main` |

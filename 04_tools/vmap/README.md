# OSM 离线矢量地图工具链

本目录是 **bicycle 码表矢量地图** 的 PC 端工具：从 OpenStreetMap 抓取数据、转换为自定义 `.vt` 瓦片，打包成设备用的 `.vpk`，供后续烧进固件。

**Canonical 路径**（HelmOne 产品仓库）：

```
HelmOne/04_tools/vmap/
```

固件树（`02_sw/vendor/vela_sifli`，git submodule）在本工具链里**只作为输出目标出现**：
`make_chuzhou_map.sh --install` 把打好的地图拷进
`<fw>/boards/sf32lb52/my_vendor/mkfs/fat/map/`。

固件树位置按这个顺序解析（见 [`vmap_paths.py`](vmap_paths.py)）：

1. `$HELMONE_FW`（显式覆盖）
2. `04_tools/vmap/../../02_sw/vendor/vela_sifli`（本仓库布局）
3. `../../vendor/vela_sifli` / `../../vendor/my_vendor`（openvela SDK 里就地跑）

> 本工具链原先位于固件树的 `vendor/my_vendor/docs/osm/`。搬出来时只改了**找路径的方式**，
> 转换/打包逻辑本身逐字节未动（`git archive` 取出的 tracked 文件）。
>
> **依赖字体**：地图路名/湖名的字形子集由固件树的
> `docs/font/subset_font.py`（`--sync-myvendor`）负责，它扫的是**固件树的
> `docs/osm/vmap/`**（即这套工具在固件树就地跑时的产出目录；
> 注意它的 `DEFAULT_VMAP_DIR` 写死在固件树里，不是本目录的 `map/`）。
> 字体工具链仍留在固件树里 —— 不在这里再放一份，避免两处漂移。

> 当前设备地图请使用 [`CITIES_MAP.md`](CITIES_MAP.md)（滁州/南京/广州，3 km
> `lon*/lat*/x*_y*.vpk`）。旧 5 km 脚本见 [`CHUZHOU_MAP.md`](CHUZHOU_MAP.md)。

---

## 目录结构

```
04_tools/vmap/
├── README.md           ← 本文件（操作指南）
├── vmap_paths.py       ← 新增：解析 tools/ 与固件树位置
├── view_map.sh         ← 一行启动地图预览器（看 map/）
├── map/                ← 内置演示包：北京 64 块 .vpk（见下节）
├── CITIES_MAP.md       ← 滁州/南京/广州 3 km 网格合包（当前）
├── CHUZHOU_MAP.md      ← 旧 5 km dNNN/rNNN 滁州脚本
├── ROUTING.md          ← 路网格式、Dijkstra、偏航重规划、界面提示
├── DESIGN.md           ← 系统架构与二进制格式（开发者向）
├── MAP_RECORD.md       ← 当前滁州 demo 数据集记录
├── requirements.txt    ← Python 依赖
├── build_vmap.py       ← OSM XML → .vt 瓦片（支持 --bbox 任意矩形）
├── build_vgraph.py     ← OSM XML → graph.vgrf（VGRF v2 + SNAP + ELEV）
├── dem.py              ← SRTM .hgt / Terrarium PNG 采样海拔
├── make_map.sh         ← 兼容入口，转发到 make_chuzhou_map.sh
├── make_chuzhou_map.sh ← osm_data/chuzhou → 原版 5 km 本地网格地图
├── make_china_map.py   ← 全国一键：china-latest.osm.pbf → osm_data/map/
├── make_china_map.sh   ← 同上（检查 venv / osmium）
├── make_cities_map.py  ← 滁州/南京/广州合进 3 km 网格（当前设备包）
├── make_cities_map.sh  ← 同上
├── tools/              ← 打包器（原 boards/.../ui/bicycle/tools/）
│   ├── pack_vmap.py    ← .vt → .vpk 分片
│   ├── build_vmap.py   ← pack_vmap.py 的别名入口（注意与上层同名文件不同用途）
│   ├── pack_map.py     ← 瓦片 + 路网 → 设备 map/（按格分包）
│   ├── vgrf_clip.py    ← VGRF 按 bbox 裁剪
│   └── portal_match.py ← 相邻格 portal 配对
├── web/                ← 网页版框选生成器（单次 ≤3°，见 web/README.md）
├── extract_osm_chars.py← 全国 PBF 抽名称字表（繁转简）
├── zh_simplify.py      ← 繁→简单字 + 设备字库过滤
├── data/
│   ├── TSCharacters.txt
│   └── china_osm_chars.txt  ← 字库底表
├── .venv/              ← 本地 Python 虚拟环境（不入库，首次跑脚本自动建）
├── osm/
│   └── chuzhou.osm     ← Overpass 下载的原始 OSM XML
├── graph.vgrf          ← build_vgraph 产物（pack 时打进 p*.vpk）
└── (vmap/)             ← 转换产物，**不随仓库发布**，跑 build_vmap.py 时生成
```

> ⚠️ 两个 `build_vmap.py` 用途不同：上层的 `build_vmap.py` 是 **OSM XML → .vt**；
> `tools/build_vmap.py` 是 **.vt → .vpk**（`pack_vmap.py` 的别名）。
> 历史原因同名，别混。

| 产物 | 用途 |
|------|------|
| `osm/*.osm` | 原始 OSM，可重复转换、换 zoom/半径 |
| `vmap/` | **转换产物**（loose `.vt` 树，或 `--pack-bytes` 的 packed 形态）。**一次 `build_vmap.py --out vmap` 就能重生成**，所以不占仓库位置：要看地图直接 `./view_map.sh` 看 `map/` |
| `graph.vgrf` | 离线路网（VGRF v2）；**必须与 vmap 一并** `pack_map.py` 进设备 |
| `<fw>/boards/sf32lb52/my_vendor/mkfs/fat/map/` | **packed** 统一包，烧录到 SF32 |

> 注：`vmap/` 与 `map/` 不是一回事 —— `map/` 是**烧进设备的成品包**（本仓库带了一份北京演示集，
> 见下节）；`vmap/` 只是生成 `map/` 之前的中间产物，随用随生成，不进仓库。

---

## 内置演示地图 `map/`（一行启动预览）

`04_tools/vmap/map/` = **北京 64 块** `.vpk`（约 7 MB，经度 116.265~116.528 E /
纬度 39.777~39.993 N），格式与烧进设备的包**完全一致**：
`lon<ix//4>/lat<iy//4>/x<ix>_y<iy>.vpk`，没有 `map.idx`。

```bash
cd 04_tools/vmap
./view_map.sh                          # 浏览器直接打开，默认就是 map/
./view_map.sh --no-open --port 8791    # 不弹浏览器、指定端口
./view_map.sh /path/to/other/map       # 看别的包（老的 map.idx 布局也认）
VMAP_CHINA_MAP=/path/to/map ./view_map.sh
```

预览器实现是 `web/view_map.py`（stdlib 起一个 HTTP 服务 + Leaflet 前端），
接口 `/api/info`、`/api/catalog` 会报出块数、经纬范围与每块字节数；
`/api/info` 里 `grid: true` + `vpk: 64` 就是它在读本目录。

**这份数据从哪来**：是固件树
`boards/sf32lb52/my_vendor/mkfs/fat/map/` 那个全国包里的一个子集，**逐字节相同**
（抽样比对）。放进仓库的目的是**不跑 OSM 生成流程也能演示** ——
全国包本体 7.8 GB / 58 万文件，不入库。
后续文档里的截图与演示统一引用这条路径。

> ⚠️ 固件树里那份 `mkfs/fat/map/**` 是 `.gitignore` 掉的（构建产物），
> 所以**只有 `04_tools/vmap/map/` 这份是版本化的**。要长期留住的演示数据放这里。

---

## 端到端流程（SF32LB52）

```
Overpass API
    ↓  curl / make_map.sh fetch
osm/chuzhou.osm
    ↓  build_vmap.py（z14 + z15）
docs/osm/vmap/          ← loose .vt
    ↓  build_vgraph.py
graph.vgrf              ← VGRF v2（12 m 节点缝合 + 200 m SNAP + 可选 ELEV）
    ↓  pack_map.py（瓦片 + graph → 同一组 p*.vpk）
boards/.../mkfs/fat/map/      ← lon*/lat*/x*_y*.vpk
    ↓  build-fs + flash-fs
设备 /mnt/lfs/map/
    ↓  vmap + vmap_route
LiveMap 矢量底图 + 离线路线导航
```

Goldfish 模拟器仍走 `make_map.sh` 后半段（同步 loose `vmap` 到 ROMFS），**不含** packed graph；导航功能仅在 SF32 设备验证。

导航算法、偏航重规划与界面提示详见 [ROUTING.md](./ROUTING.md)。

---

## 环境准备

```bash
cd vendor/my_vendor/docs/osm
# 或：cd /home/jinsc/SDK/vela/osm

python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
```

依赖：`requests`、`osmium`（pyosmium）、`shapely`。

---

## build_vmap.py

将 OSM XML 切片为 slippy-map 瓦片，写出 **VTIL** 格式 `.vt` 文件。

### 基本用法

```bash
.venv/bin/python build_vmap.py \
  --osm osm/chuzhou.osm \
  --center-gcj 118.3175,32.3025 \
  --zoom 14 \
  --radius-deg 0.12 \
  --out vmap
```

对 z15 再跑一遍（追加到同一 `vmap/` 树）：

```bash
.venv/bin/python build_vmap.py \
  --osm osm/chuzhou.osm \
  --center-gcj 118.3175,32.3025 \
  --zoom 15 \
  --radius-deg 0.12 \
  --out vmap
```

### 主要参数

| 参数 | 说明 |
|------|------|
| `--osm PATH` | 输入 OSM XML |
| `--center-gcj LON,LAT` | 切片中心（**GCJ02**，高德/腾讯坐标） |
| `--radius-deg R` | 半宽（度）；`0.12` ≈ 城区 13 km 量级 |
| `--zoom Z` | 瓦片层级（设备底图用 **14**，备用 **15**） |
| `--out DIR` | 输出根目录，生成 `<out>/<z>/<x>/<y>.vt` |

### 坐标系

| 坐标系 | 用途 |
|--------|------|
| **GCJ02** | 人输入中心点（`--center-gcj`） |
| **WGS84** | OSM、GPS、设备内部、瓦片编号 |

工具内 `gcj02_to_wgs84()` 仅用于把中心点换算后再切片；**设备端不再做 GCJ 偏移**。

### 抓取的数据类型

Overpass 查询（见 `make_map.sh`）包括：

- 道路 `highway=*`
- 水系线 `waterway`
- 水面 `natural=water`、`water=*`、`landuse=reservoir`
- 林地 `natural=wood`、`landuse=forest`
- 地名/POI 节点（`place`、`peak`、`tourism`、`historic` 等带 `name`）

### 输出统计（滁州 demo，参考）

| 层级 | 瓦片数 | x 范围 | y 范围 |
|------|--------|--------|--------|
| z14 | 131 | 13571–13581 | 6631–6642 |
| z15 | 451 | 27142–27163 | 13263–13284 |

`vmap/` 合计约 **2.5 MB**（582 个 `.vt`）—— 生成出来的中间产物，不随仓库发布。

二进制布局见 [DESIGN.md](./DESIGN.md) 与固件侧 `bicycle/src/vmap/`（须与 `pack_map.py` 一致）。

---

## build_vgraph.py

从 OSM 提取 `highway=*` 有向图，输出 **VGRF v2**（含 SNAP），供设备 Dijkstra 导航。

```bash
.venv/bin/python build_vgraph.py --osm osm/chuzhou.osm --out graph.vgrf
```

高度：建图时只对**路网点**采样 DEM（Terrarium z12，缓存 `docs/osm/cache/dem/`），写入 VGRF `ELEV`。不保存全国地形网格。

| 要点 | 说明 |
|------|------|
| 节点缝合 | 相距 ≤ **12 m** 的 OSM 端点并查集合并（仅 PC 构建；设备只读结果） |
| 路权 | 与 `vmap_route.c` 中 bicycle `road_weight` 对齐 |
| SNAP | **200 m** 均匀网格，加速起终点 snap |
| ELEV | SNAP 之后可选高度段：每个节点 `int16` 海拔（米），`-32768` 表示未知 |
| 版本 | `VGRF_VERSION = 4`：无向边、磁盘不写邻接表；边记录 18 B。设备仍可读 v2（有向 + 16 B 边）和 v3（有向 + 18 B 边 + 邻接表）。 |

格式与设备解析见 [ROUTING.md](./ROUTING.md)。

---

## make_cities_map — 滁州 / 南京 / 广州（当前设备包）

三城切片后 `--merge` 进同一 3 km 全国网格，每格 `lon*/lat*/x*_y*.vpk`，门户写在
vpk 尾部 VPOR。

```bash
cd vendor/my_vendor/docs/osm
./make_cities_map.sh --only guangzhou --reuse
./make_cities_map.sh --only chuzhou --reuse
./make_cities_map.sh --only nanjing --reuse
```

完整网格公式、合包顺序、当前 5925 格产物见 [`CITIES_MAP.md`](CITIES_MAP.md)。
设备加载与增量渲染见 [../map/RUNTIME.md](../map/RUNTIME.md)。

---

## make_china_map — 全国一次性生成

Web 框选仍限制单次 ≤3°（防误点全国、避免一次把 1.5 GB PBF 载入内存）。全国包走这条离线管道：先按 **1°** 切 PBF，再对每个 1° 做切片 / 路网 / 合并进固定全国网格。

```bash
cd vendor/my_vendor/docs/osm
./make_china_map.sh --dry-run     # 只打印 1° 数量与输出路径
./make_china_map.sh               # 可断点续跑，约数小时
```

- 源：`/home/jinsc/SDK/vela/osm_data/china-latest.osm.pbf`
- 中间：`/home/jinsc/SDK/vela/osm_data/china_1deg/*.osm.pbf`（`osmium extract` 批量裁剪）
- 产物：`/home/jinsc/SDK/vela/osm_data/map/`（`map.idx` + `dNNN/rNNN.vpk`，MTP 到 `/mnt/lfs/map`）
- 高度：只对路网点采 Terrarium（`docs/osm/cache/dem/`），**不**下载全国 DEM 栅格
- 依赖：`osmium-tool`（`apt install osmium-tool`）、本目录 `.venv`
- 续跑：已成功的 1° 会留下 `*.osm.pbf.done`；改 `--out` 或重打时删掉对应 `.done`

调试单格：`./make_china_map.sh --only N32E118`

详见 [../map/PRODUCTION.md](../map/PRODUCTION.md)「全国一次性生成」。

---

## make_chuzhou_map.sh / make_map.sh

滁州地图统一使用 `osm_data/chuzhou` 中已经确认的数据集，生成原版 5 km
本地网格和 `dNNN/rNNN.vpk` 子目录布局：

```bash
cd /home/jinsc/SDK/vela/osm
./make_chuzhou_map.sh
```

旧入口 `./make_map.sh` 会转发到同一脚本；旧的 `fetch` 参数不再使用。
完整说明见 [`CHUZHOU_MAP.md`](CHUZHOU_MAP.md)。

### SF32 板卡用法

```bash
cd /home/jinsc/SDK/vela/osm
./make_chuzhou_map.sh --install

cd /home/jinsc/SDK/vela/openvela
python3 vendor/my_vendor/build_board.py build-fs flash-fs
```

详见 [map 打包文档](../map/README.md)。

---

## 手动从 Overpass 下载

Overpass 公共端点不稳定，脚本用 **curl** 而非 Python `requests`。

Bbox 顺序为 **南,西,北,东**（S,W,N,E），WGS84。示例 query 见 `make_map.sh` 内 `/tmp/vmap_query.txt` 模板。

---

## 与字体子集的关系

全国 OSM 名称抽字、繁转简、底表统计见 [`../font/FONT_CHARSET.md`](../font/FONT_CHARSET.md)。

转换 OSM 时路名**默认简体**（`--zh hans`）。繁体用 `--zh hant`。字库默认用 `data/china_osm_chars.txt`，不再只靠 GB2312。

```bash
cd vendor/my_vendor/docs/osm
.venv/bin/python extract_osm_chars.py          # 更新全国底表

cd ../font
.venv/bin/python subset_font.py --preset osm
.venv/bin/python subset_font.py --sync-myvendor
```

---

## 换区域 / 换 GPX 轨迹时注意

1. 用 GPX 起点（WGS84）估算是否在现有 bbox 内；可 [pack 验证脚本](../map/README.md#验证瓦片覆盖) 查 tile 是否命中。
2. 若轨迹超出范围：`make_map.sh fetch <GCJ中心> <半径>` 扩大区域后重新 convert + pack。
3. 更新 `LiveMapView.cpp` 中 `VMAP_TRACK_START_*`（或与 GNSS 模拟共用 `test.gpx` 起点）。

当前 demo：

| 项 | 值 |
|----|-----|
| 地图中心（GCJ02） | `118.3175, 32.3025`（滁州主城） |
| test.gpx 起点（WGS84） | `118.334564, 32.237495`（在 bbox 内） |
| 设备底图 zoom | 14（`VMAP_DEMO_ZOOM`） |

---

## 相关文档

| 文档 | 内容 |
|------|------|
| [ROUTING.md](./ROUTING.md) | VGRF、Dijkstra、偏航重规划、notify/bottom 提示 |
| [DESIGN.md](./DESIGN.md) | 架构、渲染、`.vt` 层与标签策略 |
| [CITIES_MAP.md](./CITIES_MAP.md) | 滁州/南京/广州合包、网格公式、VPOR |
| [MAP_RECORD.md](./MAP_RECORD.md) | 滁州数据集快照与历史路径 |
| [../map/README.md](../map/README.md) | 本地图总览、部署、网格包 / VIDX |
| [../map/PRODUCTION.md](../map/PRODUCTION.md) | 制作方式：打包规则、Web 生成、格式 |
| [../map/RUNTIME.md](../map/RUNTIME.md) | 设备加载、增量渲染、主题跳过、箭头 |
| [../font/README.md](../font/README.md) | 矢量字体工具 |
| [../font/FONT_CHARSET.md](../font/FONT_CHARSET.md) | 全国 OSM 抽字、繁简、字库底表 |
| [../../boards/.../mkfs/README.md](../../boards/sf32lb52/my_vendor/mkfs/README.md) | LittleFS 根目录说明 |

---

## 常见问题

**Q: Overpass 下载失败 / 504**  
A: 多试几次；缩小 `--radius-deg`；换时段。脚本已内置重试。

**Q: 设备上 `/mnt/lfs/map` 空或没有 `lon*` 目录**  
A: 未 MTP `osm_data/map`，或未 `build-fs` + `flash-fs`。当前包没有 `map.idx`。见
[`CITIES_MAP.md`](CITIES_MAP.md)。

**Q: 导航无法规划 / graph parse fail**  
A: 须 `pack_map.py --graph graph.vgrf`；仅 `pack_vmap.py` 无底图路网。

**Q: 地图有路网无路名**  
A: 跑 `subset_font.py --sync-myvendor` 并重新 `build-fs`。

**Q: 缩小地图后外围空白**  
A: 正常；仅 bbox 内有数据。扩大抓取半径或换中心。

**Q: loose `.vt` 与 packed `map.idx` 区别**  
A: 内容相同；packed 把多瓦片合并为 ~500 KiB 的 `.vpk` 分片，适合 LittleFS 少文件、顺序读。

# 地图数据集记录（滁州城区 demo）

> **操作指南**见 [README.md](./README.md)；**打包烧录**见 [../map/README.md](../map/README.md)。  
> 本文只记录**当前数据集**的参数与统计，便于复现与对比。

**数据集版本**：2026-06-19 — Overpass 查询已包含 `multipolygon` relation（湖泊/水库/林地），`build_vmap.py` 会拼装 relation 外环与内环。

## 1. 区域与坐标

| 项 | 值 |
|---|---|
| 城市 | 安徽省滁州市 主城区（琅琊区 + 南谯区） |
| 中心（GCJ02） | `118.3175, 32.3025` |
| 中心（WGS84） | `118.312098, 32.304506` |
| 抓取 bbox（WGS84，S,W,N,E） | `32.19, 118.18, 32.42, 118.45` |
| 覆盖范围 | 约 **22.5 km × 22.2 km** |
| 瓦片层级 | z14（设备显示）、z15（备用） |
| 半径参数 | `--radius-deg 0.12` |

**test.gpx**（Mi Fitness 导出）起点 `118.334564, 32.237495` 落在上述 bbox 内。

## 2. 产物与路径

Canonical 目录：`vendor/my_vendor/docs/osm/`（SDK 根 `osm/` 为软链接）。

```
docs/osm/
├── osm/chuzhou.osm     # Overpass 原始 XML，~6.2 MB（含 21 个 multipolygon relation）
└── vmap/
    ├── 14/             # 132 × .vt
    └── 15/             # 471 × .vt

boards/.../mkfs/map/    # packed：map.idx + 共享 p*.vpk
```

## 3. 转换统计

### OSM 解析

| 类别 | 数量 |
|---|---|
| 总 feature | **2418** |
| 其中 multipolygon relation | **21** |
| 预置标签（place / 水域名 / POI 等） | **93** |
| 水面 polygon | 226 |
| 绿地 polygon | 22 |
| 道路 polyline | 2058 |
| 水域名称标签 | 51 |

道路名称标签在切片阶段按瓦片生成，不计入上表。

**相较旧版（仅 way、无 relation）**：feature 2404→2418，水面 219→226，水域标签 43→51。  
relation 补全后新增可见湖泊包括：**明湖、南湖、城西水库、沙河集水库、双洪水库、凤凰洼水库、深秀湖、良塘湖、红花桥水库** 等。

### 瓦片

| 层级 | x 范围 | y 范围 | 瓦片数 | 含水瓦片 |
|---|---|---|---|---|
| z14 | 13571..13581 | 6631..6642 | **132** | 100 |
| z15 | 27142..27163 | 13263..13284 | **471** | 223 |

- loose `vmap/` 合计约 **2.6 MB**
- packed `mkfs/map/` 合计约 **416 KB**

## 4. 复现命令

```bash
cd vendor/my_vendor/docs/osm
./make_map.sh fetch                    # 重新下载 OSM（含 relation）
./make_map.sh                          # 转换 + goldfish 同步
```

**SF32 打包**（在 `docs/osm` 目录下，路径相对本目录）：

```bash
cd vendor/my_vendor/docs/osm

python3 ../../boards/sf32lb52/my_vendor/ui/bicycle/tools/pack_vmap.py \
  -i ./vmap \
  -o ../../boards/sf32lb52/my_vendor/mkfs/map --clean

cd ../../../..   # 回到 openvela 根目录
python3 vendor/my_vendor/build_board.py build-fs flash-fs
```

或在 **openvela 根目录**：

```bash
python3 vendor/my_vendor/boards/sf32lb52/my_vendor/ui/bicycle/tools/pack_vmap.py \
  -i vendor/my_vendor/docs/osm/vmap \
  -o vendor/my_vendor/boards/sf32lb52/my_vendor/mkfs/map --clean
python3 vendor/my_vendor/build_board.py build-fs flash-fs
```

自定义区域：

```bash
./make_map.sh fetch 118.334564,32.237495 0.12
```

## 5. 代码中的 demo 常量

`LiveMapView.cpp`（SF32 my_vendor bicycle）：

| 宏 | 值 | 含义 |
|---|---|---|
| `VMAP_DEMO_LON/LAT` | WGS84 滁州核心 | 默认地图参考 |
| `VMAP_DEMO_ZOOM` | `14` | 底图瓦片层级 |
| `VMAP_TRACK_START_*` | test.gpx 起点 | 初始中心 / GNSS 轨迹 |

## 6. 字体

路名与湖名用字需 [`../font/subset_font.py`](../font/subset_font.py) 扫 `vmap/` 生成子集 TTF → `mkfs/fonts/`。  
数据集更新后若出现新汉字标签，需重新跑字体子集化。详见 [../font/README.md](../font/README.md)。

## 7. 已知边界

- 范围限于主城 bbox，缩小地图超出区域为底色
- Overpass 公共端点不稳定，下载需重试
- 更大城市需增大 `--radius-deg` 或分块下载（单次不宜过大）
- 设备端 `fillPolygon` 仅填充外环；relation 内环（湖心岛）在瓦片里保留几何，但屏上暂不按孔洞挖空

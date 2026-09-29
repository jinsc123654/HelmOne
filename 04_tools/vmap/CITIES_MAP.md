# 滁州 / 南京 / 广州城市地图

当前设备包是 **3 km 全国网格、每格一个 `.vpk`**，多城 `--merge` 进同一目录。
不要再用仓库旧 XML 或 `make_chuzhou_map.sh` 的 5 km `dNNN/rNNN.vpk` 流程。

渲染与加载算法见 [../map/RUNTIME.md](../map/RUNTIME.md)。

---

## 1. 路径

| | 默认 |
|--|------|
| 源 PBF | `/home/jinsc/SDK/vela/osm_data/china-latest.osm.pbf` |
| 城中间产物 | `/home/jinsc/SDK/vela/osm_data/cities/<city>/` |
| 设备包 | `/home/jinsc/SDK/vela/osm_data/map/` |
| 脚本 | `vendor/my_vendor/docs/osm/make_cities_map.sh` |

每城中间目录：

```
cities/<city>/
├── extract.osm.pbf
├── vmap/14/          # packed 瓦片树（tiles.idx + p000.vpk …）
└── graph.vgrf
```

设备目录（无 `map.idx`、无 `route.port`）：

```
map/
└── lon<ix/4>/lat<iy/4>/x<ix>_y<iy>.vpk
```

每个 vpk：`VPKG` + `RIDX` + VTIL + 裁剪 VGRF + 尾部 **VPOR**。

---

## 2. 网格算法

与固件 `vmap_format.h` / `pack_map.py` 相同：

```
origin = (0°, 0°)
anchor_lat = 35°
cell_km = 3
cell_lat = cell_km / 111.32
cell_lon = cell_km / (111.32 × cos(35°))
ix = floor(lon / cell_lon)
iy = floor(lat / cell_lat)
path = lon{ix//4}/lat{iy//4}/x{ix}_y{iy}.vpk
```

每个 `lon*` / `lat*` 桶最多 **16** 个文件（`BUCKET=4`），方便 MTP。

瓦片归属：用瓦片中心 WGS84 落入的 `(ix, iy)`，与设备跨 cell 查找一致。

路网：按 cell bbox **外扩 600 m** 裁剪进该 vpk（`GRAPH_CLIP_MARGIN_M`），供设备
近边界单区规划。

门户：pack 时 `--no-portals`，全部城 merge 完后一次 `--rebuild-portals`：

1. 扫描已有 cell，四邻配对。
2. `match_portals` 在相邻 VGRF 边界匹配节点。
3. 把 VPOR 写进每个 vpk 尾部，删除遗留 `route.port`。

`--skip-empty-graph`：无路网的格（海域）不写文件。

---

## 3. 城市 bbox 与合包顺序

`make_cities_map.py` 里 W,S,E,N（略外扩）：

| 城 | bbox |
|----|------|
| 滁州 `chuzhou` | `117.13,31.83,119.24,33.24` |
| 南京 `nanjing` | `118.35,31.21,119.25,32.64` |
| 广州 `guangzhou` | `112.93,22.41,114.08,23.96` |

滁州与南京 **接缝格子重叠**。`--merge` 时后打的城覆盖同 `(ix, iy)`。官方顺序
是脚本里的 `CITY_ORDER`：滁州 → 南京 → 广州。只补南京时应：

```bash
./make_cities_map.sh --only chuzhou --reuse   # 先有滁州格
./make_cities_map.sh --only nanjing --reuse   # 接缝以南京为准
```

**不要**对已有广州包加 `--clean`，否则会清空整个 `osm_data/map`。

`--only` 只打一城，但结尾仍会 **对整个 map 目录** rebuild portals。

---

## 4. 生成步骤

```
china-latest.osm.pbf
    │  osmium extract --bbox（可多城并行 --jobs）
    ▼
cities/<city>/extract.osm.pbf
    │  build_vmap.py --zoom 14 --jobs <tile_jobs>
    │  build_vgraph.py（Terrarium 路网点）
    ▼
vmap/ + graph.vgrf
    │  pack_map.py --region-km 3 --grid-global --subdir
    │             --skip-empty-graph --no-portals --merge
    │  （串行：同一 map/ 只允许一个写者）
    ▼
osm_data/map/lon*/lat*/x*_y*.vpk
    │  pack_map.py --rebuild-portals
    ▼
每格带 VPOR 的最终包
```

`--reuse`：该城已有 extract / vmap / graph 则跳过切片，只 pack。

```bash
cd vendor/my_vendor/docs/osm

./make_cities_map.sh --only guangzhou --reuse
./make_cities_map.sh --jobs 3                 # 三城并行切片，串行 pack
./make_cities_map.sh --only nanjing --reuse   # 增量合进现有 map/
```

环境：本目录 `.venv`（osmium / shapely / PIL）、系统 `osmium-tool`。

---

## 5. 当前产物（2026-09-06）

`make_cities_map.sh --only chuzhou --reuse` 再 `--only nanjing --reuse`，
合进已有广州格：

| 项 | 值 |
|----|-----|
| 目录 | `/home/jinsc/SDK/vela/osm_data/map` |
| cell `.vpk` | 5925 |
| 体积 | 约 314 MB |
| VPOR | 489898 条 |
| 广州 | `lon858–866` |
| 滁州/南京 | `lon890–906`（约 3907 格） |

`boards/.../ui/bicycle/mkfs/map` **不会自动同步**。上板：把 `osm_data/map`
整目录 MTP 到 `/mnt/lfs/map/`，或拷进 mkfs 再 `build-fs flash-fs`。

固件默认中心仍是广州（`VMAP_DEFAULT_LON/LAT` ≈ 113.28, 23.13）。要一开机在
南京/滁州，改默认中心或等 GNSS。

首次把几千个小文件写入 LittleFS 时，开机 `mkdir` 可能拖过 IWDT；MTP 增量比
整包 `flash-fs` 更稳妥。

---

## 6. 检查清单

- [ ] PBF 覆盖目标 bbox
- [ ] 合包顺序正确（重叠格后写者生效）
- [ ] 未误用 `--clean` 清掉其它城
- [ ] 日志有 `[pack_map] rebuilt … portals` 和 `[done] … cities=…`
- [ ] 设备 `/mnt/lfs/map/lon890`（或广州 `lon858`）存在
- [ ] 新汉字已 `subset_font.py --sync-myvendor`（若烧 mkfs）

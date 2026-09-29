# vmap 生成器（Web · 框选）

浏览器里在地图上**框选一块区域**，服务端自动跑离线工具链，产出 **设备可直接用**
的矢量地图包（`map.idx` + `rNNN.vpk`）。数据源是本地全国 OSM：
`/home/jinsc/SDK/vela/osm_data/china-latest.osm.pbf`。

复用 `docs/osm/` 既有工具与二进制格式（见 [../DESIGN.md](../DESIGN.md)、
[../README.md](../README.md)），只是把「选区 → 切片 → 建图 → 打包」串成一个网页。

## 快速开始

```bash
cd vendor/my_vendor/docs/osm/web
./run.sh                       # 首次会自动往 ../.venv 装 flask
# 打开 http://127.0.0.1:5000
```

## 预览已打包的 map/

解析 `map.idx` + `r*.vpk` 里的 VTIL，在浏览器里按设备配色画出矢量底图（可对照 OSM、看区域网格和跨区接点）：

```bash
cd vendor/my_vendor/docs/osm/web
./view_map.sh                              # 默认 /home/jinsc/SDK/vela/osm_data/map
./view_map.sh /path/to/map                 # 指定其它打包目录
# 浏览器 http://127.0.0.1:8766
```

也可在页面上点「打开 map 文件夹」，选本地 `map/`（含 `map.idx`）。

---

## 使用

依赖：

- 共享虚拟环境 `docs/osm/.venv`（`requests` / `osmium` / `shapely`，见上级 requirements）。
- `flask`（`run.sh` 自动装，或 `../.venv/bin/pip install -r requirements.txt`）。
- 系统 `osmium` 命令行（`apt install osmium-tool`）——用于从全国 PBF 裁剪 bbox。

## 使用

1. 填「区域名称」（决定输出目录 `out/<name>/`）。
2. 点「✏️ 在地图上框选」，在地图上拖拽出矩形；也可直接改 W/S/E/N。
3. 选 zoom（默认 z14，设备底图层级）、`region_km`（网格/分片大小）、是否建导航图。
4. 面板会用**橙色虚线**预览该 bbox 落在哪些「全国固定网格」格子里——每格打包成
   一个 `rNNN.vpk`。
5. 点「🚀 生成 vmap」，实时看日志；完成后显示分片数量与最大分片大小。

## 输出

```
out/<name>/
├── osm/extract.osm.pbf   # osmium 裁出的 bbox 子集
├── vmap/<z>/<x>/<y>.vt   # loose 矢量瓦片（中间产物，可检查/扫字体）
├── graph.vgrf            # 路网导航图（可选）
└── map/                  # ★ 上传设备的目录（MTP → /mnt/lfs/map）
    ├── map.idx           # VREG 区域索引（顶层单文件）
    ├── route.port        # 跨区域接点（建导航图时才有，顶层单文件）
    ├── d000/             # 子文件夹，每个 ≤ 20 个 rNNN.vpk
    │   ├── r000.vpk … r019.vpk
    └── d001/
        └── r020.vpk …
```

分片按 `region_id // 20` 分到 `d<bucket>/` 子文件夹，单文件夹最多 20 个文件（MTP
友好）。设备端会**先探子文件夹路径、再回退到旧的扁平 `map/rNNN.vpk`**，所以新旧包都能读。

把 **`out/<name>/map/`** 整个通过 MTP 传到设备 `/mnt/lfs/map` 即可。

## 三条规则如何满足

| 规则 | 做法 |
|---|---|
| ① 单文件不宜过大（500~700KB） | 打包按 `region_km` 网格切分，每格一个 `rNNN.vpk`；默认只出 z14。分片偏大就调小 `region_km`。结果面板会标注最大分片并给出 ⚠️。 |
| ② 单文件夹文件数不宜过多（MTP） | 打包用 `pack_map.py --subdir`：分片按 `id//20` 落到 `d<bucket>/` 子文件夹，**单文件夹 ≤ 20 个文件**；顶层只有 `map.idx`(+`route.port`)。设备端探子文件夹、回退扁平路径,新旧包都能读。loose `.vt` 留在 `vmap/` 不上传。 |
| ③ 固定规则生成全国索引 | 打包用 `pack_map.py --grid-global`：**固定锚纬 35° + (0,0) 原点**的全国网格，格子 `ix/iy` 与 bbox 是地理坐标的确定函数。不同时间分批生成的区域，网格边界一致，可逐步拼成全国索引（当前先生成其中一部分）。 |

## 瓦片缓存（重叠区域不重复切）

切片结果按全局固定的 slippy `z/x/y` 缓存在 `web/cache/tiles/<z>/<x>/<y>.vt`。
瓦片内容只取决于源 PBF,所以**不同框选之间重叠的瓦片会自动复用**:

- 每次生成只 `osmium extract` + `build_vmap` **缺失的瓦片**;
- 若新框选**完全落在**已生成范围内 → 全部命中,**跳过 extract,秒出**;
- 部分重叠 → 只切新增部分,重叠部分直接从缓存拷贝;
- 源 PBF 变化(大小/mtime)→ 自动清空缓存重建。

结果面板会显示「缓存命中 X/需要 N,新建 M」。缓存是 `web/cache/`(已 gitignore),
删掉即可强制全量重切。

> 注:`osmium extract` 需要流式扫过整份 1.5GB PBF(约 9s,与 bbox 大小基本无关),
> 所以只要有缺失瓦片这步就跑;真正省下的是重叠区域的 `build_vmap` 切片与
> shapely 裁剪。**导航图(graph.vgrf)目前不缓存**,勾了就按整个 bbox 重建。

## 环境变量

| 变量 | 默认 | 说明 |
|---|---|---|
| `VMAP_CHINA_PBF` | `/home/jinsc/SDK/vela/osm_data/china-latest.osm.pbf` | 源 PBF |
| `VMAP_WEB_HOST` / `VMAP_WEB_PORT` | `127.0.0.1` / `5000` | 监听地址 |
| `VMAP_MAX_BBOX_DEG` | `3.0` | 单次框选每边最大度数（防误选全国） |

## 说明 / 边界

- 生成后如出现新汉字路名/地名，仍需跑字体子集化（见 [../../font/README.md](../../font/README.md)）
  才能在设备上正确显示。
- `map/` 与 `pack_map.py` 常规产物**格式完全一致**，设备侧无需改动即可读取。
- 大范围框选会先被 `VMAP_MAX_BBOX_DEG` 拦截；全国一次性生成请用 `../make_china_map.sh`，不要把该上限调到覆盖全中国。

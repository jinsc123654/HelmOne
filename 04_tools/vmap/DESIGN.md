# 矢量地图系统设计文档（bicycle 码表）

面向 SF32LB52（Cortex-M33@240MHz，约 14MB 可用 PSRAM，240×400 屏）的离线矢量地图方案。
目标：在嵌入式端显示 OSM 路网 / 水系 / 绿地 + 中文路名，支持平移、缩放、骑行动画。
当前在 openvela 模拟器（`vela_goldfish-arm64-v8a-ap`）上验证。

## 1. 总体架构

分为「离线工具链（PC）」和「设备端运行时（固件）」两部分：

```
   PC 离线                                   设备端（固件 /etc/vmap）
 ┌──────────────┐   .vt 瓦片   ┌────────────────────────────────────┐
 │ Overpass OSM │ ───────────► │ VectorTile  (二进制读取器)          │
 │   ↓ build_vmap.py           │      ↓                              │
 │ 切片 + 量化 + 标签           │ VectorMapView (软件光栅化 RGB565)   │
 │   ↓                          │      ↓ lv_canvas                   │
 │ vmap/{z}/{x}/{y}.vt          │ LiveMapView (LVGL 页面：动画/按钮)  │
 └──────────────┘              └────────────────────────────────────┘
```

设计取舍：不引入完整矢量图形库；用**自定义紧凑二进制 + 自写软件光栅化器**，把内存与算力压到最低；瓦片**按需加载**（只读当前可视瓦片），避免一次性载入整城数据。

## 2. 坐标系

- 内部统一 **WGS84**（与 GPS、OSM 一致），投影用 Web Mercator（复用工程已有 `MapConv`/`TileSystem`）。
- 用户给点可用 **GCJ02**（高德/腾讯），工具内 `gcj02_to_wgs84` 换算后再处理。
- 瓦片编号沿用标准 XYZ：`tile = f(lon,lat,zoom)`，每瓦片 256 世界像素。

## 3. 离线工具链（`build_vmap.py`）

流程：`fetch_overpass → parse_osm → build_tiles → serialize_tile → write_tiles`。

1. **抓取**：Overpass 查询 highway / waterway / water / reservoir / wood-forest 的 way，以及 place/peak/tourism/historic 的 node（带 name）。
2. **分类**：
   - 道路 `classify_road`：motorway/trunk→1, primary→2, secondary→3, tertiary→4, residential/unclassified→5, service→6, path/track/foot/cycle→7。
   - 水域面 / 绿地面 / 水系线 / 道路线 分别归层。
3. **切片**：用 shapely 把每个几何按瓦片 bbox `intersection` 裁剪，落到经过的每个瓦片。
4. **量化**：瓦片内坐标量化到 `[0, EXTENT=4096]`，存 `uint16`（相对坐标，体积小、精度足够）。
5. **标签**：
   - place/POI/水域：在节点或面质心处放一个标签。
   - **道路标签按瓦片生成**——对每个经过的瓦片，取该瓦片内裁剪段中点放一个锚点（同名在多个瓦片各一个）。这样道路不管多长，**可见瓦片内必有屏内锚点**，配合设备端按名去重，可见处稳定显示一个路名（解决了“整条路一个中点锚点落屏外导致路名不显示”的问题）。

### .vt 二进制格式（小端）

```
文件头:
  magic   "VTIL"
  u8  version (=1)
  u8  zoom
  u16 extent (=4096)
  u32 tile_x
  u32 tile_y
  u16 layer_count
  u16 reserved

每层:
  u8  layer_id        # 0水面 1绿地 2水系线 3道路线 4标签
  u8  reserved
  u16 feature_count
  u32 blob_len
  blob:
    几何层(0..3) 每个 feature:
      u8  attr        # 道路为 ROAD_* 等级
      u8  flags       # bit0=closed(面)
      u16 npts
      npts × (u16 x, u16 y)   # [0,extent] 量化坐标
    标签层(4) 每条记录:
      u8  kind        # 1place 2water 3road 4poi
      u8  priority    # 越大越优先
      u16 x, u16 y    # 锚点（瓦片内量化坐标）
      u8  len, u8 reserved
      char utf8[len]  # 名称，非 NUL 结尾
```

优先级（用于碰撞时取舍）：道路 motorway=130…path=45；城市 place、水域面有各自权重。

## 4. 设备端读取（`VectorTile`）

- `load(path)`：整块读入内存，零拷贝地用指针遍历。
- `Iterator`（几何）与 `LabelIterator`（标签）：按上面的格式逐条解出 `Feature`/`Label`。
- 注意命名避坑：成员用 `_ptr` 而非 `_p`（`_p` 与 `lv_i18n` 宏冲突过）。

## 5. 设备端渲染（`VectorMapView`）

一块 `lv_canvas`（RGB565），自写软件光栅化：

- 基础图元：`putPixel / blendSpan / drawThickLine / fillPolygon(扫描线) / fillRect / clear`。
- `render()`：
  1. 由中心经纬度求世界像素 `center`；
  2. 按**缩放因子 `_scale`** 求可视世界范围 = `w/scale × h/scale`，推出需要的瓦片范围；
  3. 逐瓦片 `load` → `drawTile`（绿地→水面→水系→道路顺序叠画）→ `collectLabels`；
  4. `drawLabels` 统一画文字；
  5. `lv_obj_invalidate`。
- **投影含缩放**：每点 `screen = base + local * (256*scale/extent)`，`base = (tile*256 - center)*scale + 屏幕半宽`。几何随 `_scale` 平滑放大（矢量不糊）。

### 缩放设计（关键约定）

- `setScale()` 改的是**几何缩放**，范围 0.25×–8×。
- **文字大小恒定**：标签始终用固定字体 `lv_draw_label` 绘制，不乘 `scale`。
- **线宽恒定**：道路/水系按固定屏幕像素线宽绘制，不乘 `scale`——与标准地图一致，放大后路网相对变细、缩小后相对变密。
- 底图数据固定用 z14（也生成了 z15 备用），靠 `_scale` 做连续缩放，不依赖多层瓦片切换。

### 标签设计（`drawLabels`）

- 收集可视瓦片内标签（带屏外小余量裁剪）。
- 按「有效优先级」降序排（道路 +45 加权，避免被水域名挤掉）；同优先级时**离屏幕中心更近者优先**（保证去重后留下的是屏内那个锚点）。
- **按名去重**：同名只画一个。
- **文本框夹紧进视口**：贴边锚点也能完整显示。
- **碰撞检测**：已放置标签间留间距，避免重叠（`LABEL_COLLISION`）。
- 无文字底色矩形（之前按需求去掉）。

## 6. 页面集成与交互（`LiveMapView`）

- 把 `VectorMapView` 作为**最底层**底图；原 `MapView` 作为透明覆盖层画轨迹（瓦片源关闭）。
- 中文字体通过 `font_vector_lfs` / `ResourcePool` 传给 `VectorMapView::setFont`。
- **GNSS / GPX 回放**：`HAL_GNSS` 读 `/mnt/lfs/Track/test.gpx`，LiveMap 随坐标 `setCenter` + 导航箭头。
- **缩放按钮**：右侧 `+`/`−`，`setScale(getScale()*1.3 或 /1.3)` 后 `render()`。

## 7. 构建与资源打包

### SF32LB52（my_vendor，当前主路径）

```
docs/osm/vmap  --pack_vmap.py-->  mkfs/map  --build-fs-->  /mnt/lfs/map
```

操作说明：[README.md](./README.md)、[../map/README.md](../map/README.md)。

### Goldfish 模拟器

- 构建：`./vela_emulator_tools.py build`。
- 资源：`bicycle/etc/vmap/` loose `.vt` → ROMFS `/etc/vmap`。
- `make_map.sh` 负责同步 + 清 ROMFS 暂存（`add_board_rcraws` 不跟踪目录内文件变更）。
- Python venv 位于 `vendor/my_vendor/docs/osm/.venv`（SDK 根 `osm/` 为软链接）。

## 8. 性能 / 内存要点

- 瓦片**按需加载**：只读当前可视范围（通常数个瓦片），不常驻整城。
- `.vt` 用 u16 相对坐标 + 分层 blob，体积小（本城 z14+z15 loose 约 2.5MB，packed 约 330KB）。
- 软件光栅化在 240×320（SF32）上每帧重画；GNSS 更新约 5 Hz，需关注瓦片 I/O 与多边形填充耗时。

## 9. 后续可做

- **道路分级显示**：缩小只留主干道、放大再出次要路（按 `attr` 等级 + 当前 scale 过滤），更接近高德/Google。
- 接入真实 GPS / 沿 GPX 轨迹移动（已实现 GPX 模拟回放）。
- 等高线图层（路网点海拔已写入 VGRF `ELEV`，底图等高线另议）。
- “Heading up” 地图旋转。
- 缩放倍数 UI 提示、按钮长按连续缩放。

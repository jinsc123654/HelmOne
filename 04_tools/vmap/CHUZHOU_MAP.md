# 滁州地图生成（旧流程）

> **当前设备包**请用 [`CITIES_MAP.md`](CITIES_MAP.md)：
> `make_cities_map.sh` → 3 km 全国网格 `lon*/lat*/x*_y*.vpk`，滁州/南京/广州合包。
> 下文是旧的 5 km `dNNN/rNNN.vpk` 脚本，仅作对照。

当前滁州板卡地图统一使用以下数据，不再使用仓库内旧的
`docs/osm/osm/chuzhou.osm`：

- OSM：`/home/jinsc/SDK/vela/osm_data/chuzhou/chuzhou.osm`
- 瓦片：`/home/jinsc/SDK/vela/osm_data/chuzhou/vmap`
- 路网：`/home/jinsc/SDK/vela/osm_data/chuzhou/graph.vgrf`
- 产物：`/home/jinsc/SDK/vela/osm_data/map_chuzhou`

生成格式固定为：

- 5 km 本地区域网格（与原 36 分区地图的原点和经度步长一致）
- `dNNN/rNNN.vpk` 子目录布局（`--subdir`）
- 最高 z14
- 包含分区路网及 `route.port`

## 生成

```bash
cd /home/jinsc/SDK/vela/osm
./make_chuzhou_map.sh
```

脚本先写临时目录，成功后才替换 `map_chuzhou`；首次被替换的原始地图保存在
`map_chuzhou.bak`，后续生成不会覆盖该备份。

## 生成并安装到板卡文件系统

```bash
cd /home/jinsc/SDK/vela/osm
./make_chuzhou_map.sh --install

cd /home/jinsc/SDK/vela/openvela
python3 vendor/my_vendor/build_board.py build-fs flash-fs
```

旧入口 `./make_map.sh` 已改为转发到同一流程。

如需使用其他输出目录：

```bash
VMAP_CHUZHOU_OUT=/tmp/map_chuzhou_test ./make_chuzhou_map.sh
```

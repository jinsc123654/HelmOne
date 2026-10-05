# mkfs — 三个卷的烧录内容

本目录按卡上三块区域拆开，`build-fs` 会分别打成镜像：

| 子目录 | 镜像 | 分区 | 设备挂载 | 文件系统 |
|--------|------|------|----------|----------|
| `kv/` | `kv_root.bin` | `KV_REGION` 256 MiB @ 256 MiB | `/mnt/kv` | LittleFS |
| `lfs/` | `fs_root.bin` | `FS_REGION` 512 MiB @ 512 MiB | `/mnt/lfs` | LittleFS |
| `fat/` | `fat_root.bin` | `FAT_REGION` 1 GiB 起；打包种子 14 GiB | `/mnt/fat` | FAT32 |

`fat/` 卷根只有两棵树：

| 主机 | 设备 |
|------|------|
| `fat/map/` | `/mnt/fat/map/` 矢量瓦片 `lon*/lat*/x*_y*.vpk` |
| `fat/fonts/` | `/mnt/fat/fonts/` 矢量 TTF 子集 |

构建：

```bash
# openvela 根目录
python3 vendor/my_vendor/build_board.py build-fs
python3 vendor/my_vendor/build_board.py pack-sd-img
python3 vendor/my_vendor/build_board.py burn-sd --sd /dev/sdX
```

SD 上不要用 `flash-fs` 灌地图（余量 FAT 走 pack-sd-img）。

## `lfs/` → `/mnt/lfs`

| 路径 | 设备挂载 | 说明 |
|------|----------|------|
| `lfs/images/` | `/mnt/lfs/images/` | PNG 等资源 |
| `lfs/Track/` | `/mnt/lfs/Track/` | 旧演示 GPX（产品 MTP 不可见） |
| `lfs/mtp/` | `/mnt/lfs/mtp/` | 产品 USB MTP 用户目录 |
| `lfs/mtp/import/` | `/mnt/lfs/mtp/import/` | 外部导入的 GPX |
| `lfs/mtp/record/` | `/mnt/lfs/mtp/record/` | 本机记录的 GPX |
| `lfs/eph/` | `/mnt/lfs/eph/` | GNSS 星历 |

产品 MTP 只暴露 `/mnt/lfs/mtp`。BLE 文件管理器沙箱仍是 `/mnt/lfs`。

## `kv/` → `/mnt/kv`

OTA 固件槽 `/mnt/kv/fw/`（目录打进镜像，避免第一次 `lfs_alloc`）。空白卡仍会 autoformat。

## `fat/` → `/mnt/fat`

- **不要**手工往 `fat/map/` 里放 loose 的 `<z>/<x>/<y>.vt`；用打包工具从 OSM 流水线生成。
- 完整流程：[docs/osm/README.md](../../../docs/osm/README.md) → [docs/map/README.md](../../../docs/map/README.md)。

```bash
python3 vendor/my_vendor/boards/sf32lb52/my_vendor/ui/bicycle/tools/pack_map.py \
  -i vendor/my_vendor/docs/osm/vmap \
  --graph vendor/my_vendor/docs/osm/graph.vgrf \
  -o vendor/my_vendor/boards/sf32lb52/my_vendor/mkfs/fat/map \
  --clean --pack-size 512000
```

产品固件把 `/mnt/fat` **只读**挂载。工厂固件 **读写**挂载，MTP 盘符是 **file**（`/mnt/lfs`）、**kv**、**fat** 三个。工厂拷图放到 **fat 卷的 `map/`**，字体放到 **`fonts/`**。

默认按常见 **16GB 卡 CSD 14832 MiB** 打全国地图种子（FAT ≈ 13808 MiB，`custom.SD_PACK_CARD_MIB=14832`）。按 15 GiB 打出来的 14 GiB FAT 在这种卡上会 `mount EINVAL`。更大的卡：MTD 窗口是 CSD 余量，但 FAT 不会像 LittleFS 那样 cheap grow。要吃满余量，按目标卡设 `SD_PACK_CARD_MIB` 再 `build-fs`。

## 排查

```text
nsh> ls /mnt/lfs/mtp
nsh> ls /mnt/fat
nsh> ls /mnt/fat/map
nsh> ls /mnt/fat/fonts
nsh> mount
```

`/mnt/fat/map` 下应能看到 `lon*` 目录（无 `14/`、`graph/` 子目录）。

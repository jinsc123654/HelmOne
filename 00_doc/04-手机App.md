# 手机 App

Flutter 写的伴生 App，包名 `sifli_companion`，用途一句话：
**BLE 连接、轨迹管理、固件升级**。代码在 `03_app/flutter/`。

| 主菜单 | 导航中 | 骑行记录 | 设置 |
|---|---|---|---|
| ![](images/app-menu.png) | ![](images/app-nav.png) | ![](images/app-ride-log.png) | ![](images/app-settings.png) |

> 上面是设计稿渲染图。真机截图见下面待补项。

## 功能

**连接与设备**
- 扫描、配对、绑定车机；也扫描并绑定 BLE 传感器（心率带 / 踏频计 / 功率计）
- 设备状态页：电量、存储、线程、内存、系统状态
- 设备侧重新绑定（换手机或清过蓝牙之后用）

**骑行**
- 实时页：镜像车机当前骑行数据
- 骑行记录列表与详情、轨迹编辑
- GPX 库、收藏、导航路线、途经点编辑

**地图**
- 查看 / 编辑设备上的地图城市
- 通过 USB 传地图包（车机存储有限，地图按城市下）

**固件与诊断**
- 固件升级（OTA）
- 星历注入（GNSS 星历页）
- 日志抓取与查看、缓存浏览

## 页面清单

`lib/ui/` 下按功能分目录，主要页面：

| 目录 | 页面 |
|---|---|
| `home/` | 主菜单 |
| `ride/` | 骑行、骑行详情、导航路线、途经点编辑、收藏、GPX 库 |
| `map/` `settings/osm_cities_page` `settings/map_usb_page` | 地图查看 / 城市 / USB 传输 |
| `device/` | 设备页、重新绑定、日志抓取 |
| `settings/` | 设置、App 更新、关于 |
| `gnss/` | 星历（eph） |
| `ota/` | 固件升级 |
| `ble/` | 扫描 |
| `cache/` | 缓存浏览 |
| `log/` | 日志查看 |
| `notif/` | 通知设置 |
| `welcome/` | 首次启动引导 |

业务逻辑在 `lib/` 的其余目录：`ble/`、`transfer/`、`gnss/`、`location/`、`map/`、`ride/`、
`ota/`、`db/`、`cache/`、`i18n/`、`util(s)/`、`log/`、`app/`。

## 主题

明暗两套，跟随系统：

| 深色 | 浅色 |
|---|---|
| ![](images/app-menu.png) | ![](images/app-menu-light.png) |

## 版本号约定

```
version: 1.0.0+20260929
         │      └── 构建号 = 日期滚动 YYYYMMDD（构建前跑 scripts/sync_version.sh）
         └── 手写在 version.name
```

App 内的版本显示、Android `versionName` / `versionCode` 都从这两处来。

## 编译

```bash
cd 03_app/flutter
flutter pub get
flutter build apk --release          # 或 flutter run
```

## 待补

> 🖼️ **待补图** → `images/app-ride-live.png` —— 骑行实时页的真机截图（现在只有设计稿渲染）
> 🖼️ **待补图** → `images/app-pairing.png` —— 配对/扫描界面截图
> 🖼️ **待补图** → `images/app-store.png` —— 应用商店页（如果上架了）

> 📝 `03_app/flutter/README.md` 目前还是 `flutter create` 的模板内容（"A new Flutter project."），
> 和本目录重复，建议删掉或改成一行指向这里。

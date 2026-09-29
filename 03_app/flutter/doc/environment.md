# SiFli Companion — 环境与基座说明

本文记录本仓库（`02_sw/flutter`，包名 `sifli_companion`）的开发环境、构建约定与当前基座能力，便于复现与新人上手。

**更新日期**：2026-07-22

---

## 1. 路径总览

| 项目 | 路径 |
|------|------|
| 工程根目录 | `/home/jinsc/Desktop_My/work/my_work/sifli/02_sw/flutter` |
| Flutter SDK（**必须**） | `/home/jinsc/SDK/Flutter/flutter_3.44.6` |
| 勿用（Dart 过旧） | `/home/jinsc/SDK/Flutter/flutter_3.29_3`（Dart 3.7.2，不满足 `^3.12.2`） |
| Android SDK | `/usr/lib/android-sdk`（见 `android/local.properties` 的 `sdk.dir`，**勿提交**） |
| Gradle 完整包本地缓存 | `/home/jinsc/SDK/android_pack/gradle-9.1.0-all.zip` |
| Gradle Wrapper 解压目录 | `~/.gradle/wrapper/dists/gradle-9.1.0-all/` |

---

## 2. Flutter / Dart

| 项 | 值 |
|----|-----|
| Flutter | **3.44.6**（stable） |
| Dart | **3.12.2** |
| `pubspec.yaml` SDK 约束 | `sdk: ^3.12.2` |
| 应用名 | SiFli Companion |
| Dart package | `sifli_companion` |
| Android `applicationId` | `com.sifli.sifli_companion` |

### 版本号（手动名 + 日期滚动）

| 项 | 约定 |
|----|------|
| 手动版本名 | 根目录 `version.name`（如 `1.0.0`） |
| 构建号 | 本地日期 `YYYYMMDD`（如 `20260722`） |
| 完整 | `1.0.0+20260722` → Android **`versionName`**（系统「应用信息」可见）+ `versionCode` |

**Android Studio 点 Run / `flutter run`**：`android/app/build.gradle.kts` 每次构建自动注入，无需手跑脚本。构建日志可见：

`SiFli Companion → versionName=1.0.0+YYYYMMDD versionCode=YYYYMMDD`

命令行发版可选同步 `pubspec.yaml`：

```bash
./scripts/sync_version.sh          # 写入 pubspec version 行
WITH_TIME=1 ./scripts/sync_version.sh   # 同日多次：YYYYMMDDHHMM
./scripts/build.sh apk             # 先 sync 再 flutter build
```

### 终端使用指定 SDK

```bash
export PATH=/home/jinsc/SDK/Flutter/flutter_3.44.6/bin:$PATH
cd /home/jinsc/Desktop_My/work/my_work/sifli/02_sw/flutter
flutter --version   # 应显示 3.44.6 / Dart 3.12.2
flutter pub get
flutter run
```

### Cursor / VS Code

工程内 `.vscode/settings.json`（含本机 SDK 路径，换机器请改）：

```json
{
  "dart.flutterSdkPath": "/home/jinsc/SDK/Flutter/flutter_3.44.6",
  "dart.sdkPath": "/home/jinsc/SDK/Flutter/flutter_3.44.6/bin/cache/dart-sdk"
}
```

### Android Studio

1. **File → Settings → Languages & Frameworks → Flutter**  
   Flutter SDK path：`/home/jinsc/SDK/Flutter/flutter_3.44.6`
2. **Languages & Frameworks → Dart**  
   Dart SDK path：`/home/jinsc/SDK/Flutter/flutter_3.44.6/bin/cache/dart-sdk`
3. 若 Flutter 页选 3.44.6 时 **Apply 灰色**：工程侧已指向该 SDK；以 Dart 页为准，或 Invalidate Caches 后重开。
4. 新增原生插件（如 `share_plus`）后须 **完全停止 App 再 Run**，热重载会出现 `MissingPluginException`。

`android/local.properties`（本机生成，已在 `android/.gitignore`）：

```properties
sdk.dir=/usr/lib/android-sdk
flutter.sdk=/home/jinsc/SDK/Flutter/flutter_3.44.6
```

---

## 3. Android 构建链路

| 项 | 值 |
|----|-----|
| Gradle | **9.1.0-all** |
| AGP | **9.0.1** |
| Kotlin（插件侧过渡） | **2.3.20**（`settings.gradle.kts` apply false） |
| 应用 Java / Kotlin 目标 | **17** |
| 平台 | android、ios、linux |

### AGP 9 / Built-in Kotlin（过渡期）

`android/gradle.properties` 当前：

```properties
android.newDsl=false
android.builtInKotlin=false
```

原因：Flutter 3.44 的 Gradle 插件与部分插件（如 **`flutter_blue_plus`**）尚未完全适配 Built-in Kotlin；强行 `true` 会编不过。  
已升级 **`share_plus` 13+** / **`package_info_plus` 10+**（支持 Built-in Kotlin），可消除二者的 KGP 警告。待 BLE 等插件迁移后再开 `builtInKotlin=true`。

业务侧优先 **Dart 跨端插件**，不写自定义 Java 业务代码；`GeneratedPluginRegistrant.java` 为 Flutter 生成文件，勿手改、勿提交（已 ignore）。

### Gradle 发行包

- 官方：`https://services.gradle.org/distributions/gradle-9.1.0-all.zip`
- 本机完整包：`/home/jinsc/SDK/android_pack/gradle-9.1.0-all.zip`
- `zip END header not found`：删损坏的 `~/.gradle/wrapper/dists/gradle-9.1.0-all/`，再从 `android_pack` 拷入或重下。

### 代理注意

`~/.gradle/gradle.properties` 若配置了 `127.0.0.1:7899` 等本地代理，**代理未启动时**会导致解析失败。代理未开请注释对应 `systemProp.http(s).proxy*`。

---

## 4. 基座能力与目录

冷启动顺序：

1. `AppLog.init()` + 未捕获异常挂钩（`reportUncaught`）
2. BLE 软初始化
3. `CacheData().initInfo()`（语言、协议同意）
4. `DBManager().initDB()`
5. `runApp` → `GetMaterialApp`  
   - 未同意协议 → `/welcome`  
   - 已同意 → `/`

| 能力 | 包 / 实现 | 说明 |
|------|-----------|------|
| 路由 / 状态 | `get` 4.x | GetX |
| 多语言 | GetX Translations + `flutter_localizations` | zh-CN / en-US |
| 日志 / 崩溃 | `lib/log/app_log.dart` | 应用私有目录 `…/logs/` |
| 日志导出 | `share_plus` 13+ + `archive` | 系统分享单文件或 zip |
| 定位 | `geolocator` | `lib/location/location_manager.dart` |
| 地图 | `flutter_map` + OSM | 国内瓦片可能较慢 |
| KV | `shared_preferences` | `lib/utils/cache_data.dart` |
| DB | `sqflite` | `sifli_companion.db` |
| BLE | `flutter_blue_plus` | 扫描 / 连接 |
| 权限 | `permission_handler` + geolocator | 蓝牙 / 定位 |

### 路由

| 常量 | 路径 | 页面 |
|------|------|------|
| `AppRoutes.welcome` | `/welcome` | 首次协议 |
| `AppRoutes.home` | `/` | 首页 |
| `AppRoutes.bleScan` | `/ble/scan` | BLE 扫描 |
| `AppRoutes.logViewer` | `/log` | 本地日志 |
| `AppRoutes.locationMap` | `/map` | 定位地图 |
| `AppRoutes.notFound` | `/404` | unknownRoute |

新增页：`app_routes.dart` → `app_pages.dart` → `Get.toNamed(...)`。

### 多语言

| 路径 | 作用 |
|------|------|
| `lib/i18n/locale_keys.dart` | 文案键 |
| `lib/i18n/langs/zh_cn.dart` / `en_us.dart` | 文案表 |
| `lib/i18n/app_translations.dart` | GetX `Translations` |
| `lib/i18n/app_locale.dart` | 解析 / 切换 / KV 持久化 |

用法：`LocaleKeys.xxx.tr`、`.trParams({'n': '1'})`、`AppLocale.apply(AppLocale.enUS)`。

### 日志约定

| 项 | 约定 |
|----|------|
| 目录 | 应用私有 `…/files` 下 `logs/`（不上传、不写外部存储） |
| 落盘级别 | **release**：仅 `W` / `E` / 崩溃；`D`/`I` 只打控制台 |
| 保留 | 普通日志 **7 天**；单日文件约 **2MB** 截尾；crash 最多 **20** 个 |
| 去重 | 5 秒内相同异常只写 **1** 个 crash 文件 |
| 瞬时网络错误 | `HandshakeException` / `SocketException` 等只记 `E`，不建 crash 文件 |
| 导出 | 日志页：导出当前 / 导出全部 zip（系统分享） |

### 图标

| 路径 | 作用 |
|------|------|
| `assets/icons/bike_computer.png` | 应用内展示 |
| `assets/icons/bike_computer.svg` | 矢量源 |
| `assets/icons/bike_computer_1024.png` | 启动器源图（居中透明） |
| `lib/app/app_assets.dart` | 路径常量 |

重新生成启动器：`dart run flutter_launcher_icons`  
桌面图标 adaptive **白底 `#FFFFFF`**（透明底在多数手机壳上会变黑底）。

---

## 5. Android / iOS 权限

**Android** `AndroidManifest.xml`：

- 网络：`INTERNET`、`ACCESS_NETWORK_STATE`
- 蓝牙：`BLUETOOTH` / `ADMIN`（maxSdk 30）、`BLUETOOTH_SCAN`、`BLUETOOTH_CONNECT`
- 定位：`ACCESS_FINE_LOCATION` / `ACCESS_COARSE_LOCATION`
- 分享查询：`SEND` / `SEND_MULTIPLE`

**iOS** `Info.plist`：蓝牙 Always/Peripheral、定位 WhenInUse 说明文案。

真机扫描前需授予蓝牙与（部分机型）定位。扫描页默认过滤名称含 `MyBike` / `Vela`。

---

## 6. 持久化自测（KV / DB）

首页：

- **KV +1** → SharedPreferences  
- **DB +1** → SQLite `kv_backup`

验证：写入 → **划掉进程** → 再启动，counter 应仍在。

首次启动：欢迎页勾选同意 → KV `agree=true`；清应用数据可再次进入欢迎页。

---

## 7. 常用命令

```bash
export PATH=/home/jinsc/SDK/Flutter/flutter_3.44.6/bin:$PATH
cd /home/jinsc/Desktop_My/work/my_work/sifli/02_sw/flutter

flutter doctor
flutter pub get
flutter analyze lib
flutter run
flutter build apk --debug
dart run flutter_launcher_icons
```

---

## 8. Git 提交注意

可整体 `git add 02_sw/flutter`，已忽略：

- `build/`、`.dart_tool/`、`android/.gradle/`、`android/build/`
- `android/local.properties`、`GeneratedPluginRegistrant.java`
- `key.properties` / `*.jks` / `*.keystore`

`.vscode/settings.json` 含本机 SDK 路径，团队路径不一致时可酌情不提交。

---

## 9. 常见问题

| 现象 | 处理 |
|------|------|
| `Dart SDK version is 3.7.2` / 要求 `^3.12.2` | 改用 3.44.6；检查 IDE Flutter/Dart SDK |
| `zip END header not found` | Gradle zip 损坏；用 `android_pack` 完整包 |
| `kotlin-dsl` plugin not found | 检查 Gradle 本地代理是否未启动 |
| `MissingPluginException`（share 等） | 停掉 App 完整重装，勿仅热重载 |
| KGP / Built-in Kotlin 警告或编不过 | 保持 `builtInKotlin=false`；`share_plus` 用 13+ |
| 「源值 8 已过时」 | 根 `android/build.gradle.kts` 已统一 Java 17 |
| 应用信息只显示 `1.0.0` | 确认 Gradle 日志有 `versionName=1.0.0+日期` 后重装 |
| 扫不到码表 | 确认广播名含 `MyBike`，或关闭扫描页名称过滤 |
| OSM 地图空白 / HandshakeException | 网络或墙；属瞬时错误，不应再刷大量 crash 文件 |
| `No Linux desktop project` | `flutter create --platforms=linux .` |

---

## 10. 参考

- 码表 Companion BLE 设计：`02_sw/vendor/vela_sifli/docs/ble/companion_design.md`

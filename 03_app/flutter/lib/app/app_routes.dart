/// GetX 命名路由常量。
///
/// 与 [AppPages.routes] 中的 `name` 一一对应。
abstract class AppRoutes {
  /// 首次启动欢迎页。
  static const welcome = '/welcome';

  /// 首页（产品壳）。
  static const home = '/';

  /// 开发者联调首页（长按版本号）。
  static const lab = '/lab';

  /// 蓝牙扫描页。
  static const bleScan = '/ble/scan';

  /// Companion 协议联调页（开发者工具）。
  static const companionDebug = '/ble/companion';

  /// 消息通知设置。
  static const notifSettings = '/notif';

  /// 码表文件操作（GATT 文件管理）。
  static const companionFs = '/ble/companion/fs';

  /// 码表固件 OTA。
  static const companionOta = '/ble/companion/ota';

  /// 星历同步。
  static const companionEph = '/ble/companion/eph';

  /// 码表崩溃日志抓取。
  static const companionCoredump = '/device/coredump';

  /// 重新绑定码表（解绑 + 配对说明）。
  static const deviceRebind = '/device/rebind';

  /// App 持久化后台缓存（本地文件浏览）。
  static const appCache = '/cache';

  /// 本地日志 / 崩溃查看。
  static const logViewer = '/log';

  /// 定位与地图。
  static const locationMap = '/map';

  /// 坐标点记录库。
  static const navRoutes = '/ride/nav';

  /// 途经点地图编辑器。
  static const waypointEditor = '/ride/waypoints';

  /// GPX 库。
  static const gpxLibrary = '/ride/gpx';

  /// 常用点。
  static const favorites = '/ride/favorites';

  /// 骑行记录详情。
  static const rideDetail = '/ride/detail';


  /// 离线地图 USB 说明。
  static const mapUsb = '/settings/map-usb';

  /// App 侧按城市下载底图。
  static const osmCities = '/settings/osm-cities';

  /// 本 App 检查更新。
  static const appUpdate = '/settings/app-update';

  /// 关于。
  static const about = '/settings/about';

  /// 未知路由回退页。
  static const notFound = '/404';
}

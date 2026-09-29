import 'package:geolocator/geolocator.dart';
import 'package:sifli_companion/log/app_log.dart';

/// 定位封装：权限、服务开关与当前坐标。
class LocationManager {
  /// 返回单例。
  factory LocationManager() => _instance;

  static final LocationManager _instance = LocationManager._();

  LocationManager._();

  /// 系统定位服务是否开启。
  Future<bool> isServiceEnabled() => Geolocator.isLocationServiceEnabled();

  /// 当前权限状态。
  Future<LocationPermission> checkPermission() => Geolocator.checkPermission();

  /// 申请定位权限；返回是否已可定位。
  Future<bool> requestPermission() async {
    final service = await isServiceEnabled();
    if (!service) {
      await AppLog.w('Location', 'service disabled');
      return false;
    }

    var permission = await checkPermission();
    if (permission == LocationPermission.denied) {
      permission = await Geolocator.requestPermission();
    }
    if (permission == LocationPermission.denied ||
        permission == LocationPermission.deniedForever) {
      await AppLog.w('Location', 'permission=$permission');
      return false;
    }
    return true;
  }

  /// 上次已知位置：系统缓存里那份，**立即返回**，不会等定位。
  ///
  /// 辅助数据用它最合适：位置只要「几十公里内」，几分钟前那份完全够用，
  /// 而且不用为它开一次定位、也不占下发时间。
  Future<Position?> getLastKnownPosition() async {
    if (!await requestPermission()) return null;
    try {
      final pos = await Geolocator.getLastKnownPosition();
      await AppLog.i(
        'Location',
        'last-known lat=${pos?.latitude} lng=${pos?.longitude} acc=${pos?.accuracy}',
      );
      return pos;
    } catch (e, s) {
      await AppLog.e('Location', e, s);
      return null;
    }
  }

  /// 粗定位：低精度 + 短超时。给辅助数据用 —— 它只要「几十公里内」，
  /// 不值得为它等一次高精度定位（室内可能要 20 秒）。
  Future<Position?> getCoarsePosition() => getCurrentPosition(
        accuracy: LocationAccuracy.low,
        timeLimit: const Duration(seconds: 6),
      );

  /// 获取当前坐标（高精度优先）。
  Future<Position?> getCurrentPosition({
    LocationAccuracy accuracy = LocationAccuracy.high,
    Duration timeLimit = const Duration(seconds: 20),
  }) async {
    final ok = await requestPermission();
    if (!ok) return null;
    try {
      final pos = await Geolocator.getCurrentPosition(
        locationSettings: LocationSettings(
          accuracy: accuracy,
          timeLimit: timeLimit,
        ),
      );
      await AppLog.i(
        'Location',
        'lat=${pos.latitude} lng=${pos.longitude} acc=${pos.accuracy}',
      );
      return pos;
    } catch (e, s) {
      await AppLog.e('Location', e, s);
      return null;
    }
  }

  /// 打开系统定位设置页（若平台支持）。
  Future<bool> openLocationSettings() => Geolocator.openLocationSettings();

  /// 打开应用设置页。
  Future<bool> openAppSettings() => Geolocator.openAppSettings();
}

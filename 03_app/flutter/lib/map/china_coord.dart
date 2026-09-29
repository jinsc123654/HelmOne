import 'dart:math' as math;

import 'package:latlong2/latlong.dart';

/// WGS-84 ↔ GCJ-02（火星坐标）。国内高德底图按 GCJ-02 编号瓦片。
abstract class ChinaCoord {
  static const _a = 6378245.0;
  static const _ee = 0.00669342162296594323;

  /// 是否可视为境外（不做偏移）。
  static bool outOfChina(double lat, double lon) =>
      lon < 72.004 || lon > 137.8347 || lat < 0.8293 || lat > 55.8271;

  /// GPS / GPX 用的 WGS-84 → 高德图上的 GCJ-02。
  static LatLng wgs84ToGcj02(LatLng p) {
    final lat = p.latitude;
    final lon = p.longitude;
    if (outOfChina(lat, lon)) return p;
    final d = _delta(lat, lon);
    return LatLng(lat + d.$1, lon + d.$2);
  }

  /// 地图点选（GCJ-02）→ 存盘 / 路网用的 WGS-84。
  static LatLng gcj02ToWgs84(LatLng p) {
    final lat = p.latitude;
    final lon = p.longitude;
    if (outOfChina(lat, lon)) return p;
    var wgsLat = lat;
    var wgsLon = lon;
    for (var i = 0; i < 4; i++) {
      final d = _delta(wgsLat, wgsLon);
      wgsLat = lat - d.$1;
      wgsLon = lon - d.$2;
    }
    return LatLng(wgsLat, wgsLon);
  }

  static (double dLat, double dLon) _delta(double lat, double lon) {
    final dLat = _transformLat(lon - 105.0, lat - 35.0);
    final dLon = _transformLon(lon - 105.0, lat - 35.0);
    final radLat = lat / 180.0 * math.pi;
    var magic = math.sin(radLat);
    magic = 1 - _ee * magic * magic;
    final sqrtMagic = math.sqrt(magic);
    final latOff = (dLat * 180.0) /
        ((_a * (1 - _ee)) / (magic * sqrtMagic) * math.pi);
    final lonOff =
        (dLon * 180.0) / (_a / sqrtMagic * math.cos(radLat) * math.pi);
    return (latOff, lonOff);
  }

  static double _transformLat(double x, double y) {
    var r = -100.0 +
        2.0 * x +
        3.0 * y +
        0.2 * y * y +
        0.1 * x * y +
        0.2 * math.sqrt(x.abs());
    r += (20.0 * math.sin(6.0 * x * math.pi) +
            20.0 * math.sin(2.0 * x * math.pi)) *
        2.0 /
        3.0;
    r += (20.0 * math.sin(y * math.pi) + 40.0 * math.sin(y / 3.0 * math.pi)) *
        2.0 /
        3.0;
    r += (160.0 * math.sin(y / 12.0 * math.pi) +
            320.0 * math.sin(y * math.pi / 30.0)) *
        2.0 /
        3.0;
    return r;
  }

  static double _transformLon(double x, double y) {
    var r = 300.0 +
        x +
        2.0 * y +
        0.1 * x * x +
        0.1 * x * y +
        0.1 * math.sqrt(x.abs());
    r += (20.0 * math.sin(6.0 * x * math.pi) +
            20.0 * math.sin(2.0 * x * math.pi)) *
        2.0 /
        3.0;
    r += (20.0 * math.sin(x * math.pi) + 40.0 * math.sin(x / 3.0 * math.pi)) *
        2.0 /
        3.0;
    r += (150.0 * math.sin(x / 12.0 * math.pi) +
            300.0 * math.sin(x / 30.0 * math.pi)) *
        2.0 /
        3.0;
    return r;
  }
}

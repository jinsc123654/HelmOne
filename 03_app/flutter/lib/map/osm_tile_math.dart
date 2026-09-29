import 'dart:math' as math;

import 'package:latlong2/latlong.dart';

/// Web Mercator 瓦片编号（XYZ / OSM 方案）。
abstract class OsmTileMath {
  /// 经度 → 列号。
  static int lon2x(double lon, int z) {
    final n = 1 << z;
    final x = ((lon + 180.0) / 360.0 * n).floor();
    return x.clamp(0, n - 1);
  }

  /// 纬度 → 行号。
  static int lat2y(double lat, int z) {
    final n = 1 << z;
    final clamped = lat.clamp(-85.05112878, 85.05112878);
    final latRad = clamped * math.pi / 180.0;
    final y = ((1 -
                math.log(math.tan(latRad) + 1 / math.cos(latRad)) / math.pi) /
            2 *
            n)
        .floor();
    return y.clamp(0, n - 1);
  }

  /// 矩形范围内、给定缩放层的全部瓦片。
  static List<(int z, int x, int y)> tilesInBounds({
    required LatLng southWest,
    required LatLng northEast,
    required int minZoom,
    required int maxZoom,
  }) {
    final out = <(int, int, int)>[];
    for (var z = minZoom; z <= maxZoom; z++) {
      final x0 = lon2x(southWest.longitude, z);
      final x1 = lon2x(northEast.longitude, z);
      final y0 = lat2y(northEast.latitude, z);
      final y1 = lat2y(southWest.latitude, z);
      final minX = math.min(x0, x1);
      final maxX = math.max(x0, x1);
      final minY = math.min(y0, y1);
      final maxY = math.max(y0, y1);
      for (var x = minX; x <= maxX; x++) {
        for (var y = minY; y <= maxY; y++) {
          out.add((z, x, y));
        }
      }
    }
    return out;
  }

  /// 只计数量，不分配列表。
  static int countTiles({
    required LatLng southWest,
    required LatLng northEast,
    required int minZoom,
    required int maxZoom,
  }) {
    var n = 0;
    for (var z = minZoom; z <= maxZoom; z++) {
      final x0 = lon2x(southWest.longitude, z);
      final x1 = lon2x(northEast.longitude, z);
      final y0 = lat2y(northEast.latitude, z);
      final y1 = lat2y(southWest.latitude, z);
      final cols = (x0 - x1).abs() + 1;
      final rows = (y0 - y1).abs() + 1;
      n += cols * rows;
    }
    return n;
  }
}

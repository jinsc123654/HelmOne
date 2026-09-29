import 'dart:convert';

import 'package:latlong2/latlong.dart';

/// 与码表 `bicycle_favorites.tsv` / `navpts/*.tsv` 相同：`名字\t纬度\t经度`。
///
/// 名字按 **UTF-8 字节** 截到 [nameMax]，与固件
/// `MYVENDOR_DEVCTL_FAVORITE_NAME_MAX`（含 NUL）对齐。
abstract class WaypointTsv {
  static const nameMax = 47;
  static const maxPts = 32;

  /// 截到 [maxBytes] 个 UTF-8 字节，不切断多字节字符。
  static String capUtf8(String s, int maxBytes) {
    final units = utf8.encode(s);
    if (units.length <= maxBytes) {
      return s;
    }
    var n = maxBytes;
    while (n > 0 && (units[n] & 0xC0) == 0x80) {
      n--;
    }
    return utf8.decode(units.sublist(0, n));
  }

  static String sanitizeName(String raw, {String fallback = '点'}) {
    var s = raw.replaceAll(RegExp(r'[\t\r\n]'), ' ').trim();
    if (s.isEmpty) {
      s = fallback;
    }
    return capUtf8(s, nameMax);
  }

  static String encode(List<({String name, LatLng point})> pts) {
    final b = StringBuffer();
    final n = pts.length > maxPts ? maxPts : pts.length;
    for (var i = 0; i < n; i++) {
      final p = pts[i];
      b.write(sanitizeName(p.name, fallback: '${i + 1}'));
      b.write('\t');
      b.write(p.point.latitude.toStringAsFixed(7));
      b.write('\t');
      b.write(p.point.longitude.toStringAsFixed(7));
      b.write('\n');
    }
    return b.toString();
  }
}

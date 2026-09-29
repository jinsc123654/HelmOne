import 'dart:typed_data';

import 'package:geolocator/geolocator.dart';
import 'package:sifli_companion/ble/companion_proto.dart';

/// `struct companion_gnss_fix` 的编解码（特征 `0xFF14`，24 字节小端）。
///
/// 骑行中由 App 以 1–5 Hz 用「无响应写」推送给码表。
class CompanionGnssFix {
  /// 按协议字段构造。各字段均为已换算到协议单位的整数。
  const CompanionGnssFix({
    required this.latE7,
    required this.lonE7,
    required this.altMm,
    required this.speedCentiKmh,
    required this.courseDeg,
    required this.utcSec,
    required this.fixQuality,
    required this.satellites,
    required this.hdopX10,
  });

  /// 帧字节长度。
  static const length = 24;

  /// 纬度 × 1e7，WGS84。
  final int latE7;

  /// 经度 × 1e7。
  final int lonE7;

  /// 海拔，毫米。
  final int altMm;

  /// 速度，0.01 km/h。
  final int speedCentiKmh;

  /// 航向，0–359 度。
  final int courseDeg;

  /// UTC Unix 时间戳，秒。
  final int utcSec;

  /// 定位质量，取值见 [CompanionFixQuality]。
  final int fixQuality;

  /// 参与定位的卫星数。
  final int satellites;

  /// 水平精度因子 × 10。
  final int hdopX10;

  /// 由 geolocator 的 [Position] 换算构造。
  ///
  /// geolocator 不提供卫星数与 HDOP，因此 [satellites] 记 0，并用
  /// [Position.accuracy]（米）近似 [hdopX10]。[fixQuality] 依据是否有海拔数据
  /// 区分 2D / 3D。
  factory CompanionGnssFix.fromPosition(Position p) {
    final hasAltitude = p.altitude != 0 || p.altitudeAccuracy > 0;

    return CompanionGnssFix(
      latE7: _clampInt32((p.latitude * 1e7).round()),
      lonE7: _clampInt32((p.longitude * 1e7).round()),
      altMm: _clampInt32((p.altitude * 1000).round()),

      // Position.speed 是 m/s，协议要 0.01 km/h：m/s * 3.6 * 100 = m/s * 360。
      speedCentiKmh: _clampUint16((p.speed * 360).round()),
      courseDeg: _clampUint16(p.heading.isFinite ? p.heading.round() % 360 : 0),
      utcSec: p.timestamp.millisecondsSinceEpoch ~/ 1000,
      fixQuality:
          hasAltitude ? CompanionFixQuality.fix3d : CompanionFixQuality.fix2d,
      satellites: 0,
      hdopX10: _clampUint8((p.accuracy * 10).round()),
    );
  }

  /// 打包为 24 字节小端帧。
  Uint8List toBytes() {
    final bd = ByteData(length);
    bd.setInt32(0, latE7, Endian.little);
    bd.setInt32(4, lonE7, Endian.little);
    bd.setInt32(8, altMm, Endian.little);
    bd.setUint16(12, speedCentiKmh, Endian.little);
    bd.setUint16(14, courseDeg, Endian.little);
    bd.setUint32(16, utcSec, Endian.little);
    bd.setUint8(20, fixQuality);
    bd.setUint8(21, satellites);
    bd.setUint8(22, hdopX10);
    bd.setUint8(23, 0); // reserved
    return bd.buffer.asUint8List();
  }

  /// 解析 24 字节帧。轨迹点特征 `0xFF18` 与本结构同布局，末字节含义不同。
  factory CompanionGnssFix.fromBytes(List<int> data) {
    if (data.length < length) {
      throw FormatException('GNSS 帧长度应为 $length，实际 ${data.length}');
    }

    final bd = ByteData.sublistView(Uint8List.fromList(data));
    return CompanionGnssFix(
      latE7: bd.getInt32(0, Endian.little),
      lonE7: bd.getInt32(4, Endian.little),
      altMm: bd.getInt32(8, Endian.little),
      speedCentiKmh: bd.getUint16(12, Endian.little),
      courseDeg: bd.getUint16(14, Endian.little),
      utcSec: bd.getUint32(16, Endian.little),
      fixQuality: bd.getUint8(20),
      satellites: bd.getUint8(21),
      hdopX10: bd.getUint8(22),
    );
  }

  /// 纬度（度）。
  double get latitude => latE7 / 1e7;

  /// 经度（度）。
  double get longitude => lonE7 / 1e7;

  /// 海拔（米）。
  double get altitudeMeters => altMm / 1000.0;

  /// 速度（km/h）。
  double get speedKmh => speedCentiKmh / 100.0;

  @override
  String toString() =>
      'CompanionGnssFix(${latitude.toStringAsFixed(6)}, '
      '${longitude.toStringAsFixed(6)}, alt=${altitudeMeters.toStringAsFixed(1)}m, '
      'speed=${speedKmh.toStringAsFixed(2)}km/h, q=$fixQuality, sats=$satellites)';

  static int _clampInt32(int v) =>
      v.clamp(-2147483648, 2147483647).toInt();

  static int _clampUint16(int v) => v.clamp(0, 0xFFFF).toInt();

  static int _clampUint8(int v) => v.clamp(0, 0xFF).toInt();
}

/// 由经纬度直接构造一帧，便于联调时手填坐标。
CompanionGnssFix debugGnssFix({
  required double latitude,
  required double longitude,
  double altitudeMeters = 0,
  double speedKmh = 0,
  double courseDeg = 0,
}) {
  return CompanionGnssFix(
    latE7: (latitude * 1e7).round(),
    lonE7: (longitude * 1e7).round(),
    altMm: (altitudeMeters * 1000).round(),
    speedCentiKmh: (speedKmh * 100).round().clamp(0, 0xFFFF),
    courseDeg: courseDeg.round() % 360,
    utcSec: DateTime.now().millisecondsSinceEpoch ~/ 1000,
    fixQuality: CompanionFixQuality.fix3d,
    satellites: 12,
    hdopX10: 10,
  );
}

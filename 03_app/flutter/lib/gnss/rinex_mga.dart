import 'dart:math' as math;
import 'dart:typed_data';

import 'package:archive/archive.dart';
import 'package:sifli_companion/gnss/mga_ubx.dart';

/// 把 IGS/BRDC 广播星历（RINEX 3 MIXED NAV）编成 MAX-M10S 用的 UBX-MGA。
///
/// 不做 AssistNow 账号；公开 BRDC 下载后在手机上生成 MGA-GPS/BDS/GAL/QZSS。
abstract class RinexMga {
  /// 超过此时差的历元丢掉（秒）。
  ///
  /// 星历有效期只有 2–4 小时，且 u-blox 明确「过期/快过期的会被拒收」，
  /// 所以按 4 小时卡（原来 8 小时会放过已经不能用的历元）。
  static const maxAgeSec = 4 * 3600;

  /// 解压（若是 gzip）并转换。失败抛 [RinexMgaException]。
  static Uint8List convertBytes(Uint8List raw, {DateTime? now}) {
    var data = raw;
    if (data.length >= 2 && data[0] == 0x1f && data[1] == 0x8b) {
      data = Uint8List.fromList(GZipDecoder().decodeBytes(data));
    }
    return convertText(String.fromCharCodes(data), now: now);
  }

  /// 解析 RINEX 3 NAV 文本。
  static Uint8List convertText(String text, {DateTime? now}) {
    final utc = (now ?? DateTime.now()).toUtc();
    final recs = _parse(text);
    if (recs.isEmpty) {
      throw const RinexMgaException('不是 RINEX 3 广播星历（没有 G/C/E/J 历元）');
    }

    final best = <(String, int), _Rec>{};
    for (final r in recs) {
      if (r.vals.length < 27) continue;
      final key = (r.sys, r.prn);
      final prev = best[key];
      if (prev == null || r.age(utc) < prev.age(utc)) {
        best[key] = r;
      }
    }

    final frames = BytesBuilder(copy: false);
    var n = 0;
    final keys = best.keys.toList()
      ..sort((a, b) {
        final c = a.$1.compareTo(b.$1);
        return c != 0 ? c : a.$2.compareTo(b.$2);
      });
    for (final k in keys) {
      final r = best[k]!;
      if (r.age(utc) > maxAgeSec) continue;
      final Uint8List? fr;
      switch (r.sys) {
        case 'G':
          fr = _packGpsLike(0x00, r);
        case 'J':
          fr = _packGpsLike(0x05, r);
        case 'E':
          fr = _packGal(r);
        case 'C':
          fr = r.prn <= 37 ? _packBds(r) : null;
        default:
          fr = null;
      }
      if (fr != null) {
        frames.add(fr);
        n++;
      }
    }

    final out = frames.takeBytes();
    if (n == 0 || out.isEmpty) {
      throw const RinexMgaException('星历已过期或没有可用卫星');
    }
    return out;
  }
}

/// 转换失败。
class RinexMgaException implements Exception {
  /// 人类可读原因。
  const RinexMgaException(this.message);

  /// 原因。
  final String message;

  @override
  String toString() => message;
}

class _Rec {
  _Rec(this.sys, this.prn, this.epoch, this.vals);
  final String sys;
  final int prn;
  final DateTime epoch;
  final List<double> vals;

  double age(DateTime now) => epoch.difference(now).inSeconds.abs().toDouble();
}

final _sci = RegExp(r'[+-]?(?:\d+\.\d+|\.\d+|\d+)[Ee][+-]?\d+');
final _epoch = RegExp(
  r'^([GCEJ])(\d{2})\s+(\d{4})\s+(\d{1,2})\s+(\d{1,2})\s+(\d{1,2})\s+(\d{1,2})\s+(\d{1,2})',
);

List<double> _floats(String s) {
  return _sci
      .allMatches(s.replaceAll('D', 'E').replaceAll('d', 'e'))
      .map((m) => double.parse(m.group(0)!))
      .toList();
}

List<_Rec> _parse(String text) {
  final lines = text.split('\n');
  var start = 0;
  for (var i = 0; i < lines.length; i++) {
    if (lines[i].contains('END OF HEADER')) {
      start = i + 1;
      break;
    }
  }

  final out = <_Rec>[];
  var i = start;
  while (i < lines.length) {
    final m = _epoch.firstMatch(lines[i]);
    if (m == null) {
      i++;
      continue;
    }
    final block = <String>[lines[i]];
    var j = i + 1;
    while (j < lines.length &&
        j < i + 8 &&
        _epoch.firstMatch(lines[j]) == null) {
      block.add(lines[j]);
      j++;
    }
    try {
      final epoch = DateTime.utc(
        int.parse(m.group(3)!),
        int.parse(m.group(4)!),
        int.parse(m.group(5)!),
        int.parse(m.group(6)!),
        int.parse(m.group(7)!),
        int.parse(m.group(8)!),
      );
      final vals = <double>[];
      for (var k = 0; k < block.length; k++) {
        final ln = block[k];
        vals.addAll(_floats(k == 0 ? ln.substring(m.end) : ln));
      }
      out.add(_Rec(m.group(1)!, int.parse(m.group(2)!), epoch, vals));
    } catch (_) {}
    i = j;
  }
  return out;
}

int _clamp(int x, int lo, int hi) {
  if (x < lo) return lo;
  if (x > hi) return hi;
  return x;
}

int _iScale(num? val, double scale, int bits, {required bool signed}) {
  final v = val;
  var x = 0;
  if (v != null && v.isFinite) {
    x = (v / scale).round();
  }
  if (signed) {
    final lo = -(1 << (bits - 1));
    final hi = (1 << (bits - 1)) - 1;
    return _clamp(x, lo, hi);
  }
  return _clamp(x, 0, bits >= 32 ? 0xffffffff : (1 << bits) - 1);
}

int _ura(double? meters) {
  const table = [
    2.4, 3.4, 4.85, 6.85, 9.65, 13.65, 24.0, 48.0, 96.0, 192.0,
    384.0, 768.0, 1536.0, 3072.0, 6144.0,
  ];
  if (meters == null || meters < 0) return 15;
  for (var i = 0; i < table.length; i++) {
    if (meters <= table[i]) return i;
  }
  return 15;
}

(int, double) _gpsWeekSow(DateTime dt) {
  final epoch = DateTime.utc(1980, 1, 6);
  final sec = dt.difference(epoch).inMilliseconds / 1000.0;
  final week = sec ~/ 604800;
  return (week, sec - week * 604800);
}

double _sc(num rad) => rad / math.pi;

double _v(List<double> v, int i, [double d = 0]) =>
    i < v.length ? v[i] : d;

class _Le {
  _Le(int n) : b = ByteData(n);
  final ByteData b;
  Uint8List get bytes => b.buffer.asUint8List();
  void u1(int o, int v) => b.setUint8(o, v & 0xff);
  void i1(int o, int v) => b.setInt8(o, v);
  void u2(int o, int v) => b.setUint16(o, v & 0xffff, Endian.little);
  void i2(int o, int v) => b.setInt16(o, v, Endian.little);
  void u4(int o, int v) => b.setUint32(o, v & 0xffffffff, Endian.little);
  void i4(int o, int v) => b.setInt32(o, v, Endian.little);
}

Uint8List _packGpsLike(int mid, _Rec r) {
  final v = r.vals;
  final af0 = _v(v, 0);
  final af1 = _v(v, 1);
  final af2 = _v(v, 2);
  final iode = _v(v, 3);
  final crs = _v(v, 4);
  final dn = _v(v, 5);
  final m0 = _v(v, 6);
  final cuc = _v(v, 7);
  final e = _v(v, 8);
  final cus = _v(v, 9);
  final sqrtA = _v(v, 10);
  final toe = _v(v, 11);
  final cic = _v(v, 12);
  final omega0 = _v(v, 13);
  final cis = _v(v, 14);
  final i0 = _v(v, 15);
  final crc = _v(v, 16);
  final omega = _v(v, 17);
  final odot = _v(v, 18);
  final idot = _v(v, 19);
  final acc = _v(v, 23, 2.4);
  final health = _v(v, 24).toInt();
  final tgd = _v(v, 25);
  final iodc = v.length > 26 ? _v(v, 26).toInt() : iode.toInt();
  final fit = _v(v, 28);
  final toc = _gpsWeekSow(r.epoch).$2;
  final p = _Le(68);
  p.u1(0, 0x01);
  p.u1(2, r.prn);
  p.u1(4, (fit == 0 || fit <= 4) ? 0 : 1);
  p.u1(5, _ura(acc));
  p.u1(6, health & 0x3f);
  p.i1(7, _iScale(tgd, math.pow(2, -31).toDouble(), 8, signed: true));
  p.u2(8, _clamp(iodc, 0, 65535));
  p.u2(10, _iScale(toc, 16, 16, signed: false));
  p.i1(13, _iScale(af2, math.pow(2, -55).toDouble(), 8, signed: true));
  p.i2(14, _iScale(af1, math.pow(2, -43).toDouble(), 16, signed: true));
  p.i4(16, _iScale(af0, math.pow(2, -31).toDouble(), 32, signed: true));
  p.i2(20, _iScale(crs, math.pow(2, -5).toDouble(), 16, signed: true));
  p.i2(22, _iScale(_sc(dn), math.pow(2, -43).toDouble(), 16, signed: true));
  p.i4(24, _iScale(_sc(m0), math.pow(2, -31).toDouble(), 32, signed: true));
  p.i2(28, _iScale(cuc, math.pow(2, -29).toDouble(), 16, signed: true));
  // 顺序照 UBX-MGA-GPS/QZSS-EPH 字段表：cus 紧跟 cuc（30），e 在 32。
  // 别照 GAL 的分组习惯去猜 —— GPS/QZSS 这张表里谐波项是散开的
  // （crs 在 20、cuc/cus 在 28/30、cic/cis 在 42/48、crc 在 50）。
  // 写反了 e 会被解成 7 倍大（0.00197 → 0.0141），轨道半径偏三百多公里。
  p.i2(30, _iScale(cus, math.pow(2, -29).toDouble(), 16, signed: true));
  p.u4(32, _iScale(e, math.pow(2, -33).toDouble(), 32, signed: false));
  p.u4(36, _iScale(sqrtA, math.pow(2, -19).toDouble(), 32, signed: false));
  p.u2(40, _iScale(toe, 16, 16, signed: false));
  p.i2(42, _iScale(cic, math.pow(2, -29).toDouble(), 16, signed: true));
  p.i4(44, _iScale(_sc(omega0), math.pow(2, -31).toDouble(), 32, signed: true));
  p.i2(48, _iScale(cis, math.pow(2, -29).toDouble(), 16, signed: true));
  // 偏移照 u-blox MGA-GPS-EPH 字段表：crc 在 50（I2 ×2⁻⁵ m）、i0 在 52（I4 ×2⁻³¹ 半圆）。
  // 这两个写反了模组照样回「accepted」——ACK 只校验版本/长度/存储，不校验数值——
  // 但存进去的是垃圾：crc 取到 i0 的低半字、i0 取到 i0 的高半字拼上 crc，
  // 倾角偏 0.3°、卫星位置偏上百公里，静默劣化定位。
  p.i2(50, _iScale(crc, math.pow(2, -5).toDouble(), 16, signed: true));
  p.i4(52, _iScale(_sc(i0), math.pow(2, -31).toDouble(), 32, signed: true));
  p.i4(56, _iScale(_sc(omega), math.pow(2, -31).toDouble(), 32, signed: true));
  p.i4(60, _iScale(_sc(odot), math.pow(2, -43).toDouble(), 32, signed: true));
  p.i2(64, _iScale(_sc(idot), math.pow(2, -43).toDouble(), 16, signed: true));
  return MgaUbx.buildFrame(0x13, mid, p.bytes);
}

Uint8List _packGal(_Rec r) {
  final v = r.vals;
  final toc = _gpsWeekSow(r.epoch).$2;
  var bgd = _v(v, 25);
  if (v.length > 26) bgd = _v(v, 26);
  final p = _Le(76);
  p.u1(0, 0x01);
  p.u1(2, r.prn);
  p.u2(4, _clamp(_v(v, 3).toInt(), 0, 65535));
  p.i2(6, _iScale(_sc(_v(v, 5)), math.pow(2, -43).toDouble(), 16, signed: true));
  p.i4(8, _iScale(_sc(_v(v, 6)), math.pow(2, -31).toDouble(), 32, signed: true));
  p.u4(12, _iScale(_v(v, 8), math.pow(2, -33).toDouble(), 32, signed: false));
  p.u4(16, _iScale(_v(v, 10), math.pow(2, -19).toDouble(), 32, signed: false));
  p.i4(20, _iScale(_sc(_v(v, 13)), math.pow(2, -31).toDouble(), 32, signed: true));
  p.i4(24, _iScale(_sc(_v(v, 15)), math.pow(2, -31).toDouble(), 32, signed: true));
  p.i4(28, _iScale(_sc(_v(v, 17)), math.pow(2, -31).toDouble(), 32, signed: true));
  p.i4(32, _iScale(_sc(_v(v, 18)), math.pow(2, -43).toDouble(), 32, signed: true));
  p.i2(36, _iScale(_sc(_v(v, 19)), math.pow(2, -43).toDouble(), 16, signed: true));
  p.i2(38, _iScale(_v(v, 7), math.pow(2, -29).toDouble(), 16, signed: true));
  p.i2(40, _iScale(_v(v, 9), math.pow(2, -29).toDouble(), 16, signed: true));
  p.i2(42, _iScale(_v(v, 16), math.pow(2, -5).toDouble(), 16, signed: true));
  p.i2(44, _iScale(_v(v, 4), math.pow(2, -5).toDouble(), 16, signed: true));
  p.i2(46, _iScale(_v(v, 12), math.pow(2, -29).toDouble(), 16, signed: true));
  p.i2(48, _iScale(_v(v, 14), math.pow(2, -29).toDouble(), 16, signed: true));
  p.u2(50, _iScale(_v(v, 11), 60, 16, signed: false));
  p.i4(52, _iScale(_v(v, 0), math.pow(2, -34).toDouble(), 32, signed: true));
  p.i4(56, _iScale(_v(v, 1), math.pow(2, -46).toDouble(), 32, signed: true));
  p.i1(60, _iScale(_v(v, 2), math.pow(2, -59).toDouble(), 8, signed: true));
  p.u1(61, _clamp(_v(v, 23).toInt(), 0, 255));
  p.u2(62, _iScale(toc, 60, 16, signed: false));
  p.i2(64, _iScale(bgd, math.pow(2, -32).toDouble(), 16, signed: true));
  return MgaUbx.buildFrame(0x13, 0x02, p.bytes);
}

Uint8List _packBds(_Rec r) {
  final v = r.vals;
  final sow = _gpsWeekSow(r.epoch).$2;
  final toc = (sow - 14.0) % 604800.0;
  final p = _Le(88);
  p.u1(0, 0x01);
  p.u1(2, r.prn);
  p.u1(4, _v(v, 24) != 0 ? 1 : 0);
  p.u1(5, _clamp(_v(v, 26, _v(v, 3)).toInt(), 0, 255));
  p.i2(6, _iScale(_v(v, 2), math.pow(2, -66).toDouble(), 16, signed: true));
  p.i4(8, _iScale(_v(v, 1), math.pow(2, -50).toDouble(), 32, signed: true));
  p.i4(12, _iScale(_v(v, 0), math.pow(2, -33).toDouble(), 32, signed: true));
  p.u4(16, _iScale(toc, 8, 32, signed: false));
  p.i2(20, _iScale(_v(v, 25) * 1e9, 0.1, 16, signed: true));
  p.u1(22, _ura(_v(v, 23, 2.4)));
  p.u1(23, _clamp(_v(v, 3).toInt(), 0, 255));
  p.u4(24, _iScale(_v(v, 11), 8, 32, signed: false));
  p.u4(28, _iScale(_v(v, 10), math.pow(2, -19).toDouble(), 32, signed: false));
  p.u4(32, _iScale(_v(v, 8), math.pow(2, -33).toDouble(), 32, signed: false));
  p.i4(36, _iScale(_sc(_v(v, 17)), math.pow(2, -31).toDouble(), 32, signed: true));
  p.i2(40, _iScale(_sc(_v(v, 5)), math.pow(2, -43).toDouble(), 16, signed: true));
  p.i2(42, _iScale(_sc(_v(v, 19)), math.pow(2, -43).toDouble(), 16, signed: true));
  p.i4(44, _iScale(_sc(_v(v, 6)), math.pow(2, -31).toDouble(), 32, signed: true));
  p.i4(48, _iScale(_sc(_v(v, 13)), math.pow(2, -31).toDouble(), 32, signed: true));
  p.i4(52, _iScale(_sc(_v(v, 18)), math.pow(2, -43).toDouble(), 32, signed: true));
  p.i4(56, _iScale(_sc(_v(v, 15)), math.pow(2, -31).toDouble(), 32, signed: true));
  p.i4(60, _iScale(_v(v, 7), math.pow(2, -31).toDouble(), 32, signed: true));
  p.i4(64, _iScale(_v(v, 9), math.pow(2, -31).toDouble(), 32, signed: true));
  p.i4(68, _iScale(_v(v, 16), math.pow(2, -6).toDouble(), 32, signed: true));
  p.i4(72, _iScale(_v(v, 4), math.pow(2, -6).toDouble(), 32, signed: true));
  p.i4(76, _iScale(_v(v, 12), math.pow(2, -31).toDouble(), 32, signed: true));
  p.i4(80, _iScale(_v(v, 14), math.pow(2, -31).toDouble(), 32, signed: true));
  return MgaUbx.buildFrame(0x13, 0x03, p.bytes);
}

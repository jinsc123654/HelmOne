import 'dart:convert';
import 'dart:typed_data';

import 'package:archive/archive.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:sifli_companion/gnss/eph_brdc.dart';
import 'package:sifli_companion/gnss/mga_ubx.dart';
import 'package:sifli_companion/gnss/rinex_mga.dart';

String _sci(num v) {
  final s = v.toStringAsExponential(12).toUpperCase();
  return s.padLeft(19);
}

/// 一组默认的 GPS 星历数值（顺序同 RINEX 3 NAV 的 ICD 字段序）。
List<double> _gpsDefaults() => <double>[
  1e-4, 2e-12, 0, // af0 af1 af2
  10, 5, 1e-9, 1.0, // iode crs dn m0
  1e-7, 0.01, 1e-7, 5153.613, // cuc e cus sqrtA
  432000, 1e-8, 1.0, 1e-8, // toe cic omega0 cis
  1.0, 200, 1.0, -2e-9, // i0 crc omega odot
  1e-10, 0, 2372, 0, // idot codes week unused
  2.4, 0, -1e-8, 10, // acc health tgd iodc
  0, 0, 0, 0,
];

String _gpsNav({
  required DateTime epoch,
  required int prn,
  List<double>? values,
}) {
  final floats = values ?? _gpsDefaults();
  final y = epoch.year.toString().padLeft(4, '0');
  final mo = epoch.month.toString().padLeft(2, ' ');
  final d = epoch.day.toString().padLeft(2, ' ');
  final h = epoch.hour.toString().padLeft(2, ' ');
  final mi = epoch.minute.toString().padLeft(2, ' ');
  final head =
      'G${prn.toString().padLeft(2, '0')} $y $mo $d $h $mi  0.0000000'
      '${_sci(floats[0])}${_sci(floats[1])}${_sci(floats[2])}';
  final lines = <String>[head];
  var i = 3;
  while (i < floats.length) {
    final buf = StringBuffer('    ');
    for (var k = 0; k < 4 && i < floats.length; k++, i++) {
      buf.write(_sci(floats[i]));
    }
    lines.add(buf.toString());
  }
  return '${lines.join('\n')}\n';
}

void main() {
  test('RINEX 3 GPS NAV becomes one MGA-GPS frame', () {
    final epoch = DateTime.utc(2026, 8, 30, 12);
    final text = '     3.05           N: GNSS NAV DATA    M: MIXED\n'
        '                                                            END OF HEADER\n'
        '${_gpsNav(epoch: epoch, prn: 1)}';
    final mga = RinexMga.convertText(text, now: epoch);
    final parsed = MgaUbx.parse(mga);
    expect(parsed.ok, isTrue);
    expect(parsed.frames, 1);
    expect(parsed.types['MGA-GPS'], 1);
    expect(mga[2], 0x13);
    expect(mga[3], 0x00);
    expect(mga[4] | (mga[5] << 8), 68);
  });

  test('MGA-GPS-EPH：字段落在规范偏移上（cus/e 与 crc/i0 两处）', () {
    // 每组取「原始整数是整齐的数」的输入值：
    //   cuc = -64×2⁻²⁹、cus = 32×2⁻²⁹、e = 0.125（→ 2³⁰）、
    //   i0 = π/2 rad（半圆单位 0.5 → 2³⁰）、crc = -32 m（→ -1024）
    final vals = _gpsDefaults()
      ..[7] = -1.1920928955078125e-7
      ..[8] = 0.125
      ..[9] = 5.9604644775390625e-8
      ..[15] = 1.5707963267948966
      ..[16] = -32.0;
    final epoch = DateTime.utc(2026, 8, 30, 12);
    final text =
        '     3.05           N: GNSS NAV DATA    M: MIXED\n'
        '                                                            END OF HEADER\n'
        '${_gpsNav(epoch: epoch, prn: 1, values: vals)}';
    final mga = RinexMga.convertText(text, now: epoch);
    expect(MgaUbx.parse(mga).ok, isTrue);

    // 载荷从 mga[6] 起（B5 62 / cls / id / len / len）。
    final pl = mga.sublist(6);

    // cuc@28（-64 → C0 FF）、cus@30（32 → 20 00）、e@32（2³⁰ → 00 00 00 40）
    expect(pl.sublist(28, 30), <int>[0xC0, 0xFF]);
    expect(pl.sublist(30, 32), <int>[0x20, 0x00]);
    expect(pl.sublist(32, 36), <int>[0x00, 0x00, 0x00, 0x40]);

    // crc@50（-1024 → 00 FC）、i0@52（2³⁰ → 00 00 00 40）
    expect(pl.sublist(50, 52), <int>[0x00, 0xFC]);
    expect(pl.sublist(52, 56), <int>[0x00, 0x00, 0x00, 0x40]);

    // 写反时：cus/e 那组会变成 C0 FF 00 00 00 40 20 00，
    //          crc/i0 那组会变成 00 00 00 40 00 FC —— 两条断言都会挂。
  });

  test('gzip BRDC bytes convert the same', () {
    final epoch = DateTime.utc(2026, 8, 30, 12);
    final text = '     3.05           N: GNSS NAV DATA    M: MIXED\n'
        '                                                            END OF HEADER\n'
        '${_gpsNav(epoch: epoch, prn: 7)}';
    final gz = GZipEncoder().encode(utf8.encode(text));
    final mga = RinexMga.convertBytes(Uint8List.fromList(gz), now: epoch);
    expect(MgaUbx.parse(mga).ok, isTrue);
  });

  test('stale epochs are dropped', () {
    final epoch = DateTime.utc(2026, 8, 30, 0);
    final text = 'END OF HEADER\n${_gpsNav(epoch: epoch, prn: 1)}';
    expect(
      () => RinexMga.convertText(
        text,
        now: epoch.add(const Duration(hours: 12)),
      ),
      throwsA(isA<RinexMgaException>()),
    );
  });

  test('BKG BRDC URL uses zero-padded doy', () {
    final uris = EphBrdc.urisFor(DateTime.utc(2026, 8, 30));
    expect(uris.any((u) => u.path.contains('/2026/242/')), isTrue);
    expect(uris.any((u) => u.path.contains('20262420000')), isTrue);
    expect(uris.any((u) => u.path.contains('BRDC00WRD')), isTrue);
  });
}

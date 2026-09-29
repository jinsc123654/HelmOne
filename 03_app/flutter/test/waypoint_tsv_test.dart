import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:latlong2/latlong.dart';
import 'package:sifli_companion/ride/gpx_util.dart';
import 'package:sifli_companion/ride/waypoint_tsv.dart';

void main() {
  test('encode tabs and 7-decimal coords', () {
    final tsv = WaypointTsv.encode([
      (name: '家', point: const LatLng(32.0, 118.0)),
      (name: '公司', point: const LatLng(32.1, 118.1)),
    ]);
    expect(tsv, '家\t32.0000000\t118.0000000\n公司\t32.1000000\t118.1000000\n');
  });

  test('strips tab and caps utf-8 name to 47 bytes', () {
    expect(WaypointTsv.sanitizeName('家\t园'), '家 园');
    final long = '点' * 40;
    final name = WaypointTsv.sanitizeName(long);
    expect(utf8.encode(name).length, lessThanOrEqualTo(47));
    expect(name.startsWith('点'), isTrue);
  });

  test('caps at 32 points', () {
    final pts = [
      for (var i = 0; i < 40; i++)
        (name: '$i', point: LatLng(32 + i * 0.001, 118.0)),
    ];
    expect(WaypointTsv.encode(pts).split('\n').where((l) => l.isNotEmpty).length, 32);
  });

  test('safeTsvFileName keeps tsv and utf-8 length', () {
    expect(GpxUtil.safeTsvFileName('环 城.tsv'), '环_城.tsv');
    final name = GpxUtil.safeTsvFileName('点' * 40);
    expect(name.endsWith('.tsv'), isTrue);
    expect(utf8.encode(name).length, lessThanOrEqualTo(40));
  });
}

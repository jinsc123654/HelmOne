import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:sifli_companion/ble/companion_rle.dart';

void main() {
  test('empty roundtrip', () {
    expect(CompanionRle.encode(<int>[]), isEmpty);
    expect(CompanionRle.decode(<int>[]), isEmpty);
  });

  test('run of 100 bytes is 4-byte RLE', () {
    final src = Uint8List.fromList(List<int>.filled(100, 0x61));
    final enc = CompanionRle.encode(src);
    expect(enc.length, 4);
    expect(CompanionRle.decode(enc), src);
  });

  test('text roundtrip', () {
    final src = Uint8List.fromList(
      List<int>.generate(20, (_) => 0).expand((_) => 'hello world\n'.codeUnits).toList(),
    );
    final enc = CompanionRle.encode(src);
    expect(enc.length, lessThan(src.length ~/ 2));
    expect(CompanionRle.decode(enc), src);
  });

  test('gpx-like tags roundtrip and shrink', () {
    const pt =
        '<trkpt lat="40.048031" lon="116.309013"><ele>0.00</ele>'
        '<time>2024-11-11T11:38:04Z</time></trkpt>\n';
    final src = Uint8List.fromList(
      List<int>.generate(80, (_) => 0).expand((_) => pt.codeUnits).toList(),
    );
    final enc = CompanionRle.encode(src);
    expect(enc.length, lessThan((src.length * 0.5).floor()));
    expect(CompanionRle.decode(enc), src);
  });

  test('incompressible stays near 1:1', () {
    final src = Uint8List.fromList(
      List<int>.generate(256, (i) => i).expand((b) => [b, b ^ 0x5a, b ^ 0xa5]).toList(),
    );
    final enc = CompanionRle.encode(src);
    expect(enc.length, lessThan(src.length + src.length ~/ 8));
    expect(CompanionRle.decode(enc), src);
  });

  test('shouldCompress by extension', () {
    expect(CompanionRle.shouldCompress('Track/a.gpx'), isTrue);
    expect(CompanionRle.shouldCompress('notes.TXT'), isTrue);
    expect(CompanionRle.shouldCompress('map.bin'), isFalse);
  });

  test('encode of a mid-file slice still roundtrips', () {
    const pt =
        '<trkpt lat="40.048031" lon="116.309013"><ele>0.00</ele>'
        '<time>2024-11-11T11:38:04Z</time></trkpt>\n';
    final src = Uint8List.fromList(
      List<int>.generate(40, (_) => 0).expand((_) => pt.codeUnits).toList(),
    );
    for (final off in [1, 17, src.length ~/ 3, src.length - 8]) {
      final remain = src.sublist(off);
      expect(CompanionRle.decode(CompanionRle.encode(remain)), remain);
    }
  });
}

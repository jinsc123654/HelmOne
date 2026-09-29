import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:sifli_companion/ble/companion_proto.dart';
import 'package:sifli_companion/gnss/eph_service.dart';
import 'package:sifli_companion/gnss/mga_ubx.dart';

void main() {
  test('buildFrame checksum round-trips through parse', () {
    final dbd = MgaUbx.buildFrame(0x13, 0x80, List<int>.filled(12, 0));
    final gps = MgaUbx.buildFrame(0x13, 0x00, List<int>.filled(68, 1));
    final blob = Uint8List.fromList([...dbd, ...gps]);
    final parsed = MgaUbx.parse(blob);
    expect(parsed.ok, isTrue);
    expect(parsed.frames, 2);
    expect(parsed.mgaFrames, 2);
    expect(parsed.dbdFrames, 1);
    expect(parsed.isDbdDump, isFalse);
    expect(parsed.types['MGA-DBD'], 1);
    expect(parsed.types['MGA-GPS'], 1);
  });

  test('pure DBD dump is flagged', () {
    final a = MgaUbx.buildFrame(0x13, 0x80);
    final b = MgaUbx.buildFrame(0x13, 0x80, [1, 0, 0, 1]);
    final parsed = MgaUbx.parse(Uint8List.fromList([...a, ...b]));
    expect(parsed.ok, isTrue);
    expect(parsed.isDbdDump, isTrue);
  });

  test('rejects non-UBX and truncated tails', () {
    expect(MgaUbx.parse(Uint8List.fromList([0, 1, 2])).ok, isFalse);
    expect(MgaUbx.parse(Uint8List(0)).ok, isFalse);

    final frame = MgaUbx.buildFrame(0x13, 0x80, [1, 2, 3]);
    final cut = Uint8List.fromList(frame.sublist(0, frame.length - 1));
    final parsed = MgaUbx.parse(cut);
    expect(parsed.ok, isFalse);
    expect(parsed.leftover, greaterThan(0));
  });

  test('rejects ACK-only streams', () {
    final ack = MgaUbx.buildFrame(0x05, 0x01, [0x13, 0x80]);
    final parsed = MgaUbx.parse(ack);
    expect(parsed.frames, 1);
    expect(parsed.mgaFrames, 0);
    expect(parsed.ok, isFalse);
    expect(EphException(parsed).toString(), contains('MGA'));
  });

  test('mga_<utc>.ubx names parse and sort', () {
    expect(MgaUbx.fileName(utcSec: 1788098770), 'mga_1788098770.ubx');
    expect(MgaUbx.parseFileNameUtc('mga_1788098770.ubx'), 1788098770);
    expect(
      MgaUbx.parseFileNameUtc('eph/mga_20260830T140610Z.ubx'),
      1788098770,
    );
    expect(MgaUbx.parseFileNameUtc('mga.ubx'), isNull);
    expect(CompanionFs.ephPath('mga_1.ubx'), 'eph/mga_1.ubx');
  });
}

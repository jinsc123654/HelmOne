import 'package:flutter_test/flutter_test.dart';
import 'package:sifli_companion/ble/companion_proto.dart';

void main() {
  test('crc32part continues across a split', () {
    const a = [1, 2, 3, 4, 5];
    const b = [6, 7, 8, 9];
    final whole = [...a, ...b];
    expect(
      CompanionFs.crc32Part(b, CompanionFs.crc32Part(a)),
      CompanionFs.crc32Part(whole),
    );
  });

  test('resume prefix matches the WRITE_OPEN crc', () {
    final data = List<int>.generate(1000, (i) => i & 0xff);
    const off = 400;
    final partCrc = CompanionFs.crc32Part(data.sublist(0, off));
    expect(CompanionFs.resumePrefixMatches(data, off, partCrc), isTrue);
    expect(CompanionFs.resumePrefixMatches(data, off, partCrc ^ 1), isFalse);
  });

  test('corrupt tail is not a valid resume prefix', () {
    final data = List<int>.generate(64, (i) => i);
    final part = [...data.sublist(0, 40), 0xff, 0xff];
    final partCrc = CompanionFs.crc32Part(part);
    expect(
      CompanionFs.resumePrefixMatches(data, part.length, partCrc),
      isFalse,
    );
  });
}

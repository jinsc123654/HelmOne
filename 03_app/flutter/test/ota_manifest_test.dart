import 'package:flutter_test/flutter_test.dart';
import 'package:sifli_companion/ble/companion_proto.dart';
import 'package:sifli_companion/ota/ota_config.dart';
import 'package:sifli_companion/ota/ota_manifest.dart';
import 'package:sifli_companion/ota/ota_service.dart';

void main() {
  test('firmware.json parses and names the KV slot file', () {
    final m = OtaManifest.fromJson({
      'version': '1.0.0',
      'hardware': '1.0.0',
      'url':
          'http://ota.jinsc.top/ota/helm-one/firmware.bin?token=abc&hw=1.0.0',
      'sha256':
          '7cb5b8fa5c42f0d83f44c976ee30c3a3e413106ffc9dfa890402ee146a92edae',
      'size': 1765580,
      'note': 'V1',
    });
    expect(m.version, '1.0.0');
    expect(m.date, isEmpty);
    expect(m.slotFileName, '1.0.0-Helm-One.bin');
    expect(m.slotPath, 'fw/1.0.0-Helm-One.bin');
    expect(m.downloadUri.scheme, 'https');
    expect(m.downloadUri.host, OtaConfig.host);
  });

  test('firmware.json date accepts day, ISO and unix seconds', () {
    expect(
      OtaManifest.fromJson({
        'version': '1.2.3',
        'url': 'https://ota.jinsc.top/ota/helm-one/firmware.bin',
        'sha256':
            '7cb5b8fa5c42f0d83f44c976ee30c3a3e413106ffc9dfa890402ee146a92edae',
        'size': 100,
        'date': '2026-09-13',
      }).date,
      '2026-09-13',
    );
    expect(OtaManifest.parseDate('2026-09-13T08:00:00Z'), '2026-09-13');
    expect(OtaManifest.parseDate(1757721600), isNotEmpty);
  });

  test('version ranking prefers the highest X.Y.Z filename', () {
    expect(
      OtaVersion.highestLabel([
        'a.bin',
        '1.0.0-Helm-One.bin',
        '1.0.1.bin',
        'readme.txt',
        '0.9.9-Helm-One.bin',
      ]),
      '1.0.1.bin',
    );
    expect(
      OtaVersion.parseOrZero('1.0.1').compareTo(OtaVersion.parseOrZero('1.0.0')),
      greaterThan(0),
    );
  });

  test('0xFF1A device info parses slot, sw, hw, fw name', () {
    final raw = List<int>.filled(CompanionDevInfo.length, 0);
    raw[0] = CompanionSlot.fw;
    raw.setRange(1, 1 + '1.0.0'.length, '1.0.0'.codeUnits);
    raw.setRange(33, 33 + '1.0.0'.length, '1.0.0'.codeUnits);
    const name = '1.0.0-Helm-One.bin';
    raw.setRange(49, 49 + name.length, name.codeUnits);
    final info = CompanionDevInfo.fromBytes(raw);
    expect(info.slotName, 'fw');
    expect(info.swVersion, '1.0.0');
    expect(info.hwVersion, '1.0.0');
    expect(info.fwName, name);
    expect(info.fwLabel, '1.0.0-Helm-One');
    expect(info.bootVersion, isEmpty);
  });

  test('0xFF1A v2 appends 2SFBL version', () {
    final raw = List<int>.filled(CompanionDevInfo.length, 0);
    raw[0] = CompanionSlot.main;
    raw.setRange(1, 1 + '1.0.1'.length, '1.0.1'.codeUnits);
    raw.setRange(33, 33 + '1.0.0'.length, '1.0.0'.codeUnits);
    raw.setRange(97, 97 + '1.0.0'.length, '1.0.0'.codeUnits);
    final info = CompanionDevInfo.fromBytes(raw);
    expect(info.slotName, 'main');
    expect(info.swVersion, '1.0.1');
    expect(info.hwVersion, '1.0.0');
    expect(info.bootVersion, '1.0.0');
  });

  test('OTA cache file is named by sha256', () {
    final m = OtaManifest.fromJson({
      'version': '1.0.0',
      'hardware': '1.0.0',
      'url': 'https://ota.jinsc.top/ota/helm-one/firmware.bin',
      'sha256':
          '7cb5b8fa5c42f0d83f44c976ee30c3a3e413106ffc9dfa890402ee146a92edae',
      'size': 1765580,
    });
    expect(
      OtaService.cacheFileName(m),
      '7cb5b8fa5c42f0d83f44c976ee30c3a3e413106ffc9dfa890402ee146a92edae.bin',
    );
  });

  test('companion adv names match Helm One and leftover MyBike', () {
    expect(CompanionProto.matchesAdvName('Helm One-A3F2'), isTrue);
    expect(CompanionProto.matchesAdvName('MyBike-3412'), isTrue);
    expect(CompanionProto.matchesAdvName('Headphones'), isFalse);
    expect(CompanionProto.advNamePrefix, 'Helm One-');
  });
}

import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:sifli_companion/gnss/mga_ubx.dart';

/// 两个辅助帧（时间 / 位置）的字节布局。
///
/// 字段偏移与刻度照 u-blox UBX-21035062 的 `UBX-MGA-INI-TIME_UTC` /
/// `UBX-MGA-INI-POS_LLH` 表 —— 这两帧是直接喂给模组的，偏移错了它照样
/// 回 accepted（ACK 不校验数值），所以必须按字节锁住。
void main() {
  /// 小端 4 字节读成整数（测试值都为正，够用）。
  int le32(List<int> b, int o) =>
      b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24);

  test('MGA-INI-TIME_UTC：字段落在规范偏移上', () {
    final f = MgaUbx.iniTimeUtc(DateTime.utc(2026, 9, 17, 5, 9, 0));
    expect(f[2], 0x13);
    expect(f[3], 0x40);
    expect(f[4] | (f[5] << 8), 24); // 24 字节载荷

    final pl = f.sublist(6);
    expect(pl[0], 0x10); // type
    expect(pl[1], 0x00); // version
    expect(pl[2], 0x00); // ref.source = 0（收到即生效）
    expect(pl[3], 18); // leapSecs
    expect(pl[4] | (pl[5] << 8), 2026); // year（U2 小端）
    expect(pl[6], 9);
    expect(pl[7], 17);
    expect(pl[8], 5);
    expect(pl[9], 9);
    expect(pl[10], 0);
    expect(pl[16] | (pl[17] << 8), 10); // tAccS（U2 小端，如实给 10 s）
  });

  test('MGA-INI-POS_LLH：字段落在规范偏移上', () {
    final f = MgaUbx.iniPosLlh(lat: 32.0, lon: 118.0, altM: 30, posAccM: 100);
    expect(f[2], 0x13);
    expect(f[3], 0x40);
    expect(f[4] | (f[5] << 8), 20); // 20 字节载荷

    final pl = f.sublist(6);
    expect(pl[0], 0x01); // type
    expect(pl[1], 0x00); // version
    expect(le32(pl, 4), 320000000); // lat ×1e7
    expect(le32(pl, 8), 1180000000); // lon ×1e7
    expect(le32(pl, 12), 3000); // alt（cm）
    expect(le32(pl, 16), 10000); // posAcc（cm）= 100 m
  });

  test('位置精度不许报得比实际准：0 会被夹到 1 m', () {
    // 声称「完全精确」会让模组挑过于乐观的策略，u-blox 明确说会导致定位失败。
    final pl = MgaUbx.iniPosLlh(lat: 0, lon: 0, posAccM: 0).sublist(6);
    expect(le32(pl, 16), 100);
  });

  test('两帧辅助 + 星历拼起来仍是可注入的合法文件', () {
    final file = <int>[
      ...MgaUbx.iniTimeUtc(DateTime.utc(2026, 9, 17, 5, 9, 0)),
      ...MgaUbx.iniPosLlh(lat: 32, lon: 118, posAccM: 100),
      ...MgaUbx.buildFrame(0x13, 0x00, List<int>.filled(68, 0)),
    ];
    final parsed = MgaUbx.parse(Uint8List.fromList(file));
    expect(parsed.ok, isTrue);
    expect(parsed.frames, 3);
  });
}

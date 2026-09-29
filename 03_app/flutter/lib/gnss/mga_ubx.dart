import 'dart:typed_data';

/// 解析 / 校验码表用的 UBX-MGA 星历文件。
///
/// 固件注入要求每帧 payload ≤ 256，且只认完整 `B5 62` 流。手机侧上传前用
/// [parse] 拦住截断、坏校验和非 MGA 文件。
abstract class MgaUbx {
  /// 与固件 `GNSS_EPH_INJECT_MAX` 一致。
  static const maxPayload = 256;

  /// 单文件上限，略大于模组 DBD dump（32 KiB）。
  static const maxFileBytes = 64 * 1024;

  /// 文件名里 Unix 秒的下限（2024-01-01 UTC）。
  static const minUnix = 1704067200;

  /// 组一帧 UBX（含校验）。
  static Uint8List buildFrame(int cls, int id, [List<int> payload = const []]) {
    final plen = payload.length;
    final out = Uint8List(8 + plen);
    out[0] = 0xb5;
    out[1] = 0x62;
    out[2] = cls & 0xff;
    out[3] = id & 0xff;
    out[4] = plen & 0xff;
    out[5] = (plen >> 8) & 0xff;
    if (plen > 0) {
      out.setRange(6, 6 + plen, payload);
    }
    var a = 0;
    var b = 0;
    for (var i = 2; i < 6 + plen; i++) {
      a = (a + out[i]) & 0xff;
      b = (b + a) & 0xff;
    }
    out[6 + plen] = a;
    out[7 + plen] = b;
    return out;
  }

  /// `MGA-INI-TIME_UTC`（0x13 0x40，type 0x10，24 B 载荷）。
  ///
  /// 字段偏移照 UBX-21035062 的 UBX-MGA-INI-TIME_UTC 表：type@0、ref@2
  /// （source=0 = 「收到即生效」，没有 EXTINT 时只能填这个）、leapSecs@3、
  /// 年月日时分秒 @4..10、ns@12、tAccS@16（U2）。
  ///
  /// [tAccS] 必须**如实**填：u-blox 明确写了「报得比实际准会明显劣化甚至起不来」。
  /// 手机时间本身准，但「收到即生效」不含链路延迟，所以默认给 10 s
  /// （AssistNow 服务自己也是 10 s）。
  static Uint8List iniTimeUtc(
    DateTime utc, {
    int tAccS = 10,
    int leapSecs = 18,
  }) {
    final t = utc.toUtc();
    final p = ByteData(24);
    p.setUint8(0, 0x10);
    p.setUint8(2, 0x00); // ref.source = 0
    p.setInt8(3, leapSecs);
    p.setUint16(4, t.year, Endian.little);
    p.setUint8(6, t.month);
    p.setUint8(7, t.day);
    p.setUint8(8, t.hour);
    p.setUint8(9, t.minute);
    p.setUint8(10, t.second);
    p.setUint16(16, tAccS, Endian.little);
    return buildFrame(0x13, 0x40, p.buffer.asUint8List());
  }

  /// `MGA-INI-POS_LLH`（0x13 0x40，type 0x01，20 B 载荷）。
  ///
  /// 字段偏移照 UBX-MGA-INI-POS_LLH 表：lat@4（1e-7 deg）、lon@8（1e-7 deg）、
  /// alt@12（cm）、posAcc@16（cm）。
  ///
  /// [posAccM] 必须**如实**：模组按它挑启动策略，声称的精度好于实际会「明显劣化
  /// 甚至起不来」；≤100 km 才会走乐观策略。这里顺手夹到 1 m~1000 km，
  /// 免得传进来 0（=「完全精确」，最危险）或负数。
  static Uint8List iniPosLlh({
    required double lat,
    required double lon,
    double altM = 0,
    required double posAccM,
  }) {
    final acc = posAccM.isFinite ? posAccM.clamp(1.0, 1000000.0) : 1000000.0;
    final p = ByteData(20);
    p.setUint8(0, 0x01);
    p.setInt32(4, (lat * 1e7).round(), Endian.little);
    p.setInt32(8, (lon * 1e7).round(), Endian.little);
    p.setInt32(12, (altM * 100).round(), Endian.little);
    p.setUint32(16, (acc * 100).round(), Endian.little);
    return buildFrame(0x13, 0x40, p.buffer.asUint8List());
  }

  /// 拆成帧。不抛；结果看 [MgaUbxFile.ok]。
  static MgaUbxFile parse(Uint8List data) {
    final types = <String, int>{};
    var i = 0;
    var frames = 0;
    var checksumFail = 0;
    var tooBig = 0;
    var mga = 0;
    var dbd = 0;

    while (i + 8 <= data.length) {
      if (data[i] != 0xb5 || data[i + 1] != 0x62) {
        break;
      }
      final cls = data[i + 2];
      final id = data[i + 3];
      final plen = data[i + 4] | (data[i + 5] << 8);
      if (i + 8 + plen > data.length) {
        break;
      }
      if (plen > maxPayload) {
        tooBig++;
      }
      var a = 0;
      var b = 0;
      for (var k = 2; k < 6 + plen; k++) {
        a = (a + data[i + k]) & 0xff;
        b = (b + a) & 0xff;
      }
      final ok = a == data[i + 6 + plen] && b == data[i + 7 + plen];
      if (!ok) {
        checksumFail++;
      }
      frames++;
      if (cls == 0x13) {
        mga++;
        if (id == 0x80) dbd++;
      }
      final label = typeName(cls, id);
      types[label] = (types[label] ?? 0) + 1;
      i += 8 + plen;
    }

    return MgaUbxFile(
      bytes: data.length,
      frames: frames,
      checksumFail: checksumFail,
      leftover: data.length - i,
      tooBig: tooBig,
      mgaFrames: mga,
      dbdFrames: dbd,
      types: types,
    );
  }

  /// `mga_<unix>.ubx`；[utcSec] 缺省为现在的 UTC 秒。
  static String fileName({int? utcSec}) {
    final utc = utcSec ?? DateTime.now().toUtc().millisecondsSinceEpoch ~/ 1000;
    return 'mga_$utc.ubx';
  }

  /// 从 `mga_<unix>.ubx` 或 `mga_YYYYMMDDTHHMMSSZ.ubx` 取出 UTC 秒；认不出则 `null`。
  static int? parseFileNameUtc(String name) {
    final base = name.split('/').last;
    final unix = RegExp(r'^mga_(\d+)\.ubx$', caseSensitive: false)
        .firstMatch(base);
    if (unix != null) {
      final v = int.tryParse(unix.group(1)!);
      if (v != null && v >= minUnix && v <= 0xffffffff) return v;
    }
    final iso = RegExp(
      r'^mga_(\d{4})(\d{2})(\d{2})T(\d{2})(\d{2})(\d{2})Z\.ubx$',
      caseSensitive: false,
    ).firstMatch(base);
    if (iso != null) {
      final t = DateTime.utc(
        int.parse(iso.group(1)!),
        int.parse(iso.group(2)!),
        int.parse(iso.group(3)!),
        int.parse(iso.group(4)!),
        int.parse(iso.group(5)!),
        int.parse(iso.group(6)!),
      );
      final sec = t.millisecondsSinceEpoch ~/ 1000;
      if (sec >= minUnix) return sec;
    }
    return null;
  }

  /// 人类可读的 UBX 类/ID。
  static String typeName(int cls, int id) {
    if (cls == 0x13) {
      return switch (id) {
        0x00 => 'MGA-GPS',
        0x02 => 'MGA-GAL',
        0x03 => 'MGA-BDS',
        0x05 => 'MGA-QZSS',
        0x06 => 'MGA-GLO',
        0x20 => 'MGA-ANO',
        0x40 => 'MGA-INI',
        0x60 => 'MGA-ACK',
        0x80 => 'MGA-DBD',
        _ => 'MGA-0x${id.toRadixString(16).padLeft(2, '0')}',
      };
    }
    if (cls == 0x05) {
      return id == 0x01 ? 'ACK-ACK' : 'ACK-NAK';
    }
    return 'UBX-0x${cls.toRadixString(16).padLeft(2, '0')}'
        '-0x${id.toRadixString(16).padLeft(2, '0')}';
  }
}

/// [MgaUbx.parse] 的结果。
class MgaUbxFile {
  /// 创建解析结果。
  const MgaUbxFile({
    required this.bytes,
    required this.frames,
    required this.checksumFail,
    required this.leftover,
    required this.tooBig,
    required this.mgaFrames,
    required this.dbdFrames,
    required this.types,
  });

  /// 原始字节数。
  final int bytes;

  /// 完整帧数。
  final int frames;

  /// 校验失败帧数。
  final int checksumFail;

  /// 未能组成完整 UBX 的尾部字节。
  final int leftover;

  /// payload 超过 [MgaUbx.maxPayload] 的帧数。
  final int tooBig;

  /// class=0x13 的帧数。
  final int mgaFrames;

  /// MGA-DBD（模组 dump）帧数。
  final int dbdFrames;

  /// 类型 → 计数。
  final Map<String, int> types;

  /// 可以交给码表注入。
  bool get ok =>
      frames > 0 &&
      checksumFail == 0 &&
      leftover == 0 &&
      tooBig == 0 &&
      mgaFrames > 0 &&
      bytes <= MgaUbx.maxFileBytes;

  /// 整份都是模组导航库 dump，不是 AssistNow 星历。
  bool get isDbdDump => dbdFrames > 0 && dbdFrames == frames;

  /// `MGA-DBD ×77 · MGA-GPS ×32` 这种摘要。
  String get typeSummary {
    if (types.isEmpty) return '';
    return types.entries.map((e) => '${e.key} ×${e.value}').join(' · ');
  }
}

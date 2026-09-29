import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter_blue_plus/flutter_blue_plus.dart';

/// BLE Companion 协议常量与编解码（服务 `0xFF10`）。
///
/// 本文件是固件
/// `vendor/my_vendor/boards/sf32lb52/my_vendor/include/companion_proto.h`
/// 的镜像，**两侧必须同步修改**。协议规格见固件仓库
/// `vendor/my_vendor/docs/ble/companion_impl_plan.md`。
///
/// 拓扑为「一功能一特征」，不做多路复用信道。所有多字节字段一律 **小端**。
abstract class CompanionProto {
  /// 主服务。
  static final service = Guid('FF10');

  /// 协议版本，`uint16` 只读。
  static final protoVersionChar = Guid('FF11');

  /// 设备状态，只读 + Notify，16 字节状态 + 6 字节 Classic 地址（协议 0x0005+）。
  static final deviceStatusChar = Guid('FF12');

  /// 控制命令，写入 `[opcode:1][param…]`。
  static final controlChar = Guid('FF13');

  /// 手机 GNSS 下行，写 / 无响应写，24 字节。
  static final gnssFixChar = Guid('FF14');

  /// 文件系统命令（路径寻址，固件 P1 已实现）。
  static final fsCommandChar = Guid('FF15');

  /// 文件系统数据（写 + Notify）。文件夹传输走这条 GATT 通道。
  static final fsDataChar = Guid('FF16');

  /// 通知推送（分片 TLV，App→设备）。
  static final notificationChar = Guid('FF17');

  /// 轨迹点上行（P2，尚未实现）。
  static final trackPointChar = Guid('FF18');

  /// 导航轨迹点（分片 lat/lon，App→设备）。
  static final navRouteChar = Guid('FF19');

  /// 设备信息：运行槽 / 软件 / 硬件 / 2SFBL 版本，只读。旧固件可能没有。
  static final deviceInfoChar = Guid('FF1A');

  /// App 期望的协议版本。设备返回值不等于此值时应提示升级而非继续通信。
  static const expectedVersion = 0x0008;

  /// 广播名前缀。**扫描过滤请用 [service] UUID**，名字可被用户改。
  static const advNamePrefix = 'Helm One-';

  /// 扫描页名称关键字（子串匹配）。含旧名以便未升级的码表仍能扫到。
  static const scanKeywords = ['Helm One', 'MyBike'];

  /// 广播名是否像本产品码表。
  static bool matchesAdvName(String name) {
    final n = name.toLowerCase();
    return n.contains('helm one') ||
        n.contains('helmone') ||
        n.contains('mybike') ||
        n.contains('vela');
  }

  /// 期望协商到的 ATT MTU；单包有效载荷为 `mtu - 3`。
  static const desiredMtu = 247;
}

/// [CompanionStatus.flags] 位域。
abstract class CompanionStatusFlag {
  /// 设备正在录制（码表已开始一次骑行，含暂停）。
  static const recording = 1 << 0;

  /// 正在使用手机下行的 GNSS。
  static const gpsPhone = 1 << 1;

  /// 正在使用内置 GNSS。
  static const gpsInternal = 1 << 2;

  /// 存储已满。
  static const storageFull = 1 << 3;

  /// BLE 文件传输进行中。
  static const fsBusy = 1 << 4;

  /// USB MTP 占用存储，BLE 文件操作会返回 BUSY。
  static const usbMtpBusy = 1 << 5;

  /// 码表判定正在骑行（未暂停，速度/踏频过阈值）。
  static const moving = 1 << 6;

  /// 心率传感器已连接。
  static const sensorHr = 1 << 8;

  /// 踏频 / CSC 传感器已连接。
  static const sensorCsc = 1 << 9;

  /// 功率计 / CPS 传感器已连接。
  static const sensorCps = 1 << 10;
}

/// 控制特征 `0xFF13` 的操作码。
abstract class CompanionCtrl {
  /// 保留。
  static const nop = 0x00;

  /// 开始录制。
  static const startRecord = 0x01;

  /// 停止录制。
  static const stopRecord = 0x02;

  /// 请求设备立即回一帧状态 Notify。
  static const ping = 0x03;

  /// 同步码表 RTC：`[unix_sec:u32le][tz_min:i16le]`（UTC 秒 + 本地相对 UTC 的分钟）。
  static const timeSync = 0x04;

  /// 中止当前文件传输（P1）。
  static const fsAbort = 0x10;

  /// 进入同步态，降低其他 Notify 频率（P1）。
  static const syncPrep = 0x11;

  /// 停止手机下发的导航（不停止录制）。
  static const navStop = 0x22;

  /// 请码表**开配对窗口**（秒，缺省 30）：窗口内它接受新手机的配对。
  /// 载荷 = 可选 `[secs:u16le]`；不带宽窗口用设备默认 30 s。
  static const pairOpen = 0x30;

  /// **解除本机与码表的绑定**（码表侧清密钥 + 清"已绑定手机"记录）。
  /// 之后要重新配对：先在码表上开配对窗口（或重启码表 60 s 内），再连。
  static const pairUnbind = 0x31;
}

/// 通知特征 `0xFF17`：8 字节分片头 + TLV。
abstract class CompanionNotif {
  static const hdrLen = 8;
  static const hdrVer = 0x01;

  static const flagFirst = 1 << 0;
  static const flagLast = 1 << 1;

  static const typeGeneric = 0;
  static const typeCall = 1;
  static const typeSms = 2;
  static const typeApp = 3;
  static const typeCalendar = 4;

  static const tagAppName = 0x01;
  static const tagTitle = 0x02;
  static const tagBody = 0x03;
  static const tagTimestamp = 0x04;
  static const tagPackage = 0x05;
  static const tagKey = 0x06;
  static const tagIcon = 0x07; // 文件名，不含路径

  static const maxTotal = 1024;
  static const iconDir = 'notif_icons';
  /// 图标文件名最大字符数（不含 NUL），与固件 `COMPANION_NOTIF_ICON_NAME_MAX` 一致。
  static const iconNameMax = 80;
  static const titleMax = 48;
  static const bodyMax = 96;
  /// 包名最大字符数（不含 NUL），与固件 `COMPANION_NOTIF_PACKAGE_MAX` 一致。
  static const packageMax = 64;

  /// 与固件 `sanitize_pkg` 一致；超长截断，防止异常包名撑爆路径。
  static String sanitizePackage(String pkg, [int maxChars = packageMax]) {
    final out = StringBuffer();
    for (final r in pkg.runes) {
      if (out.length >= maxChars) break;
      final c = String.fromCharCode(r);
      if (RegExp(r'[A-Za-z0-9._-]').hasMatch(c)) {
        out.write(c);
      } else {
        out.write('_');
      }
    }
    return out.toString();
  }

  /// 线上文件名：包名清洗后加 `.png`，总长不超过 [iconNameMax]。
  static String iconFileName(String packageName) {
    const ext = '.png';
    final maxStem = iconNameMax - ext.length;
    final cap = maxStem < packageMax ? maxStem : packageMax;
    final stem = sanitizePackage(packageName, cap);
    if (stem.isEmpty) return '';
    return '$stem$ext';
  }

  /// 去掉路径、清洗非法字符。`.` / `..` / 空串视为无效。
  static String wireName(String raw) {
    var s = raw.trim().replaceAll('\\', '/');
    if (s.isEmpty) return '';
    final slash = s.lastIndexOf('/');
    if (slash >= 0) s = s.substring(slash + 1);
    s = sanitizePackage(s, iconNameMax);
    if (s.isEmpty || s == '.' || s == '..') return '';
    return s;
  }

  /// 设备侧 FS 相对路径（App 上传时应答 `FS_NEED` 时使用，不进 0xFF17）。
  static String iconFsPath(String fileName) {
    final name = wireName(fileName);
    if (name.isEmpty) return '';
    return '$iconDir/$name';
  }

  /// 旧接口：相对路径。新代码请用 [iconFileName] / [iconFsPath]。
  static String iconRelPath(String packageName) =>
      iconFsPath(iconFileName(packageName));
}

/// 导航轨迹 `0xFF19`：与通知相同的 8 字节分片头，payload 为 lat_e7/lon_e7。
abstract class CompanionNav {
  static const flagStart = 1 << 2;
  static const ptLen = 8;
  static const maxPts = 32;
  static const maxTotal = maxPts * ptLen;
}

/// 文件系统命令 `0xFF15` / 数据帧 `0xFF16`。
///
/// Command 写入：`[cmd:1][tid:1][payload…]`
/// Data 帧头 8 字节：`[cmd:1][tid:1][flags:1][status:1][seq:2 le][len:2 le]`
abstract class CompanionFs {
  static const cmdHdrLen = 2;
  static const dataHdrLen = 8;

  static const statfs = 0x01;
  static const list = 0x02;
  static const stat = 0x03;
  static const read = 0x04;
  static const writeOpen = 0x07;
  static const writeData = 0x08;
  static const writeClose = 0x09;
  static const delete = 0x0A;
  static const mkdir = 0x0B;
  static const rename = 0x0C;

  /// 设备→App：请上传该文件名到设备 `notif_icons/`。
  static const need = 0x0D;

  static const writeFlagResume = 1 << 0;
  static const writeFlagRle = 1 << 1;
  static const readFlagRle = 1 << 0;
  static const writeOpenReplyLen = 8;

  /// NuttX errno → 短文案。
  static String errnoText(int status) {
    return switch (status) {
      2 => '不存在',
      5 => '存储写入失败',
      13 => '拒绝访问',
      16 => '忙',
      17 => '已存在',
      22 => '参数无效',
      28 => '空间不足',
      38 => '未实现',
      39 => '目录非空',
      95 => '不支持',
      _ => 'errno=$status',
    };
  }

  static const flagLast = 1 << 0;
  static const flagErr = 1 << 1;
  static const flagAckReq = 1 << 2;

  static const entryFile = 0;
  static const entryDir = 1;

  /// NuttX `crc32part` 初值 0，多项式 0xedb88320（无最终异或）。
  static const crc32Init = 0;

  /// 与固件 `crc32part(buf, len, crc)` 一致，用于 WRITE_CLOSE / 续传前缀。
  static int crc32Part(List<int> data, [int crc = crc32Init]) {
    var value = crc;
    for (final b in data) {
      value ^= b & 0xff;
      for (var i = 0; i < 8; i++) {
        if ((value & 1) != 0) {
          value = (value >>> 1) ^ 0xedb88320;
        } else {
          value = value >>> 1;
        }
      }
    }
    return value >>> 0;
  }

  /// WRITE_OPEN 续传回复的 `*.part` 是否就是 [data] 的前缀。
  ///
  /// 断电后最后几页可能已经计入 size 但内容不是原文。App 若仍从 offset
  /// 接着写，CLOSE 会因 CRC 对不上返回 errno 5。
  static bool resumePrefixMatches(List<int> data, int offset, int partCrc) {
    if (offset <= 0) return true;
    if (offset > data.length) return false;
    return crc32Part(data.sublist(0, offset)) == (partCrc >>> 0);
  }

  /// 设备存储根。App **不要**把这段写进 GATT 路径。
  static const storageRoot = '/mnt/lfs';

  /// BLE OTA 专属目录，落到设备 `/mnt/kv/fw`（与 LFS 文件管理器沙箱分开）。
  static const otaDir = 'fw';

  /// GNSS 辅助星历目录，落到设备 `/mnt/lfs/eph`。
  static const ephDir = 'eph';

  /// 崩溃日志目录，落到设备 `/mnt/kv/coredump`。
  static const coredumpDir = 'coredump';

  /// 诊断日志目录，落到设备 `/mnt/kv/diag`。
  ///
  /// 设备侧由 `myvendor_diaglog` 写入，文件名 `dNNN_YYYYMMDD_HHMMSS.txt`，
  /// 与 [coredumpDir] 同构（只换前缀）。设备侧 `xfer_resolve()` 里
  /// `diag` 与 `coredump` 是两个独立的路径命名空间，不能合并。
  static const diagDir = 'diag';

  /// 常用点 TSV，落到设备 `/mnt/kv/bicycle_favorites.tsv`。
  static const favoritesPath = 'favorites';

  /// 坐标点记录目录，落到设备 `/mnt/lfs/mtp/navpts`。
  static const navptsDir = 'mtp/navpts';

  /// OTA 文件在线上的相对路径，例如 `fw/1.0.0-Helm-One.bin`。
  static String otaPath(String fileName) => joinRel(otaDir, fileName);

  /// 星历文件在线上的相对路径，例如 `eph/mga_1756557480.ubx`。
  static String ephPath(String fileName) => joinRel(ephDir, fileName);

  /// 转成线上相对路径：根目录为空，去掉误传的 `/mnt/lfs` 前缀。
  static String wirePath(String path) {
    var value = path.trim();
    if (value.startsWith(storageRoot)) {
      value = value.substring(storageRoot.length);
    }
    while (value.startsWith('/')) {
      value = value.substring(1);
    }
    if (value == '.') {
      return '';
    }
    return value;
  }

  /// 相对路径拼接。
  static String joinRel(String dir, String name) {
    final base = wirePath(dir);
    if (base.isEmpty) return name;
    return '$base/$name';
  }

  /// 上一级相对路径。
  static String parentRel(String dir) {
    final base = wirePath(dir);
    if (base.isEmpty) return '';
    final i = base.lastIndexOf('/');
    if (i <= 0) return '';
    return base.substring(0, i);
  }

  /// 界面展示用（根显示为 `/`）。
  static String displayPath(String dir) {
    final base = wirePath(dir);
    return base.isEmpty ? '/' : '/$base';
  }

  /// Classic SPP 设备主动 hello。
  static const sppHello = 0x00;

  /// SPP 一帧 payload 上限（与固件 `COMPANION_SPP_PAYLOAD_MAX` 一致）。
  static const sppPayloadMax = 4096;

  /// SPP READ：`[offset:4][want:2][path…]`。
  static const sppReadHdrLen = 6;

  /// Serial Port UUID。
  static const sppUuid = '00001101-0000-1000-8000-00805F9B34FB';

  /// `bt_address_t.addr[]` 是 LSB 在前；显示/连接用 MSB 在前的冒号串。
  static String formatBdAddr(List<int> raw) {
    if (raw.length < 6) return '';
    return List.generate(
      6,
      (i) => raw[5 - i].toRadixString(16).padLeft(2, '0').toUpperCase(),
    ).join(':');
  }
}

/// 一帧 `0xFF16` FS Data。
class CompanionFsFrame {
  /// 解析 8 字节头 + payload。
  CompanionFsFrame({
    required this.cmd,
    required this.tid,
    required this.flags,
    required this.status,
    required this.seq,
    required this.payload,
  });

  final int cmd;
  final int tid;
  final int flags;
  final int status;
  final int seq;
  final Uint8List payload;

  bool get isLast => flags & CompanionFs.flagLast != 0;
  bool get isErr => flags & CompanionFs.flagErr != 0;

  factory CompanionFsFrame.fromBytes(List<int> data) {
    if (data.length < CompanionFs.dataHdrLen) {
      throw FormatException('FS 帧短于 8 字节: ${data.length}');
    }
    final bd = ByteData.sublistView(Uint8List.fromList(data));
    final len = bd.getUint16(6, Endian.little);
    final end = CompanionFs.dataHdrLen + len;
    if (data.length < end) {
      throw FormatException('FS 帧 payload 截断: 声明 $len，实际 ${data.length - 8}');
    }
    return CompanionFsFrame(
      cmd: bd.getUint8(0),
      tid: bd.getUint8(1),
      flags: bd.getUint8(2),
      status: bd.getUint8(3),
      seq: bd.getUint16(4, Endian.little),
      payload: Uint8List.fromList(data.sublist(CompanionFs.dataHdrLen, end)),
    );
  }
}

/// LIST / STAT 条目。
class CompanionFsEntry {
  const CompanionFsEntry({
    required this.type,
    required this.size,
    required this.mtime,
    required this.name,
  });

  final int type;
  final int size;
  final int mtime;
  final String name;

  bool get isDir => type == CompanionFs.entryDir;

  static List<CompanionFsEntry> parseList(Uint8List payload) {
    final out = <CompanionFsEntry>[];
    var i = 0;
    final bd = ByteData.sublistView(payload);
    while (i + 10 <= payload.length) {
      final type = payload[i];
      final size = bd.getUint32(i + 1, Endian.little);
      final mtime = bd.getUint32(i + 5, Endian.little);
      final nlen = payload[i + 9];
      i += 10;
      if (i + nlen > payload.length) break;
      final name = utf8.decode(
        payload.sublist(i, i + nlen),
        allowMalformed: true,
      );
      i += nlen;
      out.add(CompanionFsEntry(type: type, size: size, mtime: mtime, name: name));
    }
    return out;
  }
}

/// FS 操作失败（status 为设备 errno，0 表示本端超时/序号错误）。
class CompanionFsException implements Exception {
  CompanionFsException(this.message, {this.status = 0, this.partial});

  final String message;
  final int status;
  final Uint8List? partial;

  @override
  String toString() {
    if (status == 0) return 'CompanionFsException($message)';
    return 'CompanionFsException($message, ${CompanionFs.errnoText(status)})';
  }
}

/// FS 通道当前不具备使用条件 —— 不是设备回了错误，是 App 根本没条件发起事务。
///
/// 两种情况：设备没暴露 `0xFF15`/`0xFF16`（多为 Android 缓存了旧 GATT 服务表），
/// 或 `0xFF16` 的 notify 没订上（收不到任何回复）。两者都会让文件传输整体不可用，
/// 且与设备无关，所以和 [CompanionFsException]（设备明确回了 errno）分开报，
/// 让调用方与 UI 能给出「清服务缓存 / 等补订」这类正确建议。
class CompanionFsUnavailableException implements Exception {
  CompanionFsUnavailableException(this.message, {this.hint = ''});

  final String message;

  /// 可操作的建议，为空则不追加。
  final String hint;

  @override
  String toString() => hint.isEmpty ? message : '$message（$hint）';
}

/// GNSS 定位质量。
abstract class CompanionFixQuality {
  /// 无效定位。
  static const none = 0;

  /// 二维定位。
  static const fix2d = 1;

  /// 三维定位。
  static const fix3d = 2;
}

/// 设备状态帧（特征 `0xFF12`，16 字节状态 + 6 字节 Classic 地址）。
///
/// [lastCtrlOp] 与 [gnssRxCount] 是设备侧的回显字段：前者证明控制写入已被处理，
/// 后者证明 GNSS 下行已被接收。P0 靠这两个字段完成端到端验证。
class CompanionStatus {
  /// 直接按字段构造，一般只在测试中使用。
  const CompanionStatus({
    required this.flags,
    required this.batteryPct,
    required this.lastCtrlOp,
    required this.storageFreeKb,
    required this.uptimeSec,
    required this.gnssRxCount,
    this.classicAddrRaw = const [],
  });

  /// 状态字段字节长度（不含 Classic 地址拖车）。
  static const length = 16;

  /// 0x0005 起的完整线格式。
  static const wireLength = 22;

  /// 电量未知时 [batteryPct] 的取值。
  static const batteryUnknown = 0xFF;

  /// 状态位域，取值见 [CompanionStatusFlag]。
  final int flags;

  /// 电量百分比，[batteryUnknown] 表示未知。
  final int batteryPct;

  /// 设备最近处理的控制操作码回显。
  final int lastCtrlOp;

  /// 传输存储剩余空间，KiB。
  final int storageFreeKb;

  /// 设备运行时长，秒。App 可据此发现设备重启。
  final int uptimeSec;

  /// 设备累计收到的 GNSS 下行帧数。
  final int gnssRxCount;

  /// Classic BD_ADDR 原始 6 字节（LSB 在前）。空表示旧固件未带拖车。
  final List<int> classicAddrRaw;

  /// 给 `BluetoothSocket` 用的 `AA:BB:CC:DD:EE:FF`。空串表示未知。
  String get classicAddress => CompanionFs.formatBdAddr(classicAddrRaw);

  /// 解析 16 字节状态帧（22 字节时附带 Classic 地址）；长度不足时抛 [FormatException]。
  factory CompanionStatus.fromBytes(List<int> data) {
    if (data.length < length) {
      throw FormatException(
        'Companion 状态帧长度应为 $length，实际 ${data.length}',
      );
    }

    final bd = ByteData.sublistView(Uint8List.fromList(data));
    return CompanionStatus(
      flags: bd.getUint16(0, Endian.little),
      batteryPct: bd.getUint8(2),
      lastCtrlOp: bd.getUint8(3),
      storageFreeKb: bd.getUint32(4, Endian.little),
      uptimeSec: bd.getUint32(8, Endian.little),
      gnssRxCount: bd.getUint32(12, Endian.little),
      classicAddrRaw: data.length >= wireLength
          ? List<int>.from(data.sublist(length, wireLength))
          : const [],
    );
  }

  /// 是否正在录制（码表骑行会话，含暂停）。
  bool get isRecording => flags & CompanionStatusFlag.recording != 0;

  /// 码表判定是否正在骑行（未暂停）。
  bool get isMoving => flags & CompanionStatusFlag.moving != 0;

  /// 设备是否正在采用手机下行的定位。
  bool get isUsingPhoneGnss => flags & CompanionStatusFlag.gpsPhone != 0;

  /// 是否有文件传输进行中（BLE 或 USB）。
  bool get isTransferBusy =>
      flags &
          (CompanionStatusFlag.fsBusy | CompanionStatusFlag.usbMtpBusy) !=
      0;

  /// 电量是否已知。
  bool get hasBattery => batteryPct != batteryUnknown;

  @override
  String toString() =>
      'CompanionStatus(flags=0x${flags.toRadixString(16).padLeft(4, '0')}, '
      'batt=$batteryPct, lastCtrlOp=0x${lastCtrlOp.toRadixString(16)}, '
      'freeKb=$storageFreeKb, uptime=${uptimeSec}s, gnssRx=$gnssRxCount'
      '${classicAddress.isEmpty ? '' : ', classic=$classicAddress'})';
}

/// 0xFF1A 运行槽。与固件 `COMPANION_SLOT_*` / 2SFBL `persist.boot.running` 一致。
abstract class CompanionSlot {
  static const unknown = 0;
  static const fw = 1;
  static const main = 2;
  static const factory = 3;
}

/// 设备信息帧（特征 `0xFF1A`）。V1 为 97 字节；V2 在末尾追加 16 字节 boot 版本。
class CompanionDevInfo {
  const CompanionDevInfo({
    required this.slot,
    required this.swVersion,
    required this.hwVersion,
    this.fwName = '',
    this.bootVersion = '',
  });

  static const swLen = 32;
  static const hwLen = 16;
  static const nameLen = 48;
  static const bootLen = 16;
  static const lengthV1 = 1 + swLen + hwLen + nameLen;
  static const length = lengthV1 + bootLen;

  final int slot;
  final String swVersion;
  final String hwVersion;
  final String fwName;
  final String bootVersion;

  /// 界面展示用：去掉 `.bin` 后缀，协议里仍是完整文件名。
  static String displayFwName(String name) {
    const suf = '.bin';
    if (name.length > suf.length &&
        name.toLowerCase().endsWith(suf)) {
      return name.substring(0, name.length - suf.length);
    }
    return name;
  }

  String get fwLabel => displayFwName(fwName);

  static String _cstr(List<int> data, int off, int len) {
    final end = off + len;
    var stop = end;
    for (var i = off; i < end; i++) {
      if (data[i] == 0) {
        stop = i;
        break;
      }
    }
    return utf8.decode(data.sublist(off, stop), allowMalformed: true).trim();
  }

  factory CompanionDevInfo.fromBytes(List<int> data) {
    if (data.length < lengthV1) {
      throw FormatException(
        'Companion 设备信息长度至少为 $lengthV1，实际 ${data.length}',
      );
    }
    return CompanionDevInfo(
      slot: data[0],
      swVersion: _cstr(data, 1, swLen),
      hwVersion: _cstr(data, 1 + swLen, hwLen),
      fwName: _cstr(data, 1 + swLen + hwLen, nameLen),
      bootVersion: data.length >= length
          ? _cstr(data, lengthV1, bootLen)
          : '',
    );
  }

  String get slotName => switch (slot) {
        CompanionSlot.fw => 'fw',
        CompanionSlot.main => 'main',
        CompanionSlot.factory => 'factory',
        _ => 'unknown',
      };

  @override
  String toString() =>
      'CompanionDevInfo(slot=$slotName, sw=$swVersion, hw=$hwVersion'
      '${fwName.isEmpty ? '' : ', file=$fwName'}'
      '${bootVersion.isEmpty ? '' : ', boot=$bootVersion'})';
}

import 'package:sifli_companion/ble/companion_proto.dart';
import 'package:sifli_companion/ota/ota_config.dart';

/// `firmware.json` 一份发布说明。
class OtaManifest {
  const OtaManifest({
    required this.version,
    required this.hardware,
    required this.url,
    required this.sha256,
    required this.size,
    this.note = '',
    this.date = '',
  });

  final String version;
  final String hardware;
  final String url;
  final String sha256;
  final int size;
  final String note;

  /// 展示用日期，`YYYY-MM-DD`；清单未提供时可由 HTTP Last-Modified 补上。
  final String date;

  factory OtaManifest.fromJson(Map<String, dynamic> json) {
    final version = '${json['version'] ?? ''}'.trim();
    final url = '${json['url'] ?? ''}'.trim();
    final sha = '${json['sha256'] ?? json['sha'] ?? ''}'.trim().toLowerCase();
    if (version.isEmpty || url.isEmpty || sha.length != 64) {
      throw FormatException('firmware.json 字段不完整');
    }
    final sizeRaw = json['size'];
    final size = sizeRaw is int
        ? sizeRaw
        : int.tryParse('$sizeRaw') ?? 0;
    if (size <= 0) {
      throw FormatException('firmware.json size 无效');
    }
    return OtaManifest(
      version: version,
      hardware: '${json['hardware'] ?? OtaConfig.defaultHardware}'.trim(),
      url: url,
      sha256: sha,
      size: size,
      note: '${json['note'] ?? ''}'.trim(),
      date: parseDate(
        json['date'] ??
            json['released'] ??
            json['releasedAt'] ??
            json['time'],
      ),
    );
  }

  OtaManifest copyWith({String? date}) => OtaManifest(
        version: version,
        hardware: hardware,
        url: url,
        sha256: sha256,
        size: size,
        note: note,
        date: date ?? this.date,
      );

  static String formatDay(DateTime d) {
    final l = d.toLocal();
    final y = l.year.toString().padLeft(4, '0');
    final m = l.month.toString().padLeft(2, '0');
    final day = l.day.toString().padLeft(2, '0');
    return '$y-$m-$day';
  }

  /// 清单里的日期字段：`YYYY-MM-DD`、ISO 时间或 unix 秒/毫秒。
  static String parseDate(Object? raw) {
    if (raw == null) return '';
    if (raw is int) {
      final ms = raw > 9999999999 ? raw : raw * 1000;
      return formatDay(DateTime.fromMillisecondsSinceEpoch(ms, isUtc: true));
    }
    final s = '$raw'.trim();
    if (s.isEmpty) return '';
    if (RegExp(r'^\d{4}-\d{2}-\d{2}').hasMatch(s)) {
      return s.substring(0, 10);
    }
    final n = int.tryParse(s);
    if (n != null) return parseDate(n);
    try {
      return formatDay(DateTime.parse(s));
    } catch (_) {
      return '';
    }
  }

  /// 落到码表 `fw/` 下的文件名。必须以 `X.Y.Z` 开头，2SFBL 按文件名排版本。
  String get slotFileName => '$version-${OtaConfig.fileStem}.bin';

  /// BLE 相对路径。
  String get slotPath => CompanionFs.otaPath(slotFileName);

  Uri get downloadUri => OtaConfig.preferHttps(Uri.parse(url));
}

/// 从文件名或版本字符串解析开头的 `X.Y.Z`。
class OtaVersion {
  const OtaVersion(this.major, this.minor, this.patch);

  final int major;
  final int minor;
  final int patch;

  static final _re = RegExp(r'v?(\d+)\.(\d+)\.(\d+)');

  static OtaVersion? tryParse(String raw) {
    final m = _re.firstMatch(raw.trim());
    if (m == null) return null;
    return OtaVersion(
      int.parse(m.group(1)!),
      int.parse(m.group(2)!),
      int.parse(m.group(3)!),
    );
  }

  static OtaVersion parseOrZero(String raw) =>
      tryParse(raw) ?? const OtaVersion(0, 0, 0);

  int compareTo(OtaVersion other) {
    final a = major.compareTo(other.major);
    if (a != 0) return a;
    final b = minor.compareTo(other.minor);
    if (b != 0) return b;
    return patch.compareTo(other.patch);
  }

  bool operator >(OtaVersion o) => compareTo(o) > 0;
  bool operator <(OtaVersion o) => compareTo(o) < 0;
  bool operator >=(OtaVersion o) => compareTo(o) >= 0;

  @override
  String toString() => '$major.$minor.$patch';

  /// 在一组 `*.bin` 名里取版本最高的；没有合法版本则空。
  static String? highestLabel(Iterable<String> names) {
    String? best;
    OtaVersion? bestV;
    for (final name in names) {
      if (!name.toLowerCase().endsWith('.bin')) continue;
      final v = tryParse(name);
      if (v == null) continue;
      if (bestV == null || v > bestV) {
        bestV = v;
        best = name;
      }
    }
    return best;
  }
}

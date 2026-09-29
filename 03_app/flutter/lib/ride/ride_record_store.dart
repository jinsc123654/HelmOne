import 'dart:convert';
import 'dart:io';

import 'package:latlong2/latlong.dart';
import 'package:path/path.dart' as p;
import 'package:path_provider/path_provider.dart';
import 'package:sifli_companion/log/app_log.dart';
import 'package:sifli_companion/ride/gpx_util.dart';

/// 从码表 `mtp/record/` 拉回的骑行文件。
class RideRecord {
  RideRecord({
    required this.fileName,
    required this.localPath,
    required this.size,
    required this.mtime,
    this.distanceKm,
    this.pointCount,
    this.durationSec,
    this.preview,
    this.displayName,
    this.remoteSize,
    this.userRenamed = false,
  });

  final String fileName;
  final String localPath;
  final int size;
  final int mtime;
  final double? distanceKm;
  final int? pointCount;

  /// GPX 起止时间差（秒）。`null` 表示索引里还没有算过。
  final int? durationSec;

  /// 缩略轨迹。`null` 还没抽过；空列表表示没有可用点。
  final List<LatLng>? preview;

  final String? displayName;

  /// 码表上原文件大小，用来判断是否要重新拉。不用 BLE mtime。
  final int? remoteSize;

  /// 用户在手机上改过显示名；再同步时保留，不跟 GPX 里的 `<name>` 覆盖。
  final bool userRenamed;

  String get title {
    final n = displayName?.trim();
    if (n != null && n.isNotEmpty) return n;
    final stem = fileName.toLowerCase().endsWith('.gpx')
        ? fileName.substring(0, fileName.length - 4)
        : fileName;
    return stem;
  }

  Map<String, Object?> toJson() => {
        'fileName': fileName,
        'localPath': localPath,
        'size': size,
        'mtime': mtime,
        'distanceKm': distanceKm,
        'pointCount': pointCount,
        'durationSec': durationSec,
        'preview': preview == null
            ? null
            : [
                for (final p in preview!) [p.latitude, p.longitude],
              ],
        'displayName': displayName,
        'remoteSize': remoteSize,
        'userRenamed': userRenamed,
      };

  factory RideRecord.fromJson(Map<String, Object?> json) {
    return RideRecord(
      fileName: json['fileName'] as String? ?? '',
      localPath: json['localPath'] as String? ?? '',
      size: json['size'] as int? ?? 0,
      mtime: json['mtime'] as int? ?? 0,
      distanceKm: (json['distanceKm'] as num?)?.toDouble(),
      pointCount: json['pointCount'] as int?,
      durationSec: (json['durationSec'] as num?)?.toInt(),
      preview: _previewFromJson(json['preview']),
      displayName: json['displayName'] as String?,
      remoteSize: json['remoteSize'] as int?,
      userRenamed: json['userRenamed'] as bool? ?? false,
    );
  }

  RideRecord copyWith({
    int? size,
    int? mtime,
    double? distanceKm,
    int? pointCount,
    int? durationSec,
    List<LatLng>? preview,
    String? displayName,
    int? remoteSize,
    bool? userRenamed,
  }) {
    return RideRecord(
      fileName: fileName,
      localPath: localPath,
      size: size ?? this.size,
      mtime: mtime ?? this.mtime,
      distanceKm: distanceKm ?? this.distanceKm,
      pointCount: pointCount ?? this.pointCount,
      durationSec: durationSec ?? this.durationSec,
      preview: preview ?? this.preview,
      displayName: displayName ?? this.displayName,
      remoteSize: remoteSize ?? this.remoteSize,
      userRenamed: userRenamed ?? this.userRenamed,
    );
  }
}

/// 主页统计区间。默认本周。
enum RidePeriod { week, month, all }

/// 按记录时间过滤周 / 月 / 全部。
abstract class RidePeriodFilter {
  static DateTime weekStart(DateTime now) {
    final day = DateTime(now.year, now.month, now.day);
    return day.subtract(Duration(days: day.weekday - 1));
  }

  static DateTime monthStart(DateTime now) => DateTime(now.year, now.month, 1);

  static bool matches(RideRecord rec, RidePeriod period, DateTime now) {
    if (period == RidePeriod.all) return true;
    if (rec.mtime <= 0) return false;
    final t = DateTime.fromMillisecondsSinceEpoch(rec.mtime * 1000);
    final start =
        period == RidePeriod.week ? weekStart(now) : monthStart(now);
    return !t.isBefore(start);
  }

  static List<RideRecord> apply(List<RideRecord> items, RidePeriod period) {
    final now = DateTime.now();
    return [for (final r in items) if (matches(r, period, now)) r];
  }
}

/// 本机骑行记录库（documents/rides）。
class RideRecordStore {
  factory RideRecordStore() => _instance;
  static final RideRecordStore _instance = RideRecordStore._();
  RideRecordStore._();

  Directory? _dir;
  final List<RideRecord> _items = [];

  List<RideRecord> get items => List.unmodifiable(_items);

  Future<Directory> _ensureDir() async {
    if (_dir != null) return _dir!;
    final root = await getApplicationDocumentsDirectory();
    _dir = Directory(p.join(root.path, 'rides'));
    if (!await _dir!.exists()) {
      await _dir!.create(recursive: true);
    }
    return _dir!;
  }

  File get _indexFile => File(p.join(_dir!.path, 'index.json'));

  /// 从磁盘加载。
  Future<void> load() async {
    await _ensureDir();
    _items.clear();
    if (!await _indexFile.exists()) return;
    try {
      final raw = jsonDecode(await _indexFile.readAsString());
      if (raw is List) {
        for (final e in raw) {
          if (e is Map<String, dynamic>) {
            _items.add(RideRecord.fromJson(Map<String, Object?>.from(e)));
          }
        }
      }
    } catch (_) {
      _items.clear();
    }
    _items.sort((a, b) => b.mtime.compareTo(a.mtime));
  }

  Future<void> _flush() async {
    await _ensureDir();
    await _indexFile.writeAsString(
      jsonEncode(_items.map((e) => e.toJson()).toList()),
    );
  }

  RideRecord? find(String fileName) {
    for (final e in _items) {
      if (e.fileName == fileName) return e;
    }
    return null;
  }

  /// 写入或覆盖一条本机记录。时间取 GPX `<time>`，不采用码表 FS mtime。
  Future<RideRecord> putFile({
    required String fileName,
    required List<int> bytes,
    int? remoteSize,
    DateTime? stampUtc,
  }) async {
    await _ensureDir();
    final safe = GpxUtil.safeFileName(fileName);
    var xml = GpxUtil.ensureTimestamps(
      utf8.decode(bytes, allowMalformed: true),
      nowUtc: stampUtc,
    );
    final prev = find(safe);
    final keepName = prev != null &&
        prev.userRenamed &&
        (prev.displayName?.trim().isNotEmpty ?? false);
    if (keepName) {
      xml = GpxUtil.setName(xml, prev!.displayName!.trim());
    }
    final out = utf8.encode(xml);
    final file = File(p.join(_dir!.path, safe));
    await file.writeAsBytes(out, flush: true);
    final sum = GpxUtil.parse(xml);
    final when = sum.endTime ?? stampUtc ?? DateTime.now().toUtc();
    final rec = RideRecord(
      fileName: safe,
      localPath: file.path,
      size: out.length,
      mtime: GpxUtil.unixSeconds(when),
      distanceKm: sum.distanceKm,
      pointCount: sum.points.length,
      durationSec: sum.duration?.inSeconds ?? 0,
      preview: GpxUtil.previewTrack(sum.points),
      displayName: keepName
          ? prev!.displayName!.trim()
          : (sum.name.isEmpty ? null : sum.name),
      remoteSize: remoteSize,
      userRenamed: keepName,
    );
    _items.removeWhere((e) => e.fileName == safe);
    _items.insert(0, rec);
    _items.sort((a, b) => b.mtime.compareTo(a.mtime));
    await _flush();
    return rec;
  }

  Future<void> remove(String fileName) async {
    await removeMany({fileName});
  }

  /// 批量删除本机记录，不同步删码表文件。
  Future<void> removeMany(Set<String> fileNames) async {
    if (fileNames.isEmpty) return;
    await _ensureDir();
    for (final name in fileNames) {
      final rec = find(name);
      if (rec != null) {
        final f = File(rec.localPath);
        if (await f.exists()) await f.delete();
      }
    }
    _items.removeWhere((e) => fileNames.contains(e.fileName));
    await _flush();
  }

  /// 只改手机显示名。本地文件名仍是码表 basename，再同步不会变成两条。
  Future<RideRecord?> setDisplayName(String fileName, String name) async {
    final rec = find(fileName);
    if (rec == null) return null;
    final trimmed = name.trim();
    if (trimmed.isEmpty) return rec;
    await _ensureDir();
    var size = rec.size;
    final f = File(rec.localPath);
    if (await f.exists()) {
      final xml = utf8.decode(await f.readAsBytes(), allowMalformed: true);
      final out = utf8.encode(GpxUtil.setName(xml, trimmed));
      await f.writeAsBytes(out, flush: true);
      size = out.length;
    }
    final updated = rec.copyWith(
      size: size,
      displayName: trimmed,
      userRenamed: true,
    );
    final i = _items.indexWhere((e) => e.fileName == fileName);
    if (i >= 0) _items[i] = updated;
    await _flush();
    return updated;
  }

  /// 自动命名（同步/导入后按「时间 + 起点 → 终点」起名）。
  ///
  /// 与 [setDisplayName] 的区别：**不写 GPX 里的 `<name>`**（那是全文替换，会把途经点
  /// 标签一起冲掉），也**不置 userRenamed** —— 自动命名不该冒充用户手动改名，
  /// 否则用户改完名下次同步就被覆盖了。
  Future<RideRecord?> setAutoName(String fileName, String name) async {
    final trimmed = name.trim();
    if (trimmed.isEmpty) return null;
    final rec = find(fileName);
    if (rec == null || rec.userRenamed) return rec;
    final updated = rec.copyWith(displayName: trimmed);
    final i = _items.indexWhere((e) => e.fileName == fileName);
    if (i >= 0) _items[i] = updated;
    await _flush();
    await AppLog.i('RideRecord', '自动命名 $fileName → $trimmed');
    return updated;
  }

  /// 给还没有时长 / 缩略轨迹的记录补 GPX 数据。
  Future<bool> ensureDurations() async {
    var changed = false;
    for (var i = 0; i < _items.length; i++) {
      final rec = _items[i];
      if (rec.durationSec != null && rec.preview != null) continue;
      var sec = rec.durationSec ?? 0;
      var preview = rec.preview;
      final f = File(rec.localPath);
      if (await f.exists()) {
        try {
          final xml = utf8.decode(await f.readAsBytes(), allowMalformed: true);
          final sum = GpxUtil.parse(xml);
          if (rec.durationSec == null) {
            sec = sum.duration?.inSeconds ?? 0;
          }
          preview ??= GpxUtil.previewTrack(sum.points);
        } catch (_) {
          preview ??= const [];
        }
      } else {
        preview ??= const [];
      }
      _items[i] = rec.copyWith(durationSec: sec, preview: preview);
      changed = true;
    }
    if (changed) await _flush();
    return changed;
  }
}

List<LatLng>? _previewFromJson(Object? raw) {
  if (raw is! List) return null;
  final out = <LatLng>[];
  for (final e in raw) {
    if (e is List && e.length >= 2) {
      final lat = (e[0] as num?)?.toDouble();
      final lon = (e[1] as num?)?.toDouble();
      if (lat != null && lon != null) out.add(LatLng(lat, lon));
    }
  }
  return out;
}

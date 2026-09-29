import 'dart:convert';
import 'dart:io';

import 'package:path/path.dart' as p;
import 'package:path_provider/path_provider.dart';
import 'package:sifli_companion/log/app_log.dart';
import 'package:sifli_companion/ride/gpx_util.dart';
import 'package:sifli_companion/ride/ride_record_store.dart';

/// 本机准备写入码表 `mtp/import/` 的 GPX。
class GpxLibraryStore {
  factory GpxLibraryStore() => _instance;
  static final GpxLibraryStore _instance = GpxLibraryStore._();
  GpxLibraryStore._();

  Directory? _dir;
  final List<RideRecord> _items = [];

  List<RideRecord> get items => List.unmodifiable(_items);

  Future<Directory> _ensureDir() async {
    if (_dir != null) return _dir!;
    final root = await getApplicationDocumentsDirectory();
    _dir = Directory(p.join(root.path, 'gpx_import'));
    if (!await _dir!.exists()) {
      await _dir!.create(recursive: true);
    }
    return _dir!;
  }

  File get _indexFile => File(p.join(_dir!.path, 'index.json'));

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

  Future<RideRecord> putBytes(String fileName, List<int> bytes) async {
    await _ensureDir();
    final safe = GpxUtil.safeFileName(fileName);
    final file = File(p.join(_dir!.path, safe));
    final xml = GpxUtil.ensureTimestamps(utf8.decode(bytes, allowMalformed: true));
    final out = utf8.encode(xml);
    await file.writeAsBytes(out, flush: true);
    final sum = GpxUtil.parse(xml);
    final when = sum.endTime ?? DateTime.now().toUtc();
    final rec = RideRecord(
      fileName: safe,
      localPath: file.path,
      size: out.length,
      mtime: GpxUtil.unixSeconds(when),
      distanceKm: sum.distanceKm,
      pointCount: sum.points.length,
      durationSec: sum.duration?.inSeconds ?? 0,
      preview: GpxUtil.previewTrack(sum.points),
      displayName: sum.name.isEmpty ? null : sum.name,
    );
    _items.removeWhere((e) => e.fileName == safe);
    _items.insert(0, rec);
    await _flush();
    return rec;
  }

  Future<void> remove(String fileName) async {
    RideRecord? rec;
    for (final e in _items) {
      if (e.fileName == fileName) rec = e;
    }
    if (rec != null) {
      final f = File(rec.localPath);
      if (await f.exists()) await f.delete();
    }
    _items.removeWhere((e) => e.fileName == fileName);
    await _flush();
  }

  /// 自动命名（导入后按「时间 + 起点 → 终点」起名）。
  ///
  /// 只改 displayName，**不写 GPX 里的 `<name>`**：[GpxUtil.setName] 是全文替换，
  /// 会把途经点标签一起冲掉，而且自动命名没理由改用户的文件。也不置 userRenamed ——
  /// 那是「用户自己改过」的标记，自动命名不该冒充，否则用户手动改名后会被覆盖回去。
  /// 给还没有缩略轨迹 / 时长的条目补 GPX 数据（列表左侧要画轨迹线）。
  ///
  /// 正常路径（[putBytes]）建条目时就顺手算好了；这里兜的是老条目、以及任何
  /// 绕过 putBytes 落进来的文件 —— 免得列表里一排空图标。
  Future<bool> ensurePreviews() async {
    var changed = false;
    for (var i = 0; i < _items.length; i++) {
      final rec = _items[i];
      if (rec.preview != null && rec.durationSec != null) continue;
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

  Future<void> setAutoName(String fileName, String name) async {
    final trimmed = name.trim();
    if (trimmed.isEmpty) return;
    final i = _items.indexWhere((e) => e.fileName == fileName);
    if (i < 0) return;
    final rec = _items[i];
    if (rec.userRenamed) return;
    _items[i] = rec.copyWith(displayName: trimmed);
    await _flush();
    await AppLog.i('GpxLibrary', '自动命名 $fileName → $trimmed');
  }
}

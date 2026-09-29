import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:crypto/crypto.dart';
import 'package:path/path.dart' as p;
import 'package:sifli_companion/ble/companion_proto.dart';
import 'package:sifli_companion/cache/app_cache_store.dart';

/// App 内部通知图标缓存条目。
class NotifIconEntry {
  /// 创建一条缓存记录。
  const NotifIconEntry({
    required this.fileName,
    required this.packageName,
    required this.appName,
    required this.md5hex,
    required this.updatedAtMs,
    required this.file,
  });

  /// 线上文件名，例如 `com.tencent.mm.png`。
  final String fileName;

  /// 安卓包名。
  final String packageName;

  /// 应用显示名。
  final String appName;

  /// PNG 字节的 MD5（小写 hex）。
  final String md5hex;

  /// 最近写入的毫秒时间戳。
  final int updatedAtMs;

  /// 本地 PNG 文件。
  final File file;
}

/// 把系统通知图标持久化到 `app_cache/notif_icons/`。
///
/// 仅 App 侧按 MD5 更新；码表缺文件时再走 `FS_NEED` 拉取。
class NotifIconCache {
  NotifIconCache._();

  /// 单例。
  static final NotifIconCache instance = NotifIconCache._();

  static const _indexName = 'index.json';

  final _updates = StreamController<void>.broadcast();

  /// 缓存变化（写入新图标后）。
  Stream<void> get updates => _updates.stream;

  /// 持久化目录 `app_cache/notif_icons/`。
  Future<Directory> directory() =>
      AppCacheStore.instance.subdir(CompanionNotif.iconDir);

  /// PNG 字节的 MD5 hex。
  static String md5Of(List<int> bytes) => md5.convert(bytes).toString();

  /// 缓存目录中的 PNG 路径。
  Future<File> pngFile(String fileName) async {
    final dir = await directory();
    return File(p.join(dir.path, CompanionNotif.wireName(fileName)));
  }

  Future<Map<String, dynamic>> _readIndex() async {
    final dir = await directory();
    final f = File(p.join(dir.path, _indexName));
    if (!await f.exists()) {
      return {};
    }
    try {
      final map = jsonDecode(await f.readAsString());
      if (map is Map<String, dynamic>) {
        return map;
      }
    } catch (_) {}
    return {};
  }

  Future<void> _writeIndex(Map<String, dynamic> index) async {
    final dir = await directory();
    final f = File(p.join(dir.path, _indexName));
    await f.writeAsString(const JsonEncoder.withIndent('  ').convert(index));
  }

  /// 按 MD5 写入。内容未变且文件在则返回 `false`。
  Future<bool> putIfMd5Changed({
    required String fileName,
    required Uint8List bytes,
    required String packageName,
    required String appName,
  }) async {
    final name = CompanionNotif.wireName(fileName);
    if (name.isEmpty || bytes.isEmpty) {
      return false;
    }

    final digest = md5Of(bytes);
    final index = await _readIndex();
    final prev = index[name];
    final f = await pngFile(name);
    if (prev is Map && prev['md5'] == digest && await f.exists()) {
      return false;
    }

    await f.writeAsBytes(bytes, flush: true);
    index[name] = {
      'package': packageName,
      'appName': appName,
      'md5': digest,
      'updatedAt': DateTime.now().millisecondsSinceEpoch,
    };
    await _writeIndex(index);
    if (!_updates.isClosed) {
      _updates.add(null);
    }
    return true;
  }

  /// 读出缓存的 PNG；没有则为 `null`。
  Future<Uint8List?> readPng(String fileName) async {
    final name = CompanionNotif.wireName(fileName);
    if (name.isEmpty) {
      return null;
    }
    final f = await pngFile(name);
    if (!await f.exists()) {
      return null;
    }
    return f.readAsBytes();
  }

  /// 列出已缓存图标，新的在前。
  Future<List<NotifIconEntry>> list() async {
    final dir = await directory();
    final index = await _readIndex();
    final out = <NotifIconEntry>[];
    await for (final ent in dir.list()) {
      if (ent is! File) {
        continue;
      }
      final base = p.basename(ent.path);
      if (!base.toLowerCase().endsWith('.png')) {
        continue;
      }
      final meta = index[base];
      out.add(
        NotifIconEntry(
          fileName: base,
          packageName: meta is Map ? (meta['package'] as String? ?? '') : '',
          appName: meta is Map ? (meta['appName'] as String? ?? '') : '',
          md5hex: meta is Map ? (meta['md5'] as String? ?? '') : '',
          updatedAtMs: meta is Map ? (meta['updatedAt'] as int? ?? 0) : 0,
          file: ent,
        ),
      );
    }
    out.sort((a, b) => b.updatedAtMs.compareTo(a.updatedAtMs));
    return out;
  }
}

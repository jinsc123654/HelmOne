import 'dart:io';

import 'package:path/path.dart' as p;
import 'package:path_provider/path_provider.dart';

/// App 后台持久化缓存根。
///
/// 落在 [getApplicationSupportDirectory] 下的 `app_cache/`，随应用私有数据
/// 一起保留：杀进程、重启都还在。系统清「缓存」清不到这里；只有卸载或用户
/// 清应用数据才会没。通知图标、OSM 瓦片（`osm/`）等子目录都落在这里，
/// 用 [AppCacheBrowserPage] 当本地文件管理器查看。
class AppCacheStore {
  AppCacheStore._();

  /// 单例。
  static final AppCacheStore instance = AppCacheStore._();

  /// 相对应用支持目录的根名。
  static const rootName = 'app_cache';

  Directory? _root;

  /// 持久化缓存根目录，不存在则创建。
  Future<Directory> root() async {
    if (_root != null && await _root!.exists()) {
      return _root!;
    }
    final support = await getApplicationSupportDirectory();
    final d = Directory(p.join(support.path, rootName));
    if (!await d.exists()) {
      await d.create(recursive: true);
    }
    _root = d;
    return d;
  }

  /// 根下的子目录，不存在则创建。
  Future<Directory> subdir(String name) async {
    final n = sanitizeName(name);
    if (n.isEmpty) {
      throw ArgumentError('缓存子目录名无效');
    }
    final r = await root();
    final d = Directory(p.join(r.path, n));
    if (!await d.exists()) {
      await d.create(recursive: true);
    }
    return d;
  }

  /// 把相对路径解析成根下的绝对路径；拒绝 `..` 与绝对路径。
  Future<String> resolve(String rel) async {
    final r = await root();
    final cleaned = wirePath(rel);
    if (cleaned.isEmpty) {
      return r.path;
    }
    return p.join(r.path, cleaned);
  }

  /// 清洗单一路径分量。
  static String sanitizeName(String raw) {
    var s = raw.trim();
    if (s.contains('/') || s.contains('\\')) {
      return '';
    }
    if (s.isEmpty || s == '.' || s == '..') {
      return '';
    }
    return s;
  }

  /// 相对路径：去掉首尾 `/`，拒绝 `..`。
  static String wirePath(String path) {
    var value = path.trim().replaceAll('\\', '/');
    while (value.startsWith('/')) {
      value = value.substring(1);
    }
    while (value.endsWith('/')) {
      value = value.substring(0, value.length - 1);
    }
    if (value == '.' || value.isEmpty) {
      return '';
    }
    final parts = <String>[];
    for (final part in value.split('/')) {
      if (part.isEmpty || part == '.') {
        continue;
      }
      if (part == '..') {
        return '';
      }
      parts.add(part);
    }
    return parts.join('/');
  }

  /// 相对路径拼接。
  static String joinRel(String dir, String name) {
    final base = wirePath(dir);
    final n = sanitizeName(name);
    if (n.isEmpty) {
      return base;
    }
    if (base.isEmpty) {
      return n;
    }
    return '$base/$n';
  }

  /// 上一级相对路径。
  static String parentRel(String dir) {
    final base = wirePath(dir);
    if (base.isEmpty) {
      return '';
    }
    final i = base.lastIndexOf('/');
    if (i < 0) {
      return '';
    }
    return base.substring(0, i);
  }

  /// 界面展示（根为 `/`）。
  static String displayPath(String dir) {
    final base = wirePath(dir);
    return base.isEmpty ? '/' : '/$base';
  }

  /// 删除根下的一个文件或目录（目录递归）。
  ///
  /// 路径经 [wirePath] 清洗，`..` 会被拒成空串 —— 空串代表根，绝不能删，
  /// 所以这里显式挡掉，避免界面传错参数把整个缓存根端掉。
  Future<void> deleteEntry(String rel) async {
    final clean = wirePath(rel);
    if (clean.isEmpty) {
      throw ArgumentError('拒绝删除缓存根');
    }
    final abs = await resolve(clean);
    final type = await FileSystemEntity.type(abs, followLinks: false);
    if (type == FileSystemEntityType.notFound) {
      return;
    }
    if (type == FileSystemEntityType.directory) {
      await Directory(abs).delete(recursive: true);
    } else {
      await File(abs).delete();
    }
  }

  /// 重命名根下的一个条目。
  ///
  /// 只允许改同一层里的名字（父目录不变），避免界面传相对路径造成误移动。
  Future<void> renameEntry(String rel, String newName) async {
    final clean = wirePath(rel);
    if (clean.isEmpty) {
      throw ArgumentError('拒绝重命名缓存根');
    }
    final n = sanitizeName(newName);
    if (n.isEmpty) {
      throw ArgumentError('名称无效');
    }
    final abs = await resolve(clean);
    final parent = p.dirname(abs);
    final target = p.join(parent, n);
    if (target == abs) {
      return;
    }
    final type = await FileSystemEntity.type(abs, followLinks: false);
    if (type == FileSystemEntityType.notFound) {
      throw FileSystemException('源不存在', abs);
    }
    if (type == FileSystemEntityType.directory) {
      await Directory(abs).rename(target);
    } else {
      await File(abs).rename(target);
    }
  }

  /// 根目录下全部文件合计字节数。
  Future<int> totalBytes() async {
    final r = await root();
    var n = 0;
    await for (final e in r.list(recursive: true, followLinks: false)) {
      if (e is File) {
        n += await e.length();
      }
    }
    return n;
  }
}

/// 缓存目录里的一项。
class AppCacheEntry {
  /// 创建条目。
  const AppCacheEntry({
    required this.name,
    required this.isDir,
    required this.size,
    required this.mtime,
    required this.entity,
  });

  /// 文件或目录名。
  final String name;

  /// 是否目录。
  final bool isDir;

  /// 文件字节数；目录为 0。
  final int size;

  /// 修改时间。
  final DateTime? mtime;

  /// 本地实体。
  final FileSystemEntity entity;
}

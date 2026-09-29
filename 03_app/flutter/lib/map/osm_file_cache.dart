import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_map/flutter_map.dart';
import 'package:path/path.dart' as p;
import 'package:sifli_companion/cache/app_cache_store.dart';

/// 把瓦片存成 `app_cache/osm/tiles/{z}/{x}/{y}.png`，离线可按城市统计。
class OsmFileCache implements MapCachingProvider {
  OsmFileCache._();

  /// 单例。
  static final OsmFileCache instance = OsmFileCache._();

  static const _fresh = Duration(days: 3650);

  Directory? _tiles;

  /// 瓦片根目录。
  Directory? get tilesDir => _tiles;

  /// 在 [runApp] 前调用。
  Future<void> init() async {
    if (_tiles != null && await _tiles!.exists()) return;
    final root = await AppCacheStore.instance.subdir('osm');
    final d = Directory(p.join(root.path, 'tiles'));
    if (!await d.exists()) {
      await d.create(recursive: true);
    }
    _tiles = d;
  }

  File fileFor(int z, int x, int y) {
    final root = _tiles;
    if (root == null) {
      throw StateError('OsmFileCache 未 init');
    }
    return File(p.join(root.path, '$z', '$x', '$y.png'));
  }

  Future<File> ensureFile(int z, int x, int y) async {
    await init();
    final f = fileFor(z, x, y);
    await f.parent.create(recursive: true);
    return f;
  }

  Future<bool> hasTile(int z, int x, int y) async {
    await init();
    final f = fileFor(z, x, y);
    return f.exists();
  }

  /// 从瓦片 URL 解析 z/x/y。
  static (int z, int x, int y)? parseXyz(String url) {
    final uri = Uri.tryParse(url);
    if (uri != null) {
      final z = int.tryParse(uri.queryParameters['z'] ?? '');
      final x = int.tryParse(uri.queryParameters['x'] ?? '');
      final y = int.tryParse(uri.queryParameters['y'] ?? '');
      if (z != null && x != null && y != null) {
        return (z, x, y);
      }
    }
    final m = RegExp(r'/(\d+)/(\d+)/(\d+)\.(?:png|jpg|jpeg)').firstMatch(url);
    if (m == null) return null;
    return (
      int.parse(m.group(1)!),
      int.parse(m.group(2)!),
      int.parse(m.group(3)!),
    );
  }

  @override
  bool get isSupported => true;

  @override
  Future<CachedMapTile?> getTile(String url) async {
    await init();
    final xyz = parseXyz(url);
    if (xyz == null) return null;
    final f = fileFor(xyz.$1, xyz.$2, xyz.$3);
    if (!await f.exists()) return null;
    final bytes = await f.readAsBytes();
    if (bytes.length < 32) return null;
    return (
      bytes: Uint8List.fromList(bytes),
      metadata: CachedMapTileMetadata(
        staleAt: DateTime.timestamp().add(_fresh),
        lastModified: null,
        etag: null,
      ),
    );
  }

  @override
  Future<void> putTile({
    required String url,
    required CachedMapTileMetadata metadata,
    Uint8List? bytes,
  }) async {
    if (bytes == null || bytes.length < 32) return;
    if (bytes[0] != 0x89 || bytes[1] != 0x50) return;
    await init();
    final xyz = parseXyz(url);
    if (xyz == null) return;
    final f = await ensureFile(xyz.$1, xyz.$2, xyz.$3);
    await f.writeAsBytes(bytes, flush: false);
  }
}

import 'dart:convert';
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:path/path.dart' as p;
import 'package:sifli_companion/app/app_keys.dart';
import 'package:sifli_companion/cache/app_cache_store.dart';
import 'package:sifli_companion/log/app_log.dart';
import 'package:sifli_companion/map/osm_city_catalog.dart';
import 'package:sifli_companion/map/osm_file_cache.dart';
import 'package:sifli_companion/map/osm_tile_math.dart';
import 'package:sifli_companion/map/osm_tiles.dart';

/// 一座城市的本机下载进度。
class OsmCityStatus {
  /// 创建状态。
  const OsmCityStatus({
    required this.city,
    required this.have,
    required this.bytes,
    required this.expected,
  });

  final OsmCity city;
  final int have;
  final int bytes;
  final int expected;

  bool get isComplete => expected > 0 && have >= (expected * 0.98).floor();

  bool get isPartial => have > 0 && !isComplete;

  bool get isEmpty => have <= 0;

  double get ratio => expected <= 0 ? 0 : (have / expected).clamp(0.0, 1.0);
}

/// 手动下载城市瓦片（国内高德源），并统计是否下完。
class OsmCityStore extends ChangeNotifier {
  OsmCityStore._();

  /// 单例。后台下载离开页面也继续。
  static final OsmCityStore instance = OsmCityStore._();

  static const _ua = '${AppKeys.appName} (${AppKeys.androidPackage})';

  String? _activeId;
  int _done = 0;
  int _total = 0;
  bool _cancel = false;
  String? _error;
  bool _running = false;

  /// 正在下的城市 id。
  String? get activeId => _activeId;

  int get done => _done;

  int get total => _total;

  bool get running => _running;

  String? get error => _error;

  Future<File> _indexFile() async {
    final root = await AppCacheStore.instance.subdir('osm');
    return File(p.join(root.path, 'cities.json'));
  }

  Future<Map<String, dynamic>> _readIndex() async {
    final f = await _indexFile();
    if (!await f.exists()) return {};
    try {
      final raw = jsonDecode(utf8.decode(await f.readAsBytes()));
      if (raw is Map<String, dynamic>) return raw;
      if (raw is Map) {
        return raw.map((k, v) => MapEntry('$k', v));
      }
    } catch (_) {}
    return {};
  }

  Future<void> _writeIndex(Map<String, dynamic> idx) async {
    final f = await _indexFile();
    await f.writeAsString(const JsonEncoder.withIndent('  ').convert(idx));
  }

  /// 扫描一座城市当前磁盘上的张数与体积。
  Future<OsmCityStatus> statusOf(OsmCity city) async {
    await OsmFileCache.instance.init();
    final tiles = OsmTileMath.tilesInBounds(
      southWest: city.gcjSouthWest,
      northEast: city.gcjNorthEast,
      minZoom: city.minZoom,
      maxZoom: city.maxZoom,
    );
    var have = 0;
    var bytes = 0;
    for (final t in tiles) {
      final f = OsmFileCache.instance.fileFor(t.$1, t.$2, t.$3);
      if (await f.exists()) {
        have++;
        bytes += await f.length();
      }
    }
    return OsmCityStatus(
      city: city,
      have: have,
      bytes: bytes,
      expected: tiles.length,
    );
  }

  /// 已开始过的城市（索引里有记录，或磁盘上已有瓦片）。
  Future<List<OsmCityStatus>> downloaded() async {
    final idx = await _readIndex();
    final out = <OsmCityStatus>[];
    for (final id in idx.keys) {
      final city = OsmCityCatalog.byId(id);
      if (city == null) continue;
      out.add(await statusOf(city));
    }
    out.sort((a, b) {
      if (a.isComplete != b.isComplete) return a.isComplete ? -1 : 1;
      return a.city.name.compareTo(b.city.name);
    });
    return out;
  }

  /// 已下完的座数（读索引，不扫磁盘）。
  Future<int> completeCount() async {
    final idx = await _readIndex();
    var n = 0;
    for (final v in idx.values) {
      if (v is! Map) continue;
      final have = (v['have'] as num?)?.toInt() ?? 0;
      final expected = (v['expected'] as num?)?.toInt() ?? 0;
      final cancelled = v['cancelled'] == true;
      if (!cancelled &&
          expected > 0 &&
          have >= (expected * 0.98).floor()) {
        n++;
      }
    }
    return n;
  }

  /// 开始或续传。同时只能下一座。
  Future<void> download(OsmCity city) async {
    if (_running) return;
    _running = true;
    _cancel = false;
    _error = null;
    _activeId = city.id;
    notifyListeners();

    final client = HttpClient();
    client.userAgent = _ua;
    client.connectionTimeout = const Duration(seconds: 12);

    try {
      await OsmFileCache.instance.init();
      final tiles = OsmTileMath.tilesInBounds(
        southWest: city.gcjSouthWest,
        northEast: city.gcjNorthEast,
        minZoom: city.minZoom,
        maxZoom: city.maxZoom,
      );
      _total = tiles.length;
      _done = 0;

      const workers = 4;
      var cursor = 0;

      Future<void> worker() async {
        while (true) {
          if (_cancel) return;
          final i = cursor++;
          if (i >= tiles.length) return;
          final t = tiles[i];
          final file = await OsmFileCache.instance.ensureFile(t.$1, t.$2, t.$3);
          if (await file.exists() && await file.length() > 32) {
            _done++;
            if (_done % 8 == 0) notifyListeners();
            continue;
          }
          final url = HelmOsmTiles.tileUrl(t.$1, t.$2, t.$3);
          try {
            final req = await client.getUrl(Uri.parse(url));
            req.headers.set('Referer', HelmOsmTiles.referer);
            final res = await req.close().timeout(const Duration(seconds: 20));
            if (res.statusCode != 200) {
              _done++;
              continue;
            }
            final bytes = await res.fold<List<int>>(
              <int>[],
              (p, e) => p..addAll(e),
            );
            if (bytes.length >= 32 && bytes[0] == 0x89 && bytes[1] == 0x50) {
              await file.writeAsBytes(bytes, flush: false);
            }
          } catch (_) {
            // 单张失败不中断整城；可再续传。
          }
          _done++;
          if (_done % 4 == 0) notifyListeners();
        }
      }

      await Future.wait(List.generate(workers, (_) => worker()));

      final st = await statusOf(city);
      final idx = await _readIndex();
      idx[city.id] = {
        'have': st.have,
        'expected': st.expected,
        'bytes': st.bytes,
        'updatedAt': DateTime.now().toIso8601String(),
        'cancelled': _cancel,
      };
      await _writeIndex(idx);
    } catch (e, st) {
      await AppLog.e('OsmCity', e, st);
      _error = 'failed';
    } finally {
      client.close(force: true);
      _running = false;
      _activeId = null;
      notifyListeners();
    }
  }

  /// 停止当前下载（已写入的瓦片保留，可续传）。
  void cancel() {
    _cancel = true;
  }

  /// 只从已下载列表去掉记录，不删共享瓦片文件。
  Future<void> forget(String id) async {
    final idx = await _readIndex();
    idx.remove(id);
    await _writeIndex(idx);
    notifyListeners();
  }
}

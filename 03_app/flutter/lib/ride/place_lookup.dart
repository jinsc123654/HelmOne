import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:get/get.dart';
import 'package:latlong2/latlong.dart';
import 'package:path/path.dart' as p;
import 'package:path_provider/path_provider.dart';
import 'package:sifli_companion/app/app_keys.dart';
import 'package:sifli_companion/i18n/locale_keys.dart';
import 'package:sifli_companion/log/app_log.dart';
import 'package:sifli_companion/map/osm_city_catalog.dart';

/// 坐标 → 地名，给导入的轨迹起个能认出来的名字。
///
/// 底图瓦片是图片（高德 `style=7` 的 PNG），路名地名都画在像素里取不出来；
/// 能离线用的是内置城市目录（23 个城市、WGS-84 包围盒），所以这里分两层：
/// 先用目录给到城市级，再（有网时）细化到 POI / 道路级。
///
/// 在线源用 OSM Nominatim：不需要 key，中文环境返回中文。它是公益服务，
/// 所以这里做了磁盘缓存 + 串行 + 限速，同一个点不会问第二次。
class PlaceLookup {
  PlaceLookup._();
  static final PlaceLookup instance = PlaceLookup._();

  static const _host = 'nominatim.openstreetmap.org';
  static const _ua = '${AppKeys.appName} (${AppKeys.androidPackage})';

  /// 公益服务要求 ≤1 请求/秒。点少（一条轨迹最多两个点），串行排队即可。
  static const _minGap = Duration(milliseconds: 1100);

  /// 只为取名，不值得让用户等：连不上就退城市名。
  static const _connectTimeout = Duration(seconds: 4);
  static const _totalTimeout = Duration(seconds: 6);

  static const _maxCached = 500;

  final _cache = <String, String?>{};
  bool _cacheLoaded = false;
  DateTime _lastCall = DateTime.fromMillisecondsSinceEpoch(0);
  Future<void> _queue = Future<void>.value();

  static String _key(LatLng p) =>
      '${p.latitude.toStringAsFixed(4)},${p.longitude.toStringAsFixed(4)}';

  /// 缓存文件名带版本：字段优先级、zoom 改过之后，旧缓存里的粗名字（「滁州」）
  /// 必须作废，否则永远看不到街道级的新结果。
  static Future<File> _cacheFile() async {
    final dir = await getApplicationSupportDirectory();
    return File(p.join(dir.path, 'place_cache2.json'));
  }

  Future<void> _loadCache() async {
    if (_cacheLoaded) return;
    _cacheLoaded = true;
    try {
      final f = await _cacheFile();
      if (!await f.exists()) return;
      final raw = jsonDecode(await f.readAsString());
      if (raw is! Map) return;
      raw.forEach((k, v) {
        if (k is String) _cache[k] = v is String && v.trim().isNotEmpty ? v.trim() : null;
      });
    } catch (e) {
      await AppLog.w('PlaceLookup', '读地名缓存失败: $e');
    }
  }

  Future<void> _flushCache() async {
    try {
      while (_cache.length > _maxCached) {
        _cache.remove(_cache.keys.first);
      }
      final f = await _cacheFile();
      await f.writeAsString(jsonEncode(_cache));
    } catch (e) {
      await AppLog.w('PlaceLookup', '写地名缓存失败: $e');
    }
  }

  /// 坐标 → 地名。取不到返回 `null`，由调用方降级成坐标。
  Future<String?> nameFor(LatLng wgs) async {
    await _loadCache();
    final key = _key(wgs);
    if (_cache.containsKey(key)) return _cache[key];

    final online = await _enqueue(() => _fetch(wgs));
    // **只缓存在线结果**：离线兜底的城市名（「滁州」）一旦写进缓存，下次有网了
    // 也不会再问，名字就永远停在城市级 —— 这正是「滁州→滁州」的成因。
    if (online != null) {
      _cache[key] = online;
      unawaited(_flushCache());
      return online;
    }
    return offlineCity(wgs);
  }

  /// 只认**在线**结果：拿不到返回 `null`。
  ///
  /// 给"重取一遍地名"用 —— 离线时宁可什么都不改，也不要用城市名把之前
  /// 好不容易取到的细名字盖掉。
  Future<String?> nameForOnline(LatLng wgs) async {
    await _loadCache();
    final key = _key(wgs);
    if (_cache.containsKey(key)) return _cache[key];
    final online = await _enqueue(() => _fetch(wgs));
    if (online != null) {
      _cache[key] = online;
      unawaited(_flushCache());
    }
    return online;
  }

  /// 一条轨迹的展示名：时间 + 起点 → 终点。
  Future<String?> trackName(List<LatLng> points, {DateTime? start}) async {
    if (points.length < 2) return null;
    final from = await nameFor(points.first);
    final to = await nameFor(points.last);
    return TrackAutoName.compose(
      start: start,
      from: from,
      to: to,
      first: points.first,
      last: points.last,
    );
  }

  /// 离线兜底：内置城市目录里包围盒包含该点的城市（只到城市级）。
  static String? offlineCity(LatLng wgs) {
    for (final c in OsmCityCatalog.cities) {
      if (wgs.latitude >= c.south &&
          wgs.latitude <= c.north &&
          wgs.longitude >= c.west &&
          wgs.longitude <= c.east) {
        return c.name;
      }
    }
    return null;
  }

  /// 串行 + 限速，避免并发打爆公益服务。
  Future<T> _enqueue<T>(Future<T> Function() job) {
    final prev = _queue;
    final gate = Completer<void>();
    _queue = gate.future;
    return () async {
      try {
        await prev;
        final gap = DateTime.now().difference(_lastCall);
        if (gap < _minGap) await Future<void>.delayed(_minGap - gap);
        _lastCall = DateTime.now();
        return await job();
      } finally {
        gate.complete();
      }
    }();
  }

  Future<String?> _fetch(LatLng wgs) async {
    final uri = Uri.https(_host, '/reverse', {
      'format': 'jsonv2',
      'lat': wgs.latitude.toStringAsFixed(6),
      'lon': wgs.longitude.toStringAsFixed(6),
      'accept-language': 'zh-CN',
      // 18 = 街道 / 门牌一级。16 只会给到城市，取出来的名字就是「滁州→滁州」。
      'zoom': '18',
      'addressdetails': '1',
    });
    if (!_hostAllowed(uri)) {
      await AppLog.w('PlaceLookup', '拒绝请求不可用的主机: ${uri.host}');
      return null;
    }

    final client = HttpClient();
    try {
      client.userAgent = _ua;
      client.connectionTimeout = _connectTimeout;
      final req = await client.getUrl(uri);
      req.headers.set(HttpHeaders.acceptHeader, 'application/json');
      final res = await req.close().timeout(_totalTimeout);
      if (res.statusCode < 200 || res.statusCode >= 300) {
        await AppLog.w('PlaceLookup', '反查失败 http ${res.statusCode}');
        return null;
      }
      return parseName(jsonDecode(await utf8.decodeStream(res)));
    } catch (e) {
      await AppLog.i('PlaceLookup', '反查 ${_key(wgs)} 失败: $e');
      return null;
    } finally {
      client.close(force: true);
    }
  }

  /// 只允许 http(s) + 公网主机：地址即使将来可配，也不能打到本机或内网。
  static bool _hostAllowed(Uri uri) {
    if (uri.scheme != 'https' && uri.scheme != 'http') return false;
    final h = uri.host.toLowerCase();
    if (h.isEmpty) return false;
    if (h == 'localhost' || h.endsWith('.localhost') || h.endsWith('.local')) {
      return false;
    }
    final ip = InternetAddress.tryParse(h);
    if (ip == null) return true; // 域名交给系统解析
    final b = ip.rawAddress;
    if (b.length == 4) {
      final a0 = b[0];
      final a1 = b[1];
      if (a0 == 0 || a0 == 127 || a0 == 10) return false;
      if (a0 == 172 && a1 >= 16 && a1 <= 31) return false;
      if (a0 == 192 && a1 == 168) return false;
      if (a0 == 169 && a1 == 254) return false;
      if (a0 >= 224) return false; // 组播 / 保留
      return true;
    }
    if (b.length == 16) {
      if (ip.isLoopback || ip.isLinkLocal) return false; // ::1 / fe80::/10
      if ((b[0] & 0xFE) == 0xFC) return false; // fc00::/7 唯一本地地址
      return true;
    }
    return false;
  }

  /// 从 Nominatim 返回里挑最像「地点名」的字段。
  ///
  /// 优先级按**骑行时能不能认出来**排：
  /// 道路 / 自行车道 → POI（公园、车站）→ 街区 → 区县 → 市。
  ///
  /// 以前是 `name` → `city` 打头，于是不管在哪骑，名字都是「滁州→滁州」：
  /// 城市名对"这段路是哪一段"毫无区分度，街道名才有。
  static String? parseName(Object? json) {
    if (json is! Map) return null;
    String? txt(Object? v) {
      final t = v is String ? v.trim() : '';
      return t.isEmpty ? null : t;
    }

    final addr = json['address'];
    if (addr is Map) {
      // 先要路名：绿道、滨江道、某某路，是骑行轨迹最有辨识度的标签。
      for (final k in const [
        'road',
        'cycleway',
        'footway',
        'path',
        'pedestrian',
        'residential',
      ]) {
        final v = txt(addr[k]);
        if (v != null) return v;
      }
    }
    // 其次 POI：公园、车站、商场这类名字也比城市好认。
    final top = txt(json['name']);
    if (top != null) return top;
    if (addr is Map) {
      for (final k in const [
        'neighbourhood',
        'quarter',
        'suburb',
        'hamlet',
        'village',
        'city_district',
        'district',
        'county',
        'town',
        'city',
        'state',
      ]) {
        final v = txt(addr[k]);
        if (v != null) return v;
      }
    }
    final disp = txt(json['display_name']);
    if (disp != null) {
      final first = disp.split(',').first.trim();
      return first.isEmpty ? null : first;
    }
    return null;
  }
}

/// 轨迹展示名：时间 + 起点 → 终点。
///
/// 例：`9/17 08:30 玄武湖→南京南站`。地名取不到时用坐标顶上，保证「时间 + 起点 +
/// 终点」这个结构始终成立（用户明确要这个格式）。
abstract class TrackAutoName {
  /// 单个地名最长保留几个字：名字整体还要当设备文件名用（上限 27 字符），
  /// 6（时间）+ 1（空格）+ 9 + 1（箭头）+ 9 = 26，刚好装得下。
  static const maxLabel = 9;

  static String compose({
    required DateTime? start,
    required String? from,
    required String? to,
    required LatLng first,
    required LatLng last,
  }) {
    final when = start == null ? null : _stamp(start);
    // 地名裁到 maxLabel；坐标不裁 —— 裁掉一半的坐标是没法读的垃圾。
    final a = from == null ? _coord(first) : _clip(from);
    final b = to == null ? _coord(last) : _clip(to);
    // 起终点同名（环线、或都在同一条路上）时，"X→X" 等于什么都没说 —— 标环线。
    final body = a == b ? '$a ${LocaleKeys.rideLoopSuffix.tr}' : '$a→$b';
    return <String>[?when, body].join(' ');
  }

  /// `9/17 08:30`（本地时间）。
  static String _stamp(DateTime t) {
    final l = t.toLocal();
    final hh = l.hour.toString().padLeft(2, '0');
    final mm = l.minute.toString().padLeft(2, '0');
    return '${l.month}/${l.day} $hh:$mm';
  }

  /// 兜底坐标：约 100 m 精度，够区分起点终点又不至于太长。
  static String _coord(LatLng p) =>
      '${p.latitude.toStringAsFixed(3)},${p.longitude.toStringAsFixed(3)}';

  static String _clip(String s) =>
      s.length <= maxLabel ? s : s.substring(0, maxLabel);
}

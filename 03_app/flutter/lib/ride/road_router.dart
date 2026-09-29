import 'dart:convert';
import 'dart:io';
import 'dart:math' as math;

import 'package:latlong2/latlong.dart';
import 'package:sifli_companion/app/app_keys.dart';

/// 路网吸附与两点之间沿道路补点（BRouter + OSRM，用法对齐 gpx.studio）。
class RoadRouter {
  RoadRouter._();
  static final RoadRouter instance = RoadRouter._();

  static const snapMaxMeters = 200.0;

  static const _ua = '${AppKeys.appName} (${AppKeys.androidPackage})';

  /// 结果缓存条数上限。编辑一条件路线时会反复吸附同一个点、重算同一段路
  /// （排序 / 删除 / 撤销之后大部分路段其实没变），没有缓存就每次重走公网。
  static const _maxCached = 240;

  final _snapCache = <String, LatLng>{};
  final _legCache = <String, List<LatLng>>{};

  static void _remember<K, V>(Map<K, V> cache, K key, V value) {
    cache[key] = value;
    if (cache.length > _maxCached) cache.remove(cache.keys.first);
  }

  /// 约 1.1 m 精度：同一颗钉子重算时必然命中，手指也不会点出更细的差别。
  static String _key(LatLng p) =>
      '${p.latitude.toStringAsFixed(5)},${p.longitude.toStringAsFixed(5)}';

  /// 把点击吸到最近道路。太远则抛 [RoadRouterException]。
  Future<LatLng> snap(LatLng tap) async {
    final cached = _snapCache[_key(tap)];
    if (cached != null) return cached;

    final uri = Uri.https(
      'router.project-osrm.org',
      '/nearest/v1/driving/${tap.longitude},${tap.latitude}',
      {'number': '1'},
    );
    final json = await _getJson(uri);
    final snapped = parseOsrmNearest(json, tap);
    if (snapped == null) {
      throw const RoadRouterException('no road');
    }
    _remember(_snapCache, _key(tap), snapped);
    return snapped;
  }

  /// 吸附，但失败不抛：网络不通或离路太远都退化成「就用你点的位置」。
  ///
  /// 编辑界面里因为一次超时把用户刚点的位置吞掉是最糟的选择 —— 点在哪就放在哪，
  /// 由调用方按 [RoadSnapResult.error] 提示一句并记日志。
  Future<RoadSnapResult> snapOrKeep(LatLng tap) async {
    try {
      return RoadSnapResult(await snap(tap));
    } on RoadRouterException {
      return RoadSnapResult(tap, kept: true);
    } catch (e, st) {
      return RoadSnapResult(tap, kept: true, error: e, stack: st);
    }
  }

  /// 两点之间沿路网的折线（含起终点）。骑行剖面优先 BRouter。
  Future<List<LatLng>> route(LatLng from, LatLng to) async {
    final key = '${_key(from)}|${_key(to)}';
    final cached = _legCache[key];
    if (cached != null) return cached;

    final pts = await _routeUncached(from, to);
    _remember(_legCache, key, pts);
    return pts;
  }

  Future<List<LatLng>> _routeUncached(LatLng from, LatLng to) async {
    try {
      final pts = parseBrouterGeoJson(await _getJson(_brouterUri(from, to)));
      if (pts.length >= 2) return pts;
    } catch (_) {}
    final pts = parseOsrmRoute(
      await _getJson(_osrmRouteUri(from, to)),
    );
    if (pts.length < 2) {
      throw const RoadRouterException('no route');
    }
    return pts;
  }

  /// 途经点串成完整路网折线。
  ///
  /// 各段互不依赖，[Future.wait] 并行发。串行时 N 个点要排 N-1 个来回
  /// （公网 OSRM/BRouter 单段实测 1.6–1.9 s），点一下就要干等好几秒 —— 这正是
  /// 「选点后像卡住」的主因；并行后整条重建只等最慢的一段。
  Future<List<LatLng>> routeAnchors(List<LatLng> anchors) async {
    if (anchors.length < 2) return List<LatLng>.from(anchors);
    final legs = await Future.wait([
      for (var i = 1; i < anchors.length; i++)
        route(anchors[i - 1], anchors[i]),
    ]);
    final out = <LatLng>[anchors.first];
    for (var i = 0; i < legs.length; i++) {
      final leg = legs[i];
      if (leg.length >= 2) {
        out.addAll(leg.skip(1));
      } else {
        out.add(anchors[i + 1]);
      }
    }
    return out;
  }

  Uri _brouterUri(LatLng a, LatLng b) {
    final lonlats =
        '${a.longitude.toStringAsFixed(8)},${a.latitude.toStringAsFixed(8)}|'
        '${b.longitude.toStringAsFixed(8)},${b.latitude.toStringAsFixed(8)}';
    return Uri.https('brouter.de', '/brouter', {
      'lonlats': lonlats,
      'profile': 'trekking',
      'alternativeidx': '0',
      'format': 'geojson',
    });
  }

  Uri _osrmRouteUri(LatLng a, LatLng b) {
    return Uri.https(
      'router.project-osrm.org',
      '/route/v1/driving/'
          '${a.longitude},${a.latitude};${b.longitude},${b.latitude}',
      {'overview': 'full', 'geometries': 'geojson'},
    );
  }

  Future<Object?> _getJson(Uri uri) async {
    final client = HttpClient();
    try {
      client.userAgent = _ua;
      client.connectionTimeout = const Duration(seconds: 12);
      final req = await client.getUrl(uri);
      req.headers.set(HttpHeaders.acceptHeader, 'application/json');
      final res = await req.close().timeout(const Duration(seconds: 20));
      final body = await utf8.decodeStream(res);
      if (res.statusCode < 200 || res.statusCode >= 300) {
        throw RoadRouterException('http ${res.statusCode}');
      }
      return jsonDecode(body);
    } finally {
      client.close(force: true);
    }
  }

  /// OSRM `/nearest`：距离超过 [snapMaxMeters] 视为失败。
  static LatLng? parseOsrmNearest(Object? json, LatLng tap) {
    if (json is! Map) return null;
    if (json['code'] != 'Ok') return null;
    final wps = json['waypoints'];
    if (wps is! List || wps.isEmpty) return null;
    final wp = wps.first;
    if (wp is! Map) return null;
    final loc = wp['location'];
    if (loc is! List || loc.length < 2) return null;
    final lon = (loc[0] as num).toDouble();
    final lat = (loc[1] as num).toDouble();
    final dist = (wp['distance'] as num?)?.toDouble() ??
        _haversineM(tap, LatLng(lat, lon));
    if (dist > snapMaxMeters) return null;
    return LatLng(lat, lon);
  }

  static List<LatLng> parseBrouterGeoJson(Object? json) {
    if (json is! Map) return const [];
    final features = json['features'];
    if (features is! List || features.isEmpty) return const [];
    final feat = features.first;
    if (feat is! Map) return const [];
    final geom = feat['geometry'];
    if (geom is! Map) return const [];
    return _coordsToLatLng(geom['coordinates']);
  }

  static List<LatLng> parseOsrmRoute(Object? json) {
    if (json is! Map) return const [];
    if (json['code'] != 'Ok') return const [];
    final routes = json['routes'];
    if (routes is! List || routes.isEmpty) return const [];
    final route = routes.first;
    if (route is! Map) return const [];
    final geom = route['geometry'];
    if (geom is! Map) return const [];
    return _coordsToLatLng(geom['coordinates']);
  }

  static List<LatLng> _coordsToLatLng(Object? raw) {
    if (raw is! List) return const [];
    final out = <LatLng>[];
    for (final c in raw) {
      if (c is! List || c.length < 2) continue;
      final lon = (c[0] as num).toDouble();
      final lat = (c[1] as num).toDouble();
      if (out.isNotEmpty) {
        final prev = out.last;
        if ((prev.latitude - lat).abs() < 1e-8 &&
            (prev.longitude - lon).abs() < 1e-8) {
          continue;
        }
      }
      out.add(LatLng(lat, lon));
    }
    return out;
  }

  static double _haversineM(LatLng a, LatLng b) {
    const r = 6371000.0;
    final dLat = _rad(b.latitude - a.latitude);
    final dLon = _rad(b.longitude - a.longitude);
    final x = math.sin(dLat / 2) * math.sin(dLat / 2) +
        math.cos(_rad(a.latitude)) *
            math.cos(_rad(b.latitude)) *
            math.sin(dLon / 2) *
            math.sin(dLon / 2);
    return 2 * r * math.asin(math.min(1, math.sqrt(x)));
  }

  static double _rad(double deg) => deg * math.pi / 180;
}

class RoadRouterException implements Exception {
  const RoadRouterException(this.message);
  final String message;

  @override
  String toString() => 'RoadRouterException($message)';
}

/// [RoadRouter.snapOrKeep] 的结果。
class RoadSnapResult {
  const RoadSnapResult(this.point, {this.kept = false, this.error, this.stack});

  /// 最终用的点：吸上了就是路上的点，没吸上就是用户点的原始位置。
  final LatLng point;

  /// 没吸上，[point] 是用户点的位置。
  final bool kept;

  /// 网络/服务异常（而不是「离路太远」）。有值即说明失败原因在链路。
  final Object? error;
  final StackTrace? stack;
}

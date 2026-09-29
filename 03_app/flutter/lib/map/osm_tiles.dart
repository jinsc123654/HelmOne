import 'package:flutter/foundation.dart';
import 'package:flutter_map/flutter_map.dart';
import 'package:latlong2/latlong.dart';
import 'package:sifli_companion/app/app_keys.dart';
import 'package:sifli_companion/map/china_coord.dart';
import 'package:sifli_companion/map/osm_file_cache.dart';

/// 手机地图底图：国内高德栅格 + 本地 `z/x/y.png` 缓存。
///
/// 官方 OSM 在国内经常超时，骑行详情会整片空白。高德是 GCJ-02，
/// 展示前用 [toDisplay] 把 GPS/GPX 的 WGS-84 转过去。
abstract class HelmOsmTiles {
  /// 高德矢量路网（国内 CDN）。
  static const urlTemplate =
      'https://wprd0{s}.is.autonavi.com/appmaptile?x={x}&y={y}&z={z}&lang=zh_cn&size=1&scl=1&style=7';

  static const subdomains = ['1', '2', '3', '4'];

  static const referer = 'https://www.amap.com/';

  /// [AppCacheStore] 下的子目录名。
  static const dirName = 'osm';

  static const _ua =
      '${AppKeys.appName}/1.0 (${AppKeys.androidPackage}; map tiles)';

  /// 底图是火星坐标。
  static const usesGcj02 = true;

  /// GPS / GPX → 地图显示。
  static LatLng toDisplay(LatLng wgs84) =>
      usesGcj02 ? ChinaCoord.wgs84ToGcj02(wgs84) : wgs84;

  /// 地图点选 → GPS / 路网。
  static LatLng fromDisplay(LatLng map) =>
      usesGcj02 ? ChinaCoord.gcj02ToWgs84(map) : map;

  /// 轨迹折线。
  static List<LatLng> toDisplayAll(Iterable<LatLng> pts) =>
      [for (final p in pts) toDisplay(p)];

  /// 直接拼一张瓦片 URL（城市下载用）。
  static String tileUrl(int z, int x, int y) {
    final s = subdomains[(x + y) % subdomains.length];
    return 'https://wprd0$s.is.autonavi.com/appmaptile'
        '?x=$x&y=$y&z=$z&lang=zh_cn&size=1&scl=1&style=7';
  }

  /// 在 [runApp] 之前调用。
  static Future<void> init() async {
    if (kIsWeb) return;
    await OsmFileCache.instance.init();
  }

  /// 各地图页共用的底图层。不要开 retina 模拟：会 4 倍请求且国内源不支持 @2x。
  static TileLayer layer({
    bool? retinaMode,
    int maxNativeZoom = 18,
    TileDisplay tileDisplay = const TileDisplay.fadeIn(),
  }) {
    return TileLayer(
      urlTemplate: urlTemplate,
      subdomains: subdomains,
      userAgentPackageName: AppKeys.androidPackage,
      retinaMode: false,
      maxNativeZoom: maxNativeZoom,
      maxZoom: 18,
      tileDisplay: tileDisplay,
      panBuffer: 2,
      keepBuffer: 2,
      tileProvider: NetworkTileProvider(
        silenceExceptions: true,
        headers: {
          'User-Agent': _ua,
          'Referer': referer,
        },
        cachingProvider: OsmFileCache.instance,
      ),
    );
  }
}

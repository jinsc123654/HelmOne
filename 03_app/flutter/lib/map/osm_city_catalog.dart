import 'package:latlong2/latlong.dart';
import 'package:sifli_companion/map/china_coord.dart';
import 'package:sifli_companion/map/osm_tile_math.dart';

/// 一座可手动下载的城市（包围盒为 WGS-84）。
class OsmCity {
  /// 创建城市。
  const OsmCity({
    required this.id,
    required this.name,
    required this.nameEn,
    required this.south,
    required this.west,
    required this.north,
    required this.east,
    this.minZoom = 11,
    this.maxZoom = 15,
  });

  /// 稳定 id。
  final String id;

  /// 中文名。
  final String name;

  /// 英文名。
  final String nameEn;

  /// 南纬。
  final double south;

  /// 西经。
  final double west;

  /// 北纬。
  final double north;

  /// 东经。
  final double east;

  /// 下载的最小层级。
  final int minZoom;

  /// 下载的最大层级。
  final int maxZoom;

  /// 西南角。
  LatLng get southWest => LatLng(south, west);

  /// 东北角。
  LatLng get northEast => LatLng(north, east);

  /// 高德瓦片按 GCJ-02 编号，下载范围要先转。
  LatLng get gcjSouthWest => ChinaCoord.wgs84ToGcj02(southWest);

  /// 东北角的 GCJ-02。
  LatLng get gcjNorthEast => ChinaCoord.wgs84ToGcj02(northEast);

  /// 预计张数。
  int get expectedTiles => OsmTileMath.countTiles(
        southWest: gcjSouthWest,
        northEast: gcjNorthEast,
        minZoom: minZoom,
        maxZoom: maxZoom,
      );

  /// 约 16 KB / 张。
  int get approxBytes => expectedTiles * 16 * 1024;

  /// 名称是否匹配搜索词。
  bool matches(String q) {
    final s = q.trim().toLowerCase();
    if (s.isEmpty) return true;
    return name.contains(q.trim()) || nameEn.toLowerCase().contains(s);
  }
}

/// 内置城市目录。包围盒取城区，避免把整个地级市下满。
abstract class OsmCityCatalog {
  static const cities = <OsmCity>[
    OsmCity(
      id: 'chuzhou',
      name: '滁州',
      nameEn: 'Chuzhou',
      south: 32.15,
      west: 118.15,
      north: 32.40,
      east: 118.48,
    ),
    OsmCity(
      id: 'nanjing',
      name: '南京',
      nameEn: 'Nanjing',
      south: 31.90,
      west: 118.60,
      north: 32.18,
      east: 119.05,
    ),
    OsmCity(
      id: 'hefei',
      name: '合肥',
      nameEn: 'Hefei',
      south: 31.72,
      west: 117.10,
      north: 32.00,
      east: 117.50,
    ),
    OsmCity(
      id: 'yangzhou',
      name: '扬州',
      nameEn: 'Yangzhou',
      south: 32.32,
      west: 119.30,
      north: 32.48,
      east: 119.55,
    ),
    OsmCity(
      id: 'zhenjiang',
      name: '镇江',
      nameEn: 'Zhenjiang',
      south: 32.10,
      west: 119.35,
      north: 32.28,
      east: 119.55,
    ),
    OsmCity(
      id: 'wuxi',
      name: '无锡',
      nameEn: 'Wuxi',
      south: 31.45,
      west: 120.15,
      north: 31.70,
      east: 120.45,
    ),
    OsmCity(
      id: 'suzhou',
      name: '苏州',
      nameEn: 'Suzhou',
      south: 31.20,
      west: 120.45,
      north: 31.45,
      east: 120.80,
    ),
    OsmCity(
      id: 'changzhou',
      name: '常州',
      nameEn: 'Changzhou',
      south: 31.70,
      west: 119.85,
      north: 31.90,
      east: 120.10,
    ),
    OsmCity(
      id: 'shanghai',
      name: '上海',
      nameEn: 'Shanghai',
      south: 31.05,
      west: 121.20,
      north: 31.45,
      east: 121.70,
    ),
    OsmCity(
      id: 'hangzhou',
      name: '杭州',
      nameEn: 'Hangzhou',
      south: 30.15,
      west: 120.00,
      north: 30.42,
      east: 120.35,
    ),
    OsmCity(
      id: 'beijing',
      name: '北京',
      nameEn: 'Beijing',
      south: 39.75,
      west: 116.20,
      north: 40.08,
      east: 116.60,
    ),
    OsmCity(
      id: 'tianjin',
      name: '天津',
      nameEn: 'Tianjin',
      south: 39.00,
      west: 117.05,
      north: 39.25,
      east: 117.35,
    ),
    OsmCity(
      id: 'guangzhou',
      name: '广州',
      nameEn: 'Guangzhou',
      south: 23.00,
      west: 113.15,
      north: 23.25,
      east: 113.50,
    ),
    OsmCity(
      id: 'shenzhen',
      name: '深圳',
      nameEn: 'Shenzhen',
      south: 22.48,
      west: 113.80,
      north: 22.72,
      east: 114.35,
    ),
    OsmCity(
      id: 'chengdu',
      name: '成都',
      nameEn: 'Chengdu',
      south: 30.55,
      west: 103.90,
      north: 30.80,
      east: 104.20,
    ),
    OsmCity(
      id: 'chongqing',
      name: '重庆',
      nameEn: 'Chongqing',
      south: 29.45,
      west: 106.40,
      north: 29.70,
      east: 106.70,
    ),
    OsmCity(
      id: 'wuhan',
      name: '武汉',
      nameEn: 'Wuhan',
      south: 30.45,
      west: 114.15,
      north: 30.72,
      east: 114.55,
    ),
    OsmCity(
      id: 'xian',
      name: '西安',
      nameEn: 'Xi\'an',
      south: 34.18,
      west: 108.80,
      north: 34.38,
      east: 109.08,
    ),
    OsmCity(
      id: 'qingdao',
      name: '青岛',
      nameEn: 'Qingdao',
      south: 36.00,
      west: 120.20,
      north: 36.20,
      east: 120.50,
    ),
    OsmCity(
      id: 'xiamen',
      name: '厦门',
      nameEn: 'Xiamen',
      south: 24.42,
      west: 118.04,
      north: 24.62,
      east: 118.20,
    ),
    OsmCity(
      id: 'wuhu',
      name: '芜湖',
      nameEn: 'Wuhu',
      south: 31.25,
      west: 118.25,
      north: 31.45,
      east: 118.50,
    ),
    OsmCity(
      id: 'maanshan',
      name: '马鞍山',
      nameEn: 'Maanshan',
      south: 31.62,
      west: 118.40,
      north: 31.78,
      east: 118.60,
    ),
  ];

  static OsmCity? byId(String id) {
    for (final c in cities) {
      if (c.id == id) return c;
    }
    return null;
  }
}

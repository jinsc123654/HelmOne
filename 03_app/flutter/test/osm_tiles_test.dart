import 'package:flutter_test/flutter_test.dart';
import 'package:latlong2/latlong.dart';
import 'package:sifli_companion/map/china_coord.dart';
import 'package:sifli_companion/map/osm_file_cache.dart';
import 'package:sifli_companion/map/osm_tile_math.dart';

void main() {
  test('WGS-84 to GCJ-02 shifts inside China, not outside', () {
    final nj = ChinaCoord.wgs84ToGcj02(const LatLng(32.06, 118.78));
    expect(nj.latitude, isNot(closeTo(32.06, 0.0001)));
    expect(nj.longitude, isNot(closeTo(118.78, 0.0001)));

    final nyc = ChinaCoord.wgs84ToGcj02(const LatLng(40.71, -74.01));
    expect(nyc.latitude, closeTo(40.71, 1e-9));
    expect(nyc.longitude, closeTo(-74.01, 1e-9));
  });

  test('GCJ-02 round trip stays close', () {
    const wgs = LatLng(32.258, 118.277);
    final gcj = ChinaCoord.wgs84ToGcj02(wgs);
    final back = ChinaCoord.gcj02ToWgs84(gcj);
    expect(back.latitude, closeTo(wgs.latitude, 1e-5));
    expect(back.longitude, closeTo(wgs.longitude, 1e-5));
  });

  test('tile count for a small bbox is positive', () {
    final n = OsmTileMath.countTiles(
      southWest: const LatLng(32.20, 118.25),
      northEast: const LatLng(32.30, 118.35),
      minZoom: 12,
      maxZoom: 14,
    );
    expect(n, greaterThan(10));
    expect(n, equals(
      OsmTileMath.tilesInBounds(
        southWest: const LatLng(32.20, 118.25),
        northEast: const LatLng(32.30, 118.35),
        minZoom: 12,
        maxZoom: 14,
      ).length,
    ));
  });

  test('parse Gaode and OSM tile URLs', () {
    expect(
      OsmFileCache.parseXyz(
        'https://wprd01.is.autonavi.com/appmaptile?x=6823&y=3356&z=13&lang=zh_cn',
      ),
      (13, 6823, 3356),
    );
    expect(
      OsmFileCache.parseXyz('https://osm.open.cn/13/6823/3356.png'),
      (13, 6823, 3356),
    );
  });
}

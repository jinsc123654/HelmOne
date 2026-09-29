import 'package:flutter_test/flutter_test.dart';
import 'package:latlong2/latlong.dart';
import 'package:sifli_companion/ride/road_router.dart';

void main() {
  test('parse OSRM nearest and reject far snaps', () {
    final tap = LatLng(32.25828, 118.27734);
    const json = {
      'code': 'Ok',
      'waypoints': [
        {
          'location': [118.277929, 32.256368],
          'distance': 50.0,
          'name': '览山路',
        },
      ],
    };
    final p = RoadRouter.parseOsrmNearest(json, tap);
    expect(p, isNotNull);
    expect(p!.longitude, closeTo(118.277929, 1e-6));

    const far = {
      'code': 'Ok',
      'waypoints': [
        {
          'location': [118.277929, 32.256368],
          'distance': 500.0,
        },
      ],
    };
    expect(RoadRouter.parseOsrmNearest(far, tap), isNull);
  });

  test('parse BRouter geojson line', () {
    const json = {
      'type': 'FeatureCollection',
      'features': [
        {
          'geometry': {
            'type': 'LineString',
            'coordinates': [
              [118.2779, 32.2563],
              [118.2785, 32.2564],
              [118.2790, 32.2565],
            ],
          },
        },
      ],
    };
    final pts = RoadRouter.parseBrouterGeoJson(json);
    expect(pts.length, 3);
    expect(pts.first.latitude, closeTo(32.2563, 1e-6));
    expect(pts.last.longitude, closeTo(118.2790, 1e-6));
  });
}

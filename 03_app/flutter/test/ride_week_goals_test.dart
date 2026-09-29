import 'package:flutter_test/flutter_test.dart';
import 'package:latlong2/latlong.dart';
import 'package:sifli_companion/ride/ride_record_store.dart';
import 'package:sifli_companion/ride/ride_week_goals.dart';

void main() {
  test('week goals clamp and parse hours', () {
    expect(
      const RideWeekGoals(rides: 0, minutes: 1, km: 0).clamped(),
      isA<RideWeekGoals>()
          .having((g) => g.rides, 'rides', 1)
          .having((g) => g.minutes, 'minutes', 15)
          .having((g) => g.km, 'km', 1),
    );
    expect(RideWeekGoals.minutesFromHoursText('5'), 300);
    expect(RideWeekGoals.minutesFromHoursText('1.5'), 90);
    expect(RideWeekGoals.minutesFromHoursText('bad', fallbackMinutes: 180), 180);
    expect(RideWeekGoals.progress(2, 4), 0.5);
    expect(RideWeekGoals.progress(10, 4), 1.0);
    expect(RideWeekGoals.formatMinutes(300), '5h');
    expect(RideWeekGoals.formatMinutes(80), '1:20');
    expect(RideWeekGoals.formatSeconds(4800), '1:20');
  });

  test('ride record keeps duration in json', () {
    final rec = RideRecord(
      fileName: 'a.gpx',
      localPath: 'a.gpx',
      size: 1,
      mtime: 1,
      durationSec: 3600,
      distanceKm: 12.5,
    );
    final copy = RideRecord.fromJson(rec.toJson());
    expect(copy.durationSec, 3600);
    expect(copy.distanceKm, 12.5);
    expect(copy.copyWith(durationSec: 10).durationSec, 10);
  });

  test('ride record keeps track preview in json', () {
    final rec = RideRecord(
      fileName: 'a.gpx',
      localPath: 'a.gpx',
      size: 1,
      mtime: 1,
      preview: const [LatLng(32, 118), LatLng(32.1, 118.1)],
    );
    final copy = RideRecord.fromJson(rec.toJson());
    expect(copy.preview, hasLength(2));
    expect(copy.preview!.first.latitude, 32);
    expect(copy.preview!.last.longitude, 118.1);
  });
}

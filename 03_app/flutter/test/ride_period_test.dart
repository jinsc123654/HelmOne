import 'package:flutter_test/flutter_test.dart';
import 'package:sifli_companion/ride/ride_record_store.dart';

void main() {
  RideRecord rec(DateTime t) => RideRecord(
        fileName: 'a.gpx',
        localPath: 'a.gpx',
        size: 1,
        mtime: t.millisecondsSinceEpoch ~/ 1000,
      );

  test('week is Monday through now, month is calendar month', () {
    final now = DateTime(2026, 9, 9, 21);
    expect(
      RidePeriodFilter.matches(rec(DateTime(2026, 9, 7)), RidePeriod.week, now),
      isTrue,
    );
    expect(
      RidePeriodFilter.matches(
        rec(DateTime(2026, 9, 6, 23)),
        RidePeriod.week,
        now,
      ),
      isFalse,
    );
    expect(
      RidePeriodFilter.matches(rec(DateTime(2026, 9, 1)), RidePeriod.month, now),
      isTrue,
    );
    expect(
      RidePeriodFilter.matches(
        rec(DateTime(2026, 8, 31)),
        RidePeriod.month,
        now,
      ),
      isFalse,
    );
    expect(
      RidePeriodFilter.matches(rec(DateTime(2025, 1, 1)), RidePeriod.all, now),
      isTrue,
    );
  });
}

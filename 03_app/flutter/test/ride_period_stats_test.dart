import 'package:flutter_test/flutter_test.dart';
import 'package:sifli_companion/ride/ride_period_stats.dart';
import 'package:sifli_companion/ride/ride_record_store.dart';

/// 造一条记录：只关心统计用得到的字段（mtime / 里程 / 时长）。
RideRecord rec(int mtime, {double? km, int? sec}) => RideRecord(
      fileName: 'r$mtime.gpx',
      localPath: '/tmp/r$mtime.gpx',
      size: 100,
      mtime: mtime,
      distanceKm: km,
      durationSec: sec,
    );

int unix(DateTime t) => t.millisecondsSinceEpoch ~/ 1000;

void main() {
  group('区间边界', () {
    test('一周从周一开始，共 7 天', () {
      // 2026-09-17 是周四。
      final r = RidePeriodStats.range(RidePeriod.week, DateTime(2026, 9, 17))!;
      expect(r.start, DateTime(2026, 9, 14)); // 周一
      expect(r.days, 7);
      expect(r.end, DateTime(2026, 9, 21));
      expect(r.contains(DateTime(2026, 9, 20, 23, 59)), isTrue);
      expect(r.contains(DateTime(2026, 9, 21)), isFalse); // 含头不含尾
    });

    test('月的边界：12 月要跨到次年 1 月', () {
      final dec = RidePeriodStats.range(RidePeriod.month, DateTime(2026, 12, 5))!;
      expect(dec.start, DateTime(2026, 12, 1));
      expect(dec.end, DateTime(2027, 1, 1));
      expect(dec.days, 31);
    });

    test('2 月按实际天数', () {
      final feb = RidePeriodStats.range(RidePeriod.month, DateTime(2028, 2, 9))!;
      expect(feb.days, 29); // 2028 是闰年
    });

    test('全部区间没有范围', () {
      expect(RidePeriodStats.range(RidePeriod.all, DateTime(2026, 9, 17)), isNull);
    });
  });

  group('前后翻页', () {
    test('往前一个月从月首挪，不会跳过月份', () {
      final a = RidePeriodStats.shift(RidePeriod.month, DateTime(2026, 1, 31), -1);
      expect(a, DateTime(2025, 12, 1));
    });

    test('往后一周 / 往后一月', () {
      expect(
        RidePeriodStats.shift(RidePeriod.week, DateTime(2026, 9, 14), 1),
        DateTime(2026, 9, 21),
      );
      expect(
        RidePeriodStats.shift(RidePeriod.month, DateTime(2026, 12, 1), 1),
        DateTime(2027, 1, 1),
      );
    });

    test('本周不能再往后，上周可以', () {
      final now = DateTime.now();
      expect(RidePeriodStats.canForward(RidePeriod.week, now), isFalse);
      expect(
        RidePeriodStats.canForward(
          RidePeriod.week,
          now.subtract(const Duration(days: 7)),
        ),
        isTrue,
      );
    });
  });

  group('逐日聚合', () {
    final range = RideRange(DateTime(2026, 9, 14), DateTime(2026, 9, 21));

    test('按天分格，区间外的记录不算', () {
      final items = [
        rec(unix(DateTime(2026, 9, 14, 8)), km: 20, sec: 3600), // 周一
        rec(unix(DateTime(2026, 9, 14, 18)), km: 10, sec: 1800), // 周一（第二次）
        rec(unix(DateTime(2026, 9, 18, 9)), km: 5, sec: 900), // 周五
        rec(unix(DateTime(2026, 9, 13, 9)), km: 99, sec: 9999), // 区间外
        rec(0, km: 1, sec: 1), // 没有时间戳
      ];
      final b = RidePeriodStats.buckets(items, range);
      expect(b.length, 7);
      expect(b[0].rides, 2);
      expect(b[0].km, closeTo(30, 0.001));
      expect(b[0].durationSec, 5400);
      expect(b[4].rides, 1);
      expect(b[4].km, closeTo(5, 0.001));
      expect(b[6].isEmpty, isTrue); // 周日没骑

      final t = RidePeriodStats.total(b);
      expect(t.rides, 3);
      expect(t.km, closeTo(35, 0.001));
      expect(t.durationSec, 6300);
    });

    test('列表按区间过滤，全部区间不过滤', () {
      final items = [
        rec(unix(DateTime(2026, 9, 14, 8))),
        rec(unix(DateTime(2026, 9, 13, 8))),
      ];
      expect(RidePeriodStats.inRange(items, range).length, 1);
      expect(RidePeriodStats.inRange(items, null).length, 2);
    });

    test('月视图按自然周分格：4–6 格，首尾残周不漏日子', () {
      // 2026-08-01 是周六 → 第一格只有 1–2 号，之后每个周一开一格。
      final r = RideRange(DateTime(2026, 8, 1), DateTime(2026, 9, 1));
      final grid = RidePeriodStats.slots(r, span: RideBucketSpan.week);
      expect(grid.length, 6);
      expect(grid.first.$1, DateTime(2026, 8, 1));
      expect(grid.first.$2, DateTime(2026, 8, 3)); // 1–2
      expect(grid[1].$1, DateTime(2026, 8, 3)); // 周一
      expect(grid[1].$2, DateTime(2026, 8, 10));
      expect(grid.last.$1, DateTime(2026, 8, 31)); // 31 号那格
      expect(grid.last.$2, DateTime(2026, 9, 1));
      // 一格接一格，没有缝也没有叠
      for (var i = 1; i < grid.length; i++) {
        expect(grid[i].$1, grid[i - 1].$2);
      }
    });

    test('月视图的格子能装下跨周的骑行', () {
      final r = RideRange(DateTime(2026, 8, 1), DateTime(2026, 9, 1));
      final items = [
        rec(unix(DateTime(2026, 8, 2, 9)), km: 10, sec: 1800), // 残周 1–2
        rec(unix(DateTime(2026, 8, 5, 9)), km: 20, sec: 3600), // 第 2 格
        rec(unix(DateTime(2026, 8, 6, 9)), km: 5, sec: 900), // 第 2 格
        rec(unix(DateTime(2026, 8, 9, 9)), km: 1, sec: 600), // 第 2 格（周日）
        rec(unix(DateTime(2026, 8, 10, 9)), km: 30, sec: 5400), // 第 3 格（周一）
      ];
      final b = RidePeriodStats.buckets(
        items,
        r,
        span: RideBucketSpan.week,
      );
      expect(b.length, 6);
      expect(b[0].rides, 1);
      expect(b[0].km, closeTo(10, 0.001));
      expect(b[1].rides, 3);
      expect(b[1].km, closeTo(26, 0.001));
      expect(b[2].rides, 1);
      expect(b[2].km, closeTo(30, 0.001));
      expect(RidePeriodStats.total(b).rides, 5);
    });

    test('「全部」区间：从最早记录所在月首，到今天所在月尾', () {
      final items = [
        rec(unix(DateTime(2026, 5, 20, 9))),
        rec(unix(DateTime(2026, 8, 3, 9))),
      ];
      final r = RidePeriodStats.everything(items)!;
      expect(r.start, DateTime(2026, 5, 1));
      final now = DateTime.now();
      expect(r.end.isAfter(DateTime(now.year, now.month, 1)), isTrue);

      final grid = RidePeriodStats.slots(r, span: RideBucketSpan.month);
      expect(grid.length, greaterThanOrEqualTo(4)); // 5、6、7、8… 月
      expect(grid.first.$1, DateTime(2026, 5, 1));
      expect(grid[1].$1, DateTime(2026, 6, 1));
      for (var i = 1; i < grid.length; i++) {
        expect(grid[i].$1, grid[i - 1].$2); // 一格接一格
      }
    });

    test('没有记录（或记录没有时间）时「全部」没有区间', () {
      expect(RidePeriodStats.everything(const []), isNull);
      expect(RidePeriodStats.everything([rec(0)]), isNull);
    });

    test('按月分格：跨月的记录各归各的月', () {
      final r = RideRange(DateTime(2026, 5, 1), DateTime(2026, 8, 1));
      final items = [
        rec(unix(DateTime(2026, 5, 31, 23)), km: 10, sec: 600),
        rec(unix(DateTime(2026, 6, 1, 0, 10)), km: 5, sec: 300),
      ];
      final b = RidePeriodStats.buckets(items, r, span: RideBucketSpan.month);
      expect(b.length, 3);
      expect(b[0].rides, 1);
      expect(b[0].km, closeTo(10, 0.001));
      expect(b[1].rides, 1);
      expect(b[2].isEmpty, isTrue);
      expect(RidePeriodStats.total(b).km, closeTo(15, 0.001));
    });

    test('今天在区间里的下标：本周能找到，历史区间为 -1', () {
      final now = DateTime.now();
      final thisWeek = RidePeriodStats.range(RidePeriod.week, now)!;
      final idx = thisWeek.todayIndex();
      expect(idx, inInclusiveRange(0, 6));
      expect(thisWeek.start.add(Duration(days: idx)).day, now.day);

      final old = RideRange(DateTime(2000, 1, 3), DateTime(2000, 1, 10));
      expect(old.todayIndex(), -1);
    });
  });
}

import 'package:sifli_companion/ride/ride_record_store.dart';

/// 一个统计区间：本地时间的「日」边界，含头不含尾。
///
/// 和 [RidePeriodFilter] 的区别：那一套只认「现在」（本周/本月），这里带锚点，
/// 可以看任意一周、任意一月。
class RideRange {
  /// 创建区间。
  const RideRange(this.start, this.end);

  /// 起（含）。
  final DateTime start;

  /// 止（不含）。
  final DateTime end;

  /// 区间里是不是有今天 —— 也就是「本周 / 本月」。
  bool get isCurrent {
    final now = DateTime.now();
    return !now.isBefore(start) && now.isBefore(end);
  }

  /// 区间天数：周 7 天，月 28–31 天。
  int get days => end.difference(start).inDays;

  /// [t] 是否落在区间内。
  bool contains(DateTime t) => !t.isBefore(start) && t.isBefore(end);

  /// 记录时间（Unix 秒）是否落在区间内。`mtime <= 0` 的记录一律不算。
  bool containsUnix(int unixSec) =>
      unixSec > 0 &&
      contains(DateTime.fromMillisecondsSinceEpoch(unixSec * 1000));

  /// 今天在区间里的下标（不在区间里返回 -1）：柱状图高亮用。
  int todayIndex() {
    final now = DateTime.now();
    final i = DateTime(now.year, now.month, now.day).difference(start).inDays;
    return (i >= 0 && i < days) ? i : -1;
  }
}

/// 柱子怎么分格。
///
/// 周视图一格一天（一…日 7 格，看着就是"这一周"）；
/// 月视图一格一周 —— 31 根柱子既看不清也没法比，按自然周切正好 4–6 格，
/// 而且和「本周」是同一套周界，两个视图的数字能对上。
enum RideBucketSpan { day, week, month }

/// 区间里**一格**的合计。
class RideBucket {
  /// 创建一格。
  const RideBucket({this.rides = 0, this.km = 0, this.durationSec = 0});

  /// 骑行次数。
  final int rides;

  /// 里程（km）。
  final double km;

  /// 时长（秒）。
  final int durationSec;

  /// 这一天没骑。
  bool get isEmpty => rides == 0;
}

/// 周 / 月的区间计算和逐日聚合。
abstract class RidePeriodStats {
  /// 周一算一周的第一天（和 [RidePeriodFilter.weekStart] 保持一致）。
  static DateTime weekStart(DateTime d) {
    final day = DateTime(d.year, d.month, d.day);
    return day.subtract(Duration(days: day.weekday - 1));
  }

  /// 当月一号。
  static DateTime monthStart(DateTime d) => DateTime(d.year, d.month, 1);

  /// 下月一号。
  static DateTime nextMonthStart(DateTime d) =>
      d.month == 12 ? DateTime(d.year + 1, 1, 1) : DateTime(d.year, d.month + 1, 1);

  /// 「全部」的区间：从最早那条记录所在的月首，到最晚那条（含今天）所在月的月末。
  ///
  /// 一条记录都没有时返回 null —— 界面据此走空态，而不是画一张空表。
  static RideRange? everything(List<RideRecord> items) {
    final now = DateTime.now();
    DateTime? first;
    DateTime? last;
    for (final r in items) {
      if (r.mtime <= 0) continue;
      final t = DateTime.fromMillisecondsSinceEpoch(r.mtime * 1000);
      if (t.isAfter(now)) continue; // 时钟异常的记录不参与划定范围
      final s = monthStart(t);
      if (first == null || s.isBefore(first)) first = s;
      if (last == null || s.isAfter(last)) last = s;
    }
    if (first == null || last == null) {
      return null;
      }
    final thisMonth = monthStart(now);
    if (last.isBefore(thisMonth)) last = thisMonth;
    return RideRange(first, nextMonthStart(last));
  }

  /// 锚点所在的区间。[RidePeriod.all] 没有区间（返回 null，表示不过滤）。
  static RideRange? range(RidePeriod period, DateTime anchor) {
    switch (period) {
      case RidePeriod.week:
        final s = weekStart(anchor);
        return RideRange(s, s.add(const Duration(days: 7)));
      case RidePeriod.month:
        final s = monthStart(anchor);
        return RideRange(s, nextMonthStart(s));
      case RidePeriod.all:
        return null;
    }
  }

  /// 往前（[delta] 为负）或往后挪一个周期。[RidePeriod.all] 不动。
  static DateTime shift(RidePeriod period, DateTime anchor, int delta) {
    switch (period) {
      case RidePeriod.week:
        return anchor.add(Duration(days: 7 * delta));
      case RidePeriod.month:
        // 从月首挪，避免「1/31 往前一个月」落到上上个月这类越界。
        final s = monthStart(anchor);
        return DateTime(s.year, s.month + delta, 1);
      case RidePeriod.all:
        return anchor;
    }
  }

  /// 还能不能往后挪：下一个区间不能落在今天之后（不看未来）。
  static bool canForward(RidePeriod period, DateTime anchor) {
    final next = range(period, shift(period, anchor, 1));
    if (next == null) return false;
    final now = DateTime.now();
    return !next.start.isAfter(DateTime(now.year, now.month, now.day));
  }

  /// 每格的时间范围 `[起, 止)`，下标和 [buckets] 一一对应。
  ///
  /// 按天时就是每一天；按周时从区间开头起、之后每个周一开一格 —— 首尾可能是
  /// **残周**（比如 8 月 1 日是周六，第一格就只有 1–2 号），但一天都不会漏，
  /// 标签上也会写清楚是哪几天。
  static List<(DateTime, DateTime)> slots(
    RideRange range, {
    RideBucketSpan span = RideBucketSpan.day,
  }) {
    if (span == RideBucketSpan.day) {
      return [
        for (var i = 0; i < range.days; i++)
          (
            range.start.add(Duration(days: i)),
            range.start.add(Duration(days: i + 1)),
          ),
      ];
    }
    final out = <(DateTime, DateTime)>[];
    var cur = range.start;
    if (span == RideBucketSpan.month) {
      // 月：一格一个自然月（区间起点本身就是月首，见 everything()）。
      while (cur.isBefore(range.end)) {
        var end = nextMonthStart(cur);
        if (end.isAfter(range.end)) end = range.end;
        out.add((cur, end));
        cur = end;
      }
      return out;
    }
    while (cur.isBefore(range.end)) {
      var end = cur.add(Duration(days: 8 - cur.weekday)); // 下一个周一
      if (end.isAfter(range.end)) end = range.end;
      out.add((cur, end));
      cur = end;
    }
    return out;
  }

  /// 区间内的分格合计，下标与 [slots] 对齐。
  static List<RideBucket> buckets(
    List<RideRecord> items,
    RideRange range, {
    RideBucketSpan span = RideBucketSpan.day,
  }) {
    final grid = slots(range, span: span);
    final n = grid.length;
    final rides = List<int>.filled(n, 0);
    final km = List<double>.filled(n, 0);
    final secs = List<int>.filled(n, 0);

    for (final r in items) {
      if (!range.containsUnix(r.mtime)) continue;
      final t = DateTime.fromMillisecondsSinceEpoch(r.mtime * 1000);
      final i = slotIndex(grid, t);
      if (i < 0) continue;
      rides[i]++;
      km[i] += r.distanceKm ?? 0;
      secs[i] += r.durationSec ?? 0;
    }

    return [
      for (var i = 0; i < n; i++)
        RideBucket(rides: rides[i], km: km[i], durationSec: secs[i]),
    ];
  }

  /// [t] 落在第几格；不在任何格里返回 -1。
  static int slotIndex(List<(DateTime, DateTime)> grid, DateTime t) {
    for (var i = 0; i < grid.length; i++) {
      if (!t.isBefore(grid[i].$1) && t.isBefore(grid[i].$2)) return i;
    }
    return -1;
  }

  /// 今天落在第几格（高亮用）；不在区间里返回 -1。
  static int slotOfToday(List<(DateTime, DateTime)> grid) {
    final now = DateTime.now();
    return slotIndex(grid, DateTime(now.year, now.month, now.day));
  }

  /// 区间合计。传空表返回全 0。
  static RideBucket total(List<RideBucket> buckets) {
    var rides = 0;
    var km = 0.0;
    var secs = 0;
    for (final b in buckets) {
      rides += b.rides;
      km += b.km;
      secs += b.durationSec;
    }
    return RideBucket(rides: rides, km: km, durationSec: secs);
  }

  /// 区间内的记录（列表用）。[range] 为 null 表示「全部」，原样返回。
  static List<RideRecord> inRange(List<RideRecord> items, RideRange? range) {
    if (range == null) return items;
    return [for (final r in items) if (range.containsUnix(r.mtime)) r];
  }
}

import 'package:flutter/material.dart';
import 'package:get/get.dart';
import 'package:sifli_companion/app/app_theme.dart';
import 'package:sifli_companion/i18n/locale_keys.dart';
import 'package:sifli_companion/ride/ride_period_stats.dart';
import 'package:sifli_companion/ride/ride_week_goals.dart';
import 'package:sifli_companion/ui/ride/ride_charts.dart';
import 'package:sifli_companion/ui/widgets/helm_chrome.dart';

/// 一格的名字。
///
/// day → `周一 8/17`；week → `3–9`（首尾残周就是 `1–2`、`31`）；month → `2026/8`。
String rideSlotLabel((DateTime, DateTime) slot, RideBucketSpan span) {
  final a = slot.$1;
  switch (span) {
    case RideBucketSpan.day:
      // 只有 7 根柱子，标星期几就够；带上日期反而挤。
      return _wd(a.weekday).tr;
    case RideBucketSpan.week:
      final b = slot.$2.subtract(const Duration(days: 1));
      return a.day == b.day ? '${a.day}' : '${a.day}–${b.day}';
    case RideBucketSpan.month:
      return '${a.year}/${a.month}';
  }
}

/// 图表下方标签：天 = 一…日；周 = `3–9`；月 = 只标季度首月（年）和最后一个月。
///
/// 月可能横跨好几年，全标出来会叠成一团；短区间（≤8 个月）仍然每个都标。
List<String> rideChartLabels(
  List<(DateTime, DateTime)> grid,
  RideBucketSpan span,
) {
  switch (span) {
    case RideBucketSpan.day:
      return [for (final slot in grid) _wd(slot.$1.weekday).tr];
    case RideBucketSpan.week:
      return [
        for (final slot in grid)
          () {
            final a = slot.$1;
            final b = slot.$2.subtract(const Duration(days: 1));
            return a.day == b.day ? '${a.day}' : '${a.day}–${b.day}';
          }(),
      ];
    case RideBucketSpan.month:
      return [
        for (var i = 0; i < grid.length; i++)
          () {
            final a = grid[i].$1;
            final isLast = i == grid.length - 1;
            if (grid.length > 8 && a.month != 1 && !isLast) return '';
            return a.month == 1 ? '${a.year}' : '${a.year}/${a.month}';
          }(),
      ];
  }
}

String _wd(int weekday) {
  const keys = [
    LocaleKeys.rideWd1,
    LocaleKeys.rideWd2,
    LocaleKeys.rideWd3,
    LocaleKeys.rideWd4,
    LocaleKeys.rideWd5,
    LocaleKeys.rideWd6,
    LocaleKeys.rideWd7,
  ];
  return keys[weekday - 1];
}

/// 摘要卡：三个彩色大数字（里程 / 时长 / 次数），下面一行小字给日均之类的口径。
///
/// 排版照健康类 App 的做法来：数字大、带色点、单位小一号，一眼扫得出来。
class RideStatSummaryCard extends StatelessWidget {
  /// 创建摘要卡。
  const RideStatSummaryCard({
    super.key,
    required this.km,
    required this.durationSec,
    required this.rides,
    this.caption,
  });

  /// 里程（km）。
  final double km;

  /// 时长（秒）。
  final int durationSec;

  /// 次数。
  final int rides;

  /// 底部小字（如 `日均 78.1 km · 5:17`）。
  final String? caption;

  @override
  Widget build(BuildContext context) {
    return HelmCard(
      child: Padding(
        padding: const EdgeInsets.fromLTRB(18, 16, 18, 14),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                _stat(
                  label: LocaleKeys.rideStatKm.tr,
                  value: km,
                  format: (v) => v.toStringAsFixed(1),
                  unit: 'km',
                  color: AppTheme.ringCyan,
                ),
                _stat(
                  label: LocaleKeys.rideStatTime.tr,
                  value: durationSec.toDouble(),
                  format: (v) => RideWeekGoals.formatSeconds(v.round()),
                  unit: '',
                  color: AppTheme.ringLime,
                ),
                _stat(
                  label: LocaleKeys.rideStatRides.tr,
                  value: rides.toDouble(),
                  format: (v) => '${v.round()}',
                  unit: '',
                  color: AppTheme.ringOrange,
                ),
              ],
            ),
            if (caption != null && caption!.isNotEmpty) ...[
              const SizedBox(height: 12),
              Text(
                caption!,
                style: const TextStyle(color: AppTheme.muted, fontSize: 12),
              ),
            ],
          ],
        ),
      ),
    );
  }

  Widget _stat({
    required String label,
    required double value,
    required String Function(double) format,
    required String unit,
    required Color color,
  }) {
    return Expanded(
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              Container(
                width: 7,
                height: 7,
                decoration: BoxDecoration(color: color, shape: BoxShape.circle),
              ),
              const SizedBox(width: 6),
              Text(
                label,
                style: const TextStyle(color: AppTheme.muted, fontSize: 12),
              ),
            ],
          ),
          const SizedBox(height: 6),
          // 大数字：从 0 滚到实际值（健康 App 那种"数字在长"），
          // 装不下（1234.5 km）时等比缩小，不用省略号 —— 数字截断了没意义。
          TweenAnimationBuilder<double>(
            tween: Tween<double>(begin: 0, end: value),
            duration: const Duration(milliseconds: 720),
            curve: Curves.easeOutCubic,
            builder: (context, v, _) => FittedBox(
              fit: BoxFit.scaleDown,
              alignment: Alignment.centerLeft,
              child: Text.rich(
                TextSpan(
                  text: format(v),
                style: const TextStyle(
                  color: Colors.white,
                  fontSize: 24,
                  fontWeight: FontWeight.w700,
                  height: 1.05,
                  fontFeatures: [FontFeature.tabularFigures()],
                ),
                  children: [
                    if (unit.isNotEmpty)
                      TextSpan(
                        text: ' $unit',
                        style: const TextStyle(
                          color: AppTheme.muted,
                          fontSize: 12,
                          fontWeight: FontWeight.w500,
                        ),
                      ),
                  ],
                ),
                maxLines: 1,
              ),
            ),
          ),
        ],
      ),
    );
  }
}

/// 柱状图卡：标题 + 右侧一行口径（合计 · 日均）+ 从 0 长出来的柱子。
///
/// 柱子长出来这件事是**每次重建都重放**的（切周 / 切月 / 翻月都会重放），
/// 比数字直接跳出来柔和得多。
class RideStatChartCard extends StatelessWidget {
  /// 创建柱状图卡。
  const RideStatChartCard({
    super.key,
    required this.title,
    required this.subtitle,
    required this.values,
    required this.labels,
    required this.color,
    this.avg,
    this.avgText,
    this.highlight = -1,
    this.chartHeight = 124,
  });

  /// 图题（`每周里程`）。
  final String title;

  /// 右侧一行小字（`合计 468.3 km · 日均 78.1 km`）。
  final String subtitle;

  /// 每根柱子的值。
  final List<double> values;

  /// 柱子下方标签。
  final List<String> labels;

  /// 柱色。
  final Color color;

  /// 虚线均值。
  final double? avg;

  /// 虚线右端的数值标签。
  final String? avgText;

  /// 高亮哪根（今天 / 本月）；-1 不高亮。
  final int highlight;

  /// 柱子区域高度。周视图只有 7 根柱子，矮一些更不占地方。
  final double chartHeight;

  @override
  Widget build(BuildContext context) {
    return HelmCard(
      child: Padding(
        padding: const EdgeInsets.fromLTRB(16, 14, 16, 6),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text(
              title,
              style: const TextStyle(
                color: Colors.white,
                fontSize: 13,
                fontWeight: FontWeight.w600,
              ),
            ),
            if (subtitle.isNotEmpty) ...[
              const SizedBox(height: 3),
              // 口径单独一行：和标题挤一行时，窄屏上会被截成「日均 2.…」。
              Text(
                subtitle,
                style: const TextStyle(color: AppTheme.muted, fontSize: 12),
              ),
            ],
            const SizedBox(height: 5),
            TweenAnimationBuilder<double>(
              tween: Tween(begin: 0, end: 1),
              duration: const Duration(milliseconds: 640),
              curve: Curves.easeOutCubic,
              builder: (context, t, _) => SizedBox(
                height: chartHeight,
                child: RideBarChart(
                  values: [for (final v in values) v * t],
                  labels: labels,
                  color: color,
                  avg: avg == null ? null : avg! * t,
                  avgText: avgText,
                  highlight: highlight,
                ),
              ),
            ),
          ],
        ),
      ),
    );
  }
}

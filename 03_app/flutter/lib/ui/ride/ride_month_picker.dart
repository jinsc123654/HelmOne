import 'package:flutter/material.dart';
import 'package:get/get.dart';
import 'package:sifli_companion/app/app_theme.dart';
import 'package:sifli_companion/i18n/locale_keys.dart';
import 'package:sifli_companion/ride/ride_period_stats.dart';
import 'package:sifli_companion/ride/ride_record_store.dart';
import 'package:sifli_companion/ride/ride_week_goals.dart';

/// 月份选择面板：`◀ 2026 ▶` + 12 张「这个月骑了多少」的历史牌。
///
/// 不是一排光秃秃的 1–12：月视图要往回找一次骑行，用户想知道的是**哪个月有东西**。
/// 所以每张牌上就是那个月的大致记录（里程 / 次数 / 时长），一次都没骑的月份写
/// 「无数据」，选中的那张加一圈强调色。年份行下面给全年合计，翻年时当作一眼的对照。
/// 只有未来（[latest] 之后）的牌置灰 —— 过去的月份随便点，没数据就写「无数据」。
///
/// 返回选中的月份（当月 1 号）；点空白处关掉返回 null。
Future<DateTime?> showRideMonthPicker(
  BuildContext context, {
  required DateTime selected,
  required DateTime latest,
  required List<RideRecord> rides,
}) {
  return showModalBottomSheet<DateTime>(
    context: context,
    isScrollControlled: true,
    useSafeArea: true,
    backgroundColor: AppTheme.card,
    barrierColor: Colors.black54,
    builder: (_) => _MonthPickerSheet(
      selected: selected,
      latest: latest,
      rides: rides,
    ),
  );
}

class _MonthPickerSheet extends StatefulWidget {
  const _MonthPickerSheet({
    required this.selected,
    required this.latest,
    required this.rides,
  });

  /// 当前正在看的月份（进面板时高亮它）。
  final DateTime selected;

  /// 最晚能选到哪个月（含）—— 不给看未来。
  final DateTime latest;

  /// 记录库：用来给每个月算合计。
  final List<RideRecord> rides;

  @override
  State<_MonthPickerSheet> createState() => _MonthPickerSheetState();
}

class _MonthPickerSheetState extends State<_MonthPickerSheet> {
  late int _year = widget.selected.year;

  /// 最早那条记录所在的年份：再往前的年份一眼看去全是空的，箭头就不给了。
  late final int _firstYear = _ridesFirstYear();

  int _ridesFirstYear() {
    var year = widget.latest.year;
    for (final r in widget.rides) {
      if (r.mtime <= 0) continue;
      final y = DateTime.fromMillisecondsSinceEpoch(r.mtime * 1000).year;
      if (y < year) year = y;
    }
    return year;
  }

  /// 某个月的大致记录：里程（km）、次数、时长（秒）。
  ({double km, int rides, int sec}) _sum(int month) {
    final range = RidePeriodStats.range(
      RidePeriod.month,
      DateTime(_year, month),
    );
    var km = 0.0;
    var sec = 0;
    var n = 0;
    if (range != null) {
      for (final r in RidePeriodStats.inRange(widget.rides, range)) {
        km += r.distanceKm ?? 0;
        sec += r.durationSec ?? 0;
        n++;
      }
    }
    return (km: km, rides: n, sec: sec);
  }

  /// 这一格能不能选：只看未来。过去的月份都能点，没骑过就写「无数据」。
  bool _pickable(DateTime month) => !month.isAfter(
        DateTime(widget.latest.year, widget.latest.month),
      );

  bool get _canYearBack => _year > _firstYear;

  bool get _canYearForward => _year < widget.latest.year;

  @override
  Widget build(BuildContext context) {
    return SingleChildScrollView(
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          Container(
            width: 36,
            height: 4,
            margin: const EdgeInsets.only(top: 10),
            decoration: BoxDecoration(
              color: AppTheme.muted.withValues(alpha: 0.35),
              borderRadius: BorderRadius.circular(2),
            ),
          ),
          _header(),
          Padding(
            padding: const EdgeInsets.fromLTRB(16, 6, 16, 16),
            child: GridView.count(
              crossAxisCount: 2,
              shrinkWrap: true,
              physics: const NeverScrollableScrollPhysics(),
              mainAxisSpacing: 10,
              crossAxisSpacing: 10,
              childAspectRatio: 2,
              children: [for (var m = 1; m <= 12; m++) _card(m)],
            ),
          ),
        ],
      ),
    );
  }

  Widget _header() {
    var km = 0.0;
    var sec = 0;
    var n = 0;
    for (var m = 1; m <= 12; m++) {
      final s = _sum(m);
      km += s.km;
      sec += s.sec;
      n += s.rides;
    }
    final total = n == 0
        ? LocaleKeys.rideMonthNoData.tr
        : [
            LocaleKeys.rideStatsTotal.tr,
            LocaleKeys.rideDistance.trParams({'km': km.toStringAsFixed(1)}),
            '· $n ${LocaleKeys.ridePlanRidesUnit.tr}',
            '· ${RideWeekGoals.formatSeconds(sec)}',
          ].join(' ');

    return Padding(
      padding: const EdgeInsets.fromLTRB(20, 14, 12, 0),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              Text(
                LocaleKeys.rideMonthPick.tr,
                style: const TextStyle(
                  color: Colors.white,
                  fontSize: 16,
                  fontWeight: FontWeight.w700,
                ),
              ),
              const Spacer(),
              _yearArrow(
                Icons.chevron_left,
                _canYearBack ? () => setState(() => _year--) : null,
              ),
              SizedBox(
                width: 60,
                child: Text(
                  '$_year',
                  textAlign: TextAlign.center,
                  style: const TextStyle(
                    color: Colors.white,
                    fontSize: 16,
                    fontWeight: FontWeight.w700,
                    fontFeatures: [FontFeature.tabularFigures()],
                  ),
                ),
              ),
              _yearArrow(
                Icons.chevron_right,
                _canYearForward ? () => setState(() => _year++) : null,
              ),
            ],
          ),
          Padding(
            padding: const EdgeInsets.only(right: 8, top: 2),
            child: Text(
              total,
              style: const TextStyle(color: AppTheme.muted, fontSize: 12),
            ),
          ),
        ],
      ),
    );
  }

  /// 一张月牌：月份名 +（本月）标记 + 那个月的大致记录 / 无数据。
  Widget _card(int month) {
    final target = DateTime(_year, month);
    final s = _sum(month);
    final pickable = _pickable(target);
    final selected =
        _year == widget.selected.year && month == widget.selected.month;
    final current = DateTime.now();
    final isThisMonth = _year == current.year && month == current.month;
    final title = pickable ? Colors.white : AppTheme.muted.withValues(alpha: 0.6);

    return Material(
      color: pickable ? AppTheme.field : AppTheme.field.withValues(alpha: 0.4),
      borderRadius: BorderRadius.circular(14),
      clipBehavior: Clip.antiAlias,
      child: InkWell(
        onTap: pickable ? () => Navigator.pop(context, target) : null,
        child: Container(
          decoration: BoxDecoration(
            borderRadius: BorderRadius.circular(14),
            border: Border.all(
              color: selected ? AppTheme.accent : Colors.transparent,
              width: 1.5,
            ),
          ),
          padding: const EdgeInsets.fromLTRB(14, 9, 12, 9),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            mainAxisAlignment: MainAxisAlignment.spaceBetween,
            children: [
              Row(
                children: [
                  Text(
                    LocaleKeys.rideMonths[month - 1].tr,
                    style: TextStyle(
                      color: title,
                      fontSize: 14,
                      fontWeight: FontWeight.w700,
                    ),
                  ),
                  const Spacer(),
                  if (isThisMonth)
                    _pill(LocaleKeys.rideThisMonth.tr)
                  else if (selected)
                    const Icon(
                      Icons.check_rounded,
                      size: 16,
                      color: AppTheme.accent,
                    ),
                ],
              ),
              if (s.rides > 0) ...[
                Text(
                  LocaleKeys.rideDistance.trParams({
                    'km': s.km.toStringAsFixed(1),
                  }),
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: TextStyle(
                    color: pickable ? Colors.white : AppTheme.muted,
                    fontSize: 16,
                    fontWeight: FontWeight.w700,
                    fontFeatures: const [FontFeature.tabularFigures()],
                  ),
                ),
                Text(
                  '${s.rides} ${LocaleKeys.ridePlanRidesUnit.tr}'
                  ' · ${RideWeekGoals.formatSeconds(s.sec)}',
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: const TextStyle(color: AppTheme.muted, fontSize: 12),
                ),
              ] else
                Text(
                  LocaleKeys.rideMonthNoData.tr,
                  style: TextStyle(
                    color: AppTheme.muted.withValues(alpha: 0.8),
                    fontSize: 13,
                  ),
                ),
            ],
          ),
        ),
      ),
    );
  }

  Widget _pill(String text) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 1),
      decoration: BoxDecoration(
        color: AppTheme.accent.withValues(alpha: 0.18),
        borderRadius: BorderRadius.circular(6),
      ),
      child: Text(
        text,
        style: const TextStyle(
          color: AppTheme.accent,
          fontSize: 10,
          fontWeight: FontWeight.w700,
        ),
      ),
    );
  }

  Widget _yearArrow(IconData icon, VoidCallback? onTap) {
    final on = onTap != null;
    return IconButton(
      onPressed: onTap,
      visualDensity: VisualDensity.compact,
      style: IconButton.styleFrom(
        backgroundColor: AppTheme.field,
        foregroundColor: on ? Colors.white : AppTheme.muted,
        disabledBackgroundColor: AppTheme.field.withValues(alpha: 0.45),
        disabledForegroundColor: AppTheme.muted.withValues(alpha: 0.5),
      ),
      icon: Icon(icon, size: 18),
    );
  }
}

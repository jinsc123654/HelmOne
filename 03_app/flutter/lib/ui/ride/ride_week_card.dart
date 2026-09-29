import 'package:flutter/material.dart';
import 'package:get/get.dart';
import 'package:sifli_companion/app/app_theme.dart';
import 'package:sifli_companion/i18n/locale_keys.dart';
import 'package:sifli_companion/ride/ride_week_goals.dart';
import 'package:sifli_companion/ui/widgets/helm_rings.dart';

/// 本周三环：次数 / 时长 / 里程，点按打开周规划。
class RideWeekCard extends StatelessWidget {
  const RideWeekCard({
    super.key,
    required this.rideCount,
    required this.durationSec,
    required this.km,
    required this.goals,
    required this.onEdit,
  });

  final int rideCount;
  final int durationSec;
  final double km;
  final RideWeekGoals goals;
  final VoidCallback onEdit;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.fromLTRB(4, 4, 12, 0),
      child: GestureDetector(
        onTap: onEdit,
        behavior: HitTestBehavior.opaque,
        child: LayoutBuilder(
          builder: (context, box) {
            final side = box.maxWidth >= 328;
            final rings = _rings();
            final legend = _legend();
            if (side) {
              return Row(
                children: [
                  rings,
                  const SizedBox(width: 4),
                  Expanded(child: legend),
                ],
              );
            }
            return Column(
              children: [
                rings,
                const SizedBox(height: 4),
                legend,
              ],
            );
          },
        ),
      ),
    );
  }

  Widget _rings() {
    return HelmActivityRings(
      size: 176,
      outer: RideWeekGoals.progress(rideCount, goals.rides),
      middle: RideWeekGoals.progress(durationSec, goals.minutes * 60),
      inner: RideWeekGoals.progress(km, goals.km),
      center: Text(
        LocaleKeys.ridePlanThisWeek.tr,
        style: const TextStyle(
          color: AppTheme.muted,
          fontSize: 12,
          fontWeight: FontWeight.w600,
        ),
      ),
    );
  }

  Widget _legend() {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        _row(
          color: AppTheme.ringOrange,
          label: LocaleKeys.rideStatRides.tr,
          value: '$rideCount',
          goal: '${goals.rides} ${LocaleKeys.ridePlanRidesUnit.tr}',
        ),
        _row(
          color: AppTheme.ringLime,
          label: LocaleKeys.rideStatTime.tr,
          value: RideWeekGoals.formatSeconds(durationSec),
          goal: RideWeekGoals.formatMinutes(goals.minutes),
        ),
        _row(
          color: AppTheme.ringCyan,
          label: LocaleKeys.rideStatKm.tr,
          value: km.toStringAsFixed(1),
          goal: '${_fmtKm(goals.km)} km',
        ),
        const SizedBox(height: 4),
        Row(
          children: [
            const Icon(Icons.flag_outlined, size: 14, color: AppTheme.muted),
            const SizedBox(width: 6),
            Text(
              LocaleKeys.ridePlanEdit.tr,
              style: const TextStyle(color: AppTheme.muted, fontSize: 12),
            ),
          ],
        ),
      ],
    );
  }

  Widget _row({
    required Color color,
    required String label,
    required String value,
    required String goal,
  }) {
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 5),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              Container(
                width: 8,
                height: 8,
                decoration: BoxDecoration(color: color, shape: BoxShape.circle),
              ),
              const SizedBox(width: 8),
              Text(
                label,
                style: const TextStyle(color: AppTheme.muted, fontSize: 12),
              ),
            ],
          ),
          const SizedBox(height: 3),
          Text.rich(
            TextSpan(
              text: value,
              style: const TextStyle(
                color: Colors.white,
                fontSize: 20,
                fontWeight: FontWeight.w700,
                height: 1.1,
              ),
              children: [
                TextSpan(
                  text: ' / $goal',
                  style: const TextStyle(
                    color: AppTheme.muted,
                    fontSize: 13,
                    fontWeight: FontWeight.w500,
                  ),
                ),
              ],
            ),
            maxLines: 1,
            overflow: TextOverflow.ellipsis,
          ),
        ],
      ),
    );
  }

  String _fmtKm(double km) {
    if (km == km.roundToDouble()) return '${km.round()}';
    return km.toStringAsFixed(1);
  }
}

/// 周规划：次数、时长、里程。加减调节，不弹键盘。
Future<RideWeekGoals?> showRideWeekPlanSheet(
  BuildContext context,
  RideWeekGoals current,
) {
  return showModalBottomSheet<RideWeekGoals>(
    context: context,
    isScrollControlled: true,
    useSafeArea: true,
    backgroundColor: AppTheme.card,
    barrierColor: Colors.black54,
    builder: (ctx) => _RideWeekPlanSheet(current: current),
  );
}

class _RideWeekPlanSheet extends StatefulWidget {
  const _RideWeekPlanSheet({required this.current});

  final RideWeekGoals current;

  @override
  State<_RideWeekPlanSheet> createState() => _RideWeekPlanSheetState();
}

class _RideWeekPlanSheetState extends State<_RideWeekPlanSheet> {
  late int _rides;
  late int _minutes;
  late double _km;

  @override
  void initState() {
    super.initState();
    final cur = widget.current.clamped();
    _rides = cur.rides;
    _minutes = cur.minutes;
    _km = cur.km;
  }

  void _save() {
    Navigator.pop(
      context,
      RideWeekGoals(rides: _rides, minutes: _minutes, km: _km).clamped(),
    );
  }

  @override
  Widget build(BuildContext context) {
    final bottom = MediaQuery.paddingOf(context).bottom;
    return Padding(
      padding: EdgeInsets.fromLTRB(20, 8, 20, 16 + bottom),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          Center(
            child: Container(
              width: 36,
              height: 4,
              decoration: BoxDecoration(
                color: AppTheme.field,
                borderRadius: BorderRadius.circular(2),
              ),
            ),
          ),
          const SizedBox(height: 16),
          Text(
            LocaleKeys.ridePlanTitle.tr,
            style: const TextStyle(
              color: Colors.white,
              fontSize: 20,
              fontWeight: FontWeight.w700,
            ),
          ),
          const SizedBox(height: 6),
          Text(
            LocaleKeys.ridePlanHint.tr,
            style: const TextStyle(
              color: AppTheme.muted,
              fontSize: 13,
              height: 1.4,
            ),
          ),
          const SizedBox(height: 16),
          _stepper(
            color: AppTheme.ringOrange,
            label: LocaleKeys.ridePlanRides.tr,
            value: '$_rides ${LocaleKeys.ridePlanRidesUnit.tr}',
            onMinus: () => setState(() => _rides = (_rides - 1).clamp(1, 99).toInt()),
            onPlus: () => setState(() => _rides = (_rides + 1).clamp(1, 99).toInt()),
          ),
          const SizedBox(height: 10),
          _stepper(
            color: AppTheme.ringLime,
            label: LocaleKeys.ridePlanHours.tr,
            value:
                '${RideWeekGoals(minutes: _minutes).hoursText} ${LocaleKeys.ridePlanHoursUnit.tr}',
            onMinus: () => setState(
              () => _minutes = (_minutes - 30).clamp(30, 10080).toInt(),
            ),
            onPlus: () => setState(
              () => _minutes = (_minutes + 30).clamp(30, 10080).toInt(),
            ),
          ),
          const SizedBox(height: 10),
          _stepper(
            color: AppTheme.ringCyan,
            label: LocaleKeys.ridePlanKm.tr,
            value: '${_fmtKm(_km)} km',
            onMinus: () =>
                setState(() => _km = (_km - 5).clamp(5, 2000).toDouble()),
            onPlus: () =>
                setState(() => _km = (_km + 5).clamp(5, 2000).toDouble()),
          ),
          const SizedBox(height: 16),
          FilledButton(
            onPressed: _save,
            child: Text(LocaleKeys.save.tr),
          ),
        ],
      ),
    );
  }

  String _fmtKm(double km) {
    if (km == km.roundToDouble()) return '${km.round()}';
    return km.toStringAsFixed(1);
  }

  Widget _stepper({
    required Color color,
    required String label,
    required String value,
    required VoidCallback onMinus,
    required VoidCallback onPlus,
  }) {
    return Container(
      padding: const EdgeInsets.fromLTRB(14, 10, 8, 10),
      decoration: BoxDecoration(
        color: AppTheme.field,
        borderRadius: BorderRadius.circular(12),
      ),
      child: Row(
        children: [
          Container(
            width: 8,
            height: 8,
            decoration: BoxDecoration(color: color, shape: BoxShape.circle),
          ),
          const SizedBox(width: 10),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(
                  label,
                  style: const TextStyle(color: AppTheme.muted, fontSize: 12),
                ),
                const SizedBox(height: 2),
                Text(
                  value,
                  style: const TextStyle(
                    color: Colors.white,
                    fontSize: 18,
                    fontWeight: FontWeight.w700,
                  ),
                ),
              ],
            ),
          ),
          _stepBtn(Icons.remove, onMinus),
          const SizedBox(width: 4),
          _stepBtn(Icons.add, onPlus),
        ],
      ),
    );
  }

  Widget _stepBtn(IconData icon, VoidCallback onTap) {
    return IconButton(
      onPressed: onTap,
      visualDensity: VisualDensity.compact,
      style: IconButton.styleFrom(
        backgroundColor: const Color(0xFF3A3A3C),
        foregroundColor: Colors.white,
      ),
      icon: Icon(icon, size: 18),
    );
  }
}

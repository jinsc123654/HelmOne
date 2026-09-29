import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:get/get.dart';
import 'package:sifli_companion/app/app_theme.dart';
import 'package:sifli_companion/i18n/locale_keys.dart';
import 'package:sifli_companion/ride/gpx_util.dart';
import 'package:sifli_companion/ride/ride_record_store.dart';

/// 地图 / 分享图上的毛玻璃条：顶栏说明，底栏四格数据。
class RideGlanceOverlay extends StatelessWidget {
  /// [forShare] 时顶栏带品牌和标题，底栏改为距离 / 时长等分享向数据。
  const RideGlanceOverlay({
    super.key,
    required this.rec,
    this.summary,
    this.forShare = false,
  });

  final RideRecord rec;
  final GpxSummary? summary;
  final bool forShare;

  @override
  Widget build(BuildContext context) {
    final km = summary?.distanceKm ?? rec.distanceKm ?? 0;
    final when = summary?.startTime ??
        (rec.mtime > 0
            ? DateTime.fromMillisecondsSinceEpoch(rec.mtime * 1000)
            : null);
    final stamp = when == null ? rec.title : GpxUtil.formatStamp(when);
    final kcal = GpxUtil.estimateKcal(
      avgHrBpm: summary?.avgHrBpm,
      duration: summary?.duration,
      distanceKm: km,
    );
    final avg = summary?.avgSpeedKmh;
    final climb = summary?.elevGainM;
    final hr = summary?.avgHrBpm;
    final dash = LocaleKeys.rideStatDash.tr;

    final List<({String value, String unit, String label})> metrics;
    if (forShare) {
      metrics = [
        (
          value: km < 0.001
              ? dash
              : km < 1
                  ? (km * 1000).round().toString()
                  : km < 10
                      ? km.toStringAsFixed(2)
                      : km.toStringAsFixed(1),
          unit: km < 0.001
              ? ''
              : km < 1
                  ? 'm'
                  : 'km',
          label: LocaleKeys.rideMetricDistance.tr,
        ),
        (
          value: summary?.duration == null
              ? dash
              : GpxUtil.formatHms(summary!.duration!),
          unit: '',
          label: LocaleKeys.rideMetricDuration.tr,
        ),
        (
          value: avg == null ? dash : avg.toStringAsFixed(1),
          unit: avg == null ? '' : 'km/h',
          label: LocaleKeys.rideMetricAvgSpeed.tr,
        ),
        hr != null
            ? (
                value: '$hr',
                unit: 'BPM',
                label: LocaleKeys.rideMetricAvgHr.tr,
              )
            : (
                value: kcal > 0 ? '$kcal' : dash,
                unit: kcal > 0 ? 'kcal' : '',
                label: LocaleKeys.rideMetricKcal.tr,
              ),
      ];
    } else {
      metrics = [
        (
          value: avg == null ? dash : avg.toStringAsFixed(1),
          unit: avg == null ? '' : 'km/h',
          label: LocaleKeys.rideMetricAvgSpeed.tr,
        ),
        // 应用内那四格固定摆平均心率（没有就给一条横杠）：指标位置随心率有无换人，
        // 会让人以为心率这项不存在。分享封面仍用千卡兜底 —— 封面出现横杠不好看。
        (
          value: hr == null ? dash : '$hr',
          unit: hr == null ? '' : 'BPM',
          label: LocaleKeys.rideMetricAvgHr.tr,
        ),
        (
          value: climb == null
              ? dash
              : climb >= 10
                  ? climb.round().toString()
                  : climb.toStringAsFixed(1),
          unit: climb == null ? '' : 'm',
          label: LocaleKeys.rideMetricClimb.tr,
        ),
        (
          value: kcal > 0 ? '$kcal' : dash,
          unit: kcal > 0 ? 'kcal' : '',
          label: LocaleKeys.rideMetricKcal.tr,
        ),
      ];
    }

    return Padding(
      padding: const EdgeInsets.fromLTRB(12, 12, 12, 10),
      child: Column(
        children: [
          RideGlancePanel(
            child: Padding(
              padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 12),
              child: Row(
                children: [
                  Container(
                    width: 3,
                    height: forShare ? 34 : 28,
                    margin: const EdgeInsets.only(right: 10),
                    decoration: BoxDecoration(
                      color: AppTheme.accent,
                      borderRadius: BorderRadius.circular(2),
                    ),
                  ),
                  Expanded(
                    child: forShare
                        ? Column(
                            crossAxisAlignment: CrossAxisAlignment.start,
                            children: [
                              Text(
                                rec.title,
                                maxLines: 1,
                                overflow: TextOverflow.ellipsis,
                                style: const TextStyle(
                                  color: Colors.white,
                                  fontSize: 16,
                                  fontWeight: FontWeight.w800,
                                  height: 1.15,
                                ),
                              ),
                              const SizedBox(height: 2),
                              Text(
                                '${LocaleKeys.appName.tr}  ·  $stamp',
                                maxLines: 1,
                                overflow: TextOverflow.ellipsis,
                                style: TextStyle(
                                  color: Colors.white.withValues(alpha: 0.7),
                                  fontSize: 12,
                                  fontWeight: FontWeight.w500,
                                ),
                              ),
                            ],
                          )
                        : Text(
                            LocaleKeys.rideOutdoor.tr,
                            maxLines: 1,
                            overflow: TextOverflow.ellipsis,
                            style: const TextStyle(
                              color: Colors.white,
                              fontSize: 15,
                              fontWeight: FontWeight.w700,
                            ),
                          ),
                  ),
                  if (!forShare)
                    Text(
                      stamp,
                      maxLines: 1,
                      overflow: TextOverflow.ellipsis,
                      style: TextStyle(
                        color: Colors.white.withValues(alpha: 0.72),
                        fontSize: 13,
                        fontWeight: FontWeight.w500,
                      ),
                    ),
                ],
              ),
            ),
          ),
          const Spacer(),
          RideGlancePanel(
            child: Padding(
              padding: const EdgeInsets.symmetric(vertical: 14),
              child: Row(
                children: [
                  for (var i = 0; i < metrics.length; i++) ...[
                    if (i > 0)
                      Container(
                        width: 1,
                        height: 36,
                        color: const Color(0x33FFFFFF),
                      ),
                    Expanded(
                      child: RideGlanceCell(
                        value: metrics[i].value,
                        unit: metrics[i].unit,
                        label: metrics[i].label,
                      ),
                    ),
                  ],
                ],
              ),
            ),
          ),
        ],
      ),
    );
  }
}

/// 等宽毛玻璃底板。
class RideGlancePanel extends StatelessWidget {
  /// 创建底板。
  const RideGlancePanel({super.key, required this.child});

  final Widget child;

  @override
  Widget build(BuildContext context) {
    return ClipRRect(
      borderRadius: BorderRadius.circular(AppTheme.radius),
      child: BackdropFilter(
        filter: ui.ImageFilter.blur(sigmaX: 18, sigmaY: 18),
        child: DecoratedBox(
          decoration: BoxDecoration(
            color: const Color(0xA80C0C0E),
            borderRadius: BorderRadius.circular(AppTheme.radius),
          ),
          child: child,
        ),
      ),
    );
  }
}

/// 四格里的一格。
class RideGlanceCell extends StatelessWidget {
  /// 创建一格。
  const RideGlanceCell({
    super.key,
    required this.value,
    required this.unit,
    required this.label,
  });

  final String value;
  final String unit;
  final String label;

  @override
  Widget build(BuildContext context) {
    return Column(
      mainAxisSize: MainAxisSize.min,
      children: [
        Text.rich(
          TextSpan(
            text: value,
            style: const TextStyle(
              color: Colors.white,
              fontSize: 16,
              fontWeight: FontWeight.w800,
              height: 1.1,
            ),
            children: [
              if (unit.isNotEmpty)
                TextSpan(
                  text: ' $unit',
                  style: TextStyle(
                    color: Colors.white.withValues(alpha: 0.55),
                    fontSize: 10,
                    fontWeight: FontWeight.w600,
                  ),
                ),
            ],
          ),
          maxLines: 1,
          overflow: TextOverflow.ellipsis,
          textAlign: TextAlign.center,
        ),
        const SizedBox(height: 4),
        Text(
          label,
          maxLines: 1,
          overflow: TextOverflow.ellipsis,
          textAlign: TextAlign.center,
          style: TextStyle(
            color: Colors.white.withValues(alpha: 0.58),
            fontSize: 11,
            fontWeight: FontWeight.w500,
          ),
        ),
      ],
    );
  }
}

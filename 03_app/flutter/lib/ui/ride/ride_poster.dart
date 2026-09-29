import 'package:flutter/material.dart';
import 'package:sifli_companion/ride/gpx_util.dart';
import 'package:sifli_companion/ride/ride_record_store.dart';
import 'package:sifli_companion/ui/ride/ride_glance.dart';

/// 分享图：整张地图 + 与详情页相同的毛玻璃数据条。
class RidePoster extends StatelessWidget {
  /// 创建海报。
  const RidePoster({
    super.key,
    required this.rec,
    required this.map,
    this.summary,
  });

  final RideRecord rec;
  final Widget map;
  final GpxSummary? summary;

  static String whenLine(RideRecord rec, GpxSummary? sum) {
    final start = sum?.startTime?.toLocal();
    final end = sum?.endTime?.toLocal();
    if (start != null && end != null) {
      final day = _ymd(start);
      final a = _hm(start);
      final b = _hm(end);
      if (start.year == end.year &&
          start.month == end.month &&
          start.day == end.day) {
        return '$day  $a – $b';
      }
      return '$day $a  ·  ${_ymd(end)} $b';
    }
    if (rec.mtime <= 0) return '';
    final d = DateTime.fromMillisecondsSinceEpoch(rec.mtime * 1000);
    return '${_ymd(d)}  ${_hm(d)}';
  }

  static String shareCaption(RideRecord rec, GpxSummary? sum) {
    final bits = <String>[
      rec.title,
      if ((sum?.distanceKm ?? rec.distanceKm ?? 0) > 0)
        '${(sum?.distanceKm ?? rec.distanceKm)!.toStringAsFixed(1)} km',
      if (sum?.duration != null) GpxUtil.formatElapsed(sum!.duration!),
      if (sum?.avgSpeedKmh != null)
        '${sum!.avgSpeedKmh!.toStringAsFixed(1)} km/h',
      if (sum?.avgHrBpm != null) '${sum!.avgHrBpm} bpm',
    ];
    return bits.join(' · ');
  }

  static String _ymd(DateTime d) {
    final y = d.year.toString().padLeft(4, '0');
    final m = d.month.toString().padLeft(2, '0');
    final day = d.day.toString().padLeft(2, '0');
    return '$y.$m.$day';
  }

  static String _hm(DateTime d) {
    final h = d.hour.toString().padLeft(2, '0');
    final min = d.minute.toString().padLeft(2, '0');
    return '$h:$min';
  }

  @override
  Widget build(BuildContext context) {
    return ColoredBox(
      color: const Color(0xFFEDE8DF),
      child: Stack(
        fit: StackFit.expand,
        children: [
          map,
          const IgnorePointer(
            child: DecoratedBox(
              decoration: BoxDecoration(
                gradient: LinearGradient(
                  begin: Alignment.topCenter,
                  end: Alignment.bottomCenter,
                  colors: [
                    Color(0x4D000000),
                    Color(0x00000000),
                    Color(0x00000000),
                    Color(0x4D000000),
                  ],
                  stops: [0, 0.16, 0.78, 1],
                ),
              ),
            ),
          ),
          IgnorePointer(
            child: RideGlanceOverlay(
              rec: rec,
              summary: summary,
              forShare: true,
            ),
          ),
        ],
      ),
    );
  }
}

/// 地图起终点圆标。
class RideTrackPin extends StatelessWidget {
  /// 创建圆标。
  const RideTrackPin({super.key, required this.color, required this.label});

  final Color color;
  final String label;

  @override
  Widget build(BuildContext context) {
    return Container(
      alignment: Alignment.center,
      decoration: BoxDecoration(
        color: color,
        shape: BoxShape.circle,
        border: Border.all(color: Colors.white, width: 2.4),
        boxShadow: const [
          BoxShadow(
            color: Color(0x66000000),
            blurRadius: 6,
            offset: Offset(0, 1),
          ),
        ],
      ),
      child: Text(
        label,
        style: const TextStyle(
          color: Colors.white,
          fontSize: 11,
          fontWeight: FontWeight.w800,
          height: 1,
        ),
      ),
    );
  }
}

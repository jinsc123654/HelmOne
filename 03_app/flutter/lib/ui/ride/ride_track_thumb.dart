import 'dart:math' as math;
import 'dart:ui' as ui;

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:latlong2/latlong.dart' hide Path;
import 'package:sifli_companion/app/app_theme.dart';

/// 列表左侧轨迹缩略图：橙线 + 起终点圆点。
class RideTrackThumb extends StatelessWidget {
  const RideTrackThumb({
    super.key,
    required this.points,
    this.size = 44,
  });

  final List<LatLng> points;
  final double size;

  @override
  Widget build(BuildContext context) {
    return SizedBox.square(
      dimension: size,
      child: DecoratedBox(
        decoration: BoxDecoration(
          color: AppTheme.field,
          borderRadius: BorderRadius.circular(10),
        ),
        child: points.length < 2
            ? const Icon(
                Icons.pedal_bike,
                color: AppTheme.muted,
                size: 20,
              )
            : CustomPaint(
                painter: _ThumbPainter(points),
              ),
      ),
    );
  }
}

class _ThumbPainter extends CustomPainter {
  _ThumbPainter(this.points);

  final List<LatLng> points;

  static const _line = AppTheme.ringOrange;
  static const _pad = 7.0;

  @override
  void paint(Canvas canvas, Size size) {
    final mapped = _map(size);
    if (mapped.length < 2) return;

    final path = ui.Path()..moveTo(mapped.first.dx, mapped.first.dy);
    for (var i = 1; i < mapped.length; i++) {
      path.lineTo(mapped[i].dx, mapped[i].dy);
    }

    canvas.drawPath(
      path,
      Paint()
        ..style = PaintingStyle.stroke
        ..strokeWidth = 3.4
        ..strokeCap = StrokeCap.round
        ..strokeJoin = StrokeJoin.round
        ..color = _line.withValues(alpha: 0.28),
    );
    canvas.drawPath(
      path,
      Paint()
        ..style = PaintingStyle.stroke
        ..strokeWidth = 1.9
        ..strokeCap = StrokeCap.round
        ..strokeJoin = StrokeJoin.round
        ..color = _line,
    );

    final start = mapped.first;
    final end = mapped.last;
    canvas.drawCircle(start, 3.1, Paint()..color = Colors.white);
    canvas.drawCircle(start, 2.0, Paint()..color = AppTheme.ringLime);
    canvas.drawCircle(end, 3.4, Paint()..color = Colors.white);
    canvas.drawCircle(end, 2.3, Paint()..color = _line);
  }

  List<Offset> _map(Size size) {
    var minLat = points.first.latitude;
    var maxLat = minLat;
    var minLon = points.first.longitude;
    var maxLon = minLon;
    for (final p in points) {
      minLat = math.min(minLat, p.latitude);
      maxLat = math.max(maxLat, p.latitude);
      minLon = math.min(minLon, p.longitude);
      maxLon = math.max(maxLon, p.longitude);
    }
    final midLat = (minLat + maxLat) / 2;
    final k = math.cos(midLat * math.pi / 180).clamp(0.2, 1.0);
    var minX = minLon * k;
    var maxX = maxLon * k;
    var minY = minLat;
    var maxY = maxLat;
    var dx = maxX - minX;
    var dy = maxY - minY;
    if (dx < 1e-9) dx = 1e-9;
    if (dy < 1e-9) dy = 1e-9;
    final innerW = size.width - _pad * 2;
    final innerH = size.height - _pad * 2;
    final scale = math.min(innerW / dx, innerH / dy);
    final ox = _pad + (innerW - dx * scale) / 2;
    final oy = _pad + (innerH - dy * scale) / 2;
    return [
      for (final p in points)
        Offset(
          ox + (p.longitude * k - minX) * scale,
          oy + (maxY - p.latitude) * scale,
        ),
    ];
  }

  @override
  bool shouldRepaint(_ThumbPainter old) => !listEquals(old.points, points);
}

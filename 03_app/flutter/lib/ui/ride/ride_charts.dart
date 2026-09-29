import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:sifli_companion/ride/gpx_util.dart';
import 'package:sifli_companion/ui/ride/ride_track_style.dart';

/// 折线 / 面积图，虚线标均值。
class RideSeriesChart extends StatelessWidget {
  const RideSeriesChart({
    super.key,
    required this.values,
    required this.color,
    this.fill = false,
    this.avg,
  });

  final List<double> values;
  final Color color;
  final bool fill;
  final double? avg;

  @override
  Widget build(BuildContext context) {
    return CustomPaint(
      painter: _SeriesPainter(
        values: downsample(values, 180),
        color: color,
        fill: fill,
        avg: avg,
      ),
      child: const SizedBox.expand(),
    );
  }

  static List<double> downsample(List<double> src, int maxN) {
    if (src.length <= maxN) return src;
    final out = <double>[];
    for (var i = 0; i < maxN; i++) {
      final idx = (i * (src.length - 1) / (maxN - 1)).round();
      out.add(src[idx]);
    }
    return out;
  }
}

class _SeriesPainter extends CustomPainter {
  _SeriesPainter({
    required this.values,
    required this.color,
    required this.fill,
    required this.avg,
  });

  final List<double> values;
  final Color color;
  final bool fill;
  final double? avg;

  @override
  void paint(Canvas canvas, Size size) {
    if (values.isEmpty || size.width < 8 || size.height < 8) return;
    const pad = EdgeInsets.fromLTRB(4, 10, 4, 6);
    final w = size.width - pad.left - pad.right;
    final h = size.height - pad.top - pad.bottom;
    if (w <= 0 || h <= 0) return;

    var minV = values.reduce(math.min);
    var maxV = values.reduce(math.max);
    if (avg != null) {
      minV = math.min(minV, avg!);
      maxV = math.max(maxV, avg!);
    }
    if (maxV - minV < 0.2) {
      minV -= 1;
      maxV += 1;
    }
    final span = maxV - minV;

    Offset pt(int i, double v) {
      final x = pad.left +
          (values.length == 1 ? w / 2 : w * i / (values.length - 1));
      final y = pad.top + h * (1 - (v - minV) / span);
      return Offset(x, y);
    }

    final line = Path()..moveTo(pt(0, values[0]).dx, pt(0, values[0]).dy);
    for (var i = 1; i < values.length; i++) {
      line.lineTo(pt(i, values[i]).dx, pt(i, values[i]).dy);
    }

    if (fill) {
      final area = Path.from(line)
        ..lineTo(pt(values.length - 1, values.last).dx, pad.top + h)
        ..lineTo(pad.left, pad.top + h)
        ..close();
      canvas.drawPath(
        area,
        Paint()
          ..shader = LinearGradient(
            begin: Alignment.topCenter,
            end: Alignment.bottomCenter,
            colors: [
              color.withValues(alpha: 0.42),
              color.withValues(alpha: 0.02),
            ],
          ).createShader(Offset.zero & size),
      );
    }

    canvas.drawPath(
      line,
      Paint()
        ..color = color
        ..style = PaintingStyle.stroke
        ..strokeWidth = 2.2
        ..strokeJoin = StrokeJoin.round
        ..strokeCap = StrokeCap.round,
    );

    final a = avg;
    if (a != null) {
      final y = pt(0, a).dy;
      _dash(
        canvas,
        Offset(pad.left, y),
        Offset(pad.left + w, y),
        Paint()
          ..color = color.withValues(alpha: 0.7)
          ..strokeWidth = 1.2,
      );
    }
  }

  static void _dash(Canvas canvas, Offset a, Offset b, Paint paint) {
    const dash = 5.0;
    const gap = 4.0;
    final len = (b - a).distance;
    if (len < 1) return;
    final dir = (b - a) / len;
    var t = 0.0;
    while (t < len) {
      final end = math.min(t + dash, len);
      canvas.drawLine(a + dir * t, a + dir * end, paint);
      t += dash + gap;
    }
  }

  @override
  bool shouldRepaint(covariant _SeriesPainter old) =>
      old.values != values ||
      old.color != color ||
      old.fill != fill ||
      old.avg != avg;
}

/// 柱状图：一格一根柱（周 = 逐日 7 根，月 = 逐日 28–31 根），虚线标均值。
///
/// 取值、标签由调用方按区间算好传进来 —— 图表只管画，不碰业务。
class RideBarChart extends StatelessWidget {
  /// 创建柱状图。
  const RideBarChart({
    super.key,
    required this.values,
    required this.labels,
    required this.color,
    this.avg,
    this.avgText,
    this.highlight = -1,
  });

  /// 每根柱子的值，与 [labels] 一一对应。
  final List<double> values;

  /// 柱子下方的标签；空串表示这根不标（月的 31 根只标 1/5/10…）。
  final List<String> labels;

  /// 柱色。0 值的柱子画成灰底，与「这天没骑」区分开。
  final Color color;

  /// 虚线位置（均值）。null 或 0 不画。
  final double? avg;

  /// 虚线右端的数值标签（如 `78.1`）。空则不画 —— 光一条线分不清它落在哪一档。
  final String? avgText;

  /// 高亮哪根（今天）；-1 不高亮。
  final int highlight;

  @override
  Widget build(BuildContext context) {
    return SizedBox(
      height: 112,
      width: double.infinity,
      child: CustomPaint(
        painter: _BarPainter(
          values: values,
          labels: labels,
          color: color,
          avg: avg,
          avgText: avgText,
          highlight: highlight,
        ),
        child: const SizedBox.expand(),
      ),
    );
  }
}

class _BarPainter extends CustomPainter {
  _BarPainter({
    required this.values,
    required this.labels,
    required this.color,
    required this.avg,
    required this.avgText,
    required this.highlight,
  });

  final List<double> values;
  final List<String> labels;
  final Color color;
  final double? avg;
  final String? avgText;
  final int highlight;

  static const _emptyBar = Color(0xFF3A3A3C);
  static const _labelColor = Color(0xFF8E8E93);

  @override
  void paint(Canvas canvas, Size size) {
    if (values.isEmpty || size.width < 8 || size.height < 8) return;
    final w = size.width;
    // 有标签才留那一行；迷你图（labels 全空）就贴边画，别浪费高度。
    final hasLabel = labels.any((l) => l.isNotEmpty);
    final padTop = hasLabel ? 10.0 : 3.0;
    final padBottom = hasLabel ? 17.0 : 3.0;
    final h = size.height - padTop - padBottom;
    if (h <= 0) return;

    final maxV = values.fold<double>(0, math.max);
    final scaleMax = math.max(maxV, avg ?? 0) * 1.12;
    final slot = w / values.length;
    final barW = math.min(slot * 0.62, 18.0);

    for (var i = 0; i < values.length; i++) {
      final v = values[i];
      final cx = slot * (i + 0.5);
      final frac = scaleMax <= 0 ? 0.0 : (v / scaleMax).clamp(0.0, 1.0);
      // 0 也留 2 px 灰底：能看出「这一天存在、只是没骑」。
      final barH = frac <= 0 ? 2.0 : math.max(2.0, h * frac);
      final isHi = i == highlight;

      canvas.drawRRect(
        RRect.fromRectAndRadius(
          Rect.fromLTWH(cx - barW / 2, padTop + h - barH, barW, barH),
          Radius.circular(math.min(barW / 2, 3)),
        ),
        Paint()
          ..color = v <= 0
              ? _emptyBar
              : (isHi ? color : color.withValues(alpha: 0.55)),
      );

      final label = i < labels.length ? labels[i] : '';
      if (label.isEmpty) continue;
      final tp = TextPainter(
        text: TextSpan(
          text: label,
          style: TextStyle(
            color: isHi ? Colors.white : _labelColor,
            fontSize: 10,
            fontWeight: isHi ? FontWeight.w700 : FontWeight.w400,
          ),
        ),
        textDirection: TextDirection.ltr,
      )..layout();
      tp.paint(canvas, Offset(cx - tp.width / 2, size.height - padBottom + 4));
    }

    final a = avg;
    if (a != null && a > 0) {
      final y = padTop + h * (1 - (a / scaleMax).clamp(0.0, 1.0));
      final paint = Paint()
        ..color = color.withValues(alpha: 0.85)
        ..strokeWidth = 1.4;
      _SeriesPainter._dash(canvas, Offset(0, y), Offset(w, y), paint);

      // 数值标签贴在线右端上方：只有一条线看不出它落在哪一档。
      final text = avgText;
      if (text != null && text.isNotEmpty) {
        final tp = TextPainter(
          text: TextSpan(
            text: text,
            style: TextStyle(
              color: color,
              fontSize: 9,
              fontWeight: FontWeight.w700,
            ),
          ),
          textDirection: TextDirection.ltr,
        )..layout();
        tp.paint(canvas, Offset(w - tp.width, y - tp.height - 2));
      }
    }
  }

  @override
  bool shouldRepaint(covariant _BarPainter old) =>
      old.values != values ||
      old.labels != labels ||
      old.color != color ||
      old.avg != avg ||
      old.avgText != avgText ||
      old.highlight != highlight;
}

/// 速度带：按采样点从绿到红。
class RidePaceStrip extends StatelessWidget {
  const RidePaceStrip({super.key, required this.speeds, this.range});

  final List<double> speeds;

  /// 色带归一化区间。由调用方用**原始**速度序列（和轨迹线同一份）算好传进来，
  /// 上下两条颜色才对得上；为空时自己按这段算。
  final ({double lo, double hi})? range;

  @override
  Widget build(BuildContext context) {
    return SizedBox(
      height: 10,
      width: double.infinity,
      child: CustomPaint(
        painter: _PacePainter(
          speeds: RideSeriesChart.downsample(speeds, 120),
          range: range,
        ),
        child: const SizedBox.expand(),
      ),
    );
  }
}

class _PacePainter extends CustomPainter {
  _PacePainter({required this.speeds, this.range});

  final List<double> speeds;
  final ({double lo, double hi})? range;

  @override
  void paint(Canvas canvas, Size size) {
    final r = RRect.fromRectAndRadius(
      Offset.zero & size,
      const Radius.circular(5),
    );
    canvas.save();
    canvas.clipRRect(r);
    if (speeds.length < 2) {
      canvas.drawRRect(
        r,
        Paint()
          ..shader = const LinearGradient(
            colors: [
              RideTrackStyle.bandSlow,
              RideTrackStyle.bandMid,
              RideTrackStyle.bandFast,
            ],
          ).createShader(Offset.zero & size),
      );
    } else {
      final minV = range?.lo ?? speeds.reduce(math.min);
      final maxV = range?.hi ?? speeds.reduce(math.max);
      final w = size.width / speeds.length;
      for (var i = 0; i < speeds.length; i++) {
        // 与轨迹线共用同一个映射（[RideTrackStyle.speedBandColor]），颜色才对得上。
        canvas.drawRect(
          Rect.fromLTWH(i * w, 0, w + 0.6, size.height),
          Paint()..color = RideTrackStyle.speedBandColor(speeds[i], minV, maxV),
        );
      }
    }
    canvas.restore();
  }

  @override
  bool shouldRepaint(covariant _PacePainter old) =>
      old.speeds != speeds || old.range != range;
}

/// 心率五区横条。
class RideZoneBar extends StatelessWidget {
  const RideZoneBar({
    super.key,
    required this.label,
    required this.color,
    required this.time,
    required this.maxMs,
  });

  final String label;
  final Color color;
  final Duration time;
  final int maxMs;

  @override
  Widget build(BuildContext context) {
    final frac = maxMs <= 0 ? 0.0 : (time.inMilliseconds / maxMs).clamp(0.0, 1.0);
    return Padding(
      padding: const EdgeInsets.only(bottom: 8),
      child: Row(
        children: [
          SizedBox(
            width: 64,
            child: Text(
              label,
              maxLines: 1,
              overflow: TextOverflow.ellipsis,
              style: const TextStyle(color: Color(0xFF8E8E93), fontSize: 12),
            ),
          ),
          Expanded(
            child: ClipRRect(
              borderRadius: BorderRadius.circular(3),
              child: SizedBox(
                height: 8,
                child: ColoredBox(
                  color: const Color(0xFF2C2C2E),
                  child: Align(
                    alignment: Alignment.centerLeft,
                    child: FractionallySizedBox(
                      widthFactor: frac,
                      child: ColoredBox(color: color),
                    ),
                  ),
                ),
              ),
            ),
          ),
          const SizedBox(width: 10),
          SizedBox(
            width: 64,
            child: Text(
              GpxUtil.formatHms(time),
              textAlign: TextAlign.right,
              style: const TextStyle(
                color: Colors.white,
                fontSize: 12,
                fontFeatures: [FontFeature.tabularFigures()],
              ),
            ),
          ),
        ],
      ),
    );
  }
}

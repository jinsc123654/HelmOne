import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:sifli_companion/app/app_theme.dart';

/// 小米运动式三环：入场绘制 + 端点呼吸光。
class HelmActivityRings extends StatefulWidget {
  const HelmActivityRings({
    super.key,
    required this.outer,
    required this.middle,
    required this.inner,
    this.size = 208,
    this.center,
  });

  /// 0–1，外环（次数）。
  final double outer;

  /// 0–1，中环（时长）。
  final double middle;

  /// 0–1，内环（里程）。
  final double inner;

  final double size;

  /// 环心说明，例如「本周」。
  final Widget? center;

  @override
  State<HelmActivityRings> createState() => _HelmActivityRingsState();
}

class _HelmActivityRingsState extends State<HelmActivityRings>
    with TickerProviderStateMixin {
  late final AnimationController _draw;
  late final AnimationController _pulse;
  late final AnimationController _spin;
  late final AnimationController _halo;
  late final Animation<double> _t;

  @override
  void initState() {
    super.initState();
    _draw = AnimationController(
      vsync: this,
      duration: const Duration(milliseconds: 1400),
    );
    _pulse = AnimationController(
      vsync: this,
      duration: const Duration(milliseconds: 1800),
    );
    _spin = AnimationController(
      vsync: this,
      duration: const Duration(milliseconds: 4200),
    );
    _halo = AnimationController(
      vsync: this,
      duration: const Duration(milliseconds: 2400),
    );
    _t = CurvedAnimation(parent: _draw, curve: Curves.easeOutCubic);
    _draw.forward();
    _pulse.repeat(reverse: true);
    _spin.repeat();
    _halo.repeat();
  }

  @override
  void didUpdateWidget(HelmActivityRings oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.outer != widget.outer ||
        oldWidget.middle != widget.middle ||
        oldWidget.inner != widget.inner) {
      _draw.forward(from: 0);
    }
  }

  @override
  void dispose() {
    _draw.dispose();
    _pulse.dispose();
    _spin.dispose();
    _halo.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return AnimatedBuilder(
      animation: Listenable.merge([_t, _pulse, _spin, _halo]),
      builder: (context, _) {
        return SizedBox.square(
          dimension: widget.size,
          child: Stack(
            alignment: Alignment.center,
            children: [
              CustomPaint(
                size: Size.square(widget.size),
                painter: _RingsPainter(
                  outer: widget.outer.clamp(0.0, 1.0) * _t.value,
                  middle: widget.middle.clamp(0.0, 1.0) * _t.value,
                  inner: widget.inner.clamp(0.0, 1.0) * _t.value,
                  pulse: _pulse.value,
                  spin: _spin.value,
                  halo: _halo.value,
                ),
              ),
              if (widget.center != null) widget.center!,
            ],
          ),
        );
      },
    );
  }
}

class _RingsPainter extends CustomPainter {
  _RingsPainter({
    required this.outer,
    required this.middle,
    required this.inner,
    required this.pulse,
    required this.spin,
    required this.halo,
  });

  final double outer;
  final double middle;
  final double inner;
  final double pulse;
  final double spin;
  final double halo;

  static const _orange = AppTheme.ringOrange;
  static const _lime = AppTheme.ringLime;
  static const _cyan = AppTheme.ringCyan;

  @override
  void paint(Canvas canvas, Size size) {
    final c = Offset(size.width / 2, size.height / 2);
    final haloPad = size.width * 0.16;
    final stroke = (size.width - haloPad * 2) * 0.078;
    final gap = stroke * 0.42;
    final maxR = size.width / 2 - haloPad - stroke * 0.55;

    _haloGlow(canvas, c, maxR, haloPad);
    _ring(canvas, c, maxR, stroke, _orange, outer);
    _ring(canvas, c, maxR - stroke - gap, stroke, _lime, middle);
    _ring(canvas, c, maxR - (stroke + gap) * 2, stroke, _cyan, inner);
  }

  void _haloGlow(Canvas canvas, Offset c, double maxR, double haloPad) {
    final breath = 0.55 + 0.45 * pulse;
    final fillR = maxR + haloPad * (0.35 + 0.65 * pulse);
    canvas.drawCircle(
      c,
      fillR,
      Paint()
        ..shader = RadialGradient(
          colors: [
            _orange.withValues(alpha: 0.20 * breath),
            _lime.withValues(alpha: 0.10 * breath),
            _cyan.withValues(alpha: 0.04 * breath),
            const Color(0x00000000),
          ],
          stops: const [0.42, 0.68, 0.86, 1],
        ).createShader(Rect.fromCircle(center: c, radius: fillR)),
    );

    for (var i = 0; i < 2; i++) {
      final t = (halo + i * 0.5) % 1.0;
      final ease = Curves.easeOut.transform(t);
      final r = maxR + haloPad * 0.15 + haloPad * 0.95 * ease;
      final a = (1.0 - ease) * 0.38;
      final color = i == 0 ? _orange : _cyan;
      canvas.drawCircle(
        c,
        r,
        Paint()
          ..style = PaintingStyle.stroke
          ..strokeWidth = 2.6 * (1.05 - 0.55 * ease)
          ..color = color.withValues(alpha: a),
      );
    }
  }

  void _ring(
    Canvas canvas,
    Offset c,
    double radius,
    double stroke,
    Color color,
    double progress,
  ) {
    final rect = Rect.fromCircle(center: c, radius: radius);
    const start = -math.pi / 2;

    final track = Paint()
      ..style = PaintingStyle.stroke
      ..strokeWidth = stroke
      ..strokeCap = StrokeCap.round
      ..color = color.withValues(alpha: 0.16);
    canvas.drawArc(rect, 0, math.pi * 2, false, track);

    if (progress <= 0.004) {
      final cometStart = start + spin * math.pi * 2;
      const cometSweep = 0.62;
      canvas.drawArc(
        rect,
        cometStart,
        cometSweep,
        false,
        Paint()
          ..style = PaintingStyle.stroke
          ..strokeWidth = stroke
          ..strokeCap = StrokeCap.round
          ..color = color.withValues(alpha: 0.45 + 0.25 * pulse),
      );
      final tip = Offset(
        c.dx + radius * math.cos(cometStart + cometSweep),
        c.dy + radius * math.sin(cometStart + cometSweep),
      );
      canvas.drawCircle(
        tip,
        stroke * 0.38,
        Paint()..color = Colors.white.withValues(alpha: 0.85),
      );
      return;
    }

    final sweep = math.pi * 2 * progress;
    final shader = SweepGradient(
      startAngle: start,
      endAngle: start + math.pi * 2,
      colors: [
        color.withValues(alpha: 0.55),
        color,
        Color.lerp(color, Colors.white, 0.55)!,
        color.withValues(alpha: 0.55),
      ],
      stops: const [0, 0.45, 0.78, 1],
      transform: const GradientRotation(start),
    ).createShader(rect);

    final arc = Paint()
      ..style = PaintingStyle.stroke
      ..strokeWidth = stroke
      ..strokeCap = StrokeCap.round
      ..shader = shader;
    canvas.drawArc(rect, start, sweep, false, arc);

    final tip = Offset(
      c.dx + radius * math.cos(start + sweep),
      c.dy + radius * math.sin(start + sweep),
    );
    final glow = stroke * (0.55 + 0.35 * pulse);
    canvas.drawCircle(
      tip,
      glow,
      Paint()..color = color.withValues(alpha: 0.28 + 0.22 * pulse),
    );
    canvas.drawCircle(tip, stroke * 0.42, Paint()..color = Colors.white);
    canvas.drawCircle(tip, stroke * 0.28, Paint()..color = color);
  }

  @override
  bool shouldRepaint(_RingsPainter old) {
    return old.outer != outer ||
        old.middle != middle ||
        old.inner != inner ||
        old.pulse != pulse ||
        old.spin != spin ||
        old.halo != halo;
  }
}

/// 码表图背后的呼吸光（已连接时）。
class HelmPulseGlow extends StatefulWidget {
  const HelmPulseGlow({
    super.key,
    required this.active,
    required this.child,
    this.color = const Color(0xFF3A4A62),
    this.intensity = 1.0,
  });

  final bool active;
  final Widget child;
  final Color color;

  /// 呼吸幅度：1.0 = 原来的淡光；越大越亮、范围越大。
  /// 设备页那张码表图用 [deviceGlowIntensity]（用户要"更明显"），
  /// 扫描页/中心按钮保持默认，免得别的页面一起变糊。
  final double intensity;

  /// 设备页码表图的光呼吸强度。
  static const double deviceGlowIntensity = 2.4;

  @override
  State<HelmPulseGlow> createState() => _HelmPulseGlowState();
}

class _HelmPulseGlowState extends State<HelmPulseGlow>
    with SingleTickerProviderStateMixin {
  late final AnimationController _ctrl;

  @override
  void initState() {
    super.initState();
    _ctrl = AnimationController(
      vsync: this,
      duration: const Duration(milliseconds: 2200),
    );
    if (widget.active) _ctrl.repeat(reverse: true);
  }

  @override
  void didUpdateWidget(HelmPulseGlow oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (widget.active && !_ctrl.isAnimating) {
      _ctrl.repeat(reverse: true);
    } else if (!widget.active && _ctrl.isAnimating) {
      _ctrl.stop();
      _ctrl.value = 0;
    }
  }

  @override
  void dispose() {
    _ctrl.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return AnimatedBuilder(
      animation: _ctrl,
      builder: (context, child) {
        final t = widget.active ? _ctrl.value : 0.0;
        final k = widget.intensity;
        // 两层同相位：外圈大范围光晕 + 内圈紧贴的亮边。单层只加大 blur 会"糊成一片"，
        // 加一层内圈才看得出在呼吸。
        final outerA = ((0.22 + 0.32 * t) * k).clamp(0.0, 1.0);
        final innerA = ((0.12 + 0.30 * t) * k).clamp(0.0, 1.0);
        return Container(
          decoration: BoxDecoration(
            shape: BoxShape.circle,
            boxShadow: widget.active
                ? [
                    BoxShadow(
                      color: widget.color.withValues(alpha: outerA),
                      blurRadius: (22 + 16 * t) * k,
                      spreadRadius: (2 + 4 * t) * k,
                    ),
                    BoxShadow(
                      color: widget.color.withValues(alpha: innerA),
                      blurRadius: (6 + 6 * t) * k,
                      spreadRadius: (0.5 + 1.5 * t) * k,
                    ),
                  ]
                : const [],
          ),
          child: child,
        );
      },
      child: widget.child,
    );
  }
}

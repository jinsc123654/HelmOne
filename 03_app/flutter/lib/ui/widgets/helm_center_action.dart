import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:sifli_companion/app/app_theme.dart';
import 'package:sifli_companion/ui/widgets/helm_rings.dart';

/// 单操作页：圆钮钉在可用区域几何中心，文案在上/下，不把按钮挤偏。
class HelmCenterAction extends StatefulWidget {
  const HelmCenterAction({
    super.key,
    required this.icon,
    required this.label,
    required this.color,
    this.onPressed,
    this.busy = false,
    this.progress,
    this.headline,
    this.caption,
    this.status,
    this.message,
    this.messageError = false,
    this.secondaryLabel,
    this.onSecondary,
    this.bottom,
    this.orbSize = 168,
  });

  final IconData icon;
  final String label;
  final Color color;
  final VoidCallback? onPressed;
  final bool busy;

  /// `0–1` 确定进度；忙碌且为空时转不确定弧。
  final double? progress;
  final String? headline;
  final String? caption;
  final String? status;
  final String? message;
  final bool messageError;
  final String? secondaryLabel;
  final VoidCallback? onSecondary;
  final Widget? bottom;
  final double orbSize;

  @override
  State<HelmCenterAction> createState() => _HelmCenterActionState();
}

class _HelmCenterActionState extends State<HelmCenterAction>
    with SingleTickerProviderStateMixin {
  static const _colorDur = Duration(milliseconds: 560);

  late final AnimationController _colorCtrl;
  late final CurvedAnimation _colorEase;
  late ColorTween _colorTween;

  @override
  void initState() {
    super.initState();
    _colorTween = ColorTween(begin: widget.color, end: widget.color);
    _colorCtrl = AnimationController(vsync: this, duration: _colorDur)
      ..value = 1;
    _colorEase = CurvedAnimation(
      parent: _colorCtrl,
      curve: Curves.easeInOutCubic,
    );
  }

  @override
  void didUpdateWidget(HelmCenterAction oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.color != widget.color) {
      _colorTween = ColorTween(
        begin: _colorTween.evaluate(_colorEase) ?? oldWidget.color,
        end: widget.color,
      );
      _colorCtrl.forward(from: 0);
    }
  }

  @override
  void dispose() {
    _colorEase.dispose();
    _colorCtrl.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return AnimatedBuilder(
      animation: _colorEase,
      builder: (context, _) {
        final color = _colorTween.evaluate(_colorEase) ?? widget.color;
        return _layout(color);
      },
    );
  }

  Widget _layout(Color color) {
    return Column(
      children: [
        Expanded(
          child: Padding(
            padding: const EdgeInsets.symmetric(horizontal: 28),
            child: LayoutBuilder(
              builder: (context, box) {
                final cx = box.maxWidth / 2;
                final cy = box.maxHeight / 2;
                final r = widget.orbSize / 2;
                return Stack(
                  children: [
                    if (widget.headline != null &&
                        widget.headline!.isNotEmpty)
                      Positioned(
                        top: 8,
                        left: 0,
                        right: 0,
                        child: Text(
                          widget.headline!,
                          textAlign: TextAlign.center,
                          style: const TextStyle(
                            color: Colors.white,
                            fontSize: 20,
                            fontWeight: FontWeight.w600,
                            height: 1.3,
                          ),
                        ),
                      ),
                    Positioned(
                      left: cx - r,
                      top: cy - r,
                      width: widget.orbSize,
                      height: widget.orbSize,
                      child: HelmActionOrb(
                        icon: widget.icon,
                        color: color,
                        onPressed: widget.onPressed,
                        busy: widget.busy,
                        progress: widget.progress,
                        size: widget.orbSize,
                      ),
                    ),
                    Positioned(
                      left: 0,
                      right: 0,
                      top: cy + r + 16,
                      child: Column(
                        children: [
                          GestureDetector(
                            onTap: widget.busy ? null : widget.onPressed,
                            behavior: HitTestBehavior.opaque,
                            child: Padding(
                              padding: const EdgeInsets.symmetric(
                                horizontal: 12,
                                vertical: 4,
                              ),
                              child: Text(
                                widget.label,
                                textAlign: TextAlign.center,
                                style: TextStyle(
                                  color: widget.onPressed == null &&
                                          !widget.busy
                                      ? AppTheme.muted
                                      : Colors.white,
                                  fontSize: 17,
                                  fontWeight: FontWeight.w600,
                                ),
                              ),
                            ),
                          ),
                          if (widget.caption != null &&
                              widget.caption!.isNotEmpty) ...[
                            const SizedBox(height: 8),
                            Text(
                              widget.caption!,
                              textAlign: TextAlign.center,
                              style: const TextStyle(
                                color: AppTheme.muted,
                                fontSize: 13,
                                height: 1.4,
                              ),
                            ),
                          ],
                          if (widget.status != null &&
                              widget.status!.isNotEmpty) ...[
                            const SizedBox(height: 8),
                            Text(
                              widget.status!,
                              textAlign: TextAlign.center,
                              style: TextStyle(
                                color: color,
                                fontSize: 13,
                                height: 1.4,
                                fontWeight: FontWeight.w600,
                              ),
                            ),
                          ],
                          if (widget.message != null &&
                              widget.message!.isNotEmpty) ...[
                            const SizedBox(height: 10),
                            Text(
                              widget.message!,
                              textAlign: TextAlign.center,
                              style: TextStyle(
                                color: widget.messageError
                                    ? const Color(0xFFFF8A80)
                                    : const Color(0xFFC6E54B),
                                fontSize: 14,
                                height: 1.45,
                              ),
                            ),
                          ],
                        ],
                      ),
                    ),
                  ],
                );
              },
            ),
          ),
        ),
        if (widget.bottom != null)
          widget.bottom!
        else if (widget.secondaryLabel != null)
          TextButton(
            onPressed: widget.onSecondary,
            child: Text(widget.secondaryLabel!),
          ),
      ],
    );
  }
}

/// 带进度环的圆形主按钮。图标与圆环共用同一圆心。
class HelmActionOrb extends StatefulWidget {
  const HelmActionOrb({
    super.key,
    required this.icon,
    required this.color,
    this.onPressed,
    this.busy = false,
    this.progress,
    this.size = 168,
  });

  final IconData icon;
  final Color color;
  final VoidCallback? onPressed;
  final bool busy;
  final double? progress;
  final double size;

  @override
  State<HelmActionOrb> createState() => _HelmActionOrbState();
}

class _HelmActionOrbState extends State<HelmActionOrb>
    with SingleTickerProviderStateMixin {
  late final AnimationController _spin;

  @override
  void initState() {
    super.initState();
    _spin = AnimationController(
      vsync: this,
      duration: const Duration(milliseconds: 1400),
    );
    if (_indeterminate) _spin.repeat();
  }

  @override
  void didUpdateWidget(HelmActionOrb oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (_indeterminate && !_spin.isAnimating) {
      _spin.repeat();
    } else if (!_indeterminate && _spin.isAnimating) {
      _spin.stop();
      _spin.value = 0;
    }
  }

  @override
  void dispose() {
    _spin.dispose();
    super.dispose();
  }

  bool get _indeterminate =>
      widget.busy && (widget.progress == null || widget.progress! <= 0);

  @override
  Widget build(BuildContext context) {
    final enabled = widget.onPressed != null || widget.busy;
    final fill = enabled ? widget.color : AppTheme.field;
    final inner = widget.size * 0.78;

    return HelmPulseGlow(
      active: enabled && !widget.busy,
      color: widget.color,
      child: SizedBox.square(
        dimension: widget.size,
        child: AnimatedBuilder(
          animation: _spin,
          builder: (context, _) {
            return Stack(
              alignment: Alignment.center,
              children: [
                CustomPaint(
                  size: Size.square(widget.size),
                  painter: _OrbRingPainter(
                    color: widget.color,
                    progress: widget.progress,
                    spin: _spin.value,
                    busy: widget.busy,
                  ),
                ),
                Material(
                  color: fill.withValues(alpha: enabled ? 1 : 0.7),
                  shape: const CircleBorder(),
                  clipBehavior: Clip.antiAlias,
                  child: InkWell(
                    customBorder: const CircleBorder(),
                    onTap: widget.busy ? null : widget.onPressed,
                    child: SizedBox.square(
                      dimension: inner,
                      child: Center(
                        child: AnimatedSwitcher(
                          duration: const Duration(milliseconds: 280),
                          child: Icon(
                            widget.icon,
                            key: ValueKey(widget.icon),
                            size: inner * 0.42,
                            color: Colors.white.withValues(
                              alpha: enabled ? 1 : 0.45,
                            ),
                          ),
                        ),
                      ),
                    ),
                  ),
                ),
              ],
            );
          },
        ),
      ),
    );
  }
}

class _OrbRingPainter extends CustomPainter {
  _OrbRingPainter({
    required this.color,
    required this.progress,
    required this.spin,
    required this.busy,
  });

  final Color color;
  final double? progress;
  final double spin;
  final bool busy;

  @override
  void paint(Canvas canvas, Size size) {
    final c = Offset(size.width / 2, size.height / 2);
    final stroke = size.width * 0.046;
    final r = size.width / 2 - stroke;
    final rect = Rect.fromCircle(center: c, radius: r);
    const start = -math.pi / 2;

    final track = Paint()
      ..style = PaintingStyle.stroke
      ..strokeWidth = stroke
      ..strokeCap = StrokeCap.round
      ..color = color.withValues(alpha: 0.18);
    canvas.drawArc(rect, 0, math.pi * 2, false, track);

    if (!busy && (progress == null || progress! <= 0)) return;

    final paint = Paint()
      ..style = PaintingStyle.stroke
      ..strokeWidth = stroke
      ..strokeCap = StrokeCap.round
      ..color = color;

    if (progress != null && progress! > 0) {
      canvas.drawArc(
        rect,
        start,
        math.pi * 2 * progress!.clamp(0.0, 1.0),
        false,
        paint,
      );
      return;
    }

    canvas.drawArc(
      rect,
      start + spin * math.pi * 2,
      math.pi * 0.72,
      false,
      paint,
    );
  }

  @override
  bool shouldRepaint(_OrbRingPainter old) {
    return old.color != color ||
        old.progress != progress ||
        old.spin != spin ||
        old.busy != busy;
  }
}

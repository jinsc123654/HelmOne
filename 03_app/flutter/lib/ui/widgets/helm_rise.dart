import 'package:flutter/material.dart';

/// 内容"落位"：从下方**滑入**（只位移，不动透明度）。
///
/// 刻意不用淡入 —— 透明度变化在深色底上看着像"闪一下"；位置变化才是物理感。
/// - 单个卡片直接 `HelmRise(child: ...)`；
/// - 列表用 `HelmRise(child: ..., delay: HelmRise.stagger(i))` 做错峰，
///   延迟自动封顶，长列表不会越到后面越慢。
///
/// 只做一次（首次挂载），之后的 rebuild 不再重放。
class HelmRise extends StatefulWidget {
  /// 创建落位动画。
  const HelmRise({
    super.key,
    required this.child,
    this.delay = Duration.zero,
    this.offset = 0.04,
    this.duration = const Duration(milliseconds: 320),
  });

  /// 子内容。
  final Widget child;

  /// 延迟多久开始（错峰用）。
  final Duration delay;

  /// 起始位移，按自身高度的比例算（0.04 ≈ 4%）。
  final double offset;

  /// 动画时长。
  final Duration duration;

  /// 列表错峰延迟：第 [index] 项延迟多少，最多 [maxDelay]。
  static Duration stagger(
    int index, {
    Duration step = const Duration(milliseconds: 45),
    Duration maxDelay = const Duration(milliseconds: 270),
  }) {
    final d = step * index;
    return d > maxDelay ? maxDelay : d;
  }

  @override
  State<HelmRise> createState() => _HelmRiseState();
}

class _HelmRiseState extends State<HelmRise>
    with SingleTickerProviderStateMixin {
  late final AnimationController _c = AnimationController(
    vsync: this,
    duration: widget.duration,
  );

  @override
  void initState() {
    super.initState();
    if (widget.delay == Duration.zero) {
      _c.forward();
    } else {
      // 延迟期间 widget 可能已经被回收：回来先看 mounted。
      Future<void>.delayed(widget.delay, () {
        if (mounted) _c.forward();
      });
    }
  }

  @override
  void dispose() {
    _c.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return SlideTransition(
      position: Tween<Offset>(
        begin: Offset(0, widget.offset),
        end: Offset.zero,
      ).animate(CurvedAnimation(parent: _c, curve: Curves.easeOutCubic)),
      child: widget.child,
    );
  }
}

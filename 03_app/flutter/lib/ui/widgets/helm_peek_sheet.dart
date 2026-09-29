import 'package:flutter/material.dart';
import 'package:sifli_companion/app/app_theme.dart';

/// 底部详情抽屉：收起只露把手，上拉展开。不参与上方圆钮的居中计算。
class HelmPeekSheet extends StatelessWidget {
  const HelmPeekSheet({
    super.key,
    required this.t,
    required this.handle,
    required this.children,
    required this.onHeaderTap,
    required this.onDragDelta,
    required this.onDragEnd,
    this.peekBody = peekHeight,
  });

  /// 收起时把手区域高度（不含系统底栏）。
  static const peekHeight = 72.0;

  /// 0 收起，1 展开（已做曲线）。
  final double t;
  final String handle;
  final List<Widget> children;
  final VoidCallback onHeaderTap;
  final ValueChanged<double> onDragDelta;
  final ValueChanged<double> onDragEnd;
  final double peekBody;

  /// 收起时占位，让圆钮在把手上方的可见区域里居中。
  static double reserve(BuildContext context, {double peekBody = peekHeight}) {
    return peekBody + MediaQuery.paddingOf(context).bottom;
  }

  @override
  Widget build(BuildContext context) {
    final bottom = MediaQuery.paddingOf(context).bottom;
    final bodyH = MediaQuery.sizeOf(context).height;
    final expanded = t > 0.04;
    final peekH = peekBody + bottom * (1 - t);
    final sheetH = peekH + (bodyH * 0.42 - peekH) * t;
    return SizedBox(
      width: double.infinity,
      height: sheetH.clamp(peekBody, bodyH * 0.5),
      child: Material(
        color: AppTheme.card,
        elevation: 0,
        shadowColor: Colors.transparent,
        surfaceTintColor: Colors.transparent,
        borderRadius: const BorderRadius.vertical(top: Radius.circular(20)),
        clipBehavior: Clip.antiAlias,
        child: Column(
          children: [
            GestureDetector(
              behavior: HitTestBehavior.opaque,
              onTap: onHeaderTap,
              onVerticalDragUpdate: (d) => onDragDelta(d.delta.dy),
              onVerticalDragEnd: (d) => onDragEnd(d.primaryVelocity ?? 0),
              child: Padding(
                padding: EdgeInsets.fromLTRB(
                  16,
                  8,
                  16,
                  expanded ? 12 : 12 + bottom,
                ),
                child: Column(
                  children: [
                    Container(
                      width: 36,
                      height: 4,
                      decoration: BoxDecoration(
                        color: const Color(0xFF5C5C5E),
                        borderRadius: BorderRadius.circular(2),
                      ),
                    ),
                    const SizedBox(height: 10),
                    Text(
                      handle,
                      maxLines: 1,
                      overflow: TextOverflow.ellipsis,
                      style: const TextStyle(
                        color: Colors.white,
                        fontSize: 16,
                        fontWeight: FontWeight.w700,
                        height: 1.2,
                      ),
                    ),
                  ],
                ),
              ),
            ),
            if (expanded)
              Expanded(
                child: ListView(
                  padding: EdgeInsets.fromLTRB(20, 0, 20, 20 + bottom),
                  children: children,
                ),
              ),
          ],
        ),
      ),
    );
  }
}

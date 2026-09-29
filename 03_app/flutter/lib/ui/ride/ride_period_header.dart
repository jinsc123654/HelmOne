import 'package:flutter/material.dart';
import 'package:get/get.dart';
import 'package:sifli_companion/app/app_theme.dart';
import 'package:sifli_companion/i18n/locale_keys.dart';

/// 统计区间的选择器：`◀  2026/8  ▶`，右侧「回到当前」只在看历史时出现。
///
/// 周 / 月都用它，免得两处各写一套箭头和禁用逻辑。给了 [onTapLabel] 时区间名
/// 变成可点的（带一个下拉小箭头）：月视图用它弹月份选择，省得一个月一个月翻。
class RidePeriodHeader extends StatelessWidget {
  /// 创建区间选择器。
  const RidePeriodHeader({
    super.key,
    required this.label,
    required this.canForward,
    required this.onStep,
    this.canBack = true,
    this.onNow,
    this.onTapLabel,
  });

  /// 区间名，如 `本周` / `2026/8` / `8/17 – 8/23`。
  final String label;

  /// 能否往后（不允许看未来）。
  final bool canForward;

  /// 能否往前（再往前没有记录了就别让点）。
  final bool canBack;

  /// 往前 / 往后挪一个周期：-1 往前，+1 往后。
  final void Function(int delta) onStep;

  /// 回到当前区间；为 null 表示已经在当前区间（不显示按钮）。
  final VoidCallback? onNow;

  /// 点区间名（弹月份选择用）；为 null 时区间名就是一行普通文字。
  final VoidCallback? onTapLabel;

  @override
  Widget build(BuildContext context) {
    final title = Text(
      label,
      key: const ValueKey('ridePeriodLabel'),
      textAlign: TextAlign.center,
      maxLines: 1,
      overflow: TextOverflow.ellipsis,
      style: const TextStyle(
        color: Colors.white,
        fontSize: 15,
        fontWeight: FontWeight.w700,
      ),
    );

    final labelButton = InkWell(
      onTap: onTapLabel,
      borderRadius: BorderRadius.circular(8),
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 4, vertical: 4),
        child: Row(
          mainAxisSize: MainAxisSize.min,
          children: [
            // 和右边的箭头等宽：区间名本身才是这一段的中心，箭头挂在它右边。
            if (onTapLabel != null) const SizedBox(width: 18),
            Flexible(child: title),
            if (onTapLabel != null)
              const Icon(Icons.expand_more, size: 18, color: AppTheme.muted),
          ],
        ),
      ),
    );

    // 区间名叠在正中，箭头和「回到当前」各贴一边：谁在与不在都挤不动它。
    // 之前是「三段等宽 + 按需伸缩」，右侧「回到当前」一出现就把月份名推偏了
    // 7px（还会撑出 RenderFlex overflow）。
    return Stack(
      alignment: Alignment.center,
      children: [
        Row(
          children: [
            _arrow(Icons.chevron_left, canBack ? () => onStep(-1) : null),
            const Spacer(),
            _arrow(Icons.chevron_right, canForward ? () => onStep(1) : null),
            if (onNow != null)
              TextButton(
                onPressed: onNow,
                style: TextButton.styleFrom(
                  padding: const EdgeInsets.symmetric(horizontal: 8),
                  minimumSize: const Size(0, 32),
                  tapTargetSize: MaterialTapTargetSize.shrinkWrap,
                ),
                child: Text(
                  LocaleKeys.ridePeriodNow.tr,
                  style: const TextStyle(fontSize: 13),
                ),
              ),
          ],
        ),
        // 让开两侧：右边是「箭头 + 最宽时的回到当前」，左边对称留一样宽，
        // 否则留白不对称，区间名又会被推偏。
        Padding(
          padding: const EdgeInsets.symmetric(horizontal: _sideInset),
          child: labelButton,
        ),
      ],
    );
  }

  /// 区间名两侧要让开的宽度（逻辑像素）。
  ///
  /// 右侧 = 「›」按钮 40 + 「回到当前」约 68；用对称值，居中才是真居中。
  /// 区间名比这还长时省略号收尾，不会压到两边的控件上。
  static const double _sideInset = 96;

  Widget _arrow(IconData icon, VoidCallback? onTap) {
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
      icon: Icon(icon, size: 20),
    );
  }
}

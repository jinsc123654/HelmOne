import 'package:flutter/material.dart';
import 'package:sifli_companion/app/app_theme.dart';

/// 深灰圆角卡片，无描边、无阴影。
class HelmCard extends StatelessWidget {
  const HelmCard({
    super.key,
    required this.child,
    this.padding,
    this.color,
  });

  final Widget child;
  final EdgeInsetsGeometry? padding;
  final Color? color;

  @override
  Widget build(BuildContext context) {
    return Material(
      color: color ?? AppTheme.card,
      elevation: 0,
      shadowColor: Colors.transparent,
      surfaceTintColor: Colors.transparent,
      borderRadius: BorderRadius.circular(AppTheme.radius),
      clipBehavior: Clip.antiAlias,
      child: padding == null
          ? child
          : Padding(padding: padding!, child: child),
    );
  }
}

/// 彩色圆底图标（小米运动列表 leading）。
class HelmIconBadge extends StatelessWidget {
  const HelmIconBadge({
    super.key,
    required this.color,
    required this.icon,
    this.child,
  });

  final Color color;
  final IconData icon;
  final Widget? child;

  @override
  Widget build(BuildContext context) {
    return Container(
      width: 36,
      height: 36,
      decoration: BoxDecoration(color: color, shape: BoxShape.circle),
      alignment: Alignment.center,
      child: child ?? Icon(icon, color: Colors.white, size: 18),
    );
  }
}

/// 卡内浅分割线，从图标右侧起，不用白边。
class HelmHairline extends StatelessWidget {
  const HelmHairline({super.key, this.indent = 64});

  final double indent;

  @override
  Widget build(BuildContext context) {
    return Divider(
      height: 1,
      thickness: 0.5,
      indent: indent,
      color: AppTheme.hairline,
    );
  }
}

/// 把一组列表行收进一张卡，行间用浅分割线。
class HelmTileGroup extends StatelessWidget {
  const HelmTileGroup({super.key, required this.children});

  final List<Widget> children;

  @override
  Widget build(BuildContext context) {
    if (children.isEmpty) return const SizedBox.shrink();
    return Column(
      children: [
        for (var i = 0; i < children.length; i++) ...[
          children[i],
          if (i != children.length - 1) const HelmHairline(),
        ],
      ],
    );
  }
}

/// 彩色圆底 + 标题 + 可选副标题 + 右箭头。
class HelmNavTile extends StatelessWidget {
  const HelmNavTile({
    super.key,
    required this.color,
    required this.icon,
    required this.title,
    this.subtitle,
    this.onTap,
    this.onLongPress,
    this.showChevron = true,
    this.trailing,
    this.leading,
  });

  final Color color;
  final IconData icon;
  final String title;
  final String? subtitle;
  final VoidCallback? onTap;
  final VoidCallback? onLongPress;
  final bool showChevron;
  final Widget? trailing;
  final Widget? leading;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return InkWell(
      onTap: onTap,
      onLongPress: onLongPress,
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 13),
        child: Row(
          children: [
            leading ?? HelmIconBadge(color: color, icon: icon),
            const SizedBox(width: 12),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(
                    title,
                    style: theme.textTheme.titleMedium?.copyWith(
                      color: Colors.white,
                      fontWeight: FontWeight.w500,
                      fontSize: 16,
                    ),
                  ),
                  if (subtitle != null && subtitle!.isNotEmpty) ...[
                    const SizedBox(height: 2),
                    Text(
                      subtitle!,
                      style: theme.textTheme.bodySmall?.copyWith(
                        color: AppTheme.muted,
                        fontSize: 13,
                      ),
                    ),
                  ],
                ],
              ),
            ),
            if (trailing != null)
              trailing!
            else if (showChevron)
              const Icon(Icons.chevron_right, color: AppTheme.muted, size: 22),
          ],
        ),
      ),
    );
  }
}

/// 与 [HelmNavTile] 同结构，右侧是开关。
class HelmSwitchTile extends StatelessWidget {
  const HelmSwitchTile({
    super.key,
    required this.color,
    required this.icon,
    required this.title,
    required this.value,
    required this.onChanged,
    this.subtitle,
    this.subtitleWidget,
  });

  final Color color;
  final IconData icon;
  final String title;
  final String? subtitle;
  final Widget? subtitleWidget;
  final bool value;
  final ValueChanged<bool>? onChanged;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Padding(
      padding: const EdgeInsets.fromLTRB(16, 10, 8, 10),
      child: Row(
        children: [
          HelmIconBadge(color: color, icon: icon),
          const SizedBox(width: 12),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(
                  title,
                  style: theme.textTheme.titleMedium?.copyWith(
                    color: Colors.white,
                    fontWeight: FontWeight.w500,
                    fontSize: 16,
                  ),
                ),
                if (subtitleWidget != null) ...[
                  const SizedBox(height: 2),
                  subtitleWidget!,
                ] else if (subtitle != null && subtitle!.isNotEmpty) ...[
                  const SizedBox(height: 2),
                  Text(
                    subtitle!,
                    style: theme.textTheme.bodySmall?.copyWith(
                      color: AppTheme.muted,
                      fontSize: 13,
                    ),
                  ),
                ],
              ],
            ),
          ),
          Switch(value: value, onChanged: onChanged),
        ],
      ),
    );
  }
}

/// 页顶说明，纯文字不画框。
class HelmPlaceholderNote extends StatelessWidget {
  const HelmPlaceholderNote({super.key, required this.text});

  final String text;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.fromLTRB(4, 0, 4, 4),
      child: Text(
        text,
        style: Theme.of(context).textTheme.bodySmall?.copyWith(
              color: AppTheme.muted,
              height: 1.45,
            ),
      ),
    );
  }
}

/// 大标题（无 AppBar 的 Tab 顶栏）。
class HelmLargeTitle extends StatelessWidget {
  const HelmLargeTitle(this.text, {super.key});

  final String text;

  @override
  Widget build(BuildContext context) {
    return HelmTitleRow(title: text);
  }
}

/// Tab 顶：大标题 + 可选右侧操作（无品牌、无电量胶囊）。
class HelmTitleRow extends StatelessWidget {
  const HelmTitleRow({super.key, required this.title, this.trailing});

  final String title;
  final Widget? trailing;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.fromLTRB(20, 10, 12, 4),
      child: Row(
        children: [
          Expanded(
            child: Text(
              title,
              style: const TextStyle(
                color: Colors.white,
                fontSize: 32,
                fontWeight: FontWeight.w700,
                height: 1.1,
              ),
            ),
          ),
          if (trailing != null) trailing!,
        ],
      ),
    );
  }
}

/// 健康页式三列指标：彩色小图标 + 白字，不用橙色填数字。
class HelmMetricStrip extends StatelessWidget {
  const HelmMetricStrip({super.key, required this.metrics});

  final List<HelmMetric> metrics;

  @override
  Widget build(BuildContext context) {
    return Row(
      children: [
        for (var i = 0; i < metrics.length; i++) ...[
          if (i > 0)
            Container(
              width: 1,
              height: 52,
              margin: const EdgeInsets.symmetric(horizontal: 6),
              color: AppTheme.hairline,
            ),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Row(
                  children: [
                    Icon(metrics[i].icon, size: 14, color: metrics[i].color),
                    const SizedBox(width: 4),
                    Expanded(
                      child: Text(
                        metrics[i].label,
                        maxLines: 1,
                        overflow: TextOverflow.ellipsis,
                        style: const TextStyle(
                          color: AppTheme.muted,
                          fontSize: 12,
                        ),
                      ),
                    ),
                  ],
                ),
                const SizedBox(height: 6),
                Text.rich(
                  TextSpan(
                    text: metrics[i].value,
                    style: const TextStyle(
                      color: Colors.white,
                      fontSize: 22,
                      fontWeight: FontWeight.w700,
                      height: 1.1,
                    ),
                    children: [
                      if (metrics[i].unit != null && metrics[i].unit!.isNotEmpty)
                        TextSpan(
                          text: ' ${metrics[i].unit}',
                          style: const TextStyle(
                            color: AppTheme.muted,
                            fontSize: 12,
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
          ),
        ],
      ],
    );
  }
}

class HelmMetric {
  const HelmMetric({
    required this.icon,
    required this.color,
    required this.label,
    required this.value,
    this.unit,
  });

  final IconData icon;
  final Color color;
  final String label;
  final String value;
  final String? unit;
}

/// 分组小标题。
class HelmSectionTitle extends StatelessWidget {
  const HelmSectionTitle(this.text, {super.key});

  final String text;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.fromLTRB(20, 22, 20, 8),
      child: Text(
        text,
        style: Theme.of(context).textTheme.titleMedium?.copyWith(
              color: Colors.white,
              fontWeight: FontWeight.w600,
              fontSize: 16,
            ),
      ),
    );
  }
}

/// 小米运动式底栏：选中为珊瑚橙 + 小圆点。
class HelmBottomNav extends StatelessWidget {
  const HelmBottomNav({
    super.key,
    required this.index,
    required this.onChanged,
    required this.items,
  });

  final int index;
  final ValueChanged<int> onChanged;
  final List<HelmBottomNavItem> items;

  @override
  Widget build(BuildContext context) {
    return ColoredBox(
      color: AppTheme.background,
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          const ColoredBox(
            color: AppTheme.hairline,
            child: SizedBox(height: 0.5, width: double.infinity),
          ),
          SafeArea(
            top: false,
            child: SizedBox(
              height: 60,
              child: Row(
                children: [
                  for (var i = 0; i < items.length; i++)
                    Expanded(
                      child: InkWell(
                        onTap: () => onChanged(i),
                        splashColor: Colors.transparent,
                        highlightColor: Colors.transparent,
                        child: Column(
                          mainAxisAlignment: MainAxisAlignment.center,
                          children: [
                            () {
                              final color = i == index
                                  ? AppTheme.accent
                                  : AppTheme.muted;
                              final asset = items[i].asset;
                              // 选中态：图标轻微放大 + 颜色渐变，文字平滑变粗，
                              // 而不是三个属性一起"啪"地跳。
                              return TweenAnimationBuilder<double>(
                                tween: Tween<double>(
                                  end: i == index ? 1.12 : 1,
                                ),
                                duration: const Duration(milliseconds: 220),
                                curve: Curves.easeOutCubic,
                                builder: (context, scale, child) =>
                                    Transform.scale(scale: scale, child: child),
                                child: TweenAnimationBuilder<Color?>(
                                  tween: ColorTween(end: color),
                                  duration: const Duration(milliseconds: 220),
                                  builder: (context, c, _) => asset != null
                                      ? ImageIcon(
                                          AssetImage(asset),
                                          size: 24,
                                          color: c,
                                        )
                                      : Icon(items[i].icon, size: 24, color: c),
                                ),
                              );
                            }(),
                            const SizedBox(height: 2),
                            AnimatedDefaultTextStyle(
                              duration: const Duration(milliseconds: 220),
                              style: TextStyle(
                                fontSize: 11,
                                fontWeight: i == index
                                    ? FontWeight.w600
                                    : FontWeight.w400,
                                color: i == index
                                    ? AppTheme.accent
                                    : AppTheme.muted,
                              ),
                              child: Text(items[i].label),
                            ),
                            const SizedBox(height: 4),
                            AnimatedContainer(
                              duration: const Duration(milliseconds: 220),
                              width: i == index ? 4 : 0,
                              height: 4,
                              decoration: const BoxDecoration(
                                color: AppTheme.accent,
                                shape: BoxShape.circle,
                              ),
                            ),
                          ],
                        ),
                      ),
                    ),
                ],
              ),
            ),
          ),
        ],
      ),
    );
  }
}

/// 底部导航的一项。
///
/// [icon] 与 [asset] 二选一：
///   - [icon]：Material 字形，随选中状态变色（用 `Icon` 渲染）。
///   - [asset]：单色图片资产，同样随选中状态变色（用 `ImageIcon` 渲染 ——
///     `Image.color` 默认 `BlendMode.srcIn`，会把图铺成纯色剪影，
///     所以资产请用**白色描边 + 透明底**，不要用彩色图）。
/// 为什么需要 [asset]：Material 图标库里没有"手持码表"这个字形
/// （最接近的 `speed_outlined` 读起来是汽车仪表盘），所以设备 tab 用
/// `assets/icons/bike_computer_nav.png`。
class HelmBottomNavItem {
  const HelmBottomNavItem({required this.label, this.icon, this.asset})
      : assert(icon != null || asset != null, 'icon 与 asset 至少要有一个');

  final IconData? icon;
  final String? asset;
  final String label;
}

/// 对话框自己持有输入框，避免关窗动画期间被提前 dispose。
class HelmPromptDialog extends StatefulWidget {
  const HelmPromptDialog({
    super.key,
    required this.title,
    required this.cancel,
    required this.confirm,
    this.label,
    this.hint,
    this.initial = '',
  });

  final String title;
  final String cancel;
  final String confirm;
  final String? label;
  final String? hint;
  final String initial;

  static Future<String?> show(
    BuildContext context, {
    required String title,
    required String cancel,
    required String confirm,
    String? label,
    String? hint,
    String initial = '',
  }) {
    return showDialog<String>(
      context: context,
      builder: (_) => HelmPromptDialog(
        title: title,
        cancel: cancel,
        confirm: confirm,
        label: label,
        hint: hint,
        initial: initial,
      ),
    );
  }

  @override
  State<HelmPromptDialog> createState() => _HelmPromptDialogState();
}

class _HelmPromptDialogState extends State<HelmPromptDialog> {
  late final TextEditingController _ctrl;

  @override
  void initState() {
    super.initState();
    _ctrl = TextEditingController(text: widget.initial);
  }

  @override
  void dispose() {
    _ctrl.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return AlertDialog(
      title: Text(widget.title),
      content: TextField(
        controller: _ctrl,
        autofocus: true,
        textInputAction: TextInputAction.done,
        onSubmitted: (v) => Navigator.pop(context, v.trim()),
        decoration: InputDecoration(
          labelText: widget.label,
          hintText: widget.hint,
        ),
      ),
      actions: [
        TextButton(
          onPressed: () => Navigator.pop(context),
          child: Text(widget.cancel),
        ),
        FilledButton(
          onPressed: () => Navigator.pop(context, _ctrl.text.trim()),
          child: Text(widget.confirm),
        ),
      ],
    );
  }
}

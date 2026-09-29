import 'package:flutter/material.dart';
import 'package:get/get.dart';
import 'package:sifli_companion/app/app_theme.dart';
import 'package:sifli_companion/i18n/locale_keys.dart';
import 'package:sifli_companion/ui/widgets/helm_chrome.dart';

/// 文件浏览器共用的路径栏：返回上级 + 当前路径 + 右侧操作。
///
/// 「App 后台缓存」和「码表文件操作」是同一件事的两个后端（本机 / GATT），
/// 顶部形状统一走这里，免得两边各长一个样。
class HelmPathBar extends StatelessWidget {
  /// 创建路径栏。
  const HelmPathBar({
    super.key,
    required this.path,
    this.onUp,
    this.summary,
    this.actions = const <Widget>[],
  });

  /// 当前路径（已格式化好，如 `/osm/city`）。
  final String path;

  /// 返回上一级；为 null 表示已在根目录。
  final VoidCallback? onUp;

  /// 路径下方的一行汇总，如 `12 项 · 合计 3.4 MB`。
  final String? summary;

  /// 右侧操作按钮。
  final List<Widget> actions;

  @override
  Widget build(BuildContext context) {
    final sub = summary;
    return Padding(
      padding: const EdgeInsets.fromLTRB(4, 2, 6, 2),
      child: Row(
        children: [
          IconButton(
            tooltip: LocaleKeys.pathUp.tr,
            onPressed: onUp,
            icon: const Icon(Icons.arrow_upward),
          ),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              mainAxisSize: MainAxisSize.min,
              children: [
                Text(
                  path,
                  overflow: TextOverflow.ellipsis,
                  style: const TextStyle(
                    fontFamily: 'monospace',
                    fontWeight: FontWeight.w600,
                    fontSize: 14,
                  ),
                ),
                if (sub != null && sub.isNotEmpty)
                  Text(
                    sub,
                    overflow: TextOverflow.ellipsis,
                    style: const TextStyle(
                      color: AppTheme.muted,
                      fontSize: 12,
                    ),
                  ),
              ],
            ),
          ),
          ...actions,
        ],
      ),
    );
  }
}

/// 卡片式文件列表：整块圆角卡，逐行分隔线。
///
/// 惰性构建而不是一次性 Column —— `osm/`、通知图标这类缓存目录可能有上千项，
/// 全铺开会直接卡住首帧。
class HelmFileList extends StatelessWidget {
  /// 创建列表。
  const HelmFileList({
    super.key,
    required this.count,
    required this.itemBuilder,
  });

  /// 条目数。
  final int count;

  /// 行构造器。
  final Widget Function(BuildContext context, int index) itemBuilder;

  @override
  Widget build(BuildContext context) {
    return ClipRRect(
      borderRadius: BorderRadius.circular(AppTheme.radius),
      child: ListView.builder(
        padding: EdgeInsets.zero,
        itemCount: count,
        itemBuilder: (ctx, i) => Material(
          color: AppTheme.card,
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              itemBuilder(ctx, i),
              if (i != count - 1) const HelmHairline(),
            ],
          ),
        ),
      ),
    );
  }
}

/// 文件 / 目录条目可执行的一项操作。
class HelmAction {
  /// 创建操作项。
  const HelmAction({
    required this.icon,
    required this.label,
    this.destructive = false,
    this.onTap,
  });

  /// 图标。
  final IconData icon;

  /// 文案。
  final String label;

  /// 危险操作（红色）。
  final bool destructive;

  /// 点击回调；弹窗会先关掉再执行。
  final VoidCallback? onTap;
}

/// 条目操作菜单。
///
/// 之前码表文件页是「每行下面再挂 4 个 IconButton」——一行文件占两行高度，
/// 4 个图标挤在一起还容易点错。改成点行尾「更多」弹这个菜单。
abstract final class HelmActionSheet {
  /// 弹出操作菜单。
  static Future<void> show(
    BuildContext context, {
    required String title,
    required List<HelmAction> actions,
  }) async {
    if (actions.isEmpty) return;
    await showModalBottomSheet<void>(
      context: context,
      backgroundColor: AppTheme.card,
      builder: (ctx) => SafeArea(
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            Padding(
              padding: const EdgeInsets.fromLTRB(20, 16, 20, 12),
              child: Row(
                children: [
                  Expanded(
                    child: Text(
                      title,
                      maxLines: 2,
                      overflow: TextOverflow.ellipsis,
                      style: const TextStyle(
                        color: Colors.white,
                        fontWeight: FontWeight.w600,
                        fontSize: 16,
                      ),
                    ),
                  ),
                ],
              ),
            ),
            const HelmHairline(indent: 0),
            for (final a in actions)
              ListTile(
                leading: Icon(
                  a.icon,
                  color: a.destructive ? AppTheme.danger : Colors.white,
                ),
                title: Text(
                  a.label,
                  style: TextStyle(
                    color: a.destructive ? AppTheme.danger : Colors.white,
                  ),
                ),
                onTap: () {
                  // 先关菜单再执行：回调里常常要 `await`，留着菜单会挡住
                  // 后续的确认弹窗和真机上的分享面板。
                  Navigator.pop(ctx);
                  a.onTap?.call();
                },
              ),
            const SizedBox(height: 6),
          ],
        ),
      ),
    );
  }
}

/// 「删除」确认弹窗，两页共用。
Future<bool> helmConfirmDelete(
  BuildContext context, {
  required String title,
  required String body,
}) async {
  final ok = await Get.dialog<bool>(
    AlertDialog(
      title: Text(title),
      content: Text(body),
      actions: [
        TextButton(
          onPressed: () => Get.back(result: false),
          child: Text(LocaleKeys.cancel.tr),
        ),
        TextButton(
          onPressed: () => Get.back(result: true),
          child: Text(
            LocaleKeys.actionDelete.tr,
            style: const TextStyle(color: AppTheme.danger),
          ),
        ),
      ],
    ),
  );
  return ok == true;
}

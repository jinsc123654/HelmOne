import 'dart:async';

import 'package:flutter/material.dart';
import 'package:get/get.dart';
import 'package:sifli_companion/app/app_theme.dart';
import 'package:sifli_companion/i18n/locale_keys.dart';
import 'package:sifli_companion/transfer/transfer_center.dart';

/// 右上角的后台抓取悬浮球。
///
/// 常态**不显示**；一旦有任务在跑，就从屏幕右侧滑出来（收起态是个小球，
/// 带进度环和百分比），点一下展开成任务卡，再点收起。任务结束自动滑回去。
///
/// 挂在 `GetMaterialApp.builder` 里，位置在所有路由**之外**，所以切页 /
/// 返回都带不走它 —— 这是需求「页面退出不知道是不是还在抓取」的落点。
///
/// 这一层没有 `Overlay`、也没有 `Material` 的约束链，所以：
/// 只读 [TransferCenter]，不自己发请求；不用 `tooltip:`（`RawTooltip`
/// 会找不到 `Overlay` 祖先）；宽度必须由 [Positioned.width] 钉死。
class TransferOverlay extends StatefulWidget {
  /// 创建悬浮球。
  const TransferOverlay({super.key});

  @override
  State<TransferOverlay> createState() => _TransferOverlayState();
}

class _TransferOverlayState extends State<TransferOverlay>
    with SingleTickerProviderStateMixin {
  /// 0 = 完全滑出屏幕右侧，1 = 停在右侧。
  late final AnimationController _slide;

  /// 展开成任务卡。
  bool _expanded = false;

  /// 是否应该有球（有任务在跑，或有事失败等着看）。
  bool _visible = false;

  /// 球直径。
  static const _ballSize = 56.0;

  /// 卡片最大宽度；窄屏再收一点。
  static const _panelMax = 320.0;

  StreamSubscription<void>? _sub;

  @override
  void initState() {
    super.initState();
    _slide = AnimationController(
      vsync: this,
      duration: const Duration(milliseconds: 280),
    );
    final center = TransferCenter.to;
    _visible = center.hasWork;
    if (_visible) _slide.value = 1;
    _sub = center.changes.listen((_) => _sync());
  }

  @override
  void dispose() {
    _sub?.cancel();
    _slide.dispose();
    super.dispose();
  }

  /// 任务表变了：跟着切换滑入/滑出，并重画进度。
  void _sync() {
    if (!mounted) return;
    final has = TransferCenter.to.hasWork;
    if (has != _visible) {
      _visible = has;
      if (has) {
        _slide.forward();
      } else {
        // 收回去的时候顺手把展开态也复位，下次滑出是收起的小球。
        _expanded = false;
        _slide.reverse();
      }
    }
    setState(() {});
  }

  void _toggle() => setState(() => _expanded = !_expanded);

  @override
  Widget build(BuildContext context) {
    final mq = MediaQuery.of(context);
    // 避开状态栏 + 常见 AppBar 高度，否则会压住页面右上角的操作按钮。
    final y = mq.padding.top + kToolbarHeight + 6;
    final panelW = _panelMax.clamp(0.0, mq.size.width * 0.74);

    return Positioned(
      top: y,
      right: 12,
      width: panelW,
      child: AnimatedBuilder(
        animation: _slide,
        builder: (context, child) {
          final t = Curves.easeOutCubic.transform(_slide.value);
          return FractionalTranslation(
            // 按自身宽度平移：1-t 正好是「整块在屏幕右外侧」到「贴住右边」。
            translation: Offset(1.0 - t, 0),
            child: Opacity(
              opacity: t,
              child: IgnorePointer(ignoring: !_visible, child: child),
            ),
          );
        },
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.end,
          children: [
            _ball(),
            // 展开/收起用 AnimatedSize，高度变化不突兀。
            AnimatedSize(
              duration: const Duration(milliseconds: 220),
              curve: Curves.easeOutCubic,
              alignment: Alignment.topRight,
              child: _expanded
                  ? Padding(
                      padding: const EdgeInsets.only(top: 8),
                      child: _panel(),
                    )
                  : const SizedBox(width: double.infinity),
            ),
          ],
        ),
      ),
    );
  }

  /// 收起态：带进度环的小球。
  Widget _ball() {
    final center = TransferCenter.to;
    final running = center.runningTasks;
    final failed = center.tasks.any(
      (t) => t.status == TransferStatus.failed,
    );
    final lead = running.isNotEmpty
        ? running.first
        : (center.tasks.isNotEmpty ? center.tasks.first : null);

    final color = failed && running.isEmpty
        ? AppTheme.iconOta
        : AppTheme.accent;

    return Semantics(
      button: true,
      label: lead?.title ?? LocaleKeys.captureTitle.tr,
      child: GestureDetector(
        onTap: _toggle,
        behavior: HitTestBehavior.opaque,
        child: SizedBox(
          width: _ballSize,
          height: _ballSize,
          child: Stack(
            alignment: Alignment.center,
            children: [
              // 底盘
              Container(
                width: _ballSize,
                height: _ballSize,
                decoration: BoxDecoration(
                  color: AppTheme.card,
                  shape: BoxShape.circle,
                  border: Border.all(
                    color: color.withValues(alpha: 0.55),
                    width: 1.2,
                  ),
                  boxShadow: const [
                    BoxShadow(
                      color: Colors.black54,
                      blurRadius: 12,
                      offset: Offset(0, 3),
                    ),
                  ],
                ),
              ),
              // 进度环：**始终显示真实进度**。以前展开时传 null（转圈），
              // 于是"圆环"变成不确定态、进度反而只在卡片里那条细线能看到。
              SizedBox(
                width: _ballSize - 14,
                height: _ballSize - 14,
                child: CircularProgressIndicator(
                  value: running.isEmpty ? 1 : lead?.fraction,
                  strokeWidth: 3,
                  color: color,
                  backgroundColor: AppTheme.hairline,
                ),
              ),
              // 中心：百分比 / 状态图标
              _ballCenter(lead, running.isNotEmpty, failed),
              // 多个任务时右上角挂计数
              if (running.length > 1)
                Positioned(
                  top: 0,
                  right: 0,
                  child: Container(
                    padding: const EdgeInsets.symmetric(
                      horizontal: 5,
                      vertical: 1,
                    ),
                    decoration: BoxDecoration(
                      color: AppTheme.accent,
                      borderRadius: BorderRadius.circular(9),
                      border: Border.all(color: AppTheme.card, width: 1.5),
                    ),
                    child: Text(
                      '${running.length}',
                      style: const TextStyle(
                        color: Colors.white,
                        fontSize: 11,
                        fontWeight: FontWeight.w700,
                      ),
                    ),
                  ),
                ),
            ],
          ),
        ),
      ),
    );
  }

  /// 球心内容：**还在跑就显示"在同步什么"的图标**（进度由外圈圆环表达），
  /// 结束了才显示状态图标。
  ///
  /// 以前球心写百分比：一是和外圈圆环表达同一件事（重复），二是小球里数字只有
  /// 两三位、看不清也不好看；换成图标后，一眼能看出这次同步的是星历、记录、
  /// 通知图标还是固件。
  Widget _ballCenter(TransferTask? t, bool running, bool failed) {
    if (!running) {
      return Icon(
        failed ? Icons.error_outline : Icons.check,
        size: 20,
        color: failed ? AppTheme.iconOta : AppTheme.iconKeep,
      );
    }
    if (t == null) {
      return const SizedBox.shrink();
    }
    return Icon(
      _kindIcon(t.kind),
      size: 20,
      color: AppTheme.accent,
    );
  }

  /// 任务类型 → 球心图标（`kind` 取值见各发起页的 `_kind` 常量）。
  IconData _kindIcon(String kind) => switch (kind) {
        'eph' => Icons.satellite_alt_outlined,
        'mcu_sync' => Icons.directions_bike_outlined,
        'nav_push' => Icons.route_outlined,
        'fav_push' => Icons.star_outline,
        'notif_icons' => Icons.notifications_active_outlined,
        'fs' => Icons.folder_open_outlined,
        'log' => Icons.article_outlined,
        'import_upload' => Icons.upload_file_outlined,
        'ota' => Icons.system_update_alt,
        _ => Icons.sync,
      };

  /// 展开态：任务卡列表。
  Widget _panel() {
    final tasks = TransferCenter.to.tasks;
    final shown = tasks.take(3).toList();
    final hidden = tasks.length - shown.length;

    return Container(
      decoration: BoxDecoration(
        color: AppTheme.card,
        borderRadius: BorderRadius.circular(14),
        border: Border.all(color: AppTheme.hairline),
        boxShadow: const [
          BoxShadow(
            color: Colors.black54,
            blurRadius: 14,
            offset: Offset(0, 5),
          ),
        ],
      ),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          for (var i = 0; i < shown.length; i++) ...[
            if (i > 0)
              const Divider(height: 1, thickness: 0.5, color: AppTheme.hairline),
            _row(shown[i]),
          ],
          if (hidden > 0)
            Padding(
              padding: const EdgeInsets.fromLTRB(12, 0, 12, 10),
              child: Text(
                LocaleKeys.transferMore.trParams({'n': '$hidden'}),
                textAlign: TextAlign.center,
                style: const TextStyle(color: AppTheme.muted, fontSize: 12),
              ),
            ),
        ],
      ),
    );
  }

  Widget _row(TransferTask task) {
    final running = task.isRunning;
    final color = switch (task.status) {
      TransferStatus.running => AppTheme.accent,
      TransferStatus.done => AppTheme.iconKeep,
      TransferStatus.failed => AppTheme.iconOta,
      TransferStatus.cancelled => AppTheme.muted,
    };
    final speed = running && task.kbs > 0
        ? LocaleKeys.transferSpeed.trParams({
            'kbs': task.kbs.toStringAsFixed(1),
          })
        : '';
    final detail = task.phase.isNotEmpty
        ? task.phase
        : (task.total > 0 ? '${task.done} / ${task.total}' : '');

    return Padding(
      padding: const EdgeInsets.fromLTRB(12, 10, 6, 10),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              // 结束态给个小图标：卡片里没有进度条，光看标题分不出成功还是失败。
              if (!running) ...[
                Icon(
                  switch (task.status) {
                    TransferStatus.done => Icons.check_circle_outline,
                    TransferStatus.failed => Icons.error_outline,
                    TransferStatus.cancelled => Icons.remove_circle_outline,
                    TransferStatus.running => Icons.circle_outlined,
                  },
                  size: 16,
                  color: color,
                ),
                const SizedBox(width: 6),
              ],
              Expanded(
                child: Text(
                  task.title,
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: const TextStyle(
                    color: Colors.white,
                    fontSize: 14,
                    fontWeight: FontWeight.w600,
                  ),
                ),
              ),
              if (running)
                Text(
                  '${task.percent}%',
                  style: const TextStyle(
                    color: AppTheme.muted,
                    fontSize: 13,
                    fontFeatures: [FontFeature.tabularFigures()],
                  ),
                ),
              if (running && task.cancellable)
                Semantics(
                  label: LocaleKeys.cancel.tr,
                  button: true,
                  child: IconButton(
                    visualDensity: VisualDensity.compact,
                    onPressed: () => TransferCenter.to.cancel(task),
                    icon: const Icon(
                      Icons.close,
                      size: 18,
                      color: AppTheme.muted,
                    ),
                  ),
                )
              else if (!running)
                Semantics(
                  label: LocaleKeys.transferDismiss.tr,
                  button: true,
                  child: IconButton(
                    visualDensity: VisualDensity.compact,
                    onPressed: () => TransferCenter.to.dismiss(task),
                    icon: const Icon(
                      Icons.close,
                      size: 18,
                      color: AppTheme.muted,
                    ),
                  ),
                )
              else
                const SizedBox(width: 6),
            ],
          ),
          // 标题右边已经有百分比了；这里**不再画一条压在文字下面的进度线** ——
          // 那条线在紧凑卡片里看着像文字下划线（用户："文字下面有黄色横线，很怪"），
          // 进度由球上的圆环 + 右侧百分比表达。
          if (detail.isNotEmpty || speed.isNotEmpty) ...[
            const SizedBox(height: 5),
            Row(
              children: [
                Expanded(
                  child: Text(
                    detail,
                    maxLines: 1,
                    overflow: TextOverflow.ellipsis,
                    style: const TextStyle(
                      color: AppTheme.muted,
                      fontSize: 12,
                    ),
                  ),
                ),
                if (speed.isNotEmpty)
                  Text(
                    speed,
                    style: const TextStyle(
                      color: AppTheme.muted,
                      fontSize: 12,
                      fontFeatures: [FontFeature.tabularFigures()],
                    ),
                  ),
              ],
            ),
          ],
          if (task.error != null && !running) ...[
            const SizedBox(height: 4),
            Text(
              task.error!,
              maxLines: 2,
              overflow: TextOverflow.ellipsis,
              style: const TextStyle(color: AppTheme.iconOta, fontSize: 11),
            ),
          ],
        ],
      ),
    );
  }
}

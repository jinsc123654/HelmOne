import 'dart:async';

import 'package:flutter/material.dart';
import 'package:get/get.dart';
import 'package:share_plus/share_plus.dart';
import 'package:sifli_companion/app/app_theme.dart';
import 'package:sifli_companion/ble/companion_client.dart';
import 'package:sifli_companion/i18n/locale_keys.dart';
import 'package:sifli_companion/log/captured_log_store.dart';
import 'package:sifli_companion/log/log_capture_sync.dart';
import 'package:sifli_companion/ride/gpx_util.dart';
import 'package:sifli_companion/transfer/transfer_center.dart';
import 'package:sifli_companion/ui/device/captured_log_view_page.dart';
import 'package:sifli_companion/ui/widgets/helm_chrome.dart';

/// 日志抓取页：顶部一个「抓取」按键，下面崩溃记录 / 诊断日志两个分区。
///
/// 两条通道一次抓完。抓取过程本身不依赖本页 —— 进度登记在
/// [TransferCenter]，由全局悬浮框展示，所以中途退出页面也能看到
/// 还在不在抓、多快、抓到哪了。
class LogCapturePage extends StatefulWidget {
  /// 创建日志抓取页。
  const LogCapturePage({super.key});

  @override
  State<LogCapturePage> createState() => _LogCapturePageState();
}

class _LogCapturePageState extends State<LogCapturePage> {
  final _client = CompanionClient.instance;

  List<CapturedLog> _crashes = const [];
  List<CapturedLog> _diags = const [];
  bool _loading = true;

  /// 用户点了取消；由 [LogCaptureSync] 在文件边界检查。
  bool _cancel = false;

  /// 任务类别，用来跨页面判断「是不是已经在抓了」。
  static const _kind = 'log';

  @override
  void initState() {
    super.initState();
    unawaited(_reload());
  }

  Future<void> _reload() async {
    final crashes = await CapturedLogStore.coredump.list();
    final diags = await CapturedLogStore.diag.list();
    if (!mounted) return;
    setState(() {
      _crashes = crashes;
      _diags = diags;
      _loading = false;
    });
  }

  String _sizeLabel(int n) {
    if (n >= 1024 * 1024) {
      return '${(n / (1024 * 1024)).toStringAsFixed(1)} MB';
    }
    if (n >= 1024) return '${(n / 1024).toStringAsFixed(0)} KB';
    return '$n B';
  }

  /// 悬浮框里的阶段文案：「诊断日志 2/5」。
  String _phaseOf(LogCaptureProgress p) {
    final name = p.stage == LogCaptureStage.coredump
        ? LocaleKeys.captureCoredumpSection.tr
        : LocaleKeys.captureDiagSection.tr;
    if (p.filesTotal <= 0) return name;
    return '$name ${p.filesDone + 1}/${p.filesTotal}';
  }

  String _resultText(LogCaptureResult r) {
    if (r.total <= 0) return LocaleKeys.captureNothing.tr;
    return LocaleKeys.captureDoneBoth.trParams({
      'crash': '${r.coredump}',
      'diag': '${r.diag}',
    });
  }

  Future<void> _pull() async {
    final center = TransferCenter.to;
    // 页面可能刚被重建，本地没有「在抓」的标志 —— 回到中心问。
    if (center.isKindRunning(_kind)) return;

    if (!_client.isReady) {
      AppTheme.snack(LocaleKeys.captureTitle.tr, LocaleKeys.coredumpNeedBle.tr);
      return;
    }
    if (_client.lastStatus?.isTransferBusy == true) {
      AppTheme.snack(LocaleKeys.captureTitle.tr, LocaleKeys.coredumpMtpBusy.tr);
      return;
    }

    _cancel = false;
    final task = center.begin(
      title: LocaleKeys.captureTitle.tr,
      kind: _kind,
      cancellable: true,
      onCancel: () => _cancel = true,
    );

    // 故意不 await 到本页：抓取必须能脱离页面活下去。
    unawaited(() async {
      try {
        final r = await LogCaptureSync.pull(
          _client,
          shouldStop: () => _cancel,
          onProgress: (p) => center.progress(
            task,
            done: p.bytesDone,
            total: p.bytesTotal,
            transferred: p.bytesTransferred,
            phase: _phaseOf(p),
          ),
        );
        if (r.stopped) {
          // 用户从悬浮框取消时状态已经落过，这里只兜底。
          if (task.isRunning) center.cancel(task);
        } else {
          // 带上结果文案："没日志可抓"这种零条目的收尾，光一个 ✓ 看不出所以然。
          center.finish(task, message: _resultText(r));
        }
        await _reload();
        if (!mounted) return;
        AppTheme.snack(LocaleKeys.captureTitle.tr, _resultText(r));
      } catch (e, st) {
        center.fail(task, e);
        if (!mounted) return;
        AppTheme.fail(LocaleKeys.captureTitle.tr, 'LogCapture', e, st);
      }
    }());
  }

  String _subtitle(CapturedLog it) {
    return '${LocaleKeys.captureOriginalName.trParams({'name': it.name})}\n'
        '${_sizeLabel(it.size)} · '
        '${LocaleKeys.coredumpPulledAt.trParams({'time': GpxUtil.formatStamp(it.pulledAt)})}';
  }

  Future<void> _open(CapturedLog it) async {
    await Get.to<void>(() => CapturedLogViewPage(item: it));
  }

  Future<void> _share(CapturedLog it) async {
    try {
      await SharePlus.instance.share(
        ShareParams(
          files: [XFile(it.file.path, mimeType: 'text/plain', name: it.name)],
          subject: it.name,
          text: LocaleKeys.coredumpShareSubject.tr,
        ),
      );
    } catch (e, st) {
      AppTheme.fail(LocaleKeys.captureTitle.tr, 'LogShare', e, st);
    }
  }

  Future<bool> _confirm({required String title, required String body}) async {
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
            child: Text(LocaleKeys.confirm.tr),
          ),
        ],
      ),
    );
    return ok == true;
  }

  Future<void> _delete(CapturedLog it, CapturedLogStore store) async {
    final ok = await _confirm(
      title: LocaleKeys.coredumpDelete.tr,
      body: LocaleKeys.coredumpDeleteConfirm.trParams({'name': it.name}),
    );
    if (!ok) return;
    try {
      await store.delete(it);
      await _reload();
    } catch (e, st) {
      AppTheme.fail(LocaleKeys.captureTitle.tr, 'LogDelete', e, st);
    }
  }

  Future<void> _clear(CapturedLogStore store) async {
    final ok = await _confirm(
      title: LocaleKeys.coredumpClearConfirmTitle.tr,
      body: LocaleKeys.coredumpClearConfirmBody.tr,
    );
    if (!ok) return;
    try {
      await store.clearAll();
      await _reload();
    } catch (e, st) {
      AppTheme.fail(LocaleKeys.captureTitle.tr, 'LogClear', e, st);
    }
  }

  Widget _section({
    required String title,
    required String sub,
    required String empty,
    required List<CapturedLog> items,
    required CapturedLogStore store,
    required Color color,
    required IconData icon,
  }) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        Row(
          children: [
            Expanded(child: HelmSectionTitle(title)),
            if (items.isNotEmpty)
              Padding(
                padding: const EdgeInsets.only(right: 8, top: 12),
                child: IconButton(
                  visualDensity: VisualDensity.compact,
                  tooltip: LocaleKeys.coredumpClear.tr,
                  onPressed: () => _clear(store),
                  icon: const Icon(
                    Icons.delete_outline,
                    size: 20,
                    color: AppTheme.muted,
                  ),
                ),
              ),
          ],
        ),
        Padding(
          padding: const EdgeInsets.fromLTRB(20, 0, 20, 10),
          child: Text(
            sub,
            style: const TextStyle(color: AppTheme.muted, fontSize: 13),
          ),
        ),
        Padding(
          padding: const EdgeInsets.symmetric(horizontal: 16),
          child: items.isEmpty
              ? HelmCard(
                  padding: const EdgeInsets.all(20),
                  child: Text(
                    empty,
                    style: const TextStyle(color: AppTheme.muted),
                  ),
                )
              : HelmCard(
                  child: HelmTileGroup(
                    children: [
                      for (final it in items)
                        HelmNavTile(
                          color: color,
                          icon: icon,
                          title: LocaleKeys.captureAt
                              .trParams({'time': it.title}),
                          subtitle: _subtitle(it),
                          // 点整行看内容；分享/删除靠右侧两个按钮。
                          showChevron: false,
                          onTap: () => _open(it),
                          trailing: Row(
                            mainAxisSize: MainAxisSize.min,
                            children: [
                              // 一行里既有「点开看」又有「分享/删除」，
                              // 加个箭头暗示整行可点，否则用户只会看到两个按钮。
                              const Icon(
                                Icons.chevron_right,
                                color: AppTheme.muted,
                                size: 20,
                              ),
                              IconButton(
                                tooltip: LocaleKeys.coredumpShare.tr,
                                onPressed: () => _share(it),
                                icon: const Icon(Icons.ios_share),
                              ),
                              IconButton(
                                tooltip: LocaleKeys.coredumpDelete.tr,
                                onPressed: () => _delete(it, store),
                                icon: const Icon(Icons.delete_outline),
                              ),
                            ],
                          ),
                        ),
                    ],
                  ),
                ),
        ),
      ],
    );
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: Text(LocaleKeys.captureTitle.tr),
        actions: [
          Obx(() {
            // 读一下 tasks 才会订阅刷新；任务结束时 finish()/fail()
            // 都会 refresh，按钮状态跟着回弹。
            final running = TransferCenter.to.tasks.isNotEmpty &&
                TransferCenter.to.isKindRunning(_kind);
            return TextButton(
              onPressed: running ? null : _pull,
              child: Text(
                running
                    ? LocaleKeys.coredumpPulling.tr
                    : LocaleKeys.coredumpPull.tr,
              ),
            );
          }),
        ],
      ),
      body: _loading
          ? const Center(child: CircularProgressIndicator())
          : ListView(
              padding: const EdgeInsets.fromLTRB(0, 4, 0, 32),
              children: [
                _section(
                  title: LocaleKeys.captureCoredumpSection.tr,
                  sub: LocaleKeys.captureCoredumpSub.tr,
                  empty: LocaleKeys.coredumpEmpty.tr,
                  items: _crashes,
                  store: CapturedLogStore.coredump,
                  color: AppTheme.iconCoredump,
                  icon: Icons.bug_report_outlined,
                ),
                const SizedBox(height: 8),
                _section(
                  title: LocaleKeys.captureDiagSection.tr,
                  sub: LocaleKeys.captureDiagSub.tr,
                  empty: LocaleKeys.captureDiagEmpty.tr,
                  items: _diags,
                  store: CapturedLogStore.diag,
                  color: AppTheme.iconLog,
                  icon: Icons.description_outlined,
                ),
              ],
            ),
    );
  }
}

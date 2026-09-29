import 'dart:io';

import 'package:flutter/material.dart';
import 'package:get/get.dart';
import 'package:path/path.dart' as p;
import 'package:share_plus/share_plus.dart';
import 'package:sifli_companion/app/app_theme.dart';
import 'package:sifli_companion/i18n/locale_keys.dart';
import 'package:sifli_companion/log/app_log.dart';
import 'package:sifli_companion/ui/widgets/helm_chrome.dart';
import 'package:sifli_companion/ui/widgets/log_text_view.dart';
import 'package:sifli_companion/util/byte_format.dart';

/// 本机日志 / 崩溃文件列表，点开看全文。
///
/// 之前是「固定 160 px 列表 + 下方正文」的上下分栏：列表只露 4 行、正文
/// 又窄，两边都不够用。现在改成整页可滚动的分组列表，点一行进全屏阅读
///（[LogTextPage]），与「日志抓取」页的形状保持一致。
class LogViewerPage extends StatefulWidget {
  /// 创建日志查看页。
  const LogViewerPage({super.key});

  @override
  State<LogViewerPage> createState() => _LogViewerPageState();
}

class _LogViewerPageState extends State<LogViewerPage> {
  List<File> _crashes = const [];
  List<File> _daily = const [];
  bool _loading = true;
  bool _exporting = false;

  @override
  void initState() {
    super.initState();
    _reload();
  }

  Future<void> _reload() async {
    final files = await AppLog.listFiles();
    if (!mounted) return;
    // AppLog 已按路径倒序给出（新的在前）。这里再按用途分成两组，
    // 崩溃记录优先显示 —— 那是打开这一页最想看的东西。
    setState(() {
      _crashes = [for (final f in files) if (_isCrash(f)) f];
      _daily = [for (final f in files) if (!_isCrash(f)) f];
      _loading = false;
    });
  }

  static bool _isCrash(File f) => p.basename(f.path).startsWith('crash_');

  /// `app_20260916.log` → `2026/9/16`。
  ///
  /// 按日文件的名字就是日期，直接解析比拿 mtime 更稳（拷来拷去不会变）。
  static String _dayLabel(String baseName) {
    final m = RegExp(r'^app_(\d{4})(\d{2})(\d{2})\.log$').firstMatch(baseName);
    if (m == null) return baseName;
    final y = m.group(1)!;
    final mo = int.tryParse(m.group(2)!) ?? 0;
    final d = int.tryParse(m.group(3)!) ?? 0;
    return '$y/$mo/$d';
  }

  /// `crash_20260916-212303.408.log` → `2026/9/16 21:23:03`。
  static String _crashLabel(String baseName) {
    final m = RegExp(
      r'^crash_(\d{4})(\d{2})(\d{2})-(\d{2})(\d{2})(\d{2})',
    ).firstMatch(baseName);
    if (m == null) return baseName;
    return '${m.group(1)}/${int.parse(m.group(2)!)}/${int.parse(m.group(3)!)} '
        '${m.group(4)}:${m.group(5)}:${m.group(6)}';
  }

  Future<void> _open(File file) async {
    await Get.to<void>(() => LogTextPage(file: file));
    // 阅读页里可能没动文件，但回来的成本很低，刷新一下免得状态漂移。
    if (mounted) await _reload();
  }

  Future<void> _share(File file) async {
    try {
      await AppLog.flush();
      await SharePlus.instance.share(
        ShareParams(
          files: [XFile(file.path, mimeType: 'text/plain')],
          subject: p.basename(file.path),
          text: LocaleKeys.logExportSubject.tr,
        ),
      );
    } catch (e, st) {
      AppTheme.fail(LocaleKeys.logTitle.tr, 'LogExport', e, st);
    }
  }

  Future<void> _delete(File file) async {
    final name = p.basename(file.path);
    final ok = await Get.dialog<bool>(
      AlertDialog(
        title: Text(LocaleKeys.logDelete.tr),
        content: Text(
          LocaleKeys.logDeleteConfirm.trParams({'name': name}),
        ),
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
    if (ok != true) return;
    await AppLog.deleteFile(file);
    await _reload();
  }

  Future<void> _clear() async {
    final ok = await Get.dialog<bool>(
      AlertDialog(
        title: Text(LocaleKeys.logClearConfirmTitle.tr),
        content: Text(LocaleKeys.logClearConfirmBody.tr),
        actions: [
          TextButton(
            onPressed: () => Get.back(result: false),
            child: Text(LocaleKeys.cancel.tr),
          ),
          FilledButton(
            onPressed: () => Get.back(result: true),
            child: Text(LocaleKeys.confirm.tr),
          ),
        ],
      ),
    );
    if (ok != true) return;
    await AppLog.clearAll();
    await _reload();
    if (mounted) {
      AppTheme.snack(LocaleKeys.logTitle.tr, LocaleKeys.logClear.tr);
    }
  }

  Future<void> _writeSampleCrash() async {
    await AppLog.crash(
      'ManualTest',
      'sample crash for local viewer',
      StackTrace.current,
    );
    await AppLog.i('LogViewer', 'wrote sample crash');
    await _reload();
    if (mounted) {
      AppTheme.snack(
        LocaleKeys.logTitle.tr,
        LocaleKeys.logSampleCrashDone.tr,
      );
    }
  }

  Future<void> _exportAllZip() async {
    if (_exporting) return;
    if (_crashes.isEmpty && _daily.isEmpty) {
      AppTheme.snack(LocaleKeys.logTitle.tr, LocaleKeys.logEmpty.tr);
      return;
    }
    setState(() => _exporting = true);
    try {
      final zip = await AppLog.exportZip();
      if (zip == null) {
        if (mounted) {
          AppTheme.snack(LocaleKeys.logTitle.tr, LocaleKeys.logEmpty.tr);
        }
        return;
      }
      await SharePlus.instance.share(
        ShareParams(
          files: [XFile(zip.path, mimeType: 'application/zip')],
          subject: p.basename(zip.path),
          text: LocaleKeys.logExportSubject.tr,
        ),
      );
    } catch (e, st) {
      AppTheme.fail(LocaleKeys.logTitle.tr, 'LogExport', e, st);
    } finally {
      if (mounted) setState(() => _exporting = false);
    }
  }

  Future<void> _showMenu() async {
    final pick = await showModalBottomSheet<String>(
      context: context,
      backgroundColor: AppTheme.card,
      builder: (ctx) => SafeArea(
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            ListTile(
              leading: const Icon(Icons.folder_zip_outlined),
              title: Text(LocaleKeys.logExportAll.tr),
              onTap: () => Navigator.pop(ctx, 'zip'),
            ),
            ListTile(
              leading: const Icon(Icons.bug_report_outlined),
              title: Text(LocaleKeys.logWriteSampleCrash.tr),
              onTap: () => Navigator.pop(ctx, 'sample'),
            ),
            ListTile(
              leading: const Icon(Icons.delete_outline),
              title: Text(LocaleKeys.logClear.tr),
              onTap: () => Navigator.pop(ctx, 'clear'),
            ),
          ],
        ),
      ),
    );
    switch (pick) {
      case 'zip':
        await _exportAllZip();
      case 'sample':
        await _writeSampleCrash();
      case 'clear':
        await _clear();
    }
  }

  int _sumBytes(List<File> files) {
    var n = 0;
    for (final f in files) {
      n += f.lengthSync();
    }
    return n;
  }

  Widget _section({
    required String title,
    required List<File> files,
    required Color color,
    required IconData icon,
    required String Function(String baseName) label,
    required String empty,
  }) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        Padding(
          padding: const EdgeInsets.fromLTRB(20, 22, 20, 8),
          child: Row(
            children: [
              Expanded(
                child: Text(
                  title,
                  style: const TextStyle(
                    color: Colors.white,
                    fontWeight: FontWeight.w600,
                    fontSize: 16,
                  ),
                ),
              ),
              if (files.isNotEmpty)
                Text(
                  LocaleKeys.logCount.trParams({
                    'n': '${files.length}',
                    'size': formatBytes(_sumBytes(files)),
                  }),
                  style: const TextStyle(
                    color: AppTheme.muted,
                    fontSize: 12,
                  ),
                ),
            ],
          ),
        ),
        Padding(
          padding: const EdgeInsets.symmetric(horizontal: 16),
          child: files.isEmpty
              ? HelmCard(
                  padding: const EdgeInsets.all(18),
                  child: Text(
                    empty,
                    style: const TextStyle(color: AppTheme.muted),
                  ),
                )
              : HelmCard(
                  child: HelmTileGroup(
                    children: [
                      for (final f in files)
                        HelmNavTile(
                          color: color,
                          icon: icon,
                          title: label(p.basename(f.path)),
                          subtitle:
                              '${p.basename(f.path)}  ·  '
                              '${formatBytes(f.lengthSync())}',
                          showChevron: false,
                          onTap: () => _open(f),
                          trailing: Row(
                            mainAxisSize: MainAxisSize.min,
                            children: [
                              const Icon(
                                Icons.chevron_right,
                                color: AppTheme.muted,
                                size: 20,
                              ),
                              IconButton(
                                tooltip: LocaleKeys.logExportSelected.tr,
                                onPressed: () => _share(f),
                                icon: const Icon(Icons.ios_share),
                              ),
                              IconButton(
                                tooltip: LocaleKeys.logDelete.tr,
                                onPressed: () => _delete(f),
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
    final total = _sumBytes([..._crashes, ..._daily]);

    return Scaffold(
      appBar: AppBar(
        title: Text(LocaleKeys.logTitle.tr),
        actions: [
          if (_exporting)
            const Padding(
              padding: EdgeInsets.symmetric(horizontal: 12),
              child: Center(
                child: SizedBox(
                  width: 20,
                  height: 20,
                  child: CircularProgressIndicator(strokeWidth: 2),
                ),
              ),
            )
          else
            IconButton(
              tooltip: LocaleKeys.refresh.tr,
              onPressed: _reload,
              icon: const Icon(Icons.refresh),
            ),
          IconButton(
            tooltip: LocaleKeys.logExportAll.tr,
            onPressed: _showMenu,
            icon: const Icon(Icons.more_vert),
          ),
        ],
      ),
      body: _loading
          ? const Center(child: CircularProgressIndicator())
          : (_crashes.isEmpty && _daily.isEmpty)
              ? Center(
                  child: Text(
                    LocaleKeys.logEmpty.tr,
                    style: const TextStyle(color: AppTheme.muted),
                  ),
                )
              : ListView(
                  padding: const EdgeInsets.fromLTRB(0, 4, 0, 32),
                  children: [
                    Padding(
                      padding: const EdgeInsets.fromLTRB(20, 8, 20, 0),
                      child: Row(
                        children: [
                          Expanded(
                            child: Text(
                              LocaleKeys.logDir.tr,
                              style: const TextStyle(
                                color: AppTheme.muted,
                                fontSize: 12,
                              ),
                            ),
                          ),
                          Text(
                            LocaleKeys.logTotal
                                .trParams({'size': formatBytes(total)}),
                            style: const TextStyle(
                              color: AppTheme.muted,
                              fontSize: 12,
                            ),
                          ),
                        ],
                      ),
                    ),
                    _section(
                      title: LocaleKeys.logSectionCrash.tr,
                      files: _crashes,
                      color: AppTheme.iconOta,
                      icon: Icons.bug_report_outlined,
                      label: _crashLabel,
                      empty: LocaleKeys.logEmpty.tr,
                    ),
                    _section(
                      title: LocaleKeys.logSectionApp.tr,
                      files: _daily,
                      color: AppTheme.iconLog,
                      icon: Icons.description_outlined,
                      label: _dayLabel,
                      empty: LocaleKeys.logEmpty.tr,
                    ),
                  ],
                ),
    );
  }
}

/// 单个本机日志文件的全屏阅读页。
///
/// 与「日志抓取」页里码表日志的预览页同构：正文都交给 [LogTextView]，
/// 只在这里补上分享。
class LogTextPage extends StatelessWidget {
  /// 创建阅读页。
  const LogTextPage({super.key, required this.file});

  /// 要读的文件。
  final File file;

  Future<void> _share() async {
    try {
      await AppLog.flush();
      await SharePlus.instance.share(
        ShareParams(
          files: [XFile(file.path, mimeType: 'text/plain')],
          subject: p.basename(file.path),
          text: LocaleKeys.logExportSubject.tr,
        ),
      );
    } catch (e, st) {
      AppTheme.fail(LocaleKeys.logTitle.tr, 'LogExport', e, st);
    }
  }

  @override
  Widget build(BuildContext context) {
    var size = 0;
    try {
      size = file.lengthSync();
    } catch (_) {}

    return Scaffold(
      appBar: AppBar(
        title: Text(
          p.basename(file.path),
          maxLines: 1,
          overflow: TextOverflow.ellipsis,
        ),
        actions: [
          IconButton(
            onPressed: _share,
            icon: const Icon(Icons.ios_share),
          ),
        ],
      ),
      body: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          Padding(
            padding: const EdgeInsets.fromLTRB(16, 10, 16, 8),
            child: Text(
              formatBytes(size),
              style: const TextStyle(color: AppTheme.muted, fontSize: 12),
            ),
          ),
          const Divider(height: 1, thickness: 0.5, color: AppTheme.hairline),
          Expanded(child: LogTextView(file: file)),
        ],
      ),
    );
  }
}

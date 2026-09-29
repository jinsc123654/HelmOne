import 'dart:async';

import 'package:flutter/material.dart';
import 'package:sifli_companion/app/app_theme.dart';
import 'package:sifli_companion/ble/companion_client.dart';
import 'package:sifli_companion/i18n/locale_keys.dart';
import 'package:sifli_companion/log/app_log.dart';
import 'package:sifli_companion/ride/gpx_util.dart';
import 'package:sifli_companion/ride/mcu_sync.dart';
import 'package:sifli_companion/transfer/transfer_center.dart';
import 'package:sifli_companion/ui/widgets/helm_chrome.dart';
import 'package:get/get.dart';

typedef _UploadTick = void Function(
  int done,
  int total, {
  String? label,
});

/// 把本机路线写到码表 `mtp/import/`，上传时另起文件名。
abstract class ImportUpload {
  /// 任务类别（跨页面查重入用）。
  static const _kind = 'import_upload';

  static String stem(String name) {
    final t = name.trim();
    if (t.toLowerCase().endsWith('.gpx')) {
      return t.substring(0, t.length - 4);
    }
    return t;
  }

  static Future<String?> askName(
    BuildContext context, {
    required String initial,
  }) async {
    final raw = await HelmPromptDialog.show(
      context,
      title: LocaleKeys.gpxUploadName.tr,
      cancel: LocaleKeys.cancel.tr,
      confirm: LocaleKeys.gpxUpload.tr,
      label: LocaleKeys.gpxUploadNameLabel.tr,
      hint: LocaleKeys.gpxUploadNameHint.tr,
      initial: stem(initial),
    );
    if (raw == null || raw.isEmpty) return null;
    return GpxUtil.safeFileName(raw);
  }

  static bool _needBle() {
    if (CompanionClient.instance.isReady) return false;
    AppTheme.snack(LocaleKeys.gpxUpload.tr, LocaleKeys.gpxNeedBle.tr);
    return true;
  }

  static Future<bool> push({
    required BuildContext context,
    String? localPath,
    List<int>? bytes,
    required String defaultName,
  }) async {
    assert(localPath != null || bytes != null);
    final name = await askName(context, initial: defaultName);
    if (name == null) return false;
    if (!context.mounted) return false;
    if (_needBle()) return false;
    try {
      final written = await _runWithProgress(
        label: name,
        work: (onProgress) => _send(
          fileName: name,
          localPath: localPath,
          bytes: bytes,
          displayName: stem(name),
          onProgress: (done, total) => onProgress(done, total),
        ),
      );
      AppTheme.snack(
        LocaleKeys.gpxUpload.tr,
        LocaleKeys.gpxUploaded.trParams({'name': written}),
      );
      return true;
    } catch (e, st) {
      AppTheme.fail(LocaleKeys.gpxUpload.tr, 'GpxUpload', e, st);
      return false;
    }
  }

  static Future<String> _send({
    required String fileName,
    String? localPath,
    List<int>? bytes,
    String? displayName,
    void Function(int done, int total)? onProgress,
  }) {
    final client = CompanionClient.instance;
    if (bytes != null) {
      return McuSync.pushImportBytes(
        client,
        bytes,
        fileName,
        displayName: displayName,
        onProgress: onProgress,
      );
    }
    return McuSync.pushImportFile(
      client,
      localPath!,
      fileName,
      displayName: displayName,
      onProgress: onProgress,
    );
  }

  static Future<int> pushMany(
    BuildContext context,
    List<({String path, String title})> items,
  ) async {
    if (items.isEmpty) return 0;
    if (_needBle()) return 0;
    if (!context.mounted) return 0;
    final client = CompanionClient.instance;
    var n = 0;
    Object? lastErr;
    await _runWithProgress<void>(
      label: LocaleKeys.gpxUploading.tr,
      work: (onProgress) async {
        for (var i = 0; i < items.length; i++) {
          final it = items[i];
          try {
            final name = GpxUtil.safeFileName(it.title);
            onProgress(
              0,
              1,
              label: '${i + 1}/${items.length}  $name',
            );
            await McuSync.pushImportFile(
              client,
              it.path,
              name,
              displayName: stem(name),
              onProgress: (done, total) => onProgress(
                done,
                total,
                label: '${i + 1}/${items.length}  $name',
              ),
            );
            n++;
          } catch (e, st) {
            lastErr = e;
            await AppLog.e('GpxUpload', '${it.title}: $e', st);
          }
        }
      },
    );
    if (n > 0) {
      AppTheme.snack(
        LocaleKeys.gpxUpload.tr,
        LocaleKeys.gpxUploadedMany.trParams({'n': '$n'}),
      );
    } else if (lastErr != null) {
      AppTheme.snack(LocaleKeys.gpxUpload.tr, LocaleKeys.failed.tr);
    }
    return n;
  }

  /// 把一份或多份 GPX 推上码表，进度只走全局悬浮框。
  ///
  /// 这里以前是 `useRootNavigator` 的模态对话框（还 `PopScope(canPop: false)`
  /// 锁住返回），退出页面就看不到进度了。改成登记到 [TransferCenter]：
  /// 悬浮框在路由之外，切页/返回都还在。
  static Future<T> _runWithProgress<T>({
    required String label,
    required Future<T> Function(_UploadTick onProgress) work,
  }) async {
    final center = TransferCenter.to;
    final task = center.begin(title: label, kind: _kind);

    var lastUi = DateTime.fromMillisecondsSinceEpoch(0);
    // 多文件时每个文件的 done 都从 0 重新开始，直接当 transferred 会让
    // 测速窗口看到字节倒扣。用 base 把已完成文件的字节累积起来。
    var prevDone = 0;
    var base = 0;

    try {
      final r = await work((done, total, {String? label}) {
        if (done < prevDone) {
          base += prevDone;
        }
        prevDone = done;

        final now = DateTime.now();
        if (done < total &&
            now.difference(lastUi) < const Duration(milliseconds: 120) &&
            label == null) {
          return;
        }
        lastUi = now;
        center.progress(
          task,
          done: done,
          total: total,
          transferred: base + done,
          phase: label ?? task.phase,
        );
      });
      center.finish(task);
      return r;
    } catch (e) {
      center.fail(task, e);
      rethrow;
    }
  }
}

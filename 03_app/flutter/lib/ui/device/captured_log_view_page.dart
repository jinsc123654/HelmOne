import 'package:flutter/material.dart';
import 'package:get/get.dart';
import 'package:share_plus/share_plus.dart';
import 'package:sifli_companion/app/app_theme.dart';
import 'package:sifli_companion/i18n/locale_keys.dart';
import 'package:sifli_companion/log/captured_log_store.dart';
import 'package:sifli_companion/ride/gpx_util.dart';
import 'package:sifli_companion/ui/widgets/log_text_view.dart';
import 'package:sifli_companion/util/byte_format.dart';

/// 从码表抓回来的一条日志的预览页。
///
/// 正文交给 [LogTextView] —— 与本机日志页共用同一套读取、截断、空态逻辑。
class CapturedLogViewPage extends StatelessWidget {
  /// 创建预览页。
  const CapturedLogViewPage({super.key, required this.item});

  /// 要看的记录。
  final CapturedLog item;

  Future<void> _share() async {
    try {
      await SharePlus.instance.share(
        ShareParams(
          files: [
            XFile(
              item.file.path,
              mimeType: 'text/plain',
              name: item.name,
            ),
          ],
          subject: item.name,
          text: LocaleKeys.coredumpShareSubject.tr,
        ),
      );
    } catch (e, st) {
      AppTheme.fail(LocaleKeys.captureTitle.tr, 'LogShare', e, st);
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: Text(
          item.name,
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
              '${formatBytes(item.size)}  ·  '
              '${LocaleKeys.captureAt.trParams({'time': item.title})}  ·  '
              '${LocaleKeys.coredumpPulledAt.trParams({'time': GpxUtil.formatStamp(item.pulledAt)})}',
              style: const TextStyle(color: AppTheme.muted, fontSize: 12),
            ),
          ),
          const Divider(height: 1, thickness: 0.5, color: AppTheme.hairline),
          Expanded(child: LogTextView(file: item.file)),
        ],
      ),
    );
  }
}

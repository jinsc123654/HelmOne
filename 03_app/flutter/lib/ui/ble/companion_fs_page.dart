import 'package:flutter/material.dart';
import 'package:get/get.dart';
import 'package:sifli_companion/ble/companion_client.dart';
import 'package:sifli_companion/i18n/locale_keys.dart';
import 'package:sifli_companion/ui/ble/companion_fs_browser.dart';

/// 码表文件操作二级页：浏览 / 上传 / 下载 / 重命名 / 移动 / 删除。
class CompanionFsPage extends StatelessWidget {
  /// 创建页面。
  const CompanionFsPage({super.key});

  @override
  Widget build(BuildContext context) {
    final client = CompanionClient.instance;
    return Scaffold(
      appBar: AppBar(title: Text(LocaleKeys.fsTitle.tr)),
      body: CompanionFsBrowser(
        key: ValueKey(client.device?.remoteId.str),
        gatt: client,
      ),
    );
  }
}

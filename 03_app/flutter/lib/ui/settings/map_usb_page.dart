import 'package:flutter/material.dart';
import 'package:get/get.dart';
import 'package:sifli_companion/i18n/locale_keys.dart';
import 'package:sifli_companion/ui/widgets/helm_chrome.dart';

/// 离线地图走 USB 的说明页。
class MapUsbPage extends StatelessWidget {
  /// 创建说明页。
  const MapUsbPage({super.key});

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: Text(LocaleKeys.settingsMapUsbTitle.tr)),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          HelmCard(
            padding: const EdgeInsets.all(20),
            child: Text(
              LocaleKeys.settingsMapUsbBody.tr,
              style: Theme.of(context).textTheme.bodyLarge?.copyWith(
                    height: 1.5,
                  ),
            ),
          ),
        ],
      ),
    );
  }
}

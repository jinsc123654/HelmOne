import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:get/get.dart';
import 'package:sifli_companion/app/app_assets.dart';
import 'package:sifli_companion/i18n/app_locale.dart';
import 'package:sifli_companion/i18n/locale_keys.dart';
import 'package:sifli_companion/ui/home/home_controller.dart';

/// 首页：展示 KV/DB/蓝牙状态，并提供持久化自测与扫描入口。
class HomePage extends GetView<HomeController> {
  /// 创建首页。
  const HomePage({super.key});

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: Row(
          children: [
            Image.asset(
              AppAssets.bikeComputer,
              width: 28,
              height: 28,
              filterQuality: FilterQuality.high,
            ),
            const SizedBox(width: 10),
            Expanded(child: Text(LocaleKeys.appName.tr)),
          ],
        ),
        actions: [
          PopupMenuButton<Locale>(
            tooltip: LocaleKeys.language.tr,
            icon: const Icon(Icons.language),
            onSelected: (locale) => controller.changeLocale(locale),
            itemBuilder: (context) => [
              PopupMenuItem(
                value: AppLocale.zhCN,
                child: Text(LocaleKeys.langZh.tr),
              ),
              PopupMenuItem(
                value: AppLocale.enUS,
                child: Text(LocaleKeys.langEn.tr),
              ),
            ],
          ),
        ],
      ),
      body: Obx(() {
        final ready = controller.servicesReady.value;
        final path = controller.dbPath.value;
        return ListView(
          padding: const EdgeInsets.all(16),
          children: [
            _StatusTile(
              title: LocaleKeys.homeKvTitle.tr,
              subtitle:
                  'counter=${controller.kvCounter.value}\n'
                  '${LocaleKeys.updatedAt.trParams({'time': controller.kvUpdatedAt.value})}\n'
                  'locale=${controller.locale.value}',
              ok: ready,
            ),
            FilledButton(
              onPressed: ready ? controller.bumpKv : null,
              child: Text(LocaleKeys.homeKvBump.tr),
            ),
            const SizedBox(height: 16),
            _StatusTile(
              title: LocaleKeys.homeDbTitle.tr,
              subtitle:
                  'counter=${controller.dbCounter.value}\n'
                  '${LocaleKeys.updatedAt.trParams({'time': controller.dbUpdatedAt.value})}\n'
                  '$path',
              ok: ready && path.isNotEmpty && path != '…',
            ),
            FilledButton(
              onPressed: ready ? controller.bumpDb : null,
              child: Text(LocaleKeys.homeDbBump.tr),
            ),
            const SizedBox(height: 16),
            _StatusTile(
              title: LocaleKeys.homeBleTitle.tr,
              subtitle:
                  '${controller.bleSupport.value == 'supported' ? LocaleKeys.bleSupported.tr : LocaleKeys.bleUnsupported.tr} · '
                  'adapter=${controller.adapterState.value}',
              ok: controller.bleSupport.value == 'supported',
            ),
            const SizedBox(height: 24),
            FilledButton.icon(
              onPressed: controller.openBleScan,
              icon: const Icon(Icons.bluetooth_searching),
              label: Text(LocaleKeys.homeBleScan.tr),
            ),
            const SizedBox(height: 12),
            FilledButton.tonalIcon(
              onPressed: controller.openCompanionDebug,
              icon: const Icon(Icons.settings_remote),
              label: Text(LocaleKeys.homeOpenCompanionDebug.tr),
            ),
            const SizedBox(height: 12),
            FilledButton.icon(
              onPressed: controller.openOta,
              icon: const Icon(Icons.system_update_alt),
              label: Text(LocaleKeys.homeOpenOta.tr),
            ),
            const SizedBox(height: 12),
            FilledButton.tonalIcon(
              onPressed: controller.openEph,
              icon: const Icon(Icons.satellite_alt_outlined),
              label: Text(LocaleKeys.homeOpenEph.tr),
            ),
            const SizedBox(height: 12),
            OutlinedButton.icon(
              onPressed: controller.openMap,
              icon: const Icon(Icons.map_outlined),
              label: Text(LocaleKeys.homeOpenMap.tr),
            ),
            const SizedBox(height: 12),
            OutlinedButton.icon(
              onPressed: controller.openLogs,
              icon: const Icon(Icons.bug_report_outlined),
              label: Text(LocaleKeys.homeOpenLogs.tr),
            ),
            if (!kIsWeb)
              TextButton(
                onPressed: controller.refreshStatus,
                child: Text(LocaleKeys.homeRefreshPersist.tr),
              ),
          ],
        );
      }),
    );
  }
}

/// 状态卡片：标题 + 多行副标题 + 就绪图标。
class _StatusTile extends StatelessWidget {
  const _StatusTile({
    required this.title,
    required this.subtitle,
    required this.ok,
  });

  final String title;
  final String subtitle;
  final bool ok;

  @override
  Widget build(BuildContext context) {
    return Card(
      child: ListTile(
        leading: Icon(
          ok ? Icons.check_circle : Icons.hourglass_empty,
          color: ok ? Colors.teal : Colors.orange,
        ),
        title: Text(title),
        subtitle: Text(subtitle),
        isThreeLine: true,
      ),
    );
  }
}

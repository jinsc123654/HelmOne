import 'package:flutter/material.dart';
import 'package:get/get.dart';
import 'package:sifli_companion/app/app_routes.dart';
import 'package:sifli_companion/app/app_theme.dart';
import 'package:sifli_companion/i18n/locale_keys.dart';
import 'package:sifli_companion/ui/shell/shell_controller.dart';
import 'package:sifli_companion/ui/widgets/helm_chrome.dart';

/// 重新绑定码表：解绑说明 + 解除绑定；解绑后才能进扫描配对。
class RebindPage extends GetView<ShellController> {
  /// 创建重新绑定页。
  const RebindPage({super.key});

  Future<void> _unbind() async {
    final ok = await Get.dialog<bool>(
      AlertDialog(
        title: Text(LocaleKeys.rebindConfirmTitle.tr),
        content: Text(LocaleKeys.rebindConfirmBody.tr),
        actions: [
          TextButton(
            onPressed: () => Get.back(result: false),
            child: Text(LocaleKeys.cancel.tr),
          ),
          TextButton(
            onPressed: () => Get.back(result: true),
            style: TextButton.styleFrom(foregroundColor: AppTheme.danger),
            child: Text(LocaleKeys.rebindUnbind.tr),
          ),
        ],
      ),
    );
    if (ok != true) return;

    await controller.forgetBond();
    if (!Get.isRegistered<ShellController>()) return;
    AppTheme.snack(
      LocaleKeys.bleSnackbarTitle.tr,
      LocaleKeys.rebindUnbound.tr,
    );
  }

  Future<void> _scan() async {
    if (controller.hasBond.value) {
      AppTheme.snack(
        LocaleKeys.bleSnackbarTitle.tr,
        LocaleKeys.rebindNeedUnbind.tr,
      );
      return;
    }
    await Get.toNamed(AppRoutes.bleScan);
    if (controller.isReady) await controller.onConnected();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: Text(LocaleKeys.rebindTitle.tr)),
      body: Obx(() {
        final bound = controller.hasBond.value;
        final name = controller.deviceName.isEmpty
            ? LocaleKeys.deviceNoName.tr
            : controller.deviceName;

        return ListView(
          padding: const EdgeInsets.fromLTRB(16, 8, 16, 32),
          children: [
            HelmCard(
              padding: const EdgeInsets.fromLTRB(16, 14, 16, 14),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(
                    bound
                        ? LocaleKeys.rebindBoundHint.trParams({'name': name})
                        : LocaleKeys.rebindFreeHint.tr,
                    style: const TextStyle(
                      color: Colors.white,
                      fontSize: 14,
                      height: 1.45,
                    ),
                  ),
                  const SizedBox(height: 12),
                  Text(
                    LocaleKeys.rebindWhy.tr,
                    style: const TextStyle(
                      color: AppTheme.muted,
                      fontSize: 13,
                      height: 1.45,
                    ),
                  ),
                ],
              ),
            ),
            const SizedBox(height: 12),
            HelmCard(
              padding: const EdgeInsets.fromLTRB(16, 14, 16, 14),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(
                    LocaleKeys.pairGuideTitle.tr,
                    style: const TextStyle(
                      color: Colors.white,
                      fontSize: 14,
                      fontWeight: FontWeight.w600,
                    ),
                  ),
                  const SizedBox(height: 10),
                  _step(1, LocaleKeys.pairGuideStep1.tr),
                  _step(2, LocaleKeys.pairGuideStep2.tr),
                  _step(3, LocaleKeys.rebindPairStep3.tr),
                  const SizedBox(height: 8),
                  Text(
                    LocaleKeys.pairGuideNote.tr,
                    style: const TextStyle(
                      color: AppTheme.muted,
                      fontSize: 12,
                      height: 1.4,
                    ),
                  ),
                ],
              ),
            ),
            const SizedBox(height: 20),
            if (bound)
              HelmCard(
                child: InkWell(
                  onTap: _unbind,
                  child: Padding(
                    padding: const EdgeInsets.symmetric(vertical: 16),
                    child: Text(
                      LocaleKeys.rebindUnbind.tr,
                      textAlign: TextAlign.center,
                      style: const TextStyle(
                        color: Color(0xFFEF4444),
                        fontSize: 16,
                        fontWeight: FontWeight.w600,
                      ),
                    ),
                  ),
                ),
              )
            else
              FilledButton(
                onPressed: _scan,
                style: FilledButton.styleFrom(
                  backgroundColor: AppTheme.sync,
                  foregroundColor: Colors.white,
                  minimumSize: const Size.fromHeight(44),
                  shape: const StadiumBorder(),
                ),
                child: Text(LocaleKeys.rebindScan.tr),
              ),
          ],
        );
      }),
    );
  }

  Widget _step(int n, String text) {
    return Padding(
      padding: const EdgeInsets.only(bottom: 6),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Container(
            width: 18,
            height: 18,
            alignment: Alignment.center,
            decoration: const BoxDecoration(
              color: AppTheme.field,
              shape: BoxShape.circle,
            ),
            child: Text(
              '$n',
              style: const TextStyle(
                color: Colors.white,
                fontSize: 11,
                fontWeight: FontWeight.w600,
              ),
            ),
          ),
          const SizedBox(width: 8),
          Expanded(
            child: Text(
              text,
              style: const TextStyle(
                color: Colors.white70,
                fontSize: 13,
                height: 1.45,
              ),
            ),
          ),
        ],
      ),
    );
  }
}

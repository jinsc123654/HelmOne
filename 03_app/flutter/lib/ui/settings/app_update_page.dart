import 'package:flutter/material.dart';
import 'package:get/get.dart';
import 'package:package_info_plus/package_info_plus.dart';
import 'package:sifli_companion/app/app_theme.dart';
import 'package:sifli_companion/i18n/locale_keys.dart';
import 'package:sifli_companion/ui/widgets/helm_center_action.dart';

/// 本 App 版本与检查更新。码表固件不在这里。
class AppUpdatePage extends StatefulWidget {
  /// 创建 App 更新页。
  const AppUpdatePage({super.key});

  @override
  State<AppUpdatePage> createState() => _AppUpdatePageState();
}

class _AppUpdatePageState extends State<AppUpdatePage> {
  String _ver = '…';
  bool _checking = false;

  @override
  void initState() {
    super.initState();
    PackageInfo.fromPlatform().then((info) {
      if (!mounted) return;
      setState(() => _ver = info.version);
    });
  }

  Future<void> _check() async {
    if (_checking) return;
    setState(() => _checking = true);
    await Future<void>.delayed(const Duration(milliseconds: 400));
    if (!mounted) return;
    setState(() => _checking = false);
    AppTheme.snack(
      LocaleKeys.settingsAppUpdateTitle.tr,
      LocaleKeys.settingsAppUpdateNone.tr,
    );
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: Text(LocaleKeys.settingsAppUpdateTitle.tr)),
      body: HelmCenterAction(
        icon: Icons.system_update_alt,
        label: _checking
            ? LocaleKeys.settingsAppUpdateChecking.tr
            : LocaleKeys.settingsAppUpdateCheck.tr,
        color: AppTheme.iconAppUpdate,
        onPressed: _checking ? null : _check,
        busy: _checking,
        headline: LocaleKeys.appName.tr,
        caption: LocaleKeys.settingsAppUpdateCurrent.trParams({'ver': _ver}),
      ),
    );
  }
}

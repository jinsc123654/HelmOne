import 'package:flutter/material.dart';
import 'package:get/get.dart';
import 'package:sifli_companion/app/app_assets.dart';
import 'package:sifli_companion/app/app_routes.dart';
import 'package:sifli_companion/app/app_theme.dart';
import 'package:sifli_companion/i18n/locale_keys.dart';
import 'package:sifli_companion/log/app_log.dart';
import 'package:sifli_companion/ui/widgets/helm_chrome.dart';
import 'package:sifli_companion/utils/cache_data.dart';

/// 首次启动欢迎页：说明隐私用途，同意后进入首页。
class WelcomePage extends StatefulWidget {
  /// 创建欢迎页。
  const WelcomePage({super.key});

  @override
  State<WelcomePage> createState() => _WelcomePageState();
}

class _WelcomePageState extends State<WelcomePage> {
  bool _checked = false;
  bool _busy = false;

  Future<void> _agree() async {
    if (!_checked || _busy) return;
    setState(() => _busy = true);
    await CacheData().setAgreed(true);
    await AppLog.i('Welcome', 'user agreed');
    Get.offAllNamed(AppRoutes.home);
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Scaffold(
      body: SafeArea(
        child: Padding(
          padding: const EdgeInsets.all(24),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              const Spacer(),
              Center(
                child: Image.asset(
                  AppAssets.bikeComputer,
                  width: 96,
                  height: 96,
                  filterQuality: FilterQuality.high,
                ),
              ),
              const SizedBox(height: 20),
              Text(
                LocaleKeys.appName.tr,
                textAlign: TextAlign.center,
                style: theme.textTheme.headlineSmall?.copyWith(
                  fontWeight: FontWeight.w700,
                  color: Colors.white,
                ),
              ),
              const SizedBox(height: 12),
              Text(
                LocaleKeys.welcomeSubtitle.tr,
                textAlign: TextAlign.center,
                style: theme.textTheme.bodyMedium?.copyWith(
                  color: AppTheme.muted,
                ),
              ),
              const SizedBox(height: 28),
              Expanded(
                child: HelmCard(
                  padding: const EdgeInsets.all(16),
                  child: SingleChildScrollView(
                    child: Text(
                      LocaleKeys.welcomePrivacyBody.tr,
                      style: theme.textTheme.bodyMedium?.copyWith(
                        color: Colors.white,
                        height: 1.5,
                      ),
                    ),
                  ),
                ),
              ),
              const SizedBox(height: 12),
              CheckboxListTile(
                value: _checked,
                onChanged: _busy
                    ? null
                    : (v) => setState(() => _checked = v ?? false),
                controlAffinity: ListTileControlAffinity.leading,
                contentPadding: EdgeInsets.zero,
                title: Text(LocaleKeys.welcomeAgreeCheck.tr),
              ),
              FilledButton(
                onPressed: _checked && !_busy ? _agree : null,
                child: Text(LocaleKeys.welcomeEnter.tr),
              ),
            ],
          ),
        ),
      ),
    );
  }
}

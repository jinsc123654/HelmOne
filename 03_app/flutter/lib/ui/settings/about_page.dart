import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:get/get.dart';
import 'package:package_info_plus/package_info_plus.dart';
import 'package:sifli_companion/app/app_assets.dart';
import 'package:sifli_companion/app/app_keys.dart';
import 'package:sifli_companion/app/app_routes.dart';
import 'package:sifli_companion/app/app_theme.dart';
import 'package:sifli_companion/i18n/locale_keys.dart';
import 'package:sifli_companion/ui/widgets/helm_chrome.dart';
import 'package:url_launcher/url_launcher.dart';

/// 关于页：应用信息与开发者介绍。
class AboutPage extends StatefulWidget {
  /// 创建关于页。
  const AboutPage({super.key});

  @override
  State<AboutPage> createState() => _AboutPageState();
}

class _AboutPageState extends State<AboutPage> {
  static const _developerSpaceUrl = 'https://space.bilibili.com/34165842';
  static const _fsUnlockWindow = Duration(seconds: 3);
  static const _fsUnlockTaps = 5;

  String _version = '';
  String _buildNumber = '';
  final _avatarTaps = <DateTime>[];

  @override
  void initState() {
    super.initState();
    _loadVersion();
  }

  Future<void> _loadVersion() async {
    try {
      final info = await PackageInfo.fromPlatform();
      if (!mounted) return;
      setState(() {
        _version = info.version;
        _buildNumber = info.buildNumber;
      });
    } catch (_) {
      if (!mounted) return;
      setState(() {
        _version = '1.0.0';
        _buildNumber = '';
      });
    }
  }

  void _onAvatarTap() {
    final now = DateTime.now();
    _avatarTaps.removeWhere((t) => now.difference(t) > _fsUnlockWindow);
    _avatarTaps.add(now);
    if (_avatarTaps.length < _fsUnlockTaps) {
      HapticFeedback.selectionClick();
      return;
    }
    _avatarTaps.clear();
    HapticFeedback.mediumImpact();
    Get.toNamed(AppRoutes.companionFs);
  }

  Future<void> _openDeveloperSpace() async {
    final uri = Uri.parse(_developerSpaceUrl);
    try {
      final ok = await launchUrl(uri, mode: LaunchMode.externalApplication);
      if (ok) return;
    } catch (_) {}
    await Clipboard.setData(const ClipboardData(text: _developerSpaceUrl));
    if (!mounted) return;
    AppTheme.snack(
      LocaleKeys.aboutTitle.tr,
      LocaleKeys.aboutLinkCopied.tr,
    );
  }

  @override
  Widget build(BuildContext context) {
    final verText = _version.isEmpty
        ? '…'
        : (_buildNumber.isEmpty ? _version : '$_version ($_buildNumber)');

    return Scaffold(
      appBar: AppBar(title: Text(LocaleKeys.aboutTitle.tr)),
      body: ListView(
        padding: const EdgeInsets.fromLTRB(16, 24, 16, 40),
        children: [
          Column(
            children: [
              GestureDetector(
                onTap: _onAvatarTap,
                child: ClipOval(
                  child: Image.asset(
                    AppAssets.developerLogo,
                    width: 96,
                    height: 96,
                    fit: BoxFit.cover,
                  ),
                ),
              ),
              const SizedBox(height: 16),
              Text(
                AppKeys.appName,
                textAlign: TextAlign.center,
                style: const TextStyle(
                  fontSize: 22,
                  fontWeight: FontWeight.w700,
                  color: Colors.white,
                ),
              ),
              const SizedBox(height: 6),
              Text(
                LocaleKeys.aboutVersion.trParams({'v': verText}),
                textAlign: TextAlign.center,
                style: const TextStyle(fontSize: 14, color: AppTheme.muted),
              ),
              const SizedBox(height: 8),
              InkWell(
                onTap: _openDeveloperSpace,
                borderRadius: BorderRadius.circular(6),
                child: Padding(
                  padding: const EdgeInsets.symmetric(
                    horizontal: 8,
                    vertical: 4,
                  ),
                  child: Text(
                    LocaleKeys.aboutDeveloperName.tr,
                    textAlign: TextAlign.center,
                    style: const TextStyle(
                      fontSize: 15,
                      fontWeight: FontWeight.w500,
                      color: AppTheme.accent,
                      decoration: TextDecoration.underline,
                      decorationColor: AppTheme.accent,
                    ),
                  ),
                ),
              ),
            ],
          ),
          const SizedBox(height: 24),
          HelmCard(
            padding: const EdgeInsets.fromLTRB(20, 18, 20, 18),
            child: Column(
              children: [
                Text(
                  LocaleKeys.aboutApp.tr,
                  textAlign: TextAlign.center,
                  style: const TextStyle(fontSize: 13, color: AppTheme.muted),
                ),
                const SizedBox(height: 8),
                Text(
                  LocaleKeys.aboutAppDesc.tr,
                  textAlign: TextAlign.center,
                  style: const TextStyle(
                    fontSize: 15,
                    height: 1.5,
                    color: Colors.white,
                  ),
                ),
              ],
            ),
          ),
          const SizedBox(height: 12),
          HelmCard(
            padding: const EdgeInsets.fromLTRB(20, 18, 20, 18),
            child: Text(
              LocaleKeys.aboutThanks.tr,
              textAlign: TextAlign.center,
              style: const TextStyle(
                fontSize: 15,
                height: 1.5,
                color: Colors.white,
              ),
            ),
          ),
          const SizedBox(height: 28),
          Text(
            LocaleKeys.aboutCopyright.tr,
            textAlign: TextAlign.center,
            style: const TextStyle(fontSize: 12, color: AppTheme.muted),
          ),
        ],
      ),
    );
  }
}

import 'package:flutter/material.dart';
import 'package:get/get.dart';
import 'package:package_info_plus/package_info_plus.dart';
import 'package:sifli_companion/app/app_routes.dart';
import 'package:sifli_companion/app/app_theme.dart';
import 'package:sifli_companion/ble/companion_foreground_service.dart';
import 'package:sifli_companion/cache/app_cache_store.dart';
import 'package:sifli_companion/i18n/app_locale.dart';
import 'package:sifli_companion/i18n/locale_keys.dart';
import 'package:sifli_companion/map/osm_city_store.dart';
import 'package:sifli_companion/ui/widgets/helm_chrome.dart';
import 'package:sifli_companion/ui/widgets/helm_header.dart';

/// 设置 Tab：本 App 的语言、缓存、日志、保活、更新、关于。码表操作在设备页。
class SettingsPage extends StatefulWidget {
  /// 创建设置页。
  const SettingsPage({super.key});

  @override
  State<SettingsPage> createState() => _SettingsPageState();
}

class _SettingsPageState extends State<SettingsPage> {
  bool _fgs = false;
  String _ver = '';
  String _cacheLabel = '';
  int _osmDone = 0;

  @override
  void initState() {
    super.initState();
    _refreshFgs();
    _loadMeta();
  }

  Future<void> _loadMeta() async {
    final info = await PackageInfo.fromPlatform();
    var cache = '';
    var osmDone = 0;
    try {
      cache = _fmtSize(await AppCacheStore.instance.totalBytes());
    } catch (_) {}
    try {
      osmDone = await OsmCityStore.instance.completeCount();
    } catch (_) {}
    if (!mounted) return;
    setState(() {
      _ver = info.version;
      _cacheLabel = cache;
      _osmDone = osmDone;
    });
  }

  Future<void> _refreshFgs() async {
    /* 开关显示的是**用户的意图**（落盘偏好），不是"服务此刻在不在跑"。
     *
     * 读运行态会骗人：服务只在连上/切后台时才起，所以用户明明开着这个选项，一进设置页
     * 看到的就是"关"（2026-09-25 现场："打开都是关闭状态"）。偏好才是这个开关的真相。
     */
    final v = await CompanionForegroundService.keepAliveWanted();
    if (mounted) setState(() => _fgs = v);
  }

  Future<void> _toggleFgs(bool on) async {
    // 先落盘偏好，再动服务：这个开关必须能被后台路径看见（进程被系统拉起来时
    // 要不要连码表就靠它），否则关掉它、杀掉 App，后台照样把服务拉起来连上。
    await CompanionForegroundService.setKeepAlive(on);

    if (on) {
      await CompanionForegroundService.requestNotificationPermission();
      await CompanionForegroundService.start();
    } else {
      await CompanionForegroundService.stop();
    }
    await _refreshFgs();
  }

  Future<void> _pickLocale() async {
    final next = await Get.bottomSheet<Locale>(
      DecoratedBox(
        decoration: const BoxDecoration(
          color: AppTheme.card,
          borderRadius: BorderRadius.vertical(
            top: Radius.circular(AppTheme.radius),
          ),
        ),
        child: SafeArea(
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              const SizedBox(height: 8),
              Container(
                width: 36,
                height: 4,
                decoration: BoxDecoration(
                  color: AppTheme.field,
                  borderRadius: BorderRadius.circular(2),
                ),
              ),
              HelmTileGroup(
                children: [
                  HelmNavTile(
                    color: AppTheme.iconLang,
                    icon: Icons.language,
                    title: LocaleKeys.langZh.tr,
                    showChevron: false,
                    onTap: () => Get.back(result: AppLocale.zhCN),
                  ),
                  HelmNavTile(
                    color: AppTheme.iconLang,
                    icon: Icons.language,
                    title: LocaleKeys.langEn.tr,
                    showChevron: false,
                    onTap: () => Get.back(result: AppLocale.enUS),
                  ),
                ],
              ),
              const SizedBox(height: 8),
            ],
          ),
        ),
      ),
      backgroundColor: Colors.transparent,
    );
    if (next != null) {
      await AppLocale.apply(next);
      setState(() {});
    }
  }

  String _fmtSize(int n) {
    if (n < 1024) return '$n B';
    if (n < 1024 * 1024) return '${(n / 1024).toStringAsFixed(1)} KB';
    return '${(n / (1024 * 1024)).toStringAsFixed(1)} MB';
  }

  @override
  Widget build(BuildContext context) {
    return SafeArea(
      child: ListView(
        padding: const EdgeInsets.only(bottom: 32),
        children: [
          const HelmAuthorHeader(),
          const SizedBox(height: 8),
          Padding(
            padding: const EdgeInsets.symmetric(horizontal: 16),
            child: HelmCard(
              child: HelmTileGroup(
                children: [
                  HelmNavTile(
                    color: AppTheme.iconLang,
                    icon: Icons.language,
                    title: LocaleKeys.settingsLang.tr,
                    subtitle: AppLocale.currentFromCache().languageCode == 'zh'
                        ? LocaleKeys.langZh.tr
                        : LocaleKeys.langEn.tr,
                    onTap: _pickLocale,
                  ),
                  HelmSwitchTile(
                    color: AppTheme.iconKeep,
                    icon: Icons.phonelink_lock_outlined,
                    title: LocaleKeys.settingsKeepAlive.tr,
                    subtitle: LocaleKeys.settingsKeepAliveSub.tr,
                    value: _fgs,
                    onChanged: CompanionForegroundService.isSupported
                        ? _toggleFgs
                        : null,
                  ),
                  HelmNavTile(
                    color: AppTheme.iconUsb,
                    icon: Icons.cloud_download_outlined,
                    title: LocaleKeys.osmCitiesDownload.tr,
                    subtitle: LocaleKeys.osmCitiesDownloadSub.tr,
                    onTap: () async {
                      await Get.toNamed(AppRoutes.osmCities, arguments: 0);
                      await _loadMeta();
                    },
                  ),
                  HelmNavTile(
                    color: AppTheme.iconKeep,
                    icon: Icons.download_done,
                    title: LocaleKeys.osmCitiesLocal.tr,
                    subtitle: _osmDone <= 0
                        ? LocaleKeys.osmCitiesLocalEmpty.tr
                        : LocaleKeys.osmCitiesLocalSub.trParams({
                            'n': '$_osmDone',
                          }),
                    onTap: () async {
                      await Get.toNamed(AppRoutes.osmCities, arguments: 1);
                      await _loadMeta();
                    },
                  ),
                  HelmNavTile(
                    color: AppTheme.iconCache,
                    icon: Icons.folder_outlined,
                    title: LocaleKeys.settingsCache.tr,
                    subtitle: _cacheLabel.isEmpty
                        ? LocaleKeys.settingsCacheSub.tr
                        : '$_cacheLabel · ${LocaleKeys.settingsCacheSub.tr}',
                    onTap: () async {
                      await Get.toNamed(AppRoutes.appCache);
                      await _loadMeta();
                    },
                  ),
                  HelmNavTile(
                    color: AppTheme.iconLog,
                    icon: Icons.article_outlined,
                    title: LocaleKeys.logTitle.tr,
                    subtitle: LocaleKeys.settingsLogsSub.tr,
                    onTap: () => Get.toNamed(AppRoutes.logViewer),
                  ),
                  HelmNavTile(
                    color: AppTheme.iconAppUpdate,
                    icon: Icons.system_update_alt,
                    title: LocaleKeys.settingsAppUpdate.tr,
                    subtitle: _ver.isEmpty
                        ? null
                        : LocaleKeys.settingsAppUpdateCurrent.trParams({
                            'ver': _ver,
                          }),
                    onTap: () => Get.toNamed(AppRoutes.appUpdate),
                  ),
                  HelmNavTile(
                    color: AppTheme.iconAbout,
                    icon: Icons.info_outline,
                    title: LocaleKeys.aboutTitle.tr,
                    onTap: () => Get.toNamed(AppRoutes.about),
                  ),
                ],
              ),
            ),
          ),
          const SizedBox(height: 28),
          GestureDetector(
            onLongPress: () => Get.toNamed(AppRoutes.lab),
            child: Text(
              _ver.isEmpty
                  ? LocaleKeys.settingsVersionHint.tr
                  : '${LocaleKeys.appName.tr}  ·  $_ver',
              textAlign: TextAlign.center,
              style: const TextStyle(color: AppTheme.muted, fontSize: 12),
            ),
          ),
        ],
      ),
    );
  }
}

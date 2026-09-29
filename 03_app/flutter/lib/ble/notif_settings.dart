import 'dart:convert';

import 'package:shared_preferences/shared_preferences.dart';
import 'package:sifli_companion/app/app_keys.dart';
import 'package:sifli_companion/i18n/locale_keys.dart';

/// 自定义通知里的一个应用（可对应多个包名，例如各厂商短信）。
class NotifAppPreset {
  /// 创建预设。
  const NotifAppPreset({
    required this.id,
    required this.nameKey,
    required this.packages,
  });

  /// 稳定 id，写入偏好。
  final String id;

  /// [LocaleKeys] 显示名。
  final String nameKey;

  /// 匹配的 Android 包名，第一个为主包。
  final List<String> packages;

  /// 主包名。
  String get primary => packages.first;
}

/// 消息通知页的可选应用列表。
abstract class NotifAppCatalog {
  /// 常见通讯 / 出行应用。
  static const apps = <NotifAppPreset>[
    NotifAppPreset(
      id: 'sms',
      nameKey: LocaleKeys.notifAppSms,
      packages: [
        'com.android.mms',
        'com.google.android.apps.messaging',
        'com.android.messaging',
        'com.xiaomi.mms',
        'com.samsung.android.messaging',
      ],
    ),
    NotifAppPreset(
      id: 'wechat',
      nameKey: LocaleKeys.notifAppWechat,
      packages: ['com.tencent.mm'],
    ),
    NotifAppPreset(
      id: 'alipay',
      nameKey: LocaleKeys.notifAppAlipay,
      packages: ['com.eg.android.AlipayGphone'],
    ),
    NotifAppPreset(
      id: 'baidu_map',
      nameKey: LocaleKeys.notifAppBaiduMap,
      packages: ['com.baidu.BaiduMap'],
    ),
    NotifAppPreset(
      id: 'dingtalk',
      nameKey: LocaleKeys.notifAppDingTalk,
      packages: ['com.alibaba.android.rimet'],
    ),
    NotifAppPreset(
      id: 'didi',
      nameKey: LocaleKeys.notifAppDidi,
      packages: ['com.sdu.didi.psngerapp'],
    ),
    NotifAppPreset(
      id: 'meituan',
      nameKey: LocaleKeys.notifAppMeituan,
      packages: ['com.sankuai.meituan'],
    ),
    NotifAppPreset(
      id: 'qq',
      nameKey: LocaleKeys.notifAppQq,
      packages: ['com.tencent.mobileqq', 'com.tencent.tim'],
    ),
    NotifAppPreset(
      id: 'qqmail',
      nameKey: LocaleKeys.notifAppQqMail,
      packages: ['com.tencent.androidqqmail'],
    ),
  ];

  /// 包名属于哪个预设；没有则为 `null`。
  static NotifAppPreset? presetForPackage(String pkg) {
    for (final app in apps) {
      if (app.packages.contains(pkg)) return app;
    }
    return null;
  }
}

/// 消息通知偏好（本地持久化）。
class NotifSettings {
  NotifSettings._();

  /// 单例。
  static final NotifSettings instance = NotifSettings._();

  static const _lockKey = 'companion.notif_lock_screen_only';
  static const _rideKey = 'companion.notif_ride_only';
  static const _customModeKey = 'companion.notif_custom_mode';
  static const _customPkgsKey = 'companion.notif_custom_packages';

  bool _loaded = false;

  /// 总开关，与 [AppKeys.notifForwardKey] 相同。
  bool enabled = true;

  /// 仅在手机锁屏（或息屏）时下发。
  bool lockScreenOnly = false;

  /// 仅在码表骑行中下发。
  bool rideOnly = false;

  /// `true` 为自定义应用列表；`false` 为同步全部手机通知。
  bool customMode = false;

  /// 自定义模式下允许的包名。
  Set<String> customPackages = {};

  /// 磁盘上是否已有自定义包名（含用户存过空列表）。未写入时用常见应用做默认勾选。
  bool _customFromDisk = false;

  /// 从磁盘读取；进程内只强制读一次，之后 [reload] 可再读。
  Future<void> ensureLoaded() async {
    if (_loaded) return;
    await reload();
  }

  /// 重新读盘。
  Future<void> reload() async {
    final prefs = await SharedPreferences.getInstance();
    enabled = prefs.getBool(AppKeys.notifForwardKey) ?? true;
    lockScreenOnly = prefs.getBool(_lockKey) ?? false;
    rideOnly = prefs.getBool(_rideKey) ?? false;
    customMode = prefs.getBool(_customModeKey) ?? false;
    final raw = prefs.getString(_customPkgsKey);
    _customFromDisk = raw != null;
    if (raw == null) {
      customPackages = {
        for (final app in NotifAppCatalog.apps) app.primary,
      };
    } else if (raw.isEmpty) {
      customPackages = {};
    } else {
      try {
        final list = jsonDecode(raw);
        customPackages = {
          if (list is List) for (final e in list) '$e',
        };
      } catch (_) {
        customPackages = {
          for (final app in NotifAppCatalog.apps) app.primary,
        };
      }
    }
    _loaded = true;
  }

  /// 首次进入：只把本机已安装、且属于常见通讯/出行目录的应用设为默认勾选。
  Future<void> seedCustomFromInstalled(Iterable<String> installed) async {
    if (_customFromDisk) return;
    customPackages = {
      for (final pkg in installed)
        if (NotifAppCatalog.presetForPackage(pkg) != null) pkg,
    };
    _customFromDisk = true;
    final prefs = await SharedPreferences.getInstance();
    await prefs.setString(
      _customPkgsKey,
      jsonEncode(customPackages.toList()..sort()),
    );
  }

  Future<void> setEnabled(bool v) async {
    enabled = v;
    final prefs = await SharedPreferences.getInstance();
    await prefs.setBool(AppKeys.notifForwardKey, v);
  }

  Future<void> setLockScreenOnly(bool v) async {
    lockScreenOnly = v;
    final prefs = await SharedPreferences.getInstance();
    await prefs.setBool(_lockKey, v);
  }

  Future<void> setRideOnly(bool v) async {
    rideOnly = v;
    final prefs = await SharedPreferences.getInstance();
    await prefs.setBool(_rideKey, v);
  }

  Future<void> setCustomMode(bool v) async {
    customMode = v;
    final prefs = await SharedPreferences.getInstance();
    await prefs.setBool(_customModeKey, v);
  }

  Future<void> setPackageEnabled(String primary, bool on) async {
    if (on) {
      customPackages.add(primary);
    } else {
      customPackages.remove(primary);
      final preset = NotifAppCatalog.presetForPackage(primary);
      if (preset != null) {
        customPackages.removeAll(preset.packages);
      }
    }
    final prefs = await SharedPreferences.getInstance();
    await prefs.setString(
      _customPkgsKey,
      jsonEncode(customPackages.toList()..sort()),
    );
  }

  /// 当前包是否允许下发（同步模式一律允许）。
  bool allowsPackage(String pkg) {
    if (!customMode) return true;
    if (customPackages.contains(pkg)) return true;
    final preset = NotifAppCatalog.presetForPackage(pkg);
    if (preset == null) return false;
    return preset.packages.any(customPackages.contains);
  }
}

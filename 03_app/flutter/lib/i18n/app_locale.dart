import 'package:flutter/material.dart';
import 'package:get/get.dart';
import 'package:sifli_companion/app/app_keys.dart';
import 'package:sifli_companion/utils/cache_data.dart';

/// 语言环境解析、切换与持久化。
///
/// KV 使用 `zh-CN` / `en-US`（短横线）；GetX 内部键为 `zh_CN` / `en_US`。
class AppLocale {
  AppLocale._();

  /// 简体中文。
  static const zhCN = Locale('zh', 'CN');

  /// 美式英语。
  static const enUS = Locale('en', 'US');

  /// [GetMaterialApp.supportedLocales]。
  static const supported = <Locale>[zhCN, enUS];

  /// 缺省回退语言。
  static const fallback = zhCN;

  /// 从 KV / 配置标签解析 [Locale]；无法识别时返回 [fallback]。
  static Locale fromTag(String? tag) {
    if (tag == null || tag.isEmpty) return fallback;
    final normalized = tag.trim().replaceAll('-', '_');
    final parts = normalized.split('_');
    if (parts.isEmpty || parts.first.isEmpty) return fallback;

    final language = parts.first.toLowerCase();
    final country = parts.length > 1 ? parts[1].toUpperCase() : '';

    for (final locale in supported) {
      final sameLang = locale.languageCode.toLowerCase() == language;
      final sameCountry = country.isEmpty ||
          (locale.countryCode?.toUpperCase() == country);
      if (sameLang && sameCountry) return locale;
    }
    return fallback;
  }

  /// 转为持久化标签，例如 `zh-CN`。
  static String toTag(Locale locale) {
    final country = locale.countryCode;
    if (country != null && country.isNotEmpty) {
      return '${locale.languageCode}-$country';
    }
    return locale.languageCode;
  }

  /// 启动时根据 [CacheData.localeDesc] 得到当前 [Locale]。
  static Locale currentFromCache() => fromTag(CacheData().localeDesc);

  /// 切换语言并写入 KV，同时调用 [Get.updateLocale]。
  static Future<void> apply(Locale locale) async {
    final tag = toTag(locale);
    await CacheData().setLocaleDesc(tag);
    await Get.updateLocale(locale);
  }

  /// 用标签切换语言（如 `en-US`）。
  static Future<void> applyTag(String tag) => apply(fromTag(tag));

  /// 在中英文之间切换（演示 / 设置入口用）。
  static Future<void> toggleZhEn() async {
    final now = Get.locale ?? currentFromCache();
    final next = now.languageCode == 'zh' ? enUS : zhCN;
    await apply(next);
  }

  /// 是否为默认语言标签。
  static bool isDefaultTag(String tag) =>
      tag == AppKeys.defaultLocale || fromTag(tag) == fallback;
}

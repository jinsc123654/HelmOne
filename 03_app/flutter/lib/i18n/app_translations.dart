import 'package:get/get.dart';
import 'package:sifli_companion/i18n/langs/en_us.dart';
import 'package:sifli_companion/i18n/langs/zh_cn.dart';

/// GetX 多语言表。键格式为 `languageCode_countryCode`（如 `zh_CN`）。
class AppTranslations extends Translations {
  @override
  Map<String, Map<String, String>> get keys => {
        'zh_CN': zhCn,
        'en_US': enUs,
      };
}

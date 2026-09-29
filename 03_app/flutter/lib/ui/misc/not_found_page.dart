import 'package:flutter/material.dart';
import 'package:get/get.dart';
import 'package:sifli_companion/app/app_routes.dart';
import 'package:sifli_companion/i18n/locale_keys.dart';

/// 未知路由页：引导回首页。
class NotFoundPage extends StatelessWidget {
  /// 创建未知路由页。
  const NotFoundPage({super.key});

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: Text(LocaleKeys.notFoundTitle.tr)),
      body: Center(
        child: FilledButton(
          onPressed: () => Get.offAllNamed(AppRoutes.home),
          child: Text(LocaleKeys.notFoundGoHome.tr),
        ),
      ),
    );
  }
}

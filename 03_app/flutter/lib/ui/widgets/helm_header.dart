import 'package:flutter/material.dart';
import 'package:get/get.dart';
import 'package:sifli_companion/app/app_assets.dart';
import 'package:sifli_companion/app/app_routes.dart';
import 'package:sifli_companion/app/app_theme.dart';
import 'package:sifli_companion/i18n/locale_keys.dart';

/// 设置 Tab 顶：作者信息（无登录时替代「我的」）。
class HelmAuthorHeader extends StatelessWidget {
  const HelmAuthorHeader({super.key});

  @override
  Widget build(BuildContext context) {
    return InkWell(
      onTap: () => Get.toNamed(AppRoutes.about),
      child: Padding(
        padding: const EdgeInsets.fromLTRB(20, 18, 12, 18),
        child: Row(
          children: [
            ClipOval(
              child: Image.asset(
                AppAssets.developerLogo,
                width: 64,
                height: 64,
                fit: BoxFit.cover,
              ),
            ),
            const SizedBox(width: 14),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(
                    LocaleKeys.aboutDeveloperName.tr,
                    style: const TextStyle(
                      color: Colors.white,
                      fontSize: 22,
                      fontWeight: FontWeight.w700,
                      height: 1.2,
                    ),
                  ),
                  const SizedBox(height: 4),
                  Text(
                    LocaleKeys.authorRole.tr,
                    style: const TextStyle(
                      color: AppTheme.muted,
                      fontSize: 13,
                    ),
                  ),
                ],
              ),
            ),
            const Icon(Icons.chevron_right, color: AppTheme.muted, size: 22),
          ],
        ),
      ),
    );
  }
}

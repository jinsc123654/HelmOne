import 'dart:async';
import 'dart:ui';

import 'package:flutter/material.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:flutter_localizations/flutter_localizations.dart';
import 'package:get/get.dart';
import 'package:sifli_companion/app/app_keys.dart';
import 'package:sifli_companion/app/app_pages.dart';
import 'package:sifli_companion/app/app_routes.dart';
import 'package:sifli_companion/app/app_theme.dart';
import 'package:sifli_companion/ble/ble_manager.dart';
import 'package:sifli_companion/ble/companion_background.dart';
import 'package:sifli_companion/ble/companion_notif_relay.dart';
import 'package:sifli_companion/db/db_manager.dart';
import 'package:sifli_companion/i18n/app_locale.dart';
import 'package:sifli_companion/i18n/app_translations.dart';
import 'package:sifli_companion/log/app_log.dart';
import 'package:sifli_companion/map/osm_tiles.dart';
import 'package:sifli_companion/transfer/transfer_center.dart';
import 'package:sifli_companion/ui/widgets/transfer_overlay.dart';
import 'package:sifli_companion/util/app_share.dart';
import 'package:sifli_companion/utils/cache_data.dart';

/// 应用入口。
///
/// 启动顺序：日志 → 蓝牙软初始化 → [CacheData] → [DBManager] → OSM 瓦片缓存 → [runApp]。
void main() {
  runZonedGuarded(() async {
    WidgetsFlutterBinding.ensureInitialized();

    // **必须在这里**（或更早）：`SharePlus.instance` 是 `static final`，第一次被读到时
    // 就把当时的 SharePlatform 实现拷下来了 —— 装晚了就绕过这个装饰器。
    await installShareAsk();

    await AppLog.init();

    FlutterError.onError = (details) {
      FlutterError.presentError(details);
      // 布局类报错（溢出、约束冲突）真正能定位问题的信息 —— 哪个控件、建在哪一行 ——
      // 都在 informationCollector 里，exception 本身只有一句概括。只记后者的话，
      // 真机日志上就只剩「overflowed by 55 pixels」，等于没线索。
      final info = details.informationCollector?.call();
      final context = details.context?.toString() ?? '';
      final extra = <String>[
        if (context.isNotEmpty) context,
        if (info != null) for (final node in info) node.toString(),
      ].join('\n');
      unawaited(
        AppLog.reportUncaught(
          'FlutterError',
          extra.isEmpty ? '${details.exception}' : '${details.exception}\n$extra',
          details.stack,
        ),
      );
    };
    PlatformDispatcher.instance.onError = (error, stack) {
      unawaited(AppLog.reportUncaught('PlatformDispatcher', error, stack));
      return true;
    };

    FlutterBluePlus.setLogLevel(LogLevel.none);
    await BleManager().init();
    await CacheData().initInfo();
    await DBManager().initDB();
    await HelmOsmTiles.init();
    await AppLog.i('main', 'services ready, agreed=${CacheData().hasAgreed}');

    // permanent: true —— 抓取进度不属于任何页面，路由怎么切都不能被回收。
    Get.put(TransferCenter(), permanent: true);

    /* 视图门控：`runApp` 要求平台提供"隐式视图"，而进程不一定有 —— 系统用
     * START_STICKY 重启前台服务时只创建服务、不创建 Activity，也就没有 FlutterView，
     * 那种情况下 `runApp` 会直接抛 StateError（SDK `widgets/binding.dart` 的
     * `wrapWithDefaultView`）。可"进程活着"正是保活的目的：连接、通知转发都有活要干。
     * 所以无视图时先只跑后台链路，等用户真的打开 App、视图出现，再把界面挂上去。 */
    if (WidgetsBinding.instance.platformDispatcher.implicitView != null) {
      runApp(const MyApp());
      WidgetsBinding.instance.addPostFrameCallback((_) {
        unawaited(CompanionNotifRelay.instance.boot());
      });
    } else {
      unawaited(CompanionBackground.start(attachUi: _attachUi));
    }
  }, (error, stack) {
    unawaited(AppLog.reportUncaught('Zone', error, stack));
  });
}

/// 视图出现后挂界面（无视图启动时用 `runWidget` 顶替 `runApp`）。
void _attachUi() {
  final dispatcher = WidgetsBinding.instance.platformDispatcher;
  final view = dispatcher.implicitView ??
      (dispatcher.views.isEmpty ? null : dispatcher.views.first);
  if (view == null) return;

  runWidget(View(view: view, child: const MyApp()));
  WidgetsBinding.instance.addPostFrameCallback((_) {
    unawaited(CompanionNotifRelay.instance.boot());
  });
}

/// 根组件：注册路由表与多语言。
class MyApp extends StatelessWidget {
  /// 创建根组件。
  const MyApp({super.key});

  @override
  Widget build(BuildContext context) {
    final debugStart = const bool.fromEnvironment('COMPANION_DEBUG');
    final initial = debugStart
        ? AppRoutes.companionDebug
        : (CacheData().hasAgreed ? AppRoutes.home : AppRoutes.welcome);

    return GetMaterialApp(
      title: AppKeys.appName,
      // 滚动的"手感"在这里统一定：iOS 那种回弹，加上桌面端也能拖动。
      scrollBehavior: const MaterialScrollBehavior().copyWith(
        physics: const BouncingScrollPhysics(
          parent: AlwaysScrollableScrollPhysics(),
        ),
        dragDevices: {
          PointerDeviceKind.touch,
          PointerDeviceKind.mouse,
          PointerDeviceKind.trackpad,
          PointerDeviceKind.stylus,
        },
      ),
      theme: AppTheme.dark,
      themeMode: ThemeMode.dark,
      translations: AppTranslations(),
      locale: AppLocale.currentFromCache(),
      fallbackLocale: AppLocale.fallback,
      supportedLocales: AppLocale.supported,
      localizationsDelegates: const [
        GlobalMaterialLocalizations.delegate,
        GlobalWidgetsLocalizations.delegate,
        GlobalCupertinoLocalizations.delegate,
      ],
      initialRoute: initial,
      getPages: AppPages.routes,
      unknownRoute: AppPages.unknown,
      defaultTransition: Transition.cupertino,
      // 悬浮球挂在这一层：位于 Navigator **之上**，所以任何 push/pop
      // 都不会把它带走 —— 抓取中切页也能看到进度和速度。
      builder: (context, child) => Stack(
        children: [
          ?child,
          const TransferOverlay(),
        ],
      ),
    );
  }
}

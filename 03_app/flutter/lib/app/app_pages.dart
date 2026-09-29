import 'package:get/get.dart';
import 'package:sifli_companion/app/app_routes.dart';
import 'package:sifli_companion/ui/ble/ble_scan_page.dart';
import 'package:sifli_companion/ui/ble/companion_debug_page.dart';
import 'package:sifli_companion/ui/ble/companion_fs_page.dart';
import 'package:sifli_companion/ui/cache/app_cache_browser_page.dart';
import 'package:sifli_companion/ui/device/log_capture_page.dart';
import 'package:sifli_companion/ui/device/rebind_page.dart';
import 'package:sifli_companion/ui/gnss/eph_page.dart';
import 'package:sifli_companion/ui/home/home_binding.dart';
import 'package:sifli_companion/ui/home/home_page.dart';
import 'package:sifli_companion/ui/log/log_viewer_page.dart';
import 'package:sifli_companion/ui/map/location_map_page.dart';
import 'package:sifli_companion/ui/misc/not_found_page.dart';
import 'package:sifli_companion/ui/notif/notif_settings_page.dart';
import 'package:sifli_companion/ui/ota/ota_page.dart';
import 'package:sifli_companion/ui/ride/favorites_page.dart';
import 'package:sifli_companion/ui/ride/gpx_library_page.dart';
import 'package:sifli_companion/ui/ride/nav_routes_page.dart';
import 'package:sifli_companion/ui/ride/ride_detail_page.dart';
import 'package:sifli_companion/ui/ride/waypoint_editor_page.dart';
import 'package:sifli_companion/ui/settings/about_page.dart';
import 'package:sifli_companion/ui/settings/app_update_page.dart';
import 'package:sifli_companion/ui/settings/map_usb_page.dart';
import 'package:sifli_companion/ui/settings/osm_cities_page.dart';
import 'package:sifli_companion/ui/shell/shell_binding.dart';
import 'package:sifli_companion/ui/shell/shell_page.dart';
import 'package:sifli_companion/ui/welcome/welcome_page.dart';

/// GetX 路由表：页面工厂与 Binding 注册。
///
/// 新增页面：先在 [AppRoutes] 加常量，再在 [routes] 加 [GetPage]。
class AppPages {
  /// 全部 [GetPage] 列表，交给 [GetMaterialApp.getPages]。
  static final routes = <GetPage<dynamic>>[
    GetPage(
      name: AppRoutes.welcome,
      page: () => const WelcomePage(),
    ),
    GetPage(
      name: AppRoutes.home,
      page: () => const ShellPage(),
      binding: ShellBinding(),
    ),
    GetPage(
      name: AppRoutes.lab,
      page: () => const HomePage(),
      binding: HomeBinding(),
    ),
    GetPage(
      name: AppRoutes.bleScan,
      page: () => const BleScanPage(),
    ),
    GetPage(
      name: AppRoutes.companionDebug,
      page: () => const CompanionDebugPage(),
    ),
    GetPage(
      name: AppRoutes.companionFs,
      page: () => const CompanionFsPage(),
    ),
    GetPage(
      name: AppRoutes.companionOta,
      page: () => const OtaPage(),
    ),
    GetPage(
      name: AppRoutes.companionEph,
      page: () => const EphPage(),
    ),
    GetPage(
      name: AppRoutes.companionCoredump,
      page: () => const LogCapturePage(),
    ),
    GetPage(
      name: AppRoutes.deviceRebind,
      page: () => const RebindPage(),
    ),
    GetPage(
      name: AppRoutes.notifSettings,
      page: () => const NotifSettingsPage(),
    ),
    GetPage(
      name: AppRoutes.appCache,
      page: () => const AppCacheBrowserPage(),
    ),
    GetPage(
      name: AppRoutes.logViewer,
      page: () => const LogViewerPage(),
    ),
    GetPage(
      name: AppRoutes.locationMap,
      page: () => const LocationMapPage(),
    ),
    GetPage(
      name: AppRoutes.navRoutes,
      page: () => const NavRoutesPage(),
    ),
    GetPage(
      name: AppRoutes.waypointEditor,
      page: () => const WaypointEditorPage(),
    ),
    GetPage(
      name: AppRoutes.gpxLibrary,
      page: () => const GpxLibraryPage(),
    ),
    GetPage(
      name: AppRoutes.favorites,
      page: () => const FavoritesPage(),
    ),
    GetPage(
      name: AppRoutes.rideDetail,
      page: () => const RideDetailPage(),
    ),
    GetPage(
      name: AppRoutes.mapUsb,
      page: () => const MapUsbPage(),
    ),
    GetPage(
      name: AppRoutes.osmCities,
      page: () {
        final tab = (Get.arguments is int) ? Get.arguments as int : 0;
        return OsmCitiesPage(initialTab: tab);
      },
    ),
    GetPage(
      name: AppRoutes.appUpdate,
      page: () => const AppUpdatePage(),
    ),
    GetPage(
      name: AppRoutes.about,
      page: () => const AboutPage(),
    ),
  ];

  /// 未知路由回退页。
  static final unknown = GetPage<dynamic>(
    name: AppRoutes.notFound,
    page: () => const NotFoundPage(),
  );
}

import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:get/get.dart';
import 'package:sifli_companion/app/app_keys.dart';
import 'package:sifli_companion/app/app_routes.dart';
import 'package:sifli_companion/app/app_theme.dart';
import 'package:sifli_companion/ble/ble_manager.dart';
import 'package:sifli_companion/db/db_manager.dart';
import 'package:sifli_companion/i18n/app_locale.dart';
import 'package:sifli_companion/i18n/locale_keys.dart';
import 'package:sifli_companion/utils/cache_data.dart';

/// 首页逻辑：KV/DB 状态、持久化自测与进入蓝牙扫描。
class HomeController extends GetxController {
  /// BLE 是否受支持（文案）。
  final bleSupport = '…'.obs;

  /// 适配器状态名。
  final adapterState = '…'.obs;

  /// 当前语言环境。
  final locale = AppKeys.defaultLocale.obs;

  /// 数据库文件路径。
  final dbPath = '…'.obs;

  /// KV/DB 等服务是否已就绪。
  final servicesReady = false.obs;

  /// KV 自测计数。
  final kvCounter = 0.obs;

  /// KV 自测上次写入时间。
  final kvUpdatedAt = ''.obs;

  /// DB 自测计数。
  final dbCounter = 0.obs;

  /// DB 自测上次写入时间。
  final dbUpdatedAt = ''.obs;

  StreamSubscription<BluetoothAdapterState>? _adapterSub;

  @override
  void onInit() {
    super.onInit();
    kvUpdatedAt.value = LocaleKeys.neverWritten.tr;
    dbUpdatedAt.value = LocaleKeys.neverWritten.tr;
    refreshStatus();
    _adapterSub = FlutterBluePlus.adapterState.listen(
      (state) => adapterState.value = state.name,
      onError: (_) {},
      cancelOnError: true,
    );
  }

  @override
  void onClose() {
    _adapterSub?.cancel();
    super.onClose();
  }

  String _formatMs(int? ms) {
    if (ms == null) return LocaleKeys.neverWritten.tr;
    return DateTime.fromMillisecondsSinceEpoch(ms).toString().substring(0, 19);
  }

  /// 切换语言并刷新展示文案。
  Future<void> changeLocale(Locale next) async {
    await AppLocale.apply(next);
    locale.value = AppLocale.toTag(next);
    await refreshStatus();
  }

  /// 从磁盘重新读取 KV/DB 状态与蓝牙信息。
  Future<void> refreshStatus() async {
    try {
      final supported = await BleManager().isSupported();
      final path = DBManager().db.path;
      final kv =
          await CacheData().getInt(AppKeys.persistKvCounterKey) ?? 0;
      final kvAt =
          await CacheData().getString(AppKeys.persistKvUpdatedAtKey);
      final db = await DBManager().getPersistTestCounter();
      final dbAt = await DBManager().getPersistTestUpdatedAt();

      servicesReady.value = true;
      bleSupport.value = supported ? 'supported' : 'unsupported';
      adapterState.value = BleManager().adapterState.name;
      locale.value = CacheData().localeDesc;
      dbPath.value = path;
      kvCounter.value = kv;
      kvUpdatedAt.value = kvAt ?? LocaleKeys.neverWritten.tr;
      dbCounter.value = db;
      dbUpdatedAt.value = _formatMs(dbAt);
    } catch (_) {
      servicesReady.value = false;
      locale.value = AppKeys.defaultLocale;
    }
  }

  /// KV 计数 +1 并持久化，用于验证 SharedPreferences。
  Future<void> bumpKv() async {
    final next = kvCounter.value + 1;
    final now = DateTime.now().toString().substring(0, 19);
    await CacheData().setInt(AppKeys.persistKvCounterKey, next);
    await CacheData().setString(AppKeys.persistKvUpdatedAtKey, now);
    kvCounter.value = next;
    kvUpdatedAt.value = now;
    AppTheme.snack(
      'KV',
      LocaleKeys.homeKvSnackbar.trParams({'n': '$next'}),
    );
  }

  /// DB 计数 +1 并持久化，用于验证 sqflite。
  Future<void> bumpDb() async {
    final next = await DBManager().bumpPersistTestCounter();
    final ms = await DBManager().getPersistTestUpdatedAt();
    dbCounter.value = next;
    dbUpdatedAt.value = _formatMs(ms);
    AppTheme.snack(
      'DB',
      LocaleKeys.homeDbSnackbar.trParams({'n': '$next'}),
    );
  }

  /// 申请权限并打开 [AppRoutes.bleScan]；返回后刷新状态。
  Future<void> openBleScan() async {
    final ok = await BleManager().requestPermissions();
    if (!ok) {
      AppTheme.snack(
        LocaleKeys.bleSnackbarTitle.tr,
        LocaleKeys.homeBlePermissionDenied.tr,
      );
      return;
    }
    await BleManager().turnOn();
    await Get.toNamed(AppRoutes.bleScan);
    await refreshStatus();
  }

  /// 打开本地日志查看页。
  void openLogs() => Get.toNamed(AppRoutes.logViewer);

  /// 申请权限并打开 Companion 协议联调页。
  Future<void> openCompanionDebug() async {
    final ok = await BleManager().requestPermissions();
    if (!ok) {
      AppTheme.snack(
        LocaleKeys.bleSnackbarTitle.tr,
        LocaleKeys.homeBlePermissionDenied.tr,
      );
      return;
    }
    await BleManager().turnOn();
    await Get.toNamed(AppRoutes.companionDebug);
    await refreshStatus();
  }

  /// 打开固件 OTA 页。
  Future<void> openOta() async {
    final ok = await BleManager().requestPermissions();
    if (!ok) {
      AppTheme.snack(
        LocaleKeys.bleSnackbarTitle.tr,
        LocaleKeys.homeBlePermissionDenied.tr,
      );
      return;
    }
    await BleManager().turnOn();
    await Get.toNamed(AppRoutes.companionOta);
    await refreshStatus();
  }

  /// 打开星历同步页。
  Future<void> openEph() async {
    final ok = await BleManager().requestPermissions();
    if (!ok) {
      AppTheme.snack(
        LocaleKeys.bleSnackbarTitle.tr,
        LocaleKeys.homeBlePermissionDenied.tr,
      );
      return;
    }
    await BleManager().turnOn();
    await Get.toNamed(AppRoutes.companionEph);
    await refreshStatus();
  }

  /// 打开定位与地图页。
  void openMap() => Get.toNamed(AppRoutes.locationMap);
}

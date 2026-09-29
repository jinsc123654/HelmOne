import 'dart:async';

import 'package:flutter/widgets.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:get/get.dart';
import 'package:sifli_companion/app/app_keys.dart';
import 'package:sifli_companion/i18n/locale_keys.dart';
import 'package:sifli_companion/ble/companion_client.dart';
import 'package:sifli_companion/ble/companion_foreground_service.dart';
import 'package:sifli_companion/ble/companion_notif_relay.dart';
import 'package:sifli_companion/ble/companion_proto.dart';
import 'package:sifli_companion/gnss/eph_auto_sync.dart';
import 'package:sifli_companion/log/app_log.dart';
import 'package:sifli_companion/ride/favorite_store.dart';
import 'package:sifli_companion/ride/gpx_library_store.dart';
import 'package:sifli_companion/ride/mcu_sync.dart';
import 'package:sifli_companion/ride/nav_route_store.dart';
import 'package:sifli_companion/ride/ride_record_store.dart';
import 'package:sifli_companion/utils/cache_data.dart';

/// 产品壳：Tab、Companion 状态、本机路书库。
class ShellController extends GetxController with WidgetsBindingObserver {
  /// 底部 Tab：0 骑行 / 1 设备 / 2 设置。
  ///
  /// **入口落点按"当前有没有连上"决定**（见 [onInit]）：未连接落在设备页（默认
  /// 就是这里），已连接落在骑行页 —— 针对"后台/前台服务已经把码表连上了，用户
  /// 这会儿才打开界面"的场景，不要先甩他一个"未连接"的设备页让他自己切。
  final tab = 1.obs;

  /// Companion 链路。
  final conn = CompanionConnState.disconnected.obs;

  /// 最近一帧设备状态。
  final status = Rxn<CompanionStatus>();

  /// 0xFF1A。
  final info = Rxn<CompanionDevInfo>();

  /// 本机骑行记录条数（驱动列表刷新）。
  final rideTick = 0.obs;

  /// 正在同步记录。
  final syncing = false.obs;

  /// 当前同步进度；空表示未在传数据。
  final syncProg = Rxn<McuSyncProgress>();

  /// 滑动窗口测得的 KB/s。
  final syncKbs = 0.0.obs;

  Stopwatch? _syncSw;
  int _syncLastXfer = 0;
  int _syncLastMs = 0;
  DateTime _syncLastUi = DateTime.fromMillisecondsSinceEpoch(0);

  StreamSubscription<CompanionConnState>? _stateSub;
  StreamSubscription<CompanionStatus>? _statusSub;

  /// 用户点「断开」之前，后台也继续找上次的码表。
  bool _holdLink = true;

  /// 是否已记住绑定设备（缓存 id 或系统已配对）。有绑定就禁止再扫，只能回连/解绑。
  final hasBond = false.obs;

  /// 最近一次**用户主动操作**失败的原因。自动回连失败不写这里，避免红字刷屏。
  final linkError = RxnString();

  CompanionClient get client => CompanionClient.instance;

  String get deviceName {
    final d = client.device;
    if (d == null) return '';
    if (d.platformName.isNotEmpty) return d.platformName;
    if (d.advName.isNotEmpty) return d.advName;
    return '';
  }

  bool get isReady => conn.value == CompanionConnState.ready;

  /// 正在连 / 发现，或已绑定且后台静默回连中。
  bool get isReconnecting =>
      conn.value == CompanionConnState.connecting ||
      conn.value == CompanionConnState.discovering ||
      (hasBond.value &&
          !isReady &&
          _holdLink &&
          (client.autoReconnect || client.isLinking));

  @override
  void onInit() {
    super.onInit();
    WidgetsBinding.instance.addObserver(this);
    conn.value = client.state;
    status.value = client.lastStatus;
    info.value = client.lastDevInfo;
    _stateSub = client.stateStream.listen((s) {
      /* 通知栏跟着状态走：服务在跑就把真实状态刷进常驻通知，没在跑就不去碰
       * （服务不在时 update 会被平台当成一次启动）。升级固件等抢占文案由
       * CompanionForegroundService.pin 挡掉。 */
      unawaited(CompanionForegroundService.updateLinkState(_fgsTextFor(s)));
      conn.value = s;
      info.value = client.lastDevInfo;
      switch (s) {
        case CompanionConnState.ready:
          linkError.value = null;
          hasBond.value = true;
        case CompanionConnState.versionMismatch:
          linkError.value = LocaleKeys.deviceMismatch.tr;
        case CompanionConnState.error:
          // 自动回连中的瞬时失败不写 UI、不弹 toast —— 用户只看到「正在回连」。
          if (_holdLink && client.autoReconnect) {
            linkError.value = null;
          } else {
            linkError.value =
                client.connectHint ?? LocaleKeys.bleScanConnectFailed.tr;
          }
        case CompanionConnState.connecting:
        case CompanionConnState.discovering:
        case CompanionConnState.disconnected:
          break;
      }
    });
    _statusSub = client.statusStream.listen((s) => status.value = s);
    // 通知栏可能在上次进程里就起着（前台服务不随 Activity 重建而消失）：先把服务状态
    // 问回来，否则「服务没在跑」的印象会让连接状态再也刷不进通知。文案同时被记下，
    // 供之后任何一次 start() 使用（见 CompanionForegroundService._lastLinkText）。
    unawaited(() async {
      await CompanionForegroundService.isRunning();
      await CompanionForegroundService.updateLinkState(_fgsTextFor(conn.value));
    }());
    // 星历自动同步：连上后按需补、连接期间每 2 小时一次（见 EphAutoSync）。
    EphAutoSync.instance.init();
    // 切到设备页时顺手刷新「下次同步」（节流过，点来点去也只查一次）。
    ever(tab, (int i) {
      if (i == 1) unawaited(EphAutoSync.instance.refreshStatus());
    });
    unawaited(_loadStores());
    unawaited(_refreshBondFlag());
    unawaited(_tryReconnect());

    // 入口落点：已连接 ⇒ 骑行页（0），未连接 ⇒ 设备页（1）。
    //
    // 两个都要考虑：
    // - 客户端在这条进程里**已经是 ready**（前台服务先连上了、用户这会儿才打开
    //   界面）⇒ 直接去骑行页；
    // - 冷启动 + 自动回连**正在路上**（`_tryReconnect` 刚发出去）⇒ 给 1.5 s 宽限，
    //   这一拍内连上、且用户还没自己切过页（tab 仍是 1）就补跳一次。超过 1.5 s
    //   就不追了 —— 用户在设备页上操作时页面自己跳走比"少跳一次"更糟。
    if (client.isReady) {
      tab.value = 0;
    } else {
      Future<void>.delayed(const Duration(milliseconds: 1500), () {
        if (client.isReady && tab.value == 1) tab.value = 0;
      });
    }
  }

  @override
  void onClose() {
    WidgetsBinding.instance.removeObserver(this);
    _stateSub?.cancel();
    _statusSub?.cancel();
    super.onClose();
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    switch (state) {
      case AppLifecycleState.resumed:
        unawaited(_tryReconnect());
      case AppLifecycleState.paused:
      case AppLifecycleState.hidden:
        unawaited(_onWentBackground());
      case AppLifecycleState.inactive:
      case AppLifecycleState.detached:
        break;
    }
  }

  Future<void> _loadStores() async {
    await RideRecordStore().load();
    await NavRouteStore().load();
    await FavoriteStore().load();
    await GpxLibraryStore().load();
    rideTick.value++;
  }

  /// 常驻通知显示**真实状态**。
  ///
  /// 原来固定一句"正在保持与码表的连接"，连接断了、在重连、版本不匹配都看不出来 ——
  /// 兜里掏出手机只看通知栏时，那句话等于没有信息。文案本体在 `CompanionForegroundService`
  /// 里（后台启动、还没有 UI 时也要用它刷新通知栏，两处各一份必然漂移）。
  String _fgsTextFor(CompanionConnState s) =>
      CompanionForegroundService.textForState(s);

  Future<void> _onWentBackground() async {
    if (!_holdLink) return;
    await _tryReconnect();
    if (!CompanionForegroundService.isSupported) return;
    final id = await CacheData().getString(AppKeys.lastBleDeviceIdKey);
    if (id == null || id.isEmpty) return;
    // 「保持连接」关掉时不拉前台服务：用户要的就是退出 App 后别再后台常驻。
    // 链路本身照旧（进程还活着就继续用），只是不再"保活"。
    if (!await CompanionForegroundService.keepAliveWanted()) return;
    // 不在这里弹通知权限：进后台时系统对话框会卡住。有权限才拉起保活。
    await CompanionForegroundService.start();

    /* 服务起来只是"保活"，**连接得自己踢**（后台没有任何 UI 路径会去连码表，
     * 不踢的话通知转发/文件/GNSS 推送都"活着但不工作"）。
     *
     * 但这一步**绝不能放进 `start()` 里面**：它最多等 15 s，而且抛的不是
     * `PlatformException`（`start()` 里那个 catch 兜不住），会把前台服务这条
     * 时序敏感的调用路径拖住甚至带崩 —— 2026-09-18 现场：通知栏保活不见了。
     * 所以放在这里、异步跑、自己吞异常。 */
    unawaited(() async {
      try {
        await client.ensureConnected();
      } catch (e) {
        await AppLog.w('Shell', '后台连接等待失败: $e');
      }
    }());
  }

  Future<void> _tryReconnect() async {
    if (!_holdLink) return;
    if (client.isReady) {
      await client.refreshTimeSyncIfDue();
      // 回前台补一次星历检查：后台定时器可能被系统冻住。
      unawaited(EphAutoSync.instance.kick());
      return;
    }
    if (client.isLinking) return;
    if (client.device != null && client.autoReconnect) {
      await client.kickReconnect(resetBackoff: true);
      return;
    }
    final d = await _cachedDevice();
    if (d == null) return;
    try {
      await client.connect(d);
      CompanionNotifRelay.instance.attachIconNeed();
    } catch (e) {
      await AppLog.w('Shell', '自动重连失败: $e');
    }
  }

  /// 刷新「是否已绑定」：缓存 id，或系统里恰好一台 Helm One。
  Future<void> _refreshBondFlag() async {
    final id = await CacheData().getString(AppKeys.lastBleDeviceIdKey);
    if (id != null && id.isNotEmpty) {
      hasBond.value = true;
      return;
    }
    try {
      final bonded = await FlutterBluePlus.bondedDevices;
      final helm = bonded
          .where((d) => CompanionProto.matchesAdvName(d.platformName))
          .toList();
      if (helm.length == 1) {
        await CacheData().setString(
          AppKeys.lastBleDeviceIdKey,
          helm.first.remoteId.str,
        );
        hasBond.value = true;
        return;
      }
    } catch (_) {}
    hasBond.value = false;
  }

  /// 已绑定设备的**身份地址**（拿不到就返回 null，调用方退回 remoteId）。
  ///
  /// 匹配顺序：地址一致 → 名字一致（`platformName`，形如 `Helm One-77C8`）。
  /// 名字那条是必需的：配对前扫描看到的是 RPA，和设备身份地址对不上。
  Future<String?> _identityIdFor(BluetoothDevice? device) async {
    if (device == null) return null;
    try {
      final bonded = await FlutterBluePlus.bondedDevices;
      final name = device.platformName;

      // 1) 地址一致（已绑定时扫描结果给的就是身份地址，最准）。
      for (final d in bonded) {
        if (d.remoteId == device.remoteId) return d.remoteId.str;
      }
      // 2) 名字**精确**一致 —— 用户可能有好几台 `Helm One-XXXX`，只有后缀能区分。
      if (name.isNotEmpty) {
        for (final d in bonded) {
          if (d.platformName == name) return d.remoteId.str;
        }
      }
      // 3) 最后才退到"同产品名"（GAP 名是 `Helm One`、广播名带后缀时会走到这里）——
      //    只在只有一台已绑定设备时才采用，避免多台之间挑错。
      if (name.isNotEmpty &&
          bonded.length == 1 &&
          _sameProduct(bonded.first.platformName, name)) {
        return bonded.first.remoteId.str;
      }
    } catch (_) {
      // 查询失败就退回 remoteId。
    }
    return null;
  }

  /// 两个显示名是不是同一台码表：比较去掉 `-XXXX` 后缀后的部分。
  bool _sameProduct(String a, String b) {
    String base(String s) {
      final i = s.lastIndexOf('-');
      return (i > 0 ? s.substring(0, i) : s).trim().toLowerCase();
    }

    final ba = base(a);
    final bb = base(b);
    return ba.isNotEmpty && ba == bb;
  }

  /// 刷新本机列表。
  Future<void> reloadLocal() async {
    await _loadStores();
  }

  /// 连接成功后记住设备并拉起前台服务。
  Future<void> onConnected() async {
    _holdLink = true;
    // **缓存"身份地址"，不是扫描看到的 RPA**（2026-09-20）：
    // LE 对端常用可解析私有地址，扫描结果里的 remoteId 是 RPA、会轮换；下次拿旧
    // RPA 去 connect/createBond 必然失败，而**设备侧的绑定记录与准入认的是身份地址**。
    // 已绑定设备的 `bondedDevices` 给的就是身份地址 —— 优先用它。
    final id =
        await _identityIdFor(client.device) ?? client.device?.remoteId.str;
    if (id != null) {
      await CacheData().setString(AppKeys.lastBleDeviceIdKey, id);
      hasBond.value = true;
    }
    CompanionNotifRelay.instance.attachIconNeed();

    // 「保持连接」关掉时连上来也不拉前台服务 —— 开关说的就是"要不要后台常驻"。
    if (!await CompanionForegroundService.keepAliveWanted()) return;
    await CompanionForegroundService.requestNotificationPermission();
    await CompanionForegroundService.start();
  }

  Future<void> disconnect() async {
    _holdLink = false;
    linkError.value = null;
    await client.disconnect();
  }

  /// 解除绑定：码表侧清记录（已连接时）+ 手机 removeBond + 清本机缓存。
  ///
  /// 之后才允许再进扫描页配对新码表。
  Future<void> forgetBond() async {
    final d = client.device ?? await _cachedDevice();
    linkError.value = null;
    _holdLink = false;

    if (client.isReady) {
      try {
        await client.unbindPhone();
        // 给码表一点时间落盘 / 断链。
        await Future<void>.delayed(const Duration(milliseconds: 400));
      } catch (e) {
        await AppLog.w('Shell', '码表侧解绑指令失败: $e');
      }
    }

    if (d != null) {
      await client.removePhoneBond(d);
    }

    await CacheData().remove(AppKeys.lastBleDeviceIdKey);
    hasBond.value = false;
    await client.disconnect();
    await CompanionForegroundService.stop();
  }

  /// 重试上一次的设备。已绑定才应调用；没缓存返回 false。
  Future<bool> retryConnect() async {
    final d = client.device ?? await _cachedDevice();
    if (d == null) return false;
    linkError.value = null;
    _holdLink = true;
    try {
      await client.connect(d);
      if (client.state == CompanionConnState.ready) {
        await onConnected();
      } else if (client.state == CompanionConnState.versionMismatch) {
        linkError.value = LocaleKeys.deviceMismatch.tr;
      } else if (client.state == CompanionConnState.error) {
        // 已转保管回连时不算失败；只把「正在回连」留给 UI。
        if (!client.autoReconnect) {
          linkError.value =
              client.connectHint ?? LocaleKeys.bleScanConnectFailed.tr;
        }
      }
    } catch (e, st) {
      await AppLog.e('Shell', '重试连接失败: $e', st);
      linkError.value =
          client.connectHint ?? LocaleKeys.bleScanConnectFailed.tr;
    }
    return true;
  }

  /// 缓存过的上次设备；顺带把旧版误存的 RPA 修成身份地址。
  Future<BluetoothDevice?> _cachedDevice() async {
    final id = await CacheData().getString(AppKeys.lastBleDeviceIdKey);
    try {
      final bonded = await FlutterBluePlus.bondedDevices;
      final helm = bonded
          .where((d) => CompanionProto.matchesAdvName(d.platformName))
          .toList();

      if (id != null && id.isNotEmpty) {
        final hit = bonded.where((d) => d.remoteId.str == id).toList();
        if (hit.isNotEmpty) {
          hasBond.value = true;
          return hit.first;
        }
        // 缓存可能是旧 RPA：恰有一台已绑 Helm One 就迁过去。
        if (helm.length == 1) {
          final fixed = helm.first.remoteId.str;
          await CacheData().setString(AppKeys.lastBleDeviceIdKey, fixed);
          hasBond.value = true;
          return helm.first;
        }
        // 仍试 fromId（系统有时能解析）。
        hasBond.value = true;
        return BluetoothDevice.fromId(id);
      }

      if (helm.length == 1) {
        await CacheData().setString(
          AppKeys.lastBleDeviceIdKey,
          helm.first.remoteId.str,
        );
        hasBond.value = true;
        return helm.first;
      }
    } catch (e) {
      await AppLog.w('Shell', '读取绑定设备失败: $e');
      if (id != null && id.isNotEmpty) {
        try {
          hasBond.value = true;
          return BluetoothDevice.fromId(id);
        } catch (_) {}
      }
    }
    return null;
  }

  void bumpRides() => rideTick.value++;

  /// 同步进度回调：节流刷新，并估算 KB/s。
  void applySyncProgress(McuSyncProgress p) {
    _syncSw ??= Stopwatch()..start();
    final ms = _syncSw!.elapsedMilliseconds;
    final dt = ms - _syncLastMs;
    if (dt >= 280) {
      final db = p.transferred - _syncLastXfer;
      if (db >= 0) {
        syncKbs.value = db * 1000 / (dt < 1 ? 1 : dt) / 1024;
      }
      _syncLastXfer = p.transferred;
      _syncLastMs = ms;
    }
    final now = DateTime.now();
    if (syncProg.value?.phase == p.phase &&
        p.done < p.total &&
        now.difference(_syncLastUi) < const Duration(milliseconds: 80)) {
      return;
    }
    _syncLastUi = now;
    syncProg.value = p;
  }

  void clearSyncProgress() {
    syncing.value = false;
    syncProg.value = null;
    syncKbs.value = 0;
    _syncSw = null;
    _syncLastXfer = 0;
    _syncLastMs = 0;
  }
}

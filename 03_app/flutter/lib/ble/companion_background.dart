import 'dart:async';

import 'package:flutter/widgets.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:sifli_companion/app/app_keys.dart';
import 'package:sifli_companion/ble/companion_client.dart';
import 'package:sifli_companion/ble/companion_foreground_service.dart';
import 'package:sifli_companion/ble/companion_notif_relay.dart';
import 'package:sifli_companion/ble/companion_proto.dart';
import 'package:sifli_companion/log/app_log.dart';
import 'package:sifli_companion/utils/cache_data.dart';

/// 无视图（没有 UI）启动时的后台链路。
///
/// 进程并不总是"用户点图标"起来的：系统用 START_STICKY 重启前台服务时只创建服务，
/// 没有 Activity 也就没有 FlutterView。那种情况下 `runApp` 会直接抛 `StateError`
/// （见 SDK `widgets/binding.dart` 的 `wrapWithDefaultView`），可是**进程活着正是保活
/// 的目的** —— 连接、通知转发、星历推送都有活要干。
///
/// 所以这里做三件事：把通知转发拉起来、连上缓存里那台码表、刷新常驻通知；等用户真的
/// 打开 App（视图出现）再由调用方挂上 UI。
abstract class CompanionBackground {
  static const _tag = 'BgBoot';

  /// 通知文案的后台订阅；UI 接上后交给 `ShellController`，这里退订避免重复推送。
  static StreamSubscription<CompanionConnState>? _stateSub;

  /// 后台启动入口。
  ///
  /// [attachUi] 在**第一个视图出现时**被调用一次，负责把界面挂上去。
  static Future<void> start({required void Function() attachUi}) async {
    // 用户关掉了「保持连接」：后台什么都不做 —— 不连码表、不拉前台服务、不刷通知。
    // 这条要在最前面，连视图观察都不必挂（原生侧其实已经因此不建引擎了，这里是第二道）。
    if (!await CompanionForegroundService.keepAliveWanted()) {
      await AppLog.i(_tag, '「保持连接」已关闭，后台不动作');
      return;
    }

    _watchForView(() {
      unawaited(_stateSub?.cancel());
      _stateSub = null;
      attachUi();
    });

    /* 通知转发在 UI 路径里是 post-frame 回调里启动的 —— 这里永远不会有帧，所以直接起。
     * 它只依赖 Application Context（插件侧已经这么取了），不需要 Activity。 */
    unawaited(() async {
      try {
        await CompanionNotifRelay.instance.boot();
      } catch (e) {
        await AppLog.w(_tag, '通知转发启动失败: $e');
      }
    }());

    await _startNotificationText();
    await _connect();
  }
  /// 等第一个视图出现。
  ///
  /// 两个触发都挂上，谁也不保证一定先到：`didChangeMetrics` 会因引擎新增视图而被调用
  /// （`dart:ui` 的 `_addView` 里触发），`didChangeAppLifecycleState` 则跟着 Activity
  /// 的 resume 走。
  static void _watchForView(void Function() attachUi) {
    final watcher = _ViewWatcher(attachUi);
    WidgetsBinding.instance.addObserver(watcher);
    watcher.tryAttach();
  }

  /// 让常驻通知说真话。
  ///
  /// 服务的重启会把上一次的文案一起重放，所以这里要重新推一次当前状态；顺便订阅状态流，
  /// 让"正在连接码表…"/"已连接"在兜里掏出来看时是准的。
  static Future<void> _startNotificationText() async {
    // 服务刚被拉起来时可能还没走到 startForeground，等它几拍。
    var up = false;
    for (var i = 0; i < 6; i++) {
      if (await CompanionForegroundService.isRunning()) {
        up = true;
        break;
      }
      await Future<void>.delayed(const Duration(milliseconds: 500));
    }

    /* 没有前台服务就要自己补一个：本进程很可能是被"通知监听"那条路径拉起来的 —— 用户
     * 从最近任务划掉 App 后，MIUI 只把监听绑回来，前台服务会拖很久甚至不投递（实测 100 s
     * 仍未到）。没有前台服务就没有保活，进程随时会被回收、连接跟着断。
     *
     * 从后台启动前台服务在 Android 12+ 可能被系统拒（会以 PlatformException 回来，
     * `CompanionForegroundService.start` 自己吞掉并记日志）；被拒也不影响连接本身。 */
    if (!up) {
      await AppLog.i(_tag, '前台服务没在跑，尝试自行拉起');
      await CompanionForegroundService.start();
      if (!await CompanionForegroundService.isRunning()) {
        await AppLog.w(_tag, '前台服务仍没起来，只做后台连接');
        return;
      }
    }

    final client = CompanionClient();
    await CompanionForegroundService.updateLinkState(
      CompanionForegroundService.textForState(client.state),
    );
    _stateSub ??= client.stateStream.listen((s) {
      unawaited(
        CompanionForegroundService.updateLinkState(
          CompanionForegroundService.textForState(s),
        ),
      );
    });
  }

  /// 连上缓存里记住的那台码表。
  ///
  /// 与 `ShellController._tryReconnect` 走同一条路：有界超时试一次，撞进"不广播窗口"
  /// 时 `CompanionClient` 自己会转成 `autoConnect` 保管模式接着等。区别只有一个 ——
  /// 这里不依赖任何界面。
  static Future<void> _connect() async {
    final client = CompanionClient();
    if (client.isReady) return;

    final device = await _cachedDevice();
    if (device == null) {
      await AppLog.i(_tag, '没有已配对的码表，后台不发起连接');
      return;
    }

    try {
      await AppLog.i(_tag, '后台自动连接 ${device.remoteId}');
      await client.connect(device);
      CompanionNotifRelay.instance.attachIconNeed();
    } catch (e) {
      await AppLog.w(_tag, '后台自动连接失败: $e');
    }
  }

  /// 缓存里那台码表。
  ///
  /// 判据与 `ShellController._cachedDevice` 保持一致：先认缓存地址，缓存是旧 RPA 时
  /// 迁到系统里唯一那台已绑定 Helm One，最后才退回按地址构造。
  static Future<BluetoothDevice?> _cachedDevice() async {
    final id = await CacheData().getString(AppKeys.lastBleDeviceIdKey);

    try {
      final bonded = await FlutterBluePlus.bondedDevices;
      final helm = bonded
          .where((d) => CompanionProto.matchesAdvName(d.platformName))
          .toList();

      if (id != null && id.isNotEmpty) {
        final hit = bonded.where((d) => d.remoteId.str == id).toList();
        if (hit.isNotEmpty) return hit.first;

        // 缓存可能是旧 RPA：恰有一台已绑 Helm One 就迁过去。
        if (helm.length == 1) {
          await CacheData().setString(
            AppKeys.lastBleDeviceIdKey,
            helm.first.remoteId.str,
          );
          return helm.first;
        }

        // 仍试 fromId（系统有时能解析）。
        return BluetoothDevice.fromId(id);
      }

      if (helm.length == 1) {
        await CacheData().setString(
          AppKeys.lastBleDeviceIdKey,
          helm.first.remoteId.str,
        );
        return helm.first;
      }
    } catch (e) {
      await AppLog.w(_tag, '读取已绑定设备失败: $e');
    }

    return null;
  }
}

/// "视图出现了"的观察者，命中一次就自行退订。
class _ViewWatcher with WidgetsBindingObserver {
  _ViewWatcher(this._attachUi);

  final void Function() _attachUi;

  bool _done = false;

  void tryAttach() {
    if (_done) return;
    if (WidgetsBinding.instance.platformDispatcher.views.isEmpty) return;
    _done = true;
    WidgetsBinding.instance.removeObserver(this);
    _attachUi();
  }

  @override
  void didChangeMetrics() => tryAttach();

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) => tryAttach();
}

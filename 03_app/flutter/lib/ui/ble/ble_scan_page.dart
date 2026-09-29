import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:get/get.dart';
import 'package:sifli_companion/app/app_theme.dart';
import 'package:sifli_companion/ble/ble_manager.dart';
import 'package:sifli_companion/ble/companion_client.dart';
import 'package:sifli_companion/ble/companion_proto.dart';
import 'package:sifli_companion/i18n/locale_keys.dart';
import 'package:sifli_companion/log/app_log.dart';
import 'package:sifli_companion/ui/shell/shell_controller.dart';
import 'package:sifli_companion/ui/widgets/helm_chrome.dart';
import 'package:sifli_companion/ui/widgets/helm_rings.dart';

/// 蓝牙扫描页：配对码表的二级页面。
///
/// 默认按 Companion 约定过滤名称含 `Helm One`（及旧名 MyBike / Vela）的设备；
/// 可关闭过滤以列出附近全部可连接 BLE 外设。
///
/// 连上是这一页唯一的目的，所以**成功就自己退出**：先让那一行显示成已连接停一下，
/// 再返回设备页 —— 直接 pop 掉的话用户来不及确认连的是哪台。
class BleScanPage extends StatefulWidget {
  /// 创建扫描页。
  const BleScanPage({super.key});

  @override
  State<BleScanPage> createState() => _BleScanPageState();
}

class _BleScanPageState extends State<BleScanPage> {
  final Map<String, ScanResult> _devices = {};
  StreamSubscription<List<ScanResult>>? _scanSub;
  StreamSubscription<bool>? _isScanningSub;

  /// 链路状态订阅：**连上就退**不能只依赖本页那次 `_connect()` 的返回值。
  ///
  /// 现场（2026-09-19）：点一次没连上（30 s 的超时窗口撞上板子在复位 LCPU /
  /// 注入星历），用户退回再进来，或者后台自动重连把它连上了 —— 而这一页只在
  /// "本页发起的连接成功"那条路径里 pop，于是页面停在"扫描中"，看起来像
  /// "连上了但二级页面不退出"。
  StreamSubscription<CompanionConnState>? _stateSub;

  /// 已经安排过自动返回（本页 `_connect()` 与状态监听共用，避免 pop 两次）。
  bool _leaving = false;

  bool _scanning = false;
  String? _error;

  /// 正在连接的设备 id（行内显示进度，不用模态弹窗）。
  String? _connectingId;

  /// 已连上的设备 id：先显示成已连接再退出。
  String? _linkedId;

  /// 连上后停留多久再自动返回。
  static const _exitDelay = Duration(milliseconds: 700);

  /// 一次点击内部最多试几次（每次都会先 teardown 再连，见 retryConnect）。
  static const _connectAttempts = 3;

  /// 两次尝试之间等多久。
  static const _retryGap = Duration(milliseconds: 1200);

  /// 列表空着时多久自动重扫一次（只在这一页停留期间）。
  static const _rescanEvery = Duration(seconds: 15);

  Timer? _rescanTimer;

  /// 为 `true` 时仅保留名称匹配 Helm One / MyBike / Vela 的设备。
  bool _filterCompanion = true;

  @override
  void initState() {
    super.initState();
    // 已绑定禁止再扫：换机只能先解绑。
    if (Get.isRegistered<ShellController>() &&
        Get.find<ShellController>().hasBond.value) {
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (!mounted) return;
        AppTheme.snack(
          LocaleKeys.bleSnackbarTitle.tr,
          LocaleKeys.rebindNeedUnbind.tr,
        );
        Get.back();
      });
      return;
    }

    _isScanningSub = FlutterBluePlus.isScanning.listen((v) {
      if (mounted) setState(() => _scanning = v);
    });
    _stateSub = CompanionClient.instance.stateStream.listen(_onConnState);
    _startScan();

    // 空列表自动重扫：一窗 15 s 扫不到设备时，过去只能靠用户切过滤开关/退出重进
    // （"半天连不上"的一部分）。这里只在这一页停留期间、且没在连接时重扫。
    _rescanTimer = Timer.periodic(_rescanEvery, (_) {
      if (!mounted) return;
      if (_connectingId != null || _linkedId != null || _leaving) return;
      if (FlutterBluePlus.isScanningNow) return;
      if (_devices.isEmpty) _startScan();
    });
  }

  @override
  void dispose() {
    _rescanTimer?.cancel();
    _stateSub?.cancel();
    _scanSub?.cancel();
    _isScanningSub?.cancel();
    BleManager().stopScan();
    super.dispose();
  }

  /// 链路变 ready ⇒ 记住身份地址并退出（勿把 RPA 写进缓存）。
  void _onConnState(CompanionConnState s) async {
    if (s != CompanionConnState.ready || !mounted || _leaving) return;
    if (Get.isRegistered<ShellController>()) {
      await Get.find<ShellController>().onConnected();
    }
    if (!mounted) return;
    _leaveAfterConnected();
  }

  /// 先让那一行显示成"已连接"停一下，再返回设备页。
  ///
  /// 退之前要确认**本页就是当前路由**：自动重试与状态监听都会走到这里，而这期间
  /// 可能还压着别的路由（比如"通知权限"对话框），直接 `Get.back()` 会把对话框关掉、
  /// 页面留着。所以等一下（有界 5 s），回到最上层再退。
  void _leaveAfterConnected() {
    if (_leaving) return;
    _leaving = true;
    setState(() {
      _connectingId = null;
      _linkedId = CompanionClient.instance.device?.remoteId.str ?? _connectingId;
    });
    AppTheme.snack(
      LocaleKeys.bleSnackbarTitle.tr,
      LocaleKeys.bleScanConnected.trParams({'name': _linkedName()}),
    );
    Future<void>.delayed(_exitDelay, () async {
      final deadline = DateTime.now().add(const Duration(seconds: 5));
      while (DateTime.now().isBefore(deadline)) {
        if (!mounted) return;
        if (_isCurrentRoute()) break;
        await Future<void>.delayed(const Duration(milliseconds: 200));
      }
      if (!mounted) return;
      Get.back();
    });
  }

  /// 本页是否是最上层路由（退之前确认，别把上面的对话框关掉）。
  bool _isCurrentRoute() {
    final route = ModalRoute.of(context);
    return route == null || route.isCurrent;
  }

  String _linkedName() {
    final id = _linkedId;
    if (id != null) {
      final r = _devices[id];
      if (r != null) return _deviceName(r.device);
    }
    return LocaleKeys.bleScanUnknownDevice.tr;
  }

  /// 申请权限、确保适配器开启并开始扫描。
  Future<void> _startScan() async {
    setState(() {
      _error = null;
      _devices.clear();
    });

    final ok = await BleManager().requestPermissions();
    if (!ok) {
      setState(() => _error = LocaleKeys.bleScanPermissionDenied.tr);
      return;
    }

    if (FlutterBluePlus.adapterStateNow != BluetoothAdapterState.on) {
      await BleManager().turnOn();
      await Future<void>.delayed(const Duration(milliseconds: 500));
      if (FlutterBluePlus.adapterStateNow != BluetoothAdapterState.on) {
        setState(() => _error = LocaleKeys.bleScanTurnOnBluetooth.tr);
        return;
      }
    }

    await _scanSub?.cancel();
    _scanSub = FlutterBluePlus.scanResults.listen((results) {
      if (!mounted) return;
      setState(() {
        for (final r in results) {
          if (!r.advertisementData.connectable) continue;
          final name = _deviceName(r.device);
          if (_filterCompanion && !CompanionProto.matchesAdvName(name)) {
            continue;
          }
          _devices[r.device.remoteId.str] = r;
        }
      });
    }, onError: (e, st) {
      if (mounted) {
        setState(() => _error = AppTheme.failText('BleScan', e, st));
      }
    });

    try {
      await FlutterBluePlus.startScan(
        withKeywords: _filterCompanion ? CompanionProto.scanKeywords : const [],
        timeout: const Duration(seconds: 15),
      );
    } catch (e, st) {
      if (mounted) {
        setState(() => _error = AppTheme.failText('BleScan', e, st));
      }
    }
  }

  Future<void> _stopScan() => BleManager().stopScan();

  String _deviceName(BluetoothDevice d) {
    if (d.platformName.isNotEmpty) return d.platformName;
    if (d.advName.isNotEmpty) return d.advName;
    return LocaleKeys.bleScanUnknownDevice.tr;
  }

  /// 连接选中设备。进度显示在那一行里，成功则停一下自动返回。
  ///
  /// **一次点击内自动重试**（默认 3 次，每次间隔 1.2 s）：板子侧可能在复位 LCPU
  /// （adapter cycle 8 s）或注入星历（~10 s），单次尝试撞上这些窗口就会失败，用户
  /// 看到的是"点了没连上、再点还是没连上"（"有时候半天连不上"）。重试走
  /// `CompanionClient.retryConnect()` —— 它先把上一次在飞的连接拆干净，否则正在跑的
  /// 那次 30 s 尝试会把新请求静默吞掉。
  Future<void> _connect(ScanResult result) async {
    if (_connectingId != null || _leaving) return;
    final id = result.device.remoteId.str;
    setState(() {
      _connectingId = id;
      _error = null;
    });
    await _stopScan();

    final client = CompanionClient.instance;
    try {
      for (var attempt = 1; attempt <= _connectAttempts; attempt++) {
        if (attempt == 1) {
          await client.connect(result.device);
        } else {
          await AppLog.i('BleScan', '重试连接 $id（第 $attempt 次）');
          await client.retryConnect();
        }

        if (client.state == CompanionConnState.ready) {
          if (Get.isRegistered<ShellController>()) {
            await Get.find<ShellController>().onConnected();
          }
          if (!mounted) return;
          _leaveAfterConnected();
          return;
        }

        // 版本不匹配 = 连上了但协议不对，重试没有意义。
        if (client.state == CompanionConnState.versionMismatch) break;

        if (attempt < _connectAttempts) {
          if (!mounted) return;
          setState(() => _error = '正在重试（$attempt/$_connectAttempts）…');
          await Future<void>.delayed(_retryGap);
        }
      }

      final reason = client.state == CompanionConnState.versionMismatch
          ? LocaleKeys.deviceMismatch.tr
          : LocaleKeys.failed.tr;
      if (!mounted) return;
      setState(() {
        _connectingId = null;
        _error = null;
      });
      AppTheme.snack(LocaleKeys.bleSnackbarTitle.tr, reason);
      // 失败后把扫描放回去：原来 `_stopScan()` 之后就没再扫，列表冻着，
      // 用户只能退出去重进（"半天连不上"的另一半）。
      await _startScan();
    } catch (e, st) {
      if (!mounted) return;
      setState(() {
        _connectingId = null;
        _error = null;
      });
      AppTheme.fail(LocaleKeys.bleSnackbarTitle.tr, 'BleConnect', e, st);
      await _startScan();
    }
  }

  @override
  Widget build(BuildContext context) {
    final list = _devices.values.toList()
      ..sort((a, b) => b.rssi.compareTo(a.rssi));
    final busy = _connectingId != null;

    return Scaffold(
      appBar: AppBar(title: Text(LocaleKeys.bleScanTitle.tr)),
      body: Column(
        children: [
          Padding(
            padding: const EdgeInsets.fromLTRB(16, 8, 16, 8),
            child: HelmCard(
              child: HelmSwitchTile(
                color: AppTheme.iconBle,
                icon: Icons.filter_alt_outlined,
                title: LocaleKeys.bleScanFilterTitle.tr,
                subtitle: LocaleKeys.bleScanFilterSubtitle.tr,
                value: _filterCompanion,
                onChanged: busy
                    ? null
                    : (v) {
                        setState(() => _filterCompanion = v);
                        _startScan();
                      },
              ),
            ),
          ),
          if (_error != null)
            Padding(
              padding: const EdgeInsets.fromLTRB(20, 0, 20, 8),
              child: Text(
                _error!,
                style: const TextStyle(color: AppTheme.danger),
              ),
            ),
          Expanded(
            child: list.isEmpty ? _empty() : _deviceList(list, busy),
          ),
          SafeArea(
            child: Padding(
              padding: const EdgeInsets.all(16),
              child: Row(
                children: [
                  Expanded(
                    child: OutlinedButton(
                      onPressed: _scanning && !busy ? _stopScan : null,
                      child: Text(LocaleKeys.bleScanStop.tr),
                    ),
                  ),
                  const SizedBox(width: 12),
                  Expanded(
                    child: FilledButton(
                      onPressed: _scanning || busy ? null : _startScan,
                      child: Text(LocaleKeys.bleScanRescan.tr),
                    ),
                  ),
                ],
              ),
            ),
          ),
        ],
      ),
    );
  }

  /// 空态：一个会呼吸的蓝牙图标 + 两行说明，比干巴巴一行字好读。
  Widget _empty() {
    return Center(
      child: Padding(
        padding: const EdgeInsets.fromLTRB(32, 0, 32, 40),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            HelmPulseGlow(
              active: _scanning,
              child: const Icon(
                Icons.bluetooth_searching,
                size: 44,
                color: AppTheme.muted,
              ),
            ),
            const SizedBox(height: 18),
            Text(
              _scanning
                  ? LocaleKeys.connSearching.tr
                  : LocaleKeys.bleScanEmptyIdle.tr,
              textAlign: TextAlign.center,
              style: const TextStyle(
                color: Colors.white,
                fontSize: 15,
                fontWeight: FontWeight.w600,
                height: 1.3,
              ),
            ),
            if (_scanning) ...[
              const SizedBox(height: 6),
              Text(
                LocaleKeys.connSearchHint.tr,
                textAlign: TextAlign.center,
                style: const TextStyle(
                  color: AppTheme.muted,
                  fontSize: 13,
                  height: 1.4,
                ),
              ),
            ] else ...[
              // 扫不到最常见的原因就是"码表还没开配对窗口"（没配对时它根本不广播），
              // 直接把该在码表上按什么写在这里 —— app 侧没有开窗口的入口了。
              const SizedBox(height: 10),
              Text(
                LocaleKeys.bleScanPairHint.tr,
                textAlign: TextAlign.center,
                style: const TextStyle(
                  color: AppTheme.muted,
                  fontSize: 13,
                  height: 1.5,
                ),
              ),
            ],
          ],
        ),
      ),
    );
  }

  Widget _deviceList(List<ScanResult> list, bool busy) {
    return ListView(
      padding: const EdgeInsets.fromLTRB(16, 0, 16, 8),
      children: [
        HelmCard(
          child: HelmTileGroup(
            children: [
              for (final r in list) _deviceTile(r, busy),
            ],
          ),
        ),
      ],
    );
  }

  Widget _deviceTile(ScanResult r, bool busy) {
    final id = r.device.remoteId.str;
    final linking = _connectingId == id;
    final linked = _linkedId == id;

    return HelmNavTile(
      color: AppTheme.iconBle,
      icon: Icons.bluetooth,
      title: _deviceName(r.device),
      subtitle: id,
      // 连上之后不再可点，免得刚连上又被点一下
      onTap: linked || busy ? null : () => _connect(r),
      showChevron: false,
      trailing: linking
          ? const SizedBox.square(
              dimension: 18,
              child: CircularProgressIndicator(strokeWidth: 2),
            )
          : linked
          ? const Icon(
              Icons.check_circle,
              size: 20,
              color: AppTheme.iconKeep,
            )
          : Row(
              mainAxisSize: MainAxisSize.min,
              children: [
                _signalBars(r.rssi),
                const SizedBox(width: 12),
                FilledButton(
                  onPressed: busy ? null : () => _connect(r),
                  child: Text(LocaleKeys.bleScanConnect.tr),
                ),
              ],
            ),
    );
  }

  /// 信号强度：4 格，按 RSSI 分档着色。比一行「RSSI -62」直观。
  Widget _signalBars(int rssi) {
    final level = rssi >= -55
        ? 4
        : rssi >= -65
        ? 3
        : rssi >= -75
        ? 2
        : 1;
    final color = level >= 3
        ? AppTheme.iconKeep
        : (level == 2 ? const Color(0xFFFFD54A) : AppTheme.iconOta);
    return Row(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.end,
      children: [
        for (var i = 1; i <= 4; i++)
          Container(
            width: 3,
            height: 5.0 + i * 3,
            margin: const EdgeInsets.only(left: 2),
            decoration: BoxDecoration(
              color: i <= level ? color : Colors.white.withValues(alpha: 0.14),
              borderRadius: BorderRadius.circular(1.5),
            ),
          ),
      ],
    );
  }
}

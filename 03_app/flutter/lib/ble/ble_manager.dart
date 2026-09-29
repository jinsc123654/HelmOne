import 'dart:async';
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:permission_handler/permission_handler.dart';
import 'package:sifli_companion/ble/companion_proto.dart';

/// 蓝牙门面，封装 [FlutterBluePlus] 的常用能力。
///
/// 冷启动只做日志关闭与适配器状态监听；真正扫描/连接由 UI 触发。
class BleManager {
  /// 返回单例。
  factory BleManager() => _instance;

  static final BleManager _instance = BleManager._internal();

  /// 与 [BleManager.new] 相同的单例入口。
  static BleManager get instance => _instance;

  BleManager._internal();

  StreamSubscription<BluetoothAdapterState>? _adapterSub;
  BluetoothAdapterState _adapterState = BluetoothAdapterState.unknown;

  /// 最近一次已知的适配器状态。
  BluetoothAdapterState get adapterState => _adapterState;

  /// 冷启动软初始化：关闭插件日志并订阅适配器状态。
  Future<void> init() async {
    FlutterBluePlus.setLogLevel(LogLevel.none);
    _adapterState = FlutterBluePlus.adapterStateNow;
    await _adapterSub?.cancel();
    _adapterSub = FlutterBluePlus.adapterState.listen((state) {
      _adapterState = state;
    });
  }

  /// 当前平台是否支持 BLE。
  Future<bool> isSupported() => FlutterBluePlus.isSupported;

  /// 申请扫描/连接所需权限。
  ///
  /// Android：扫描、连接、粗/精定位；iOS：蓝牙。
  Future<bool> requestPermissions() async {
    if (kIsWeb) return true;

    if (Platform.isAndroid) {
      final statuses = await [
        Permission.bluetoothScan,
        Permission.bluetoothConnect,
        Permission.locationWhenInUse,
      ].request();
      return statuses.values.every((s) => s.isGranted || s.isLimited);
    }

    if (Platform.isIOS) {
      final status = await Permission.bluetooth.request();
      return status.isGranted || status.isLimited;
    }

    return true;
  }

  /// 打开蓝牙适配器（仅 Android 可主动请求；iOS 需用户在系统设置中开启）。
  Future<void> turnOn() async {
    if (!kIsWeb && Platform.isAndroid) {
      await FlutterBluePlus.turnOn();
    }
  }

  /// 开始扫描。
  ///
  /// [withKeywords] 默认匹配 Helm One 广播名（兼旧名 MyBike）。
  Future<void> startScan({
    List<String> withKeywords = CompanionProto.scanKeywords,
    Duration timeout = const Duration(seconds: 15),
  }) async {
    await FlutterBluePlus.startScan(
      withKeywords: withKeywords,
      timeout: timeout,
    );
  }

  /// 停止扫描。
  Future<void> stopScan() => FlutterBluePlus.stopScan();

  /// 扫描结果流。
  Stream<List<ScanResult>> get scanResults => FlutterBluePlus.scanResults;

  /// 取消适配器状态订阅。
  Future<void> dispose() async {
    await _adapterSub?.cancel();
    _adapterSub = null;
  }
}

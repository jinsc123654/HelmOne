import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:geolocator/geolocator.dart';
import 'package:sifli_companion/ble/companion_proto.dart';
import 'package:get/get.dart';
import 'package:sifli_companion/i18n/locale_keys.dart';
import 'package:sifli_companion/ble/companion_rle.dart';
import 'package:sifli_companion/ble/gnss_codec.dart';
import 'package:sifli_companion/ble/notif_icon_cache.dart';
import 'package:sifli_companion/log/app_log.dart';

/// Companion 链路阶段。
enum CompanionConnState {
  /// 未连接。
  disconnected,

  /// 正在建立 GATT 连接。
  connecting,

  /// 已连接，正在协商 MTU / 发现服务 / 订阅状态。
  discovering,

  /// 就绪，可收发。
  ready,

  /// 设备协议版本与 App 不一致，已断开。
  versionMismatch,

  /// 连接或发现过程出错。
  error,
}

/// 码表 Companion 服务（`0xFF10`）的客户端。
///
/// 负责扫描、连接、MTU 协商、服务发现、状态订阅、控制下发、GNSS 推送与
/// 文件传输（上传/下载/删除/移动，支持断线续传）。意外断开后自动重连；
/// App 在后台时靠看门狗 + Android 前台服务 / iOS `bluetooth-central` 继续试。
///
/// 协议见 [CompanionProto]，规格见固件仓库
/// `vendor/my_vendor/docs/ble/companion_impl_plan.md`。
class CompanionClient {
  /// 返回单例。
  factory CompanionClient() => _instance;

  static final CompanionClient _instance = CompanionClient._internal();

  /// 与 [CompanionClient.new] 相同的单例入口。
  static CompanionClient get instance => _instance;

  CompanionClient._internal() {
    _adapterSub = FlutterBluePlus.adapterState.listen(_onAdapterState);
  }

  static const _tag = 'CompanionClient';

  /// 单次下载尝试里允许的**连续**序号断裂次数（2026-09-25：原来写死 3，太紧）。
  ///
  /// 设备侧 40 ms 信用超时（`FS_CREDIT_TIMEOUT_MS`）在广播/扫描抢射频时会周期性
  /// 丢页，文件一大就很容易连丢 4 次 ⇒ 整段失败、只能重启 App。放宽到 12 之后，
  /// 配合 `fsDownload()` 的外层续传重试，普通抖动都能自愈。
  static const _fsDownloadMaxGaps = 12;

  /// 重连退避的起始与上限。
  ///
  /// 起始 500 ms：刚就绪过的链路断了应尽快接回（设备空闲广播约 1.28 s 一拍，
  /// 再叠更长会感觉「回连很慢」）。失败后仍按 ×2 涨到 [_reconnectMaxDelay]。
  /// 意外断链后设备通常很快又广播，首跳 200 ms 足够让 HCI 收尾。
  static const _reconnectMinDelay = Duration(milliseconds: 200);

  /// 「保管模式」的连接请求能挂多久。
  ///
  /// 不是"等这么久才放弃"，而是给个上界：正常情况设备一进广播窗口就连上（几秒），
  /// 挂着不管只会在设备长期关机时白占一个连接槽。
  static const _heldConnectTimeout = Duration(minutes: 30);

  /// 用户手动发起的那次连接等多久：人要即时反馈，十几秒没动静就该给个说法。
  static const _manualConnectTimeout = Duration(seconds: 15);
  static const _reconnectMaxDelay = Duration(seconds: 30);

  /// 后台/锁屏时单次退避定时器可能被冻住，用看门狗兜底继续试。
  static const _watchdogPeriod = Duration(seconds: 20);

  /// 连接成功后立刻对时，之后每小时再发一次 UTC + 时区。
  static const _timeSyncPeriod = Duration(hours: 1);

  final _stateCtrl = StreamController<CompanionConnState>.broadcast();
  final _statusCtrl = StreamController<CompanionStatus>.broadcast();

  BluetoothDevice? _device;
  BluetoothCharacteristic? _protoVerChar;
  BluetoothCharacteristic? _statusChar;
  BluetoothCharacteristic? _controlChar;
  BluetoothCharacteristic? _gnssChar;
  BluetoothCharacteristic? _fsCmdChar;
  BluetoothCharacteristic? _fsDataChar;
  BluetoothCharacteristic? _notifChar;
  BluetoothCharacteristic? _navChar;
  BluetoothCharacteristic? _devInfoChar;

  StreamSubscription<BluetoothConnectionState>? _connSub;
  StreamSubscription<BluetoothAdapterState>? _adapterSub;
  StreamSubscription<List<int>>? _statusSub;
  Timer? _statusPollTimer;   // 状态轮询（见 _subscribeStatus）
  StreamSubscription<List<int>>? _fsSub;
  StreamSubscription<Position>? _positionSub;
  Timer? _reconnectTimer;
  Timer? _watchdog;
  Timer? _timeSyncTimer;
  DateTime? _lastTimeSyncAt;

  CompanionConnState _state = CompanionConnState.disconnected;
  CompanionStatus? _lastStatus;
  CompanionDevInfo? _lastDevInfo;
  int? _deviceProtoVersion;
  int _mtu = 23;
  Duration _reconnectDelay = _reconnectMinDelay;
  bool _autoReconnect = false;
  bool _connectInFlight = false;
  DateTime? _lastGnssSentAt;
  int _fsTid = 1;
  Completer<List<CompanionFsFrame>>? _fsWait;
  int _fsWaitTid = -1;
  int _fsWaitCmd = -1;
  final _fsCollected = <CompanionFsFrame>[];
  CompanionFsException? _fsWritePushErr;

  /// `0xFF16` 的 CCCD 写成功了吗。
  ///
  /// 设备只通过 notify 回 FS 帧，所以没订上就等于一条回复都收不到 —— 命令写得
  /// 再成功也只能等到 20 s 超时。
  bool _fsSubscribed = false;
  /// 上次因"文件传输特征缺失"自动清 GATT 缓存的时间（限频，见 [_healFsIfNeeded]）。
  DateTime? _fsHealAt;
  /// 状态通知是否已订阅**且已验证**（真收到过帧）。
  bool _statusSubscribed = false;
  /// 订阅验证用的等待器：`_subscribeStatus` 订阅后等第一帧，`_onStatusFrame` 完成它。
  Completer<void>? _statusWait;
  /// 最近一次状态帧的时刻（epoch ms）与看门狗。设备每 5 s 推一次心跳，所以
  /// "3× 心跳没帧"就是链路已死的直接证据。
  int _lastStatusRxMs = 0;
  Timer? _statusWatchTimer;
  static const int _statusSilentMs = 45000;
  /// 连续静默的窗口数（见 _startStatusWatch：连续两个才判死）。
  int _statusSilentStrikes = 0;

  /// 退避到点时"上一次连接仍在途"而跳过的次数（见 _scheduleReconnect）。
  /// 连续跳过说明那个"在途"是陈旧的（FBP 卡在 connecting），必须强制断开重来。
  int _reconnectSkips = 0;

  /// 没订上时的补订定时器（连上了、只有 FS 订阅失败时用来自愈）。
  Timer? _fsResubTimer;

  /// `0xFF16` notify 租约：空闲超过 [_fsNotifyIdle] 就退订 CCC，少占控制器 notify。
  ///
  /// **2026-09-21**：空闲退订会写 CCC=0；若这笔 ATT 写迟到落到**下一次**连接上，
  /// MCU 会变成 `subscribe -> 0` 而 App 仍读到缓存 `01`。先把空闲退订拉长到
  /// 10 分钟，优先保证 FS 可用（控制器缓冲压力次之）。
  Timer? _fsNotifyLease;
  static const _fsNotifyIdle = Duration(minutes: 10);

  /// FS 单通道事务锁。
  ///
  /// `0xFF15`/`0xFF16` 只有一份事务上下文（[_fsWaitTid]/[_fsWaitCmd]/[_fsCollected]），
  /// 两个调用方并发会让彼此的回复互相丢弃、双双超时。设备主动索要图标（`FS_NEED`）
  /// 的时机完全随机，和 App 发起的同步/OTA/浏览撞车就表现为「有概率 FS 不可用」。
  Future<void> _fsLock = Future<void>.value();

  /// Android GATT **整连接**只允许一条在途写（含 CCCD / WRITE_CMD / WRITE_REQ）。
  ///
  /// FS 事务锁只管协议层；对时、GNSS、通知分片、`setNotifyValue` 补订仍会跟星历
  /// 上传抢写。现场 `gatt.writeCharacteristic() returned 201` =
  /// `ERROR_GATT_WRITE_REQUEST_BUSY`：MCU 没挂，重启 App 清空手机栈就好。
  /// 所有特征写 / CCC 写都走 [_withGattOp] 串行，并对 201 做短退避重试。
  Future<void> _gattLock = Future<void>.value();

  int _downlinkMsgId = 0;

  /// 码表 `FS_NEED` 时用来取本地 PNG。由 [CompanionNotifRelay] 注入。
  Future<Uint8List?> Function(String fileName)? iconNeedLoader;

  final _iconNeedQ = <String>[];
  bool _iconNeedBusy = false;

  /// 协议版本不一致时是否仍继续通信。默认 `false`：不一致即断开。
  ///
  /// 仅联调页面需要置 `true`。
  bool allowVersionMismatch = false;

  /// GNSS 推送的最小间隔，默认 1 Hz。固件按 1–5 Hz 设计。
  Duration gnssMinInterval = const Duration(seconds: 1);

  // 位置辅助不在连接期间常推，而是下发星历时抓一次（见 EphService）：那份位置随
  // 星历文件一起进模组，抓取时机正好，也不用一直开着定位。调试页的手动推流照旧。

  String? _connectHint;

  /// 最近一次连接失败/降级的原因（人类可读，给界面显示）。
  ///
  /// 原来只报「连接失败」，用户没法区分"码表没开机"和"信号差" —— 一个是去开机、
  /// 一个是走近点，给同一句话等于什么都没说。
  String? get connectHint => _connectHint;

  /// 链路阶段变化流。
  Stream<CompanionConnState> get stateStream => _stateCtrl.stream;

  /// 设备状态帧流（特征 `0xFF12`）。
  Stream<CompanionStatus> get statusStream => _statusCtrl.stream;

  /// 当前链路阶段。
  CompanionConnState get state => _state;

  /// 最近一帧设备状态，尚未收到则为 `null`。
  CompanionStatus? get lastStatus => _lastStatus;

  /// 0xFF1A 运行槽 / 版本；旧固件没有该特征则为 `null`。
  CompanionDevInfo? get lastDevInfo => _lastDevInfo;

  /// 设备上报的协议版本，尚未读取则为 `null`。
  int? get deviceProtoVersion => _deviceProtoVersion;

  /// 协商后的 ATT MTU；单包有效载荷为 `mtu - 3`。
  int get mtu => _mtu;

  /// 当前连接的设备。
  BluetoothDevice? get device => _device;

  /// 是否已就绪。
  bool get isReady => _state == CompanionConnState.ready;

  /// 本次服务发现里拿到 `0xFF17` 通知特征了吗。
  ///
  /// 为 `false` 时 [sendNotification] 一定失败。常见原因是 **Android 缓存了旧的
  /// GATT 服务表**：固件加了新特征，手机侧不会自动重新发现，表现就是
  /// `0xFF17`/`0xFF19`/`0xFF1A` 这整整一段凭空消失。可用 [refreshGattCache] 救。
  bool get canRelayNotification => _notifChar != null;

  /// 文件传输通道是否具备：有 `0xFF15`/`0xFF16` 即可。
  ///
  /// **2026-09-21**：`0xFF16` 的 notify 改为**按需订阅**（空闲退订），不再要求
  /// 连接期间一直占着 CCC。真正发 FS 命令前 [_withFsLock] 会确保订上。
  bool get canTransferFiles =>
      _fsCmdChar != null && _fsDataChar != null;

  /// 状态通知是否**订阅成功且收到过帧**。进 `ready` 的前置条件之一：
  /// 只有它为真，才说明 CCCD 写入 → 对端真的发 → 手机真的收 这条链是通的。
  bool get statusSubscribed => _statusSubscribed;

  /// 正在连 GATT 或发现服务。
  bool get isLinking =>
      _state == CompanionConnState.connecting ||
      _state == CompanionConnState.discovering;

  /// 意外断开后是否继续自动重连。用户点「断开」后为 `false`。
  bool get autoReconnect => _autoReconnect;

  /// 按服务 UUID 扫描码表。
  ///
  /// 用 [CompanionProto.service] 过滤而非广播名关键字：名字可被用户修改，
  /// 服务 UUID 不会。
  Future<void> startScan({
    Duration timeout = const Duration(seconds: 30),
  }) async {
    await FlutterBluePlus.startScan(
      withServices: [CompanionProto.service],
      timeout: timeout,
    );
  }

  /// 停止扫描。
  Future<void> stopScan() => FlutterBluePlus.stopScan();

  /// 扫描结果流。
  Stream<List<ScanResult>> get scanResults => FlutterBluePlus.scanResults;

  /// 连接设备并完成服务发现与状态订阅。
  ///
  /// [autoReconnect] 为 `true` 时，意外断开后按退避自动重连，直到调用
  /// [disconnect]。

  /// 发起**系统配对**（Android）。iOS 没有显式配对 API：它在访问"要求加密"的特征时
  /// 由系统隐式配对，所以这里直接算成功。
  ///
  /// [allowCreate] = false 时只查已绑定列表，不弹系统配对框 —— 静默回连用：
  /// 回连路径里弹框等于打断用户，而且窗口外设备也会拒掉未绑定链路。
  ///
  /// 返回 `true` = 已绑定（含 iOS 的隐式路径）。
  Future<bool> ensureBonded(
    BluetoothDevice device, {
    bool allowCreate = true,
  }) async {
    if (!Platform.isAndroid) return true;

    // 已配对 = 空操作：**别依赖 bondState 流去确认当前值**（这个 flutter_blue_plus
    // 版本没有 bondStateNow，流不保证补发当前状态，会白等 25 s×2）。
    try {
      final bonded = await FlutterBluePlus.bondedDevices;
      if (bonded.any((d) => d.remoteId == device.remoteId)) {
        return true;
      }
    } catch (_) {
      // 查询失败就照常走配对流程。
    }

    if (!allowCreate) return false;

    for (var attempt = 0; attempt < 2; attempt++) {
      try {
        await device.createBond();
        await device.bondState
            .where((s) => s == BluetoothBondState.bonded)
            .first
            .timeout(const Duration(seconds: 25));
        return true;
      } catch (_) {
        // 第一次常失败（用户还没在系统弹框上点"配对/允许"）：再试一次。
      }
    }

    return false;
  }

  /// 手机侧清系统绑定（Android）。iOS 无 API，靠系统设置手动清。
  Future<void> removePhoneBond(BluetoothDevice device) async {
    if (!Platform.isAndroid) return;
    try {
      await device.removeBond();
    } catch (e) {
      await AppLog.w(_tag, 'removeBond 失败: $e');
    }
  }
  Future<void> connect(
    BluetoothDevice device, {
    bool autoReconnect = true,
  }) async {
    await _teardown();

    _device = device;
    _autoReconnect = autoReconnect;
    _reconnectDelay = _reconnectMinDelay;
    if (_autoReconnect) _armWatchdog();

    await _connectInternal();
  }

  /// 立刻再试一次（App 回前台、蓝牙刚打开、进后台时调用）。
  /// 需要链路时调用：未连接就先发起连接，最多等 [timeout]（默认 15 s）。
  ///
  /// 与自动重连的区别是**按需路径必须绕开退避**：`kickReconnect(resetBackoff: true)`
  /// 会把 `_scheduleReconnect()` 排下的 16 s 退避清掉立刻重连 —— 否则后台刚起来
  /// 要转发通知时，连接还在退避里排队，调用方只能立刻报失败（现场就是这样）。
  ///
  /// 返回 true = 15 s 内连上并 ready；false = 超时（连接仍在后台继续尝试）。
  Future<bool> ensureConnected({
    Duration timeout = const Duration(seconds: 15),
  }) async {
    if (isReady) return true;

    final deadline = DateTime.now().add(timeout);
    while (DateTime.now().isBefore(deadline)) {
      // 正在连（`device.connect` 最长 30 s）时**不要放弃、也不要重复发起**：
      // 原来这里 await kickReconnect 而它在 `isLinking/_connectInFlight` 时直接
      // return，于是"等待"其实什么都没等到，15 s 一到就报失败给用户 —— 而连接
      // 还在后台跑（现场："有时候半天连不上"，其实是连上了但调用方已经放弃）。
      if (isLinking || _connectInFlight) {
        await Future<void>.delayed(const Duration(milliseconds: 200));
        continue;
      }
      await kickReconnect(resetBackoff: true);
      await Future<void>.delayed(const Duration(milliseconds: 200));
      if (isReady) return true;
    }

    await AppLog.w(_tag, '等待连接超时 ${timeout.inSeconds}s');
    return false;
  }

  /// 放弃当前尝试、立刻重新连一次（扫描页"重试"用）。
  ///
  /// 为什么需要单独一个入口：`_connectInternal()` 开头有 `isLinking ||
  /// _connectInFlight` 守卫，正在跑的那次 30 s 尝试没结束之前，任何新请求都会被
  /// 静默忽略 —— 用户连点几次都"没反应"，只能等它自己失败（这也是"半天连不上"
  /// 的一部分）。这里先把在飞的那次拆干净（`_teardown()` 会 disconnect），再起新的。
  Future<void> retryConnect() async {
    if (isReady || _device == null) return;
    _connectInFlight = false;
    await _teardown();
    _reconnectDelay = _reconnectMinDelay;
    _setState(CompanionConnState.disconnected);
    // 用户点的那次：有界超时（超时后自动转保管模式，见 _connectInternal）。
    await _connectInternal();
  }

  Future<void> kickReconnect({bool resetBackoff = false}) async {
    if (!_autoReconnect || _device == null) return;
    if (isReady || isLinking || _connectInFlight) return;
    if (resetBackoff) _reconnectDelay = _reconnectMinDelay;
    _reconnectTimer?.cancel();
    _reconnectTimer = null;
    // 自动路径：交给控制器保管连接请求（设备一进广播窗口就建链）。
    await _connectInternal(held: true);
  }

  /// 发起一次连接。
  ///
  /// [held] = 自动重连：用 `autoConnect` 把请求交给控制器**保管**。
  ///
  /// 为什么必须这样：码表把射频按时间片分给"广播给手机"和"扫心率/踏频传感器"，
  /// 实测只有约 1/3–1/2 的时间在广播（固件注释里连"射频全空 0.5~1.5 s、手机可见
  /// 窗口 2 s/4.5 s"都记着）。普通 connect 很可能整段落在没人应答的窗口里，只能
  /// 干等超时 —— 2026-09-19 实测：一天 24 次 30 秒超时，冷启动 54 秒才连上。
  /// 保管模式下没有白等的超时，也不会把退避越堆越长。
  ///
  /// 用户手点的那次仍用有界超时（要即时反馈），但**超时不等于放弃**：转成保管
  /// 模式接着连，界面上说明白。
  Future<void> _connectInternal({bool held = false, bool force = false}) async {
    final device = _device;
    if (device == null || _connectInFlight || isReady) return;

    /* 已经是 connecting/discovering 时不该再叠一次尝试。唯一的例外是"手动那次刚超时、
     * 正要转保管模式"（[force]）：那一刻状态被**刻意**留在 connecting 上（避免界面闪成
     * 未连接），于是这里会被自己的守卫挡回去 —— 交接变成空操作，界面永远停在"正在连接"，
     * 看门狗也因同一个守卫不再兜底（2026-09-25 实测：15 s 超时后再无任何一次尝试）。
     * 而 `_connectInFlight` 此时已在 finally 里放下，接续正是该做的事。 */
    if (isLinking && !force) return;

    _connectInFlight = true;
    var handoff = false;
    _setState(CompanionConnState.connecting);
    await AppLog.i(_tag, '连接 ${device.remoteId}');

    try {
      if (FlutterBluePlus.adapterStateNow != BluetoothAdapterState.on &&
          FlutterBluePlus.adapterStateNow != BluetoothAdapterState.unknown) {
        await AppLog.w(_tag, '蓝牙未开，稍后重连');
        _setState(CompanionConnState.disconnected);
        _scheduleReconnect();
        return;
      }

      // 先挂监听再连接，避免漏掉紧随其后的断开事件。
      await _connSub?.cancel();
      _connSub = device.connectionState.listen(_onConnectionStateChanged);

      // mtu:null 关掉 flutter_blue_plus 自带的 MTU 请求。Android 每条连接只允许
      // 一次 MTU 交换，我们要让那一次用 CompanionProto.desiredMtu。
      await device.connect(
        license: License.nonprofit,
        // 自动路径：请求由控制器保管，设备一进广播窗口就建链。
        autoConnect: held,
        // FBP 里 autoConnect 与 mtu 互斥（插件内有 assert），MTU 我们自己请求。
        mtu: null,
        timeout: held ? _heldConnectTimeout : _manualConnectTimeout,
      );
      // FBP：`autoConnect:true` 时 connect() **不等待建链**（只把请求交给控制器），
      // 必须再等 connectionState=connected，否则立刻 discover 会报
      // `device is not connected`（adb 2026-09-21 20:26 连打退避）。
      // 不信任「connect 返回」；也不信任短暂的 isConnected 毛刺——再读一次。
      if (!device.isConnected) {
        await device.connectionState
            .where((s) => s == BluetoothConnectionState.connected)
            .first
            .timeout(held ? _heldConnectTimeout : _manualConnectTimeout);
      }
      if (!device.isConnected) {
        throw StateError('connect returned but device still disconnected');
      }
      await AppLog.i(
        _tag,
        '链路已建立 held=$held，开始配对/发现',
      );
      // **连接 → 配对 → 发现/订阅**（顺序不能反，两条都是硬约束）：
      // ① `createBond()` 在本插件里要求"设备已连接"（bluetooth_device.dart 里
      //    `if (isDisconnected) throw ...`），所以配对必须在 connect 之后；
      // ② 配对会触发加密（必要时重建链路），**订阅必须排在它后面**，否则订阅会被
      //    加密流程冲掉 —— 设备侧 `ccc=0`、app 一直转圈（2026-09-19 的老坑）。
      // 静默回连（held）绝不弹 createBond：已绑定应直接过；未绑定就等下次。
      if (!await ensureBonded(device, allowCreate: !held)) {
        _connectHint = held
            ? LocaleKeys.bleErrRetrying.tr
            : LocaleKeys.bleErrNeedPair.tr;
        _failSoft(held: held);
        return;
      }

      await _discoverAndSubscribe(device, soft: held);
    } catch (e, st) {
      await AppLog.e(_tag, '连接失败: $e', st);
      final timedOut = _looksLikeTimeout(e);
      if (held) {
        // 自动回连：不要进 error（否则 UI 弹失败、写 linkError），保持安静重试。
        _connectHint = LocaleKeys.bleErrRetrying.tr;
        _setState(CompanionConnState.disconnected);
        _scheduleReconnect();
      } else if (timedOut) {
        // 手动超时：多半撞进了"不广播"窗口 —— 别闪失败，直接转保管接着连。
        _connectHint = LocaleKeys.bleErrRetrying.tr;
        _setState(CompanionConnState.connecting);
        handoff = true;
      } else {
        _connectHint = LocaleKeys.bleErrFailed.tr;
        _setState(CompanionConnState.error);
        _scheduleReconnect();
      }
    } finally {
      _connectInFlight = false;
    }

    if (handoff) unawaited(_connectInternal(held: true, force: true));
  }

  /// 连接/配对失败收尾：held 路径只回 disconnected，避免 UI 失败风暴。
  void _failSoft({required bool held}) {
    if (held) {
      _setState(CompanionConnState.disconnected);
    } else {
      _setState(CompanionConnState.error);
    }
    _scheduleReconnect();
  }

  /// 看着像"没人应答"（超时）而不是"明确被拒"。
  static bool _looksLikeTimeout(Object e) {
    if (e is TimeoutException) return true;
    final s = e.toString();
    return s.contains('Timed out') ||
        s.contains('TimeoutException') ||
        s.contains('GATT_CONNECTION_TIMEOUT') ||
        s.contains('android-code: 147');
  }

  Future<void> _discoverAndSubscribe(
    BluetoothDevice device, {
    bool soft = false,
  }) async {
    _setState(CompanionConnState.discovering);
    final t0 = DateTime.now();
    int ms() => DateTime.now().difference(t0).inMilliseconds;

    // 协商 MTU 必须在订阅之前：16 字节的状态帧在默认 23 字节 MTU 下也放得下，
    // 但 P1 的文件与通知分片需要大包，且重协商会打断已有订阅。
    if (!kIsWeb && Platform.isAndroid) {
      try {
        _mtu = await device.requestMtu(CompanionProto.desiredMtu);
      } catch (e) {
        _mtu = device.mtuNow;
        await AppLog.w(_tag, 'MTU 协商失败，沿用 $_mtu: $e');
      }
      // 一次 high 即可。以前双写 + 中间空等 400 ms，回连体感明显变慢。
      try {
        await device.requestConnectionPriority(
          connectionPriorityRequest: ConnectionPriority.high,
        );
      } catch (e) {
        await AppLog.w(_tag, '连接优先级 high 失败: $e');
      }
    } else {
      _mtu = device.mtuNow;
    }
    await AppLog.i(_tag, 'MTU=$_mtu（有效载荷 ${_mtu - 3}）+${ms()}ms');

    // **不要每次回连都 clearGattCache**：refresh 会逼整张表空口重发现，常多耗
    // 1–3 s。MCU CCC 上报 bug 已修；仅当 Companion 服务/P1 特征缺席时再清。
    var services = await device.discoverServices();
    await AppLog.i(_tag, 'discoverServices 完成 +${ms()}ms');

    var service = services.firstWhereOrNull(
      (s) => s.uuid == CompanionProto.service,
    );

    // 服务整段找不到：也先怀疑缓存（跟特征缺席同一处置）。
    if (service == null && !kIsWeb && Platform.isAndroid) {
      await AppLog.w(_tag, '未发现 Companion 服务，清 GATT 缓存后重发现');
      if (await _clearGattCache(device)) {
        services = await device.discoverServices();
        service = services.firstWhereOrNull(
          (s) => s.uuid == CompanionProto.service,
        );
      }
    }

    if (service == null) {
      await AppLog.e(_tag, '未发现 Companion 服务 ${CompanionProto.service}');
      _connectHint = LocaleKeys.bleErrFailed.tr;
      _failSoft(held: soft);
      await _disconnectDevice();
      return;
    }

    var missing = _bindChars(service);

    // 已知的 P1/P2 特征整段缺席时，先怀疑手机侧的 GATT 服务表缓存，而不是固件：
    // Android 按「设备地址 + 服务」缓存整张表，固件加了特征它也不会重新发现，
    // 于是 0xFF17/0xFF19/0xFF1A 一起消失（通知就再也推不出去）。清一次缓存
    // 重新发现，正常固件上这一次就能补齐。
    if (missing.isNotEmpty && !kIsWeb && Platform.isAndroid) {
      await AppLog.w(
        _tag,
        'Companion 服务缺 ${missing.join("/")}，疑似 Android 缓存了旧 GATT 表，'
        '清缓存后重新发现',
      );
      if (await _clearGattCache(device)) {
        services = await device.discoverServices();
        service = services.firstWhereOrNull(
          (s) => s.uuid == CompanionProto.service,
        );
        if (service == null) {
          await AppLog.e(_tag, '清缓存后未发现 Companion 服务');
          _connectHint = LocaleKeys.bleErrFailed.tr;
          _failSoft(held: soft);
          await _disconnectDevice();
          return;
        }
        missing = _bindChars(service);
        await AppLog.i(
          _tag,
          missing.isEmpty ? '清缓存后特征齐全' : '清缓存后仍缺 ${missing.join("/")}',
        );
      }
    }

    // 特征缺席不会拦连接，但 App 会一直到真正要用时才报「特征不可用」—— 那时离
    // "为什么"已经很远了（固件没编进去？还是 Android 缓存了旧 GATT 表、而 refresh()
    // 被系统屏蔽了？）。所以把实际发现的特征摊开记一笔，并把 FS 的后果说清楚。
    if (missing.isNotEmpty) {
      await AppLog.w(
        _tag,
        'Companion 服务缺 ${missing.join("/")}；实际发现: '
        '${service.characteristics.map((c) => c.uuid.str).join(', ')}',
      );
    }
    if (_fsCmdChar == null || _fsDataChar == null) {
      await AppLog.e(
        _tag,
        '缺文件传输特征（0xFF15/0xFF16），本次连接内同步、OTA、星历、图标上传都会失败。'
        '清缓存也没救回来时，一般是系统屏蔽了 refresh()，需用户重启蓝牙或清除蓝牙数据。',
      );
    }

    if (_protoVerChar == null ||
        _statusChar == null ||
        _controlChar == null ||
        _gnssChar == null) {
      await AppLog.e(
        _tag,
        'Companion 服务缺少 P0 特征: '
        'ver=${_protoVerChar != null} status=${_statusChar != null} '
        'ctrl=${_controlChar != null} gnss=${_gnssChar != null}',
      );
      _connectHint = LocaleKeys.bleErrFailed.tr;
      _failSoft(held: soft);
      await _disconnectDevice();
      return;
    }

    if (!await _checkProtocolVersion()) return;

    if (!await _subscribeStatus(device)) {
      // 状态读不上 = 这条会话不可用：别进 ready 假装能用，
      // 直接判错并退避重连 —— "连上但没数据"正是这么来的。
      _connectHint = LocaleKeys.bleErrFailed.tr;
      _failSoft(held: soft);
      await _disconnectDevice();
      return;
    }
    // **不在连接阶段订 0xFF16**：GATT 里带 Notify 的只剩它（0xFF12 已改轮询）。
    // 常开 CCC 会跟状态心跳/FS 突发抢控制器缓冲；改为首次 FS / 图标应答时再订，
    // 空闲 [_fsNotifyIdle] 后退订。FS_NEED 只在租约窗口内能推到手机。
    _fsSubscribed = false;

    _reconnectDelay = _reconnectMinDelay;
    _setState(CompanionConnState.ready);
    _startStatusWatch();
    _connectHint = null;
    await AppLog.i(_tag, '就绪 +${ms()}ms（devInfo/ping 后台）');

    // 设备信息与 ping 不挡 UI 进 ready；回连体感主要卡在这一段之前。
    unawaited(() async {
      await _readDevInfo();
      await ping();
      _startTimeSync();
    }());

    /* 握手做完就把链路降回省电档：空闲期的空口唤醒次数大约能少 7 倍
     * （15 ms → 100 ms 以上），而协议里这些来回（状态帧、通知推送、FS 命令）
     * 多等几十毫秒根本看不出来。批量传输会临时提回高速档（见 [_withFastLink]）——
     * 固件那边的吞吐（~15 KB/s）是按 15 ms 间隔实测出来的，不能一直挂低速档。 */
    await _applyLinkPriority(bulk: false);
  }

  /// 传输窗口计数：>0 表示正在搬数据，链路该跑高速档。
  int _bulkDepth = 0;

  /// 连接优先级两档。
  ///
  /// 为什么不按"前台/后台"两档：UI 上所有的交互（点击、1 Hz 骑行数据）都比 100 ms
  /// 慢得多，唯一在乎间隔的是搬文件 —— 那由 [_withFastLink] 覆盖。
  Future<void> _applyLinkPriority({required bool bulk}) async {
    final device = _device;
    if (device == null || !isReady) return;

    try {
      await device.requestConnectionPriority(
        connectionPriorityRequest:
            bulk ? ConnectionPriority.high : ConnectionPriority.lowPower,
      );
    } catch (e) {
      await AppLog.w(_tag, '连接优先级设置失败(bulk=$bulk): $e');
    }
  }

  /// 批量传输（[fsDownload] / [fsUpload]）期间把链路提到高速档，结束回落。
  ///
  /// 计数式：传输内部还会逐片拿 FS 锁，不能每片都发一次链路参数更新。
  Future<T> _withFastLink<T>(Future<T> Function() body) async {
    final first = _bulkDepth++ == 0;
    if (first) await _applyLinkPriority(bulk: true);

    try {
      return await body();
    } finally {
      if (--_bulkDepth == 0) await _applyLinkPriority(bulk: false);
    }
  }

  /// 把 [service] 里的特征指针全部重新解析一遍。
  ///
  /// 返回「协议里有、但这次没发现」的短 UUID 列表：`0xFF15`/`0xFF16` 文件传输、
  /// `0xFF17` 通知、`0xFF19` 导航下发、`0xFF1A` 设备信息。P0 特征是必需项，缺了
  /// 不在这里报，交给调用方断开。
  ///
  /// **FS 那一对必须列在这里**：它们缺席时连接照样能进 `ready`（状态/控制/GNSS
  /// 都还能用），但 Android 缓存旧 GATT 表这条最可能的成因就永远不会触发清缓存，
  /// 于是整个会话里文件传输都处于「特征不可用」。
  List<String> _bindChars(BluetoothService service) {
    _protoVerChar = _findChar(service, CompanionProto.protoVersionChar);
    _statusChar = _findChar(service, CompanionProto.deviceStatusChar);
    _controlChar = _findChar(service, CompanionProto.controlChar);
    _gnssChar = _findChar(service, CompanionProto.gnssFixChar);
    _fsCmdChar = _findChar(service, CompanionProto.fsCommandChar);
    _fsDataChar = _findChar(service, CompanionProto.fsDataChar);
    _notifChar = _findChar(service, CompanionProto.notificationChar);
    _navChar = _findChar(service, CompanionProto.navRouteChar);
    _devInfoChar = _findChar(service, CompanionProto.deviceInfoChar);

    // 指针换成新发现了，之前挂在旧对象上的订阅收不到任何东西。
    _fsSubscribed = false;

    return [
      if (_fsCmdChar == null) '0xFF15',
      if (_fsDataChar == null) '0xFF16',
      if (_notifChar == null) '0xFF17',
      if (_navChar == null) '0xFF19',
      if (_devInfoChar == null) '0xFF1A',
    ];
  }

  /// 清掉 Android 缓存的 GATT 服务表（反射调 `BluetoothGatt.refresh()`）。
  ///
  /// 只清缓存，不含重新发现；调用方需要自己再 `discoverServices()` 一次。
  /// 部分系统版本屏蔽了这个隐藏 API，这时返回 `false`，只能让用户去系统设置里
  /// 清蓝牙数据。返回 `true` 表示缓存已清、可以重发现。
  Future<bool> _clearGattCache(BluetoothDevice device) async {
    try {
      await device.clearGattCache();
    } catch (e) {
      await AppLog.w(
        _tag,
        '清 GATT 缓存失败（系统可能屏蔽了隐藏 API，需手动清蓝牙数据）: $e',
      );
      return false;
    }
    return true;
  }

  /// 手动清 GATT 缓存并重新发现服务（仅 Android）。
  ///
  /// 给「通知转发」那类只有 P1 特征才用得上的页面当补救按钮：连接本身是好的、
  /// 只是手机缓存了旧服务表时，点一下就不用去系统设置清蓝牙数据。
  /// 返回刷新后是否拿到了 `0xFF17`。
  /// 文件传输特征缺失时自救一次：清 GATT 缓存 → 重新发现 → 重绑特征。
  ///
  /// 为什么要有：0xFF15/0xFF16 缺失会让**同步、OTA、星历、图标上传全部失效**。
  /// 连接路径里已经有一次自动清缓存（`missing.isNotEmpty` 时触发），但**它只在连接
  /// 那一刻**跑；一旦那次没救回来（系统屏蔽了 `refresh()`），连接会**带着这个降级状态
  /// 继续**，之后就没有任何自动重试了 —— 只剩通知页那个手动按钮，或者用户自己发现
  /// "彻底杀掉 App 再进"（2026-09-27 现场），那等于在应用层把文件传输判死一段时间。
  /// 这里在**真正发起事务前**再自动做一遍同样的动作。
  ///
  /// 限频 30 s：上层（同步/上传）会重试，清缓存要重发现一遍，不值得连着做。
  /// 只有系统屏蔽了 refresh()（或设备确实没暴露这两个特征）时才仍会失败，那时
  /// 才需要人工重启蓝牙 —— 与 [CompanionFsUnavailableException] 的 hint 一致。
  Future<void> _healFsIfNeeded() async {
    if (_fsCmdChar != null && _fsDataChar != null) return;

    final now = DateTime.now();
    final last = _fsHealAt;
    if (last != null && now.difference(last) < const Duration(seconds: 30)) return;
    _fsHealAt = now;

    await AppLog.w(_tag, '文件传输特征缺失，自动清 GATT 缓存并重新发现一次');
    try {
      final ok = await refreshGattCache();
      await AppLog.i(
        _tag,
        ok ? '自动清缓存后特征已回来' : '自动清缓存后特征仍缺失（可能要重启蓝牙）',
      );
    } catch (e) {
      await AppLog.w(_tag, '自动清缓存失败: $e');
    }
  }

  Future<bool> refreshGattCache() async {
    final dev = _device;
    if (dev == null || _state == CompanionConnState.disconnected) {
      throw StateError('未连接，无法刷新服务缓存');
    }
    if (kIsWeb || !Platform.isAndroid) {
      throw StateError('只有 Android 需要清 GATT 缓存');
    }

    await AppLog.i(_tag, '手动清 GATT 缓存并重新发现');
    if (!await _clearGattCache(dev)) return false;

    // refresh() 只丢本地缓存，必须再发现一次才会重新走 read-by-group-type。
    final services = await dev.discoverServices();
    final service = services.firstWhereOrNull(
      (s) => s.uuid == CompanionProto.service,
    );
    if (service == null) {
      await AppLog.e(_tag, '手动刷新后未发现 Companion 服务');
      return false;
    }

    final missing = _bindChars(service);
    await AppLog.i(
      _tag,
      missing.isEmpty
          ? '手动刷新后特征齐全'
          : '手动刷新后仍缺 ${missing.join("/")}（${service.characteristics.length} 个特征）',
    );

    // P0 特征若被这次重新发现弄丢了（极少见），重新订阅一次保证链路可用。
    if (_statusChar != null) await _subscribeStatus(dev);
    // 0xFF16 仍按需：刷新后清订阅标志，下次 FS 再订。
    _fsSubscribed = false;
    _fsNotifyLease?.cancel();
    _fsNotifyLease = null;
    return _notifChar != null;
  }

  /// 读 `0xFF11` 并校验版本。返回是否可以继续。
  Future<bool> _checkProtocolVersion() async {
    final raw = await _protoVerChar!.read();
    if (raw.length < 2) {
      await AppLog.e(_tag, '协议版本响应长度异常: ${raw.length}');
      _setState(CompanionConnState.error);
      await _disconnectDevice();
      return false;
    }

    _deviceProtoVersion = raw[0] | (raw[1] << 8);
    final hex = _deviceProtoVersion!.toRadixString(16).padLeft(4, '0');

    if (_deviceProtoVersion != CompanionProto.expectedVersion) {
      final expected = CompanionProto.expectedVersion
          .toRadixString(16)
          .padLeft(4, '0');
      await AppLog.w(_tag, '协议版本不一致: 设备 0x$hex，App 期望 0x$expected');

      if (!allowVersionMismatch) {
        _setState(CompanionConnState.versionMismatch);
        _autoReconnect = false;
        _watchdog?.cancel();
        _watchdog = null;
        await _disconnectDevice();
        return false;
      }
    } else {
      await AppLog.i(_tag, '协议版本 0x$hex');
    }

    return true;
  }

  Future<void> _readDevInfo() async {
    final ch = _devInfoChar;
    if (ch == null) {
      _lastDevInfo = null;
      return;
    }

    try {
      final raw = await ch.read();
      _lastDevInfo = CompanionDevInfo.fromBytes(raw);
      await AppLog.i(_tag, '$_lastDevInfo');
    } catch (e) {
      _lastDevInfo = null;
      await AppLog.w(_tag, '读 0xFF1A 失败: $e');
    }
  }

  /// 心跳看门狗：`ready` 之下若 3× 心跳周期没有状态帧，主动断开走既有重连。
  ///
  /// 为什么需要它：状态通知是设备**主动**每 5 s 推一次的心跳，所以"没有帧"就是
  /// 链路已死的直接证据 —— 而 Android 侧 `connectionState` 有时长期不报断连
  /// （对端悄悄走了，或断连事件丢了），UI 就一直停在"已连接"却没有任何数据。
  /// 看门狗把这种状态变成**自动恢复**，不必再手动重启 App。
  void _startStatusWatch() {
    _statusWatchTimer?.cancel();
    _statusWatchTimer = Timer.periodic(const Duration(seconds: 5), (_) {
      if (_state != CompanionConnState.ready || _lastStatusRxMs == 0) return;

      final quiet = DateTime.now().millisecondsSinceEpoch - _lastStatusRxMs;
      if (quiet < _statusSilentMs) {
        _statusSilentStrikes = 0;   /* 又说话了：清零 */
        return;
      }

      /* **连续两个静默窗口才判死。** 设备侧可能在复位 LCPU（adapter cycle 8 s）
       * 或注入星历（~10 s），那一会儿没有状态帧是正常的 —— 一个窗口就断链，会把
       * 健康的连接自己踢掉（和固件那个粘滞 connected 是同一类自伤）。第一次只记
       * 一行继续观察。 */
      _statusSilentStrikes++;
      if (_statusSilentStrikes == 1) {
        unawaited(AppLog.w(_tag, '状态心跳静默 ${quiet}ms（第 1 次，继续观察）'));
        return;
      }

      _statusWatchTimer?.cancel();
      _statusWatchTimer = null;
      unawaited(() async {
        await AppLog.w(_tag, '状态心跳静默 ${quiet}ms，判链路已死，断开重连');
        _setState(CompanionConnState.error);
        await _disconnectDevice();
      }());
    });
  }

  /// 订阅 `0xFF12`（设备状态）并**验证订阅真的成立**。
  ///
  /// 设备在 CCCD 写成功的那一刻会立刻推一份快照，之后每 5 s 一次心跳 —— 所以
  /// **等到第一帧**才是订阅成立的证据。只看 `setNotifyValue()` 返回是不够的：
  /// 它只说明 CCCD 写入被应答；链路半死、或对端其实没记上时它照样成功返回，
  /// 表现就是现场那次「两边都显示已连接，却没有任何数据交互」。
  ///
  /// 两次都验证不过就返回 false，由调用方**判整条连接失败并退避重连** ——
  /// 订不上的会话继续挂在 `ready` 里没有任何意义。
  Future<bool> _subscribeStatus(BluetoothDevice device) async {
    final statusChar = _statusChar!;

    // **状态改成轮询读**（用户 2026-09-19 定）：
    //   不写 CCCD、不等"首帧通知"，连接后读一次取初值，之后按周期读。
    // 为什么：设备侧本来就 `Read + Notify` 且**可读值与通知同源**（g_status_buf），
    // 而现场出现过"CCCD=01 00 写进去了、设备侧首帧通知却没来" → app 判连接失败 →
    // 退避重连的死循环。轮询把这条依赖彻底去掉；通知只留给事件型特征（如 0xFF14）。
    _statusPollTimer?.cancel();
    await _statusSub?.cancel();
    _statusSub = null;

    var ok = false;
    for (var attempt = 0; attempt < 3 && !ok; attempt++) {
      try {
        final v = await statusChar.read(timeout: 8);
        _onStatusFrame(v);              // 复用同一套解析
        ok = true;
      } catch (e) {
        await AppLog.w(_tag, '读 0xFF12 失败（第 ${attempt + 1}/3 次）: $e');
        await Future<void>.delayed(const Duration(milliseconds: 100));
      }
    }

    if (!ok) {
      _statusSubscribed = false;
      await AppLog.e(_tag, '0xFF12 读取失败，判连接不可用');
      return false;
    }

    _statusSubscribed = true;
    _startStatusPoll(statusChar);
    return true;
  }

  /// 周期读 0xFF12（默认 3 s）。断线/重连由 `_statusSubscribed` 与重连路径收口：
  /// 定时器里读失败不弹错，只是跳过这一拍。
  void _startStatusPoll(BluetoothCharacteristic ch) {
    _statusPollTimer?.cancel();
    _statusPollTimer = Timer.periodic(const Duration(seconds: 3), (t) async {
      if (!_statusSubscribed) {
        t.cancel();
        return;
      }
      try {
        final v = await ch.read(timeout: 6);
        _onStatusFrame(v);
      } catch (_) {
        // 这一拍读失败：可能是设备忙/链路抖动，下一拍再试。
      }
    });
  }


  /// 读回某个特征的 CCCD（0x2902），看**设备侧实际**记成了什么。
  ///
  /// 为什么要读回：`setNotifyValue(true)` 只等"CCCD 写入被应答"，它并不保证设备
  /// 真的记上了。现场（2026-09-18/19 的 MCU 侧日志）出现过写成功但 FS 一帧都收不到、
  /// 20 s 后才超时的会话；设备侧的 CCC 还可能被它自己复位（对端已走/未配对时
  /// zblue 把 slot 的值清 0 并回调）。所以判据取设备侧读到的那两个字节。
  ///
  /// 读不到（超时/不支持读）返回 null，由调用方按"未知"处理 —— 不因为读回失败
  /// 就把整条链路判死。
  Future<List<int>?> _readCccd(BluetoothCharacteristic char) async {
    try {
      const cccdUuidStr = "00002902-0000-1000-8000-00805f9b34fb";
      final cccd = char.descriptors
          .where((d) => d.descriptorUuid == Guid(cccdUuidStr))
          .firstOrNull;
      if (cccd == null) return null;
      return await cccd.read(timeout: 5);
    } catch (_) {
      return null;
    }
  }

  /// 把 CCCD 的原始字节打成 `01 00` 这种形式（日志里跟 MCU 侧对账用）。
  static String _hexBytes(List<int> b) =>
      b.map((v) => v.toRadixString(16).padLeft(2, '0')).join(' ');

  /// 订阅 `0xFF16` 的 notify。
  ///
  /// 设备只通过 notify 回 FS 帧，所以这条订阅就是文件传输的命门。`setNotifyValue`
  /// 内部会等 CCCD 写入结果（失败或 15 s 超时即抛），Android 上偶发 133/超时是常态；
  /// 以前任异常冒泡，一次写失败就把整条连接判成 error 再退避重连 —— 表现出来就是
  /// 「过一段时间内 FS 完全不可用」。
  ///
  /// 现在：本地重试一次；仍失败也不拖垮 P0 链路（状态/控制/GNSS 照常），只把
  /// [canTransferFiles] 置 false 并排一次补订自愈。
  ///
  /// **2026-09-19 加了一道读回验证**：写完 CCCD 立刻读回，设备侧 notify 位是 0
  /// 就当场当失败（记警告 + 走补订），而不是等 20 s 的 FS 超时再倒推。MCU 侧同时
  /// 会打 `gatt: ccc write h=… raw=…` 与 `ble_companion: fs subscribe -> N`，
  /// 两边一对就知道是"客户端写了 0"还是"设备没生效"。
  Future<void> _subscribeFs(BluetoothDevice device) async {
    final char = _fsDataChar;
    if (char == null) {
      _fsSubscribed = false;
      return;
    }

    /// 先挂监听再写 CCCD：避免「写完立刻有通知、但 listener 还没接上」丢首帧。
    //
    // **2026-09-21**：静默回连后特征对象已换；若复用断线前的 `_fsSub`，CCCD 写
    // 仍会成功，但 Dart 监听挂在死 characteristic 上 → App「0 帧超时」、MCU 却
    // 可能正常 notify。每次订阅都重新 bind 到当前 `_fsDataChar`。
    await _dropFsListener();
    _fsSub = char.onValueReceived.listen(_onFsFrame);
    device.cancelWhenDisconnected(_fsSub!);

    for (var attempt = 0; attempt < 3; attempt++) {
      try {
        // 现场（2026-09-21）：App 读回 `cccd=01 00`，MCU 却是
        // `fs ccc raw=0000 → subscribe 0`，随后 `fs session aborted (no ccc)`。
        // 常见原因：① Android 描述符读回走本地缓存；② 断线/空闲退订的 CCC=0
        // 迟到覆盖。所以 **强制写两次** Notify=on，不单信一次读回。
        // 两次之间**不再显式 delay**：[_setNotify] 里的 [_cccdGap]（150 ms）已经
        // 保证了间隔，而且对所有调用点（含 `unawaited(...)` 的补订路径）一致生效。
        await _setNotify(char, true);
        await _setNotify(char, true);

        final readback = await _readCccd(char);
        if (readback != null && readback.isNotEmpty && (readback[0] & 0x01) == 0) {
          throw StateError('设备侧 CCCD 仍是 ${_hexBytes(readback)}（notify 未生效，'
              'isNotifying=${char.isNotifying}）');
        }

        _fsSubscribed = true;
        _fsResubTimer?.cancel();
        _fsResubTimer = null;
        _bumpFsNotifyLease();
        await AppLog.i(
          _tag,
          '0xFF16 已订阅 cccd=${readback == null ? "?" : _hexBytes(readback)} '
          'isNotifying=${char.isNotifying} (双写 CCC)',
        );
        if (_iconNeedQ.isNotEmpty) unawaited(_pumpIconNeed());
        return;
      } catch (e) {
        _fsSubscribed = false;
        await AppLog.w(_tag, '订阅 0xFF16 失败（第 ${attempt + 1}/3 次）: $e');
        if (attempt < 2) {
          await Future<void>.delayed(const Duration(milliseconds: 200));
        }
      }
    }

    await AppLog.e(_tag, '0xFF16 订阅失败，文件传输暂不可用，稍后补订');
    _scheduleFsResubscribe();
  }

  /// 事务进行中的补订：0 帧时**强制**再写 Notify=on。
  ///
  /// **2026-09-21 现场**：App 描述符读回 `01 00`，MCU 却是 `fs ccc raw=0000` /
  /// `subscribe -> 0` / `aborted (no ccc)`。读回不可信（本地缓存），「已是 01
  /// 就别乱动」会让假阳性卡死整段 FS。无第一帧时一律重写 CCC。
  Future<void> _ensureFsNotify(BluetoothDevice device) async {
    final char = _fsDataChar;
    if (char == null) return;

    await _dropFsListener();
    _fsSub = char.onValueReceived.listen(_onFsFrame);
    device.cancelWhenDisconnected(_fsSub!);

    try {
      final before = await _readCccd(char);
      await AppLog.w(
        _tag,
        'FS 补订：强制重写 CCC '
        'cccd_before=${before == null ? "?" : _hexBytes(before)} '
        'isNotifying=${char.isNotifying}',
      );
      await _setNotify(char, true);
      await Future<void>.delayed(const Duration(milliseconds: 80));
      await _setNotify(char, true);
      final again = await _readCccd(char);
      if (again != null && again.isNotEmpty && (again[0] & 0x01) == 0) {
        _fsSubscribed = false;
        await AppLog.w(_tag, 'FS 补订后 CCCD 仍是 ${_hexBytes(again)}');
        _scheduleFsResubscribe();
        return;
      }
      _fsSubscribed = true;
      await AppLog.i(
        _tag,
        'FS 补订完成 cccd=${again == null ? "?" : _hexBytes(again)}',
      );
      _bumpFsNotifyLease();
    } catch (e) {
      await AppLog.w(_tag, 'FS 轻量补订失败: $e');
      _scheduleFsResubscribe();
    }
  }

  /// 续租 0xFF16：有 FS 活动就推迟退订。
  void _bumpFsNotifyLease() {
    if (!_fsSubscribed) return;
    _fsNotifyLease?.cancel();
    _fsNotifyLease = Timer(_fsNotifyIdle, () {
      _fsNotifyLease = null;
      unawaited(_releaseFsNotifyIdle());
    });
  }

  /// 空闲退订 CCC（监听保留，下次 `_subscribeFs` 只写 CCCD）。
  Future<void> _releaseFsNotifyIdle() async {
    if (_fsWait != null || _iconNeedBusy) {
      _bumpFsNotifyLease();
      return;
    }
    if (!_fsSubscribed) return;
    final char = _fsDataChar;
    if (char == null) {
      _fsSubscribed = false;
      return;
    }
    try {
      await _setNotify(char, false);
      await AppLog.i(_tag, '0xFF16 空闲 ${_fsNotifyIdle.inSeconds}s，退订 notify');
    } catch (e) {
      await AppLog.w(_tag, '0xFF16 退订失败: $e');
    }
    _fsSubscribed = false;
  }

  /// CCCD 写没成功时补订，别让 FS 一直哑到下次重连。
  ///
  /// 首次发现阶段链路还没进 `ready`，所以 [isReady] 的判断放在回调里。
  void _scheduleFsResubscribe() {
    if (_fsResubTimer != null) return;
    _fsResubTimer = Timer(const Duration(seconds: 3), () {
      _fsResubTimer = null;
      final dev = _device;
      if (dev == null || _fsSubscribed) return;
      if (_state != CompanionConnState.ready) return;
      // 有事务在跑就让一次：换 CCCD 期间可能丢掉正在等的那帧回复。
      if (_fsWait != null) {
        _scheduleFsResubscribe();
        return;
      }
      unawaited(_subscribeFs(dev));
    });
  }

  void _onFsFrame(List<int> data) {
    final CompanionFsFrame frame;
    try {
      frame = CompanionFsFrame.fromBytes(data);
    } catch (e) {
      AppLog.w(_tag, 'FS 帧解析失败: $e');
      return;
    }

    if (frame.cmd == CompanionFs.need) {
      if (!frame.isErr && frame.status == 0) {
        final name = utf8.decode(frame.payload, allowMalformed: true);
        _enqueueIconNeed(name);
      }
      return;
    }

    if (frame.cmd == CompanionFs.writeData &&
        (frame.isErr || frame.status != 0)) {
      _fsWritePushErr ??= CompanionFsException(
        '设备返回 errno=${frame.status}',
        status: frame.status,
      );
      return;
    }

    final wait = _fsWait;
    if (wait == null || wait.isCompleted) return;
    if (frame.tid != _fsWaitTid || frame.cmd != _fsWaitCmd) return;

    if (frame.isErr || frame.status != 0) {
      wait.completeError(
        CompanionFsException(
          '设备返回 errno=${frame.status}',
          status: frame.status,
          partial: _concatFsPayloads(),
        ),
      );
      _fsWait = null;
      return;
    }

    if (frame.seq != _fsCollected.length) {
      /* 固件在 LIST 之后可能没把 seq 清零，单帧 STAT/READ 会带着 seq=1 LAST。
       * 这仍是完整回复，不能当成缺包。多帧中间缺号才失败。
       */
      if (frame.isLast && _fsCollected.isEmpty && frame.seq <= 1) {
        AppLog.w(
          _tag,
          'FS seq=${frame.seq} 但仅 LAST 一帧，按完整回复接收 '
          'cmd=0x${frame.cmd.toRadixString(16)} tid=${frame.tid}',
        );
        _fsCollected.add(frame);
        wait.complete(List<CompanionFsFrame>.from(_fsCollected));
        _fsWait = null;
        return;
      }
      AppLog.w(
        _tag,
        'FS seq 不连续: cmd=0x${frame.cmd.toRadixString(16)} '
        'tid=${frame.tid} 期望 ${_fsCollected.length} 收到 ${frame.seq}'
        '${frame.isLast ? " LAST" : ""}',
      );
      wait.completeError(
        CompanionFsException(
          'FS seq 不连续: 期望 ${_fsCollected.length} 收到 ${frame.seq}',
          partial: _concatFsPayloads(),
        ),
      );
      _fsWait = null;
      return;
    }

    _fsCollected.add(frame);
    if (frame.isLast) {
      wait.complete(List<CompanionFsFrame>.from(_fsCollected));
      _fsWait = null;
    }
  }

  Uint8List _concatFsPayloads() {
    final n = _fsCollected.fold<int>(0, (s, f) => s + f.payload.length);
    final out = Uint8List(n);
    var o = 0;
    for (final f in _fsCollected) {
      out.setAll(o, f.payload);
      o += f.payload.length;
    }
    return out;
  }

  int _nextFsTid() {
    final tid = _fsTid;
    _fsTid = _fsTid >= 255 ? 1 : _fsTid + 1;
    return tid;
  }

  /// 串行执行一次 FS 事务。
  ///
  /// `0xFF15`/`0xFF16` 是单通道：同一时刻只能有一个事务在等回复，否则
  /// [_fsWaitTid]/[_fsWaitCmd]/[_fsCollected] 会被后一个覆盖，两个调用方双双超时。
  /// 设备主动索要图标（`FS_NEED`）的时机随机，所以并发是常态而非例外 —— 以前只有
  /// icon 那条路径用忙等规避，其余调用方（同步 / OTA / 星历 / 文件浏览）撞上就直接
  /// 失败。这里改成 FIFO 排队：
  ///
  /// - 上传/下载这类多步事务整体持锁，中间不会被别的命令插进设备会话；
  /// - 后来者排队等待，而不是立刻报错。
  Future<T> _withFsLock<T>(Future<T> Function() body) async {
    final prev = _fsLock;
    final gate = Completer<void>();
    _fsLock = gate.future;
    try {
      await prev;
      final dev = _device;
      if (dev == null || _state != CompanionConnState.ready) {
        throw CompanionFsUnavailableException(
          'Companion 未就绪，无法做文件传输',
          hint: '请先连上码表',
        );
      }
      // 特征缺失就先自救一次（清 GATT 缓存 + 重发现 + 重绑），见 _healFsIfNeeded。
      // 放在这里而不是只靠连接路径：真正暴露"没有这两个特征"的时刻是**发起事务**时。
      await _healFsIfNeeded();

      // 按需打开 0xFF16 notify；失败则本事务直接不可用。
      if (!_fsSubscribed) {
        await _subscribeFs(dev);
      }
      if (!_fsSubscribed) {
        throw CompanionFsUnavailableException(
          '0xFF16 的通知没订上，收不到设备回复',
          hint: '正在自动补订，稍后重试或重连一次',
        );
      }
      _bumpFsNotifyLease();
      return await body();
    } finally {
      _bumpFsNotifyLease();
      // gate 只 complete 不 completeError，一次失败不会把后面的队列卡死。
      gate.complete();
    }
  }

  /// Android 整连接 GATT 写串行化（与 [_withFsLock] 独立：控制/GNSS/CCC 也要排队）。
  Future<T> _withGattOp<T>(Future<T> Function() body) async {
    final prev = _gattLock;
    final gate = Completer<void>();
    _gattLock = gate.future;
    try {
      await prev;
      return await body();
    } finally {
      gate.complete();
    }
  }

  /// `gatt.writeCharacteristic() returned 201` → ERROR_GATT_WRITE_REQUEST_BUSY。
  static bool _isGattWriteBusy(Object e) {
    if (e is! PlatformException) return false;
    final msg = e.message ?? '';
    final details = '${e.details ?? ''}';
    final blob = '${e.code} $msg $details'.toLowerCase();
    return blob.contains('201') ||
        blob.contains('write_request_busy') ||
        blob.contains('gatt_write_request_busy');
  }

  /// 特征写：串行 + 201 短退避重试。
  Future<void> _charWrite(
    BluetoothCharacteristic char,
    List<int> value, {
    bool withoutResponse = false,
    bool allowLongWrite = false,
    int maxTries = 5,
  }) {
    return _withGattOp(() async {
      Object? last;
      for (var i = 0; i < maxTries; i++) {
        try {
          await char.write(
            value,
            withoutResponse: withoutResponse,
            allowLongWrite: allowLongWrite,
          );
          return;
        } on PlatformException catch (e) {
          last = e;
          if (!_isGattWriteBusy(e) || i == maxTries - 1) rethrow;
          final waitMs = 40 * (1 << i);
          await AppLog.w(
            _tag,
            'GATT write busy(201)，${waitMs}ms 后重试 ${i + 1}/$maxTries',
          );
          await Future<void>.delayed(Duration(milliseconds: waitMs));
        }
      }
      throw last!;
    });
  }

  /// 连续写 CCCD（描述符）之间至少要留的间隔。
  ///
  /// 联发科（MTK）手机的 BLE 方案在**订阅类操作连续下发**时会直接把链路断掉
  /// （其他项目实测；高通无此问题）。订阅 / 退订 / 补订 / "双写 CCC" 全部会经过
  /// [_setNotify]，所以把间隔做在这**唯一入口**里，比在每个调用点补 delay 可靠 ——
  /// 也不会漏掉 `unawaited(...)` 那几条补订路径。想按平台区分（MTK 用更长间隔、
  /// 高通走快路）需要引入 device_info_plus 之类的插件，这里先用统一保守值。
  static const _cccdGap = Duration(milliseconds: 150);
  DateTime? _lastCccdWrite;

  /// 距上一次描述符写不足 [_cccdGap] 就等一下（调用点已在 [_withGattOp] 里串行）。
  Future<void> _cccdSettleGap() async {
    final last = _lastCccdWrite;
    if (last != null) {
      final spent = DateTime.now().difference(last);
      if (spent < _cccdGap) {
        await Future<void>.delayed(_cccdGap - spent);
      }
    }
  }

  /// CCCD 写同样占 Android 唯一 GATT 槽，必须跟特征写排队。
  Future<void> _setNotify(
    BluetoothCharacteristic char,
    bool enable, {
    int maxTries = 4,
  }) {
    return _withGattOp(() async {
      Object? last;
      for (var i = 0; i < maxTries; i++) {
        try {
          await _cccdSettleGap();
          try {
            await char.setNotifyValue(enable);
          } finally {
            _lastCccdWrite = DateTime.now();
          }
          return;
        } on PlatformException catch (e) {
          last = e;
          if (!_isGattWriteBusy(e) || i == maxTries - 1) rethrow;
          final waitMs = 40 * (1 << i);
          await AppLog.w(
            _tag,
            'GATT CCC busy(201)，${waitMs}ms 后重试 ${i + 1}/$maxTries',
          );
          await Future<void>.delayed(Duration(milliseconds: waitMs));
        }
      }
      throw last!;
    });
  }

  Future<Uint8List> _fsCommand(
    int cmd,
    List<int> payload, {
    Duration timeout = const Duration(seconds: 20),
  }) async {
    final cmdChar = _fsCmdChar;
    if (cmdChar == null || _fsDataChar == null) {
      throw CompanionFsUnavailableException(
        '设备没有暴露文件传输特征'
        '（0xFF15 ${_fsCmdChar == null ? "缺" : "有"}，'
        '0xFF16 ${_fsDataChar == null ? "缺" : "有"}）',
        hint: '多为手机缓存了旧 GATT 服务表，可在通知页清一次服务缓存',
      );
    }
    // [_withFsLock] 已保证订上；落到这里仍为 false 属于内部次序错误。
    if (!_fsSubscribed) {
      throw CompanionFsUnavailableException(
        '0xFF16 的通知没订上，收不到设备回复',
        hint: '正在自动补订，稍后重试或重连一次',
      );
    }
    _bumpFsNotifyLease();
    // 走到这里说明调用方没走 [_withFsLock]：并发事务会互相吞掉回复。
    if (_fsWait != null) {
      throw StateError('FS 事务未串行化（内部错误）');
    }

    final tid = _nextFsTid();
    _fsCollected.clear();
    _fsWaitTid = tid;
    _fsWaitCmd = cmd;
    final wait = Completer<List<CompanionFsFrame>>();
    _fsWait = wait;

    final pkt = Uint8List(2 + payload.length);
    pkt[0] = cmd;
    pkt[1] = tid;
    pkt.setAll(2, payload);

    // **第一帧看门狗**（2026-09-20 / 2026-09-21）：设备侧的 CCC 可能已被手机栈
    // 悄悄清零，而我们这边的 `_fsSubscribed` 还是 true。3 s 无第一帧时做**轻量
    // 补订**（[_ensureFsNotify]）：只在 CCCD 真是 0 时重写，不拆监听、不先把
    // `_fsSubscribed` 打假 —— 否则会把正在等的回复和后续命令一起弄死。
    Timer? firstFrame;
    final devAtSend = _device;
    if (_fsSubscribed && devAtSend != null) {
      firstFrame = Timer(const Duration(seconds: 3), () {
        if (!wait.isCompleted && _fsCollected.isEmpty) {
          unawaited(AppLog.w(_tag, 'FS 3 s 无第一帧：检查/补写设备侧 CCC'));
          unawaited(_ensureFsNotify(devAtSend));
        }
      });
    }

    try {
      await AppLog.i(
        _tag,
        'FS 发命令 cmd=0x${cmd.toRadixString(16)} tid=$tid '
        'len=${pkt.length} subscribed=$_fsSubscribed',
      );
      await _charWrite(
        cmdChar,
        pkt,
        withoutResponse: cmdChar.properties.writeWithoutResponse,
      );
      await wait.future.timeout(timeout);
    } on TimeoutException {
      if (!wait.isCompleted) {
        wait.completeError(
          CompanionFsException('FS 等待超时', partial: _concatFsPayloads()),
        );
      }
      _fsWait = null;
      // 订阅标志为真却一帧未收：再做一次轻量检查；仍不行才排完整补订。
      if (_fsCollected.isEmpty) {
        final dev = _device;
        if (dev != null) {
          unawaited(_ensureFsNotify(dev));
        } else {
          _fsSubscribed = false;
          _scheduleFsResubscribe();
        }
      }
      await AppLog.w(
        _tag,
        'FS 等待超时 cmd=0x${cmd.toRadixString(16)} tid=$tid '
        '已收 ${_fsCollected.length} 帧'
        '${_fsCollected.isEmpty ? "（一帧未收：已检查/补写 CCC）" : ""}',
      );
      throw CompanionFsException(
        'FS 等待超时',
        partial: _concatFsPayloads(),
      );
    } catch (e) {
      _fsWait = null;
      rethrow;
    } finally {
      firstFrame?.cancel();
    }

    final body = _concatFsPayloads();
    if (cmd == CompanionFs.read) {
      AppLog.i(
        _tag,
        'READ tid=$tid ${_fsCollected.length} 帧 ${body.length} B',
      );
    }
    return body;
  }

  void _onStatusFrame(List<int> data) {
    _lastStatusRxMs = DateTime.now().millisecondsSinceEpoch;

    // 订阅验证：订阅成功后设备会立刻推一帧，这里把它当作"订阅真的成立"的证据。
    final wait = _statusWait;
    if (wait != null && !wait.isCompleted) wait.complete();

    try {
      final status = CompanionStatus.fromBytes(data);
      _lastStatus = status;
      _statusCtrl.add(status);
    } catch (e) {
      AppLog.w(_tag, '状态帧解析失败: $e');
    }
  }

  void _onConnectionStateChanged(BluetoothConnectionState s) {
    if (s != BluetoothConnectionState.disconnected) return;

    // autoConnect 排队期间、重挂 connectionState 流时都会立刻回放一条
    // disconnected。若此时正在 `_connectInternal`，当成真断开会拆掉在飞的
    // 发现并叠退避 —— adb 现场：`链路已建立` 后立刻 `device is not connected`，
    // 退避 200→6400ms 空转。建链中的断开一律忽略，由 connect/wait 的超时收口。
    if (_connectInFlight || isLinking) {
      AppLog.i(_tag, '建链中收到断开事件，忽略');
      return;
    }

    AppLog.i(_tag, '连接断开');
    final wasReady = _state == CompanionConnState.ready;
    // 上传循环不靠 _fsWait，靠这个立刻停掉，避免断链后继续写撞 201。
    _fsWritePushErr ??= CompanionFsException('连接断开');
    final wait = _fsWait;
    if (wait != null && !wait.isCompleted) {
      wait.completeError(
        CompanionFsException('连接断开', partial: _concatFsPayloads()),
      );
    }
    _fsWait = null;
    _fsSubscribed = false;
    _fsResubTimer?.cancel();
    _fsResubTimer = null;
    _fsNotifyLease?.cancel();
    _fsNotifyLease = null;
    // 必须拆掉监听：否则静默回连后 `_subscribeFs` 见 `_fsSub != null` 会跳过
    // `onValueReceived.listen`，挂在旧特征上 → CCCD=01 却 0 帧。
    unawaited(_dropFsListener());
    _statusPollTimer?.cancel();
    _statusPollTimer = null;
    _statusSubscribed = false;
    _statusWatchTimer?.cancel();
    _statusWatchTimer = null;
    _statusSilentStrikes = 0;
    unawaited(_statusSub?.cancel());
    _statusSub = null;
    _iconNeedQ.clear();
    _statusChar = null;
    _controlChar = null;
    _gnssChar = null;
    _protoVerChar = null;
    _fsCmdChar = null;
    _fsDataChar = null;
    _notifChar = null;
    _navChar = null;
    _devInfoChar = null;
    _lastDevInfo = null;

    // versionMismatch 是我们主动断的，不要覆盖掉这个结论。
    if (_state != CompanionConnState.versionMismatch) {
      _setState(CompanionConnState.disconnected);
    }

    // 刚就绪过的链路断了：从最短退避起跳，别背着上次失败堆高的 16/30 s。
    if (wasReady) {
      _reconnectDelay = _reconnectMinDelay;
    }

    _scheduleReconnect();
  }

  /// 取消 `0xFF16` 的 [onValueReceived] 订阅（不断 CCC；CCC 由调用方另行处理）。
  Future<void> _dropFsListener() async {
    final sub = _fsSub;
    _fsSub = null;
    if (sub != null) {
      try {
        await sub.cancel();
      } catch (_) {}
    }
  }

  void _scheduleReconnect() {
    if (!_autoReconnect || _device == null) return;

    _reconnectTimer?.cancel();
    final delay = _reconnectDelay;
    AppLog.i(_tag, '${delay.inMilliseconds}ms 后重连');

    _reconnectTimer = Timer(delay, () {
      // 指数退避，避免设备关机时无休止地唤醒无线电耗电。
      final next = _reconnectDelay * 2;
      _reconnectDelay = next > _reconnectMaxDelay ? _reconnectMaxDelay : next;

      /* 上一次连接还在飞就别再开一条：Android BLE 一个设备只允许一个在途连接，
       * 重复 connect() 会把前一次顶掉/抛异常，实机表现为"连上就断"的循环。
       *
       * **但跳过必须有上限**：如果 `_connectInFlight`/`isLinking` 卡在 true（FBP
       * 停在 connecting、事件丢失），无限重排就等于"再也不会自动连" —— 2026-09-18
       * 现场。连跳 6 次（约十几秒）就认定那个"在途"是陈旧的，主动断开重来。 */
      if (_connectInFlight || isLinking) {
        _reconnectSkips++;
        if (_reconnectSkips < 6) {
          AppLog.i(_tag, '重连跳过（上一次仍在进行，第 $_reconnectSkips 次）');
          _scheduleReconnect();
          return;
        }

        unawaited(AppLog.w(_tag, '连接在途状态陈旧 $_reconnectSkips 次，强制断开后重连'));
        _reconnectSkips = 0;
        unawaited(() async {
          await _disconnectDevice();
          await _connectInternal(held: true);
        }());
        return;
      }

      _reconnectSkips = 0;
      unawaited(_connectInternal(held: true));
    });
  }

  void _armWatchdog() {
    _watchdog?.cancel();
    _watchdog = Timer.periodic(_watchdogPeriod, (_) {
      if (!_autoReconnect || _device == null || isReady) return;
      if (isLinking || _connectInFlight) return;

      /* **退避计时器已经排上时，看门狗不许插队。**
       *
       * 现场（2026-09-17 23:24-23:25 的 logcat）：`16s 后重连` 刚排上，看门狗
       * 每 5 s 又发起一次连接，把还没超时的那次请求顶掉（每次都是
       * registerApp→close→unregisterApp），于是"连接→立刻断开→再连"循环，
       * 成功率反而更低。两个触发器只该留一个：退避在跑就让看门狗待着，
       * 它只负责"退避丢了/没人排"的兜底。 */
      final timer = _reconnectTimer;
      if (timer != null && timer.isActive) {
        return;
      }

      AppLog.i(_tag, '看门狗重连');
      unawaited(_connectInternal(held: true));
    });
  }

  void _onAdapterState(BluetoothAdapterState s) {
    if (s == BluetoothAdapterState.on) {
      unawaited(kickReconnect(resetBackoff: true));
    }
  }

  /// 写控制命令（特征 `0xFF13`）。
  ///
  /// [param] 为可选参数字节。设备处理后会回一帧状态，其 `lastCtrlOp` 即本次
  /// [opcode] 的回显。
  Future<void> sendControl(int opcode, {List<int> param = const []}) async {
    final char = _controlChar;
    if (char == null) {
      throw StateError('Companion 未就绪，无法下发控制命令');
    }

    await _charWrite(char, [opcode, ...param]);
  }

  /// 请求设备立即回一帧状态。
  Future<void> ping() => sendControl(CompanionCtrl.ping);

  /// 请码表开配对窗口（[seconds] 秒）：之后**另一台**手机才能扫到并配上。
  /// 已绑定的那台不需要它（设备是定向广播给它的）。
  Future<void> openPairWindow({int seconds = 30}) async {
    final d = ByteData(2)..setUint16(0, seconds, Endian.little);
    await sendControl(
      CompanionCtrl.pairOpen,
      param: d.buffer.asUint8List(),
    );
  }

  /// 解除本机与码表的绑定（码表侧清密钥 + 清已绑定手机记录）。
  Future<void> unbindPhone() => sendControl(CompanionCtrl.pairUnbind);

  /// 把手机当前 UTC 秒和本地时区分钟写到码表，供 MCU 同步 RTC。
  Future<void> sendTimeSync() async {
    final now = DateTime.now();
    final utcSec = now.toUtc().millisecondsSinceEpoch ~/ 1000;
    final tzMin = now.timeZoneOffset.inMinutes;
    final payload = ByteData(6)
      ..setUint32(0, utcSec, Endian.little)
      ..setInt16(4, tzMin, Endian.little);
    await sendControl(
      CompanionCtrl.timeSync,
      param: payload.buffer.asUint8List(),
    );
  }

  void _startTimeSync() {
    _stopTimeSync();
    unawaited(_sendTimeSyncSafe());
    _timeSyncTimer = Timer.periodic(_timeSyncPeriod, (_) {
      unawaited(_sendTimeSyncSafe());
    });
  }

  void _stopTimeSync() {
    _timeSyncTimer?.cancel();
    _timeSyncTimer = null;
  }

  Future<void> _sendTimeSyncSafe() async {
    if (!isReady) return;
    try {
      await sendTimeSync();
      _lastTimeSyncAt = DateTime.now();
      await AppLog.i(_tag, '已同步时间戳到码表');
    } catch (e) {
      await AppLog.w(_tag, '同步时间戳失败: $e');
    }
  }

  /// 前台恢复时：链路仍在但后台定时器可能被冻，超过 1 小时再补一次。
  Future<void> refreshTimeSyncIfDue() async {
    if (!isReady) return;
    final last = _lastTimeSyncAt;
    if (last != null && DateTime.now().difference(last) < _timeSyncPeriod) {
      return;
    }
    await _sendTimeSyncSafe();
  }

  /// 开始录制 / 开始骑行。
  Future<void> startRecord() => sendControl(CompanionCtrl.startRecord);

  /// 停止录制 / 结束骑行。
  Future<void> stopRecord() => sendControl(CompanionCtrl.stopRecord);

  /// [startRecord] 的别名。
  Future<void> startRide() => startRecord();

  /// [stopRecord] 的别名。
  Future<void> stopRide() => stopRecord();

  /// 停止导航，不停止录制。
  Future<void> stopNav() => sendControl(CompanionCtrl.navStop);

  /// 推送一帧手机 GNSS（特征 `0xFF14`）。
  ///
  /// 默认用「无响应写」：定位是高频且可丢的，等每一帧的 ACK 只会堆积延迟。
  /// 设备不支持无响应写时自动退回普通写。
  Future<void> sendGnssFix(CompanionGnssFix fix) async {
    final char = _gnssChar;
    if (char == null) {
      throw StateError('Companion 未就绪，无法推送 GNSS');
    }

    final withoutResponse = char.properties.writeWithoutResponse;
    await _charWrite(
      char,
      fix.toBytes(),
      withoutResponse: withoutResponse,
    );
    _lastGnssSentAt = DateTime.now();
  }

  /// 开始把系统定位按 [gnssMinInterval] 限速推送给码表。
  ///
  /// 调用前需已取得定位权限。重复调用会先停掉上一次的订阅。
  Future<void> startGnssStreaming({
    LocationAccuracy accuracy = LocationAccuracy.best,
  }) async {
    await stopGnssStreaming();

    _positionSub = Geolocator.getPositionStream(
      // intervalDuration 交给平台限速：不设的话 GPS 按原生频率（通常 1 Hz）唤醒，
      // 我们在 Dart 侧再把多余的丢掉 —— 唤醒照样发生了，纯浪费电。
      // 只有 AndroidSettings 有这一项，其余平台沿用 [LocationSettings]。
      locationSettings: !kIsWeb && Platform.isAndroid
          ? AndroidSettings(
              accuracy: accuracy,
              intervalDuration: gnssMinInterval,
            )
          : LocationSettings(accuracy: accuracy),
    ).listen(
      (position) {
        final last = _lastGnssSentAt;
        if (last != null &&
            DateTime.now().difference(last) < gnssMinInterval) {
          return;
        }

        if (!isReady) return;

        sendGnssFix(CompanionGnssFix.fromPosition(position)).catchError(
          (Object e) => AppLog.w(_tag, 'GNSS 推送失败: $e'),
        );
      },
      onError: (Object e) => AppLog.w(_tag, '定位流出错: $e'),
    );

    await AppLog.i(_tag, 'GNSS 推送已开启（≤${gnssMinInterval.inMilliseconds}ms 一帧）');
  }

  /// 停止 GNSS 推送。
  Future<void> stopGnssStreaming() async {
    await _positionSub?.cancel();
    _positionSub = null;
  }

  /// 推送一条手机通知（特征 `0xFF17`）。
  ///
  /// [iconFileName] 仅为文件名（如 `com.tencent.mm.png`），不含路径。
  Future<void> sendNotification({
    required String title,
    required String body,
    String appName = '',
    String packageName = '',
    String iconFileName = '',
    int type = CompanionNotif.typeApp,
  }) async {
    final char = _notifChar;
    if (char == null) {
      throw StateError('设备无 0xFF17 通知特征');
    }

    final tlv = BytesBuilder();
    void addTag(int tag, List<int> value) {
      if (value.isEmpty) return;
      tlv.addByte(tag);
      tlv.add(_u16le(value.length));
      tlv.add(value);
    }

    addTag(
      CompanionNotif.tagAppName,
      utf8.encode(_clipChars(appName, CompanionNotif.titleMax)),
    );
    addTag(
      CompanionNotif.tagTitle,
      utf8.encode(_clipChars(title, CompanionNotif.titleMax)),
    );
    addTag(
      CompanionNotif.tagBody,
      utf8.encode(_clipChars(body, CompanionNotif.bodyMax)),
    );
    addTag(
      CompanionNotif.tagPackage,
      utf8.encode(CompanionNotif.sanitizePackage(packageName)),
    );
    addTag(
      CompanionNotif.tagIcon,
      utf8.encode(CompanionNotif.wireName(iconFileName)),
    );

    await _writeFragments(
      char,
      tlv.takeBytes(),
      type: type,
      extraFlags: 0,
      maxTotal: CompanionNotif.maxTotal,
    );
  }

  /// 下发导航轨迹点。[start] 为 true 时设备收到最后一包即开始导航。
  ///
  /// [points] 为 WGS84 `(lat, lon)`。
  Future<void> sendNavRoute(
    List<(double, double)> points, {
    bool start = true,
  }) async {
    final char = _navChar;
    if (char == null) {
      throw StateError('设备无 0xFF19 导航特征');
    }
    if (points.isEmpty) {
      throw ArgumentError('导航点不能为空');
    }

    final n = points.length > CompanionNav.maxPts
        ? CompanionNav.maxPts
        : points.length;
    final payload = Uint8List(n * CompanionNav.ptLen);
    final bd = ByteData.sublistView(payload);
    for (var i = 0; i < n; i++) {
      final latE7 = (points[i].$1 * 1e7).round();
      final lonE7 = (points[i].$2 * 1e7).round();
      bd.setInt32(i * CompanionNav.ptLen, latE7, Endian.little);
      bd.setInt32(i * CompanionNav.ptLen + 4, lonE7, Endian.little);
    }

    await _writeFragments(
      char,
      payload,
      type: 0,
      extraFlags: start ? CompanionNav.flagStart : 0,
      maxTotal: CompanionNav.maxTotal,
    );
  }

  Future<void> _writeFragments(
    BluetoothCharacteristic char,
    List<int> payload, {
    required int type,
    required int extraFlags,
    required int maxTotal,
  }) async {
    if (payload.length > maxTotal) {
      throw ArgumentError('下行载荷 ${payload.length} 超过 $maxTotal');
    }

    final msgId = _downlinkMsgId++ & 0xff;
    final chunkMax = (_mtu - 3 - CompanionNotif.hdrLen).clamp(1, 244);
    final withoutResponse = char.properties.writeWithoutResponse;
    var off = 0;
    final total = payload.length == 0 ? 0 : payload.length;

    if (total == 0) {
      throw ArgumentError('下行载荷为空');
    }

    while (off < total) {
      final n = (total - off) > chunkMax ? chunkMax : (total - off);
      var flags = extraFlags;
      if (off == 0) flags |= CompanionNotif.flagFirst;
      if (off + n >= total) flags |= CompanionNotif.flagLast;

      final frame = Uint8List(CompanionNotif.hdrLen + n);
      frame[0] = CompanionNotif.hdrVer;
      frame[1] = type & 0xff;
      frame[2] = flags & 0xff;
      frame[3] = msgId;
      frame.setAll(4, _u16le(total));
      frame.setAll(6, _u16le(off));
      frame.setAll(CompanionNotif.hdrLen, payload.sublist(off, off + n));
      await _charWrite(char, frame, withoutResponse: withoutResponse);
      off += n;
    }
  }

  Uint8List _u16le(int v) => Uint8List.fromList([v & 0xff, (v >> 8) & 0xff]);

  Uint8List _u32leBytes(int v) => Uint8List.fromList([
        v & 0xff,
        (v >> 8) & 0xff,
        (v >> 16) & 0xff,
        (v >> 24) & 0xff,
      ]);

  int _u32At(Uint8List b, [int o = 0]) =>
      b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24);

  Uint8List _pathBytes(String path) =>
      Uint8List.fromList(utf8.encode(CompanionFs.wirePath(path)));

  String _clipChars(String s, int maxChars) =>
      s.length <= maxChars ? s : s.substring(0, maxChars);

  /// 列目录。不要 LIST 巨大的 `vmap/` / `map/`。
  Future<List<CompanionFsEntry>> fsList(String path, {int cursor = 0}) =>
      _withFsLock(() async {
        final pathb = _pathBytes(path);
        final payload = Uint8List(2 + pathb.length);
        payload.setAll(0, _u16le(cursor));
        payload.setAll(2, pathb);
        final body = await _fsCommand(CompanionFs.list, payload);
        return CompanionFsEntry.parseList(body);
      });

  /// 查询单个路径。
  Future<CompanionFsEntry> fsStat(String path) =>
      _withFsLock(() => _fsStatLocked(path));

  /// 已在 FS 锁内时用的 [fsStat]（[fsDownload] 自己要拿锁，不能重入）。
  Future<CompanionFsEntry> _fsStatLocked(String path) async {
    final body = await _fsCommand(CompanionFs.stat, _pathBytes(path));
    if (body.length < 9) {
      throw CompanionFsException('STAT 回复过短');
    }
    return CompanionFsEntry(
      type: body[0],
      size: _u32At(body, 1),
      mtime: _u32At(body, 5),
      name: path.split('/').where((s) => s.isNotEmpty).last,
    );
  }

  /// 单次 READ 的文件载荷上限：一次命令连发多帧 notify，避免每包往返。
  int get _fsReadChunkMax =>
      (_mtu - 3 - CompanionFs.dataHdrLen).clamp(1, 244);

  int get _fsReadBurst =>
      (_fsReadChunkMax * 48).clamp(_fsReadChunkMax, 16384);

  /// 下载。[offset] 为已收到的字节数，断线后续传从这里接着读。
  ///
  /// 整个多轮 READ 过程持 FS 锁：中途被别的命令插进来会让设备的读会话错乱。
  /// 下载：**带自动续传重试**（2026-09-25）。
  ///
  /// 为什么要外层重试：设备侧下行流控把 **ATT 确认当信用**
  /// （`FS_CREDIT_TIMEOUT_MS = 40`，40 ms 收不到确认就强行把 inflight 归零继续发），
  /// 在广播/扫描抢射频时会把"已发未确认"的页算丢 ⇒ App 这边看到**序号断裂**
  /// （设备侧计数器 `credit_to/credit_lost` 就是同一件事）。
  /// 内层已能按 offset 续读，但连续 [_fsDownloadMaxGaps] 次 gap 就放弃整段，
  /// 而失败只冒到 UI ⇒ 用户只能重启 App。这里补上与上传同形的重试：把失败时
  /// **已收到的字节**接上、从新 offset 续读，最多 [maxTries] 次；期间不打折
  /// 进度（`total` 只增不减），并在日志里写明重试轮次。
  Future<Uint8List> fsDownload(
    String path, {
    int offset = 0,
    int? length,
    void Function(int done, int total)? onProgress,
    int maxTries = 3,
  }) => _withFastLink(() async {
    final out = BytesBuilder();
    var got = 0;
    var seenTotal = 0;

    void relay(int done, int total) {
      if (total > seenTotal) seenTotal = total;
      final d = done > seenTotal ? seenTotal : done;
      onProgress?.call(d, seenTotal);
    }

    for (var i = 0; i < maxTries; i++) {
      final int? remaining = length == null ? null : length - got;
      if (remaining != null && remaining <= 0) break;

      try {
        final chunk = await _withFsLock(
          () => _fsDownloadLocked(
            path,
            offset: offset + got,
            length: remaining,
            onProgress: relay,
          ),
        );
        out.add(chunk);
        return out.takeBytes();
      } on CompanionFsUnavailableException {
        // 回连中/没订上：别空转重试，交给上层等 ready（与上传一致）。
        rethrow;
      } on CompanionFsException catch (e) {
        final part = e.partial;
        if (part == null || part.isEmpty) rethrow;
        out.add(part);
        got += part.length;
        if (i == maxTries - 1) rethrow;
        AppLog.w(
          _tag,
          '下载中断，续传重试 ${i + 1}/${maxTries - 1}（已收 $got B）: $e',
        );
        await Future<void>.delayed(Duration(milliseconds: 120 * (i + 1)));
      }
    }

    return out.takeBytes();
  });

  Future<Uint8List> _fsDownloadLocked(
    String path, {
    required int offset,
    int? length,
    void Function(int done, int total)? onProgress,
  }) async {
    final int remain;
    if (length != null) {
      remain = length;
    } else {
      final st = await _fsStatLocked(path);
      remain = (st.size - offset).clamp(0, st.size);
    }

    final out = BytesBuilder(copy: false);
    var off = offset;
    var left = remain;
    var gaps = 0;
    final total = offset + remain;
    final sw = Stopwatch()..start();
    var lastLogMs = 0;
    void report(int done) {
      onProgress?.call(done, total);
      final ms = sw.elapsedMilliseconds;
      if (done < total && ms - lastLogMs < 250) return;
      lastLogMs = ms;
      final kbs = (done - offset) * 1000 / (ms < 1 ? 1 : ms) / 1024;
      AppLog.i(
        _tag,
        '下载 ${kbs.toStringAsFixed(1)} KB/s  $done/$total '
        '(${sw.elapsedMilliseconds} ms)',
      );
    }

    report(off);
    final rle = CompanionRle.shouldCompress(path);
    var wireTotal = 0;
    var loggedRle = false;
    while (left > 0) {
      final want = left > _fsReadBurst ? _fsReadBurst : left;
      final pathb = _pathBytes(path);
      final pkt = Uint8List(9 + pathb.length);
      pkt.setAll(0, _u32leBytes(off));
      pkt.setAll(4, _u32leBytes(want));
      pkt[8] = rle ? CompanionFs.readFlagRle : 0;
      pkt.setAll(9, pathb);
      try {
        var chunk = await _fsCommand(CompanionFs.read, pkt);
        wireTotal += chunk.length;
        if (rle) {
          final wire = chunk.length;
          chunk = CompanionRle.decode(chunk);
          if (!loggedRle) {
            loggedRle = true;
            AppLog.i(
              _tag,
              '下载 RLE ${chunk.length}←$wire '
              '(${(chunk.length / (wire == 0 ? 1 : wire)).toStringAsFixed(1)}x)',
            );
          }
        }
        out.add(chunk);
        off += chunk.length;
        left -= chunk.length;
        gaps = 0;
        report(off);
        if (chunk.isEmpty) break;
      } on CompanionFsException catch (e) {
        if (!rle && e.partial != null && e.partial!.isNotEmpty) {
          out.add(e.partial!);
          off += e.partial!.length;
          left -= e.partial!.length;
          report(off);
        }
        gaps++;
        if (gaps > _fsDownloadMaxGaps || left <= 0) {
          /* 放弃**本次尝试**，但把已收到的字节带上 —— 外层 `fsDownload()` 据此
           * 按 offset 续传。2026-09-25 之前这里直接 `rethrow`，整段失败只能重启
           * App（现场："fs 通道异常，app 重启恢复"）。 */
          final partial = out.takeBytes();
          final want = partial.length + (left > 0 ? left : 0);
          throw CompanionFsException(
            '下载中断（连续 gap $gaps，已收 ${partial.length}/$want B）',
            status: e.status,
            partial: partial,
          );
        }
        await AppLog.w(_tag, 'READ 重试 offset=$off ($gaps/$_fsDownloadMaxGaps): $e');
        await Future<void>.delayed(const Duration(milliseconds: 80));
      }
    }
    if (rle && wireTotal > 0) {
      final raw = off - offset;
      AppLog.i(
        _tag,
        '下载 RLE 合计 $raw←$wireTotal '
        '(${(raw / wireTotal).toStringAsFixed(1)}x, ${sw.elapsedMilliseconds} ms)',
      );
    }
    return out.takeBytes();
  }

  /// 上传。[resume] 为 true 时接着设备上未完成的 `*.part` 写。
  ///
  /// 整个上传（含续传重试）持 FS 锁：设备侧是一条写会话，别的命令插进来会打断它。
  Future<void> fsUpload(
    String path,
    List<int> data, {
    bool resume = true,
    void Function(int done, int total)? onProgress,
  }) => _withFastLink(
    () => _withFsLock(
      () => _fsUploadLocked(path, data, resume: resume, onProgress: onProgress),
    ),
  );

  Future<void> _fsUploadLocked(
    String path,
    List<int> data, {
    required bool resume,
    void Function(int done, int total)? onProgress,
  }) async {
    const maxTries = 4;
    CompanionFsException? last;
    final tries = resume ? maxTries : 1;
    for (var i = 0; i < tries; i++) {
      try {
        await _fsUploadOnce(
          path,
          data,
          resume: resume,
          onProgress: onProgress,
        );
        return;
      } on CompanionFsUnavailableException {
        // 回连中：别空转重试，等下次 ready / 用户再点。
        rethrow;
      } on PlatformException catch (e) {
        if (!_isGattWriteBusy(e) || i == tries - 1) rethrow;
        await AppLog.w(
          _tag,
          '上传 GATT busy(201)，续传重试 ${i + 1}/${tries - 1}',
        );
        await Future<void>.delayed(const Duration(milliseconds: 250));
      } on CompanionFsException catch (e) {
        last = e;
        final canResumeRetry =
            resume && (e.status == 5 || e.status == 22);
        if (!canResumeRetry || i == tries - 1) {
          rethrow;
        }
        await AppLog.w(
          _tag,
          '上传中断 (${CompanionFs.errnoText(e.status)})，续传重试 ${i + 1}/${tries - 1}',
        );
        await Future<void>.delayed(const Duration(milliseconds: 250));
      }
    }
    throw last!;
  }

  Future<void> _fsUploadOnce(
    String path,
    List<int> data, {
    required bool resume,
    void Function(int done, int total)? onProgress,
  }) async {
    final dataChar = _fsDataChar;
    if (dataChar == null) {
      throw CompanionFsUnavailableException(
        '设备没有暴露 0xFF16 文件传输特征',
        hint: '多为手机缓存了旧 GATT 服务表，可在通知页清一次服务缓存',
      );
    }

    _fsWritePushErr = null;

    final crcAll = CompanionFs.crc32Part(data);
    final pathb = _pathBytes(path);
    var useRle = CompanionRle.shouldCompress(path);
    Uint8List? encodedAll;
    if (useRle) {
      encodedAll = CompanionRle.encode(data);
      if (encodedAll.length >= (data.length * 9) ~/ 10) {
        useRle = false;
        encodedAll = null;
      }
    }

    var opened = await _fsWriteOpen(
      pathb,
      data.length,
      resume: resume,
      rle: useRle,
    );
    if (opened.offset > 0 &&
        !CompanionFs.resumePrefixMatches(data, opened.offset, opened.crc)) {
      final partHex = opened.crc.toRadixString(16).padLeft(8, '0');
      await AppLog.w(
        _tag,
        '续传前缀 CRC 不符 off=${opened.offset}/$partHex，从头上传',
      );
      if (!resume) {
        throw CompanionFsException('续传前缀损坏', status: 5);
      }
      await _fsUploadOnce(path, data, resume: false, onProgress: onProgress);
      return;
    }

    /* .part 已是明文。从中间续传再开一条 RLE 流，CLOSE 很容易 CRC/长度
     * 对不上（errno 5）。有进度时改发明文。 */
    if (opened.offset > 0 && useRle) {
      useRle = false;
      encodedAll = null;
      opened = await _fsWriteOpen(
        pathb,
        data.length,
        resume: true,
        rle: false,
      );
      if (!CompanionFs.resumePrefixMatches(data, opened.offset, opened.crc)) {
        await AppLog.w(_tag, '续传前缀在重开后仍不符，从头上传');
        await _fsUploadOnce(path, data, resume: false, onProgress: onProgress);
        return;
      }
    }

    final offset = opened.offset;
    if (offset > 0) {
      await AppLog.i(_tag, '续传 $offset/${data.length}');
    }
    onProgress?.call(offset, data.length);
    if (offset == data.length) {
      await _fsCommand(CompanionFs.writeClose, _u32leBytes(crcAll));
      return;
    }

    final rawRemain = data is Uint8List
        ? data.sublist(offset)
        : Uint8List.fromList(data.sublist(offset));
    final payload = useRle && encodedAll != null ? encodedAll : rawRemain;

    final chunkMax = (_mtu - 3 - CompanionFs.dataHdrLen).clamp(1, 244);
    var seq = 0;
    var wireOff = 0;
    final tid = _fsTid == 1 ? 255 : _fsTid - 1;
    final sw = Stopwatch()..start();
    var lastLogMs = 0;
    void report(int done) {
      onProgress?.call(done, data.length);
      final ms = sw.elapsedMilliseconds;
      if (done < data.length && ms - lastLogMs < 250) return;
      lastLogMs = ms;
      final kbs = (done - offset) * 1000 / (ms < 1 ? 1 : ms) / 1024;
      AppLog.i(
        _tag,
        '上传 ${kbs.toStringAsFixed(1)} KB/s  $done/${data.length} '
        '(${sw.elapsedMilliseconds} ms)',
      );
    }

    if (useRle) {
      AppLog.i(
        _tag,
        '上传 RLE ${rawRemain.length}→${payload.length} '
        '(${(rawRemain.length / (payload.isEmpty ? 1 : payload.length)).toStringAsFixed(1)}x)',
      );
    }

    void throwIfPushFailed() {
      final err = _fsWritePushErr;
      if (err != null) {
        throw err;
      }
    }

    while (wireOff < payload.length) {
      throwIfPushFailed();
      // 回连中继续写 → Android 201 BUSY；MCU 可能还活着，只是手机栈拒写。
      if (!isReady) {
        throw CompanionFsUnavailableException(
          '连接已断开，上传中止',
          hint: '回连后会自动续传',
        );
      }
      final n = (payload.length - wireOff) > chunkMax
          ? chunkMax
          : (payload.length - wireOff);
      final frame = Uint8List(CompanionFs.dataHdrLen + n);
      frame[0] = CompanionFs.writeData;
      frame[1] = tid;
      frame[2] = 0;
      frame[3] = 0;
      frame[4] = seq & 0xff;
      frame[5] = (seq >> 8) & 0xff;
      frame[6] = n & 0xff;
      frame[7] = (n >> 8) & 0xff;
      frame.setAll(
        CompanionFs.dataHdrLen,
        payload.sublist(wireOff, wireOff + n),
      );
      final noRsp = dataChar.properties.writeWithoutResponse;
      // 中间帧 WRITE_CMD；每 16 包带一次响应，避免码表 RX 环满丢包。
      // 设备落盘时若每包都等 ATT ACK，整段上传会拖成几十秒。
      final last = wireOff + n >= payload.length;
      final credit = (seq % 16) == 15;
      final useNr = noRsp && !last && !credit;
      await _charWrite(dataChar, frame, withoutResponse: useNr);
      wireOff += n;
      seq++;
      final done = offset +
          ((wireOff * rawRemain.length) /
                  (payload.isEmpty ? 1 : payload.length))
              .floor();
      report(done > data.length ? data.length : done);
    }

    throwIfPushFailed();
    await _fsCommand(CompanionFs.writeClose, _u32leBytes(crcAll));
  }

  Future<({int offset, int crc})> _fsWriteOpen(
    Uint8List pathb,
    int size, {
    required bool resume,
    required bool rle,
  }) async {
    final open = Uint8List(5 + pathb.length);
    open.setAll(0, _u32leBytes(size));
    open[4] = (resume ? CompanionFs.writeFlagResume : 0) |
        (rle ? CompanionFs.writeFlagRle : 0);
    open.setAll(5, pathb);
    final reply = await _fsCommand(CompanionFs.writeOpen, open);
    if (reply.length < CompanionFs.writeOpenReplyLen) {
      throw CompanionFsException('WRITE_OPEN 回复过短');
    }
    final offset = _u32At(reply, 0);
    if (offset > size) {
      throw CompanionFsException('续传偏移 $offset 超出文件 $size');
    }
    return (offset: offset, crc: _u32At(reply, 4));
  }

  /// 删除文件或空目录，同时清掉未完成的 `*.part`。
  Future<void> fsDelete(String path) =>
      _withFsLock(() => _fsCommand(CompanionFs.delete, _pathBytes(path)));

  /// 创建目录。
  Future<void> fsMkdir(String path) =>
      _withFsLock(() => _fsCommand(CompanionFs.mkdir, _pathBytes(path)));

  /// 移动 / 重命名。
  Future<void> fsRename(String from, String to) => _withFsLock(() async {
    final oldb = _pathBytes(from);
    final newb = _pathBytes(to);
    if (oldb.length > 255) {
      throw CompanionFsException('源路径过长');
    }
    final payload = Uint8List(1 + oldb.length + newb.length);
    payload[0] = oldb.length;
    payload.setAll(1, oldb);
    payload.setAll(1 + oldb.length, newb);
    await _fsCommand(CompanionFs.rename, payload);
  });

  void _enqueueIconNeed(String raw) {
    final name = CompanionNotif.wireName(raw);
    if (name.isEmpty) {
      return;
    }
    if (!_iconNeedQ.contains(name)) {
      _iconNeedQ.add(name);
      AppLog.i(_tag, '码表索要图标 $name');
    }
    unawaited(_pumpIconNeed());
  }

  Future<void> _pumpIconNeed() async {
    if (_iconNeedBusy) {
      return;
    }
    _iconNeedBusy = true;
    try {
      while (_iconNeedQ.isNotEmpty) {
        if (!isReady) {
          _iconNeedQ.clear();
          return;
        }
        // 订阅没成功时先留着队：补订成功会再踢一次 [_pumpIconNeed]。
        if (!canTransferFiles) return;
        final name = _iconNeedQ.removeAt(0);
        try {
          await _fulfillIconNeed(name);
        } catch (e) {
          await AppLog.w(_tag, '上传码表所要图标 $name 失败: $e');
        }
      }
    } finally {
      _iconNeedBusy = false;
      if (_iconNeedQ.isNotEmpty && isReady && canTransferFiles) {
        unawaited(_pumpIconNeed());
      }
    }
  }

  Future<void> _fulfillIconNeed(String name) async {
    var bytes = await iconNeedLoader?.call(name);
    bytes ??= await NotifIconCache.instance.readPng(name);
    if (bytes == null || bytes.isEmpty) {
      await AppLog.w(_tag, '码表索要图标 $name，本地没有');
      return;
    }

    final dest = CompanionNotif.iconFsPath(name);
    if (dest.isEmpty) {
      return;
    }
    try {
      await fsMkdir(CompanionNotif.iconDir);
    } catch (_) {
      // 目录可能已存在。
    }
    await fsUpload(dest, bytes, resume: false);
    await AppLog.i(_tag, '已按码表请求上传 $name (${bytes.length} B)');
  }

  /// 丢掉当前上传的 `*.part`（明确取消，不是断线）。
  Future<void> fsAbort() => sendControl(CompanionCtrl.fsAbort);

  /// 主动断开并停止自动重连。
  Future<void> disconnect() async {
    _autoReconnect = false;
    await _teardown();
    _setState(CompanionConnState.disconnected);
  }

  Future<void> _disconnectDevice() async {
    try {
      await _device?.disconnect();
    } catch (_) {
      // 已经断了就没什么可做的。
    }
  }

  Future<void> _teardown() async {
    _stopTimeSync();
    _reconnectTimer?.cancel();
    _reconnectTimer = null;
    _watchdog?.cancel();
    _watchdog = null;
    _fsResubTimer?.cancel();
    _fsResubTimer = null;
    _fsNotifyLease?.cancel();
    _fsNotifyLease = null;
    _fsSubscribed = false;
    _statusPollTimer?.cancel();
    _statusPollTimer = null;
    _statusSubscribed = false;
    await stopGnssStreaming();
    await _dropFsListener();
    await _statusSub?.cancel();
    _statusSub = null;
    _statusWatchTimer?.cancel();
    _statusWatchTimer = null;
    _lastStatusRxMs = 0;
    await _connSub?.cancel();
    _connSub = null;
    await _disconnectDevice();

    _protoVerChar = null;
    _statusChar = null;
    _controlChar = null;
    _gnssChar = null;
    _fsCmdChar = null;
    _fsDataChar = null;
    _notifChar = null;
    _navChar = null;
    _devInfoChar = null;
    _lastDevInfo = null;
    _device = null;
  }

  /// 释放资源。仅在 App 退出时调用。
  Future<void> dispose() async {
    await _adapterSub?.cancel();
    _adapterSub = null;
    await _teardown();
    await _stateCtrl.close();
    await _statusCtrl.close();
  }

  void _setState(CompanionConnState s) {
    if (_state == s) return;
    _state = s;
    if (!_stateCtrl.isClosed) _stateCtrl.add(s);
  }

  BluetoothCharacteristic? _findChar(BluetoothService service, Guid uuid) =>
      service.characteristics.firstWhereOrNull((c) => c.uuid == uuid);
}

import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:get/get.dart';
import 'package:sifli_companion/app/app_routes.dart';
import 'package:sifli_companion/ble/ble_manager.dart';
import 'package:sifli_companion/ble/companion_client.dart';
import 'package:sifli_companion/ble/companion_foreground_service.dart';
import 'package:sifli_companion/ble/companion_notif_relay.dart';
import 'package:sifli_companion/ble/companion_proto.dart';
import 'package:sifli_companion/ble/gnss_codec.dart';
import 'package:sifli_companion/ui/ble/companion_notif_guide.dart';

/// Companion 联调页：驱动 P0 的端到端验收。
///
/// 覆盖 `companion_impl_plan.md` §5.3 的全部验收项：按服务 UUID 扫描、连接、
/// 读协议版本、订阅状态、PING 回显、GNSS 推送计数与 `GPS_PHONE` 置位/超时清除、
/// MTU 协商结果、前台服务保活。
///
/// 这是开发者工具而非产品界面，因此文案直接内联，未走 `LocaleKeys` 国际化。
class CompanionDebugPage extends StatefulWidget {
  /// 创建联调页。
  const CompanionDebugPage({super.key});

  @override
  State<CompanionDebugPage> createState() => _CompanionDebugPageState();
}

class _CompanionDebugPageState extends State<CompanionDebugPage> {
  final _client = CompanionClient.instance;

  final Map<String, ScanResult> _found = {};
  StreamSubscription<List<ScanResult>>? _scanSub;
  StreamSubscription<bool>? _isScanningSub;
  StreamSubscription<CompanionConnState>? _stateSub;
  StreamSubscription<CompanionStatus>? _statusSub;
  Timer? _uiTicker;

  bool _scanning = false;
  bool _gnssStreaming = false;
  bool _fgsRunning = false;
  String? _error;

  int _statusFrames = 0;
  DateTime? _lastStatusAt;

  @override
  void initState() {
    super.initState();

    _isScanningSub = FlutterBluePlus.isScanning.listen((v) {
      if (mounted) setState(() => _scanning = v);
    });

    _stateSub = _client.stateStream.listen((_) {
      if (mounted) setState(() {});
    });

    _statusSub = _client.statusStream.listen((_) {
      if (!mounted) return;
      setState(() {
        _statusFrames++;
        _lastStatusAt = DateTime.now();
      });
    });

    // 状态帧之间也要刷新「距上一帧多久」，否则看不出心跳有没有停。
    _uiTicker = Timer.periodic(const Duration(seconds: 1), (_) {
      if (mounted) setState(() {});
    });

    _refreshFgsState();
  }

  @override
  void dispose() {
    _scanSub?.cancel();
    _isScanningSub?.cancel();
    _stateSub?.cancel();
    _statusSub?.cancel();
    _uiTicker?.cancel();
    _client.stopScan();
    super.dispose();
  }

  Future<void> _refreshFgsState() async {
    final running = await CompanionForegroundService.isRunning();
    if (mounted) setState(() => _fgsRunning = running);
  }

  Future<void> _startScan() async {
    setState(() {
      _error = null;
      _found.clear();
    });

    if (!await BleManager().requestPermissions()) {
      setState(() => _error = '扫描权限被拒绝');
      return;
    }

    if (FlutterBluePlus.adapterStateNow != BluetoothAdapterState.on) {
      await BleManager().turnOn();
      await Future<void>.delayed(const Duration(milliseconds: 500));
      if (FlutterBluePlus.adapterStateNow != BluetoothAdapterState.on) {
        setState(() => _error = '请先打开蓝牙');
        return;
      }
    }

    await _scanSub?.cancel();
    _scanSub = _client.scanResults.listen((results) {
      if (!mounted) return;
      setState(() {
        for (final r in results) {
          _found[r.device.remoteId.str] = r;
        }
      });
    });

    try {
      await _client.startScan();
    } catch (e) {
      if (mounted) setState(() => _error = '扫描失败: $e');
    }
  }

  Future<void> _connect(BluetoothDevice device) async {
    setState(() {
      _error = null;
      _statusFrames = 0;
      _lastStatusAt = null;
    });

    await _client.stopScan();
    await _client.connect(device);
    CompanionNotifRelay.instance.attachIconNeed();
  }

  Future<void> _disconnect() async {
    await _client.stopGnssStreaming();
    await _client.disconnect();
    if (mounted) setState(() => _gnssStreaming = false);
  }

  Future<void> _guard(Future<void> Function() action) async {
    try {
      await action();
      if (mounted) setState(() => _error = null);
    } catch (e) {
      if (mounted) setState(() => _error = '$e');
    }
  }

  Future<void> _toggleGnssStreaming() async {
    if (_gnssStreaming) {
      await _client.stopGnssStreaming();
      if (mounted) setState(() => _gnssStreaming = false);
      return;
    }

    await _guard(() async {
      await _client.startGnssStreaming();
      if (mounted) setState(() => _gnssStreaming = true);
    });
  }

  Future<void> _toggleForegroundService() async {
    if (_fgsRunning) {
      await CompanionForegroundService.stop();
    } else {
      await CompanionForegroundService.requestNotificationPermission();
      await CompanionForegroundService.start();
    }

    // 服务状态由原生侧维护，回读而不是本地取反。
    await Future<void>.delayed(const Duration(milliseconds: 300));
    await _refreshFgsState();
  }

  @override
  Widget build(BuildContext context) {
    final state = _client.state;
    final connected = state == CompanionConnState.ready;

    return Scaffold(
      appBar: AppBar(
        title: const Text('Companion 联调'),
      ),
      body: Column(
        children: [
          _bleHeader(state),
          Expanded(
            child: ListView(
              padding: const EdgeInsets.all(12),
              children: [
                _featureMenu(connected),
                const SizedBox(height: 12),
                if (connected) ...[
                  _devInfoCard(),
                  const SizedBox(height: 12),
                  _statusCard(),
                  const SizedBox(height: 12),
                  _controlCard(),
                  const SizedBox(height: 12),
                  _navCard(),
                  const SizedBox(height: 12),
                  _gnssCard(),
                  const SizedBox(height: 12),
                ],
                const CompanionNotifGuideCard(),
                const SizedBox(height: 12),
                _foregroundServiceCard(),
              ],
            ),
          ),
        ],
      ),
    );
  }

  Widget _errorBanner(String message) => Card(
        color: Theme.of(context).colorScheme.errorContainer,
        child: Padding(
          padding: const EdgeInsets.all(12),
          child: Row(
            children: [
              const Icon(Icons.error_outline),
              const SizedBox(width: 8),
              Expanded(child: Text(message)),
            ],
          ),
        ),
      );

  /// 固定在顶部：扫描/连接/断开不随下面功能卡片滚走。
  Widget _bleHeader(CompanionConnState state) {
    final scheme = Theme.of(context).colorScheme;
    final ready = state == CompanionConnState.ready;
    final connecting = state == CompanionConnState.connecting ||
        state == CompanionConnState.discovering;
    final version = _client.deviceProtoVersion;
    final versionText = version == null
        ? '未读取'
        : '0x${version.toRadixString(16).padLeft(4, '0')}'
            '${version == CompanionProto.expectedVersion ? '' : ' (不匹配!)'}';
    final deviceName = _client.device?.platformName ?? '';
    final deviceId = _client.device?.remoteId.str ?? '';
    final subtitle = ready
        ? [
            if (deviceName.isNotEmpty) deviceName,
            if (deviceId.isNotEmpty) deviceId,
            '协议 $versionText',
            'MTU ${_client.mtu}',
          ].join(' · ')
        : connecting
            ? _stateLabel(state)
            : '通知、轨迹、GNSS、文件都要先连上码表';

    return Material(
      elevation: 1,
      color: scheme.surfaceContainerHighest,
      child: Padding(
        padding: const EdgeInsets.fromLTRB(12, 10, 12, 12),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            Row(
              children: [
                Icon(
                  ready ? Icons.bluetooth_connected : Icons.bluetooth_disabled,
                  color: ready ? Colors.teal : scheme.onSurfaceVariant,
                ),
                const SizedBox(width: 8),
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Text(
                        ready ? '已连接' : _stateLabel(state),
                        style: Theme.of(context).textTheme.titleMedium,
                      ),
                      Text(
                        subtitle,
                        style: Theme.of(context).textTheme.bodySmall,
                      ),
                    ],
                  ),
                ),
                if (ready)
                  OutlinedButton(
                    onPressed: _disconnect,
                    child: const Text('断开'),
                  )
                else if (_scanning)
                  OutlinedButton(
                    onPressed: () => _client.stopScan(),
                    child: const Text('停止'),
                  )
                else
                  FilledButton.icon(
                    onPressed: connecting ? null : _startScan,
                    icon: const Icon(Icons.search),
                    label: const Text('扫描'),
                  ),
              ],
            ),
            if (connecting || _scanning)
              const Padding(
                padding: EdgeInsets.only(top: 8),
                child: LinearProgressIndicator(),
              ),
            if (_error != null) ...[
              const SizedBox(height: 8),
              _errorBanner(_error!),
            ],
            if (state == CompanionConnState.versionMismatch)
              const Padding(
                padding: EdgeInsets.only(top: 8),
                child: Text(
                  '设备协议版本与 App 不一致，已断开。请更新固件或 App。',
                  style: TextStyle(fontWeight: FontWeight.bold),
                ),
              ),
            if (!ready) ...[
              const SizedBox(height: 8),
              Text(
                '扫描服务 ${CompanionProto.service}',
                style: Theme.of(context).textTheme.bodySmall,
              ),
              if (_found.isEmpty)
                Padding(
                  padding: const EdgeInsets.only(top: 4),
                  child: Text(_scanning ? '扫描中…' : '点「扫描」查找附近码表'),
                )
              else
                ConstrainedBox(
                  constraints: const BoxConstraints(maxHeight: 220),
                  child: ListView(
                    shrinkWrap: true,
                    children: [
                      for (final r in _found.values)
                        ListTile(
                          dense: true,
                          contentPadding: EdgeInsets.zero,
                          title: Text(
                            r.device.platformName.isEmpty
                                ? '(无名)'
                                : r.device.platformName,
                          ),
                          subtitle: Text(
                            '${r.device.remoteId.str}  ${r.rssi} dBm',
                          ),
                          trailing: FilledButton(
                            onPressed: connecting
                                ? null
                                : () => _connect(r.device),
                            child: const Text('连接'),
                          ),
                        ),
                    ],
                  ),
                ),
            ],
          ],
        ),
      ),
    );
  }

  Widget _devInfoCard() {
    final info = _client.lastDevInfo;
    return _card(
      title: '设备信息 0xFF1A',
      children: [
        if (info == null)
          const Text('旧固件没有 0xFF1A，或读取失败')
        else ...[
          _kv('运行槽', info.slotName),
          _kv('软件版本', info.swVersion.isEmpty ? '—' : info.swVersion),
          _kv('硬件版本', info.hwVersion.isEmpty ? '—' : info.hwVersion),
          _kv('引导版本', info.bootVersion.isEmpty ? '—' : info.bootVersion),
          if (info.slot == CompanionSlot.fw && info.fwName.isNotEmpty)
            _kv('fw 文件', info.fwLabel),
        ],
      ],
    );
  }

  Widget _statusCard() {
    final status = _client.lastStatus;
    final since = _lastStatusAt == null
        ? '—'
        : '${DateTime.now().difference(_lastStatusAt!).inSeconds}s 前';

    return _card(
      title: '设备状态 0xFF12',
      children: [
        _kv('已收帧数', '$_statusFrames'),
        _kv('最近一帧', since),
        if (status == null)
          const Text('尚未收到状态帧')
        else ...[
          _kv(
            'lastCtrlOp',
            '0x${status.lastCtrlOp.toRadixString(16).padLeft(2, '0')}',
          ),
          _kv('gnssRxCount', '${status.gnssRxCount}'),
          _kv('剩余空间', '${status.storageFreeKb} KiB'),
          _kv('运行时长', '${status.uptimeSec}s'),
          _kv('BD_ADDR', status.classicAddress.isEmpty ? '—' : status.classicAddress),
          _kv(
            '电量',
            status.hasBattery ? '${status.batteryPct}%' : '未知',
          ),
          const SizedBox(height: 8),
          Wrap(
            spacing: 6,
            runSpacing: 6,
            children: _flagChips(status.flags),
          ),
        ],
      ],
    );
  }

  Widget _featureMenu(bool connected) {
    return Card(
      clipBehavior: Clip.antiAlias,
      child: Column(
        children: [
          _menuTile(
            color: const Color(0xFF3B82F6),
            icon: Icons.notifications_outlined,
            title: '消息通知',
            onTap: () => Get.toNamed(AppRoutes.notifSettings),
          ),
          const Divider(height: 1, indent: 72),
          _menuTile(
            color: const Color(0xFFEF4444),
            icon: Icons.folder_outlined,
            title: '码表文件操作',
            subtitle: connected ? null : '请先连接码表',
            onTap: connected
                ? () => Get.toNamed(AppRoutes.companionFs)
                : null,
          ),
          const Divider(height: 1, indent: 72),
          _menuTile(
            color: const Color(0xFF0D9488),
            icon: Icons.system_update_alt,
            title: '固件升级',
            onTap: () => Get.toNamed(AppRoutes.companionOta),
          ),
          const Divider(height: 1, indent: 72),
          _menuTile(
            color: const Color(0xFF7C3AED),
            icon: Icons.satellite_alt_outlined,
            title: '同步星历',
            subtitle: connected ? null : '请先连接码表',
            onTap: connected
                ? () => Get.toNamed(AppRoutes.companionEph)
                : null,
          ),
          const Divider(height: 1, indent: 72),
          _menuTile(
            color: const Color(0xFF38BDF8),
            icon: Icons.sd_storage_outlined,
            title: 'App 后台缓存',
            onTap: () => Get.toNamed(AppRoutes.appCache),
          ),
        ],
      ),
    );
  }

  Widget _menuTile({
    required Color color,
    required IconData icon,
    required String title,
    String? subtitle,
    required VoidCallback? onTap,
  }) {
    return ListTile(
      leading: CircleAvatar(
        backgroundColor: color,
        foregroundColor: Colors.white,
        child: Icon(icon, size: 22),
      ),
      title: Text(title),
      subtitle: subtitle == null ? null : Text(subtitle),
      trailing: const Icon(Icons.chevron_right, color: Colors.grey),
      enabled: onTap != null,
      onTap: onTap,
    );
  }

  Widget _controlCard() => _card(
        title: '控制 0xFF13',
        children: [
          const Text('PING 后 lastCtrlOp 应立即变为 0x03；对时为 0x04。'),
          const SizedBox(height: 8),
          Wrap(
            spacing: 8,
            runSpacing: 8,
            children: [
              FilledButton.icon(
                icon: const Icon(Icons.network_ping),
                label: const Text('PING'),
                onPressed: () => _guard(_client.ping),
              ),
              OutlinedButton(
                onPressed: () => _guard(_client.sendTimeSync),
                child: const Text('同步时间'),
              ),
              OutlinedButton(
                onPressed: () => _guard(_client.startRide),
                child: const Text('开始骑行'),
              ),
              OutlinedButton(
                onPressed: () => _guard(_client.stopRide),
                child: const Text('结束骑行'),
              ),
            ],
          ),
        ],
      );


  Widget _navCard() => _card(
        title: '导航轨迹 0xFF19',
        children: [
          const Text(
            '下发连续轨迹点。带「开始」标志时码表立即规划导航；也可先下发点位再点开始骑行。',
          ),
          const SizedBox(height: 8),
          Wrap(
            spacing: 8,
            runSpacing: 8,
            children: [
              FilledButton.icon(
                icon: const Icon(Icons.route),
                label: const Text('下发演示轨迹并开始导航'),
                onPressed: () => _guard(() async {
                  await _client.sendNavRoute(
                    const [
                      (31.2304, 121.4737),
                      (31.2335, 121.4808),
                      (31.2380, 121.4880),
                      (31.2420, 121.4950),
                    ],
                    start: true,
                  );
                }),
              ),
              OutlinedButton(
                onPressed: () => _guard(_client.stopNav),
                child: const Text('停止导航'),
              ),
            ],
          ),
        ],
      );

  Widget _gnssCard() => _card(
        title: 'GNSS 下行 0xFF14',
        children: [
          const Text(
            '推送后 gnssRxCount 应递增、GPS_PHONE 置位；停推 5s 后该位应自动清除。',
          ),
          const SizedBox(height: 8),
          Wrap(
            spacing: 8,
            runSpacing: 8,
            children: [
              OutlinedButton.icon(
                icon: const Icon(Icons.my_location),
                label: const Text('发一帧（固定坐标）'),
                onPressed: () => _guard(
                  () => _client.sendGnssFix(
                    debugGnssFix(
                      latitude: 31.2304,
                      longitude: 121.4737,
                      altitudeMeters: 12,
                      speedKmh: 24.5,
                      courseDeg: 90,
                    ),
                  ),
                ),
              ),
              FilledButton.icon(
                icon: Icon(_gnssStreaming ? Icons.stop : Icons.play_arrow),
                label: Text(_gnssStreaming ? '停止真实定位推送' : '推送真实定位 (1Hz)'),
                onPressed: _toggleGnssStreaming,
              ),
            ],
          ),
        ],
      );

  Widget _foregroundServiceCard() => _card(
        title: '前台服务',
        children: [
          const Text(
            '未开启时 App 切后台会被回收、连接断开。验收项：切后台 5 分钟连接不断。',
          ),
          const SizedBox(height: 8),
          Row(
            children: [
              Expanded(child: _kv('运行中', _fgsRunning ? '是' : '否')),
              Switch(
                value: _fgsRunning,
                onChanged: CompanionForegroundService.isSupported
                    ? (_) => _toggleForegroundService()
                    : null,
              ),
            ],
          ),
          if (!CompanionForegroundService.isSupported)
            const Text('当前平台不需要前台服务。'),
        ],
      );

  Widget _card({required String title, required List<Widget> children}) => Card(
        child: Padding(
          padding: const EdgeInsets.all(12),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Text(title, style: Theme.of(context).textTheme.titleMedium),
              const SizedBox(height: 8),
              ...children,
            ],
          ),
        ),
      );

  Widget _kv(String k, String v) => Padding(
        padding: const EdgeInsets.symmetric(vertical: 2),
        child: Row(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            SizedBox(
              width: 110,
              child: Text(k, style: const TextStyle(color: Colors.grey)),
            ),
            Expanded(
              child: Text(
                v,
                style: const TextStyle(fontFamily: 'monospace'),
                maxLines: 4,
                overflow: TextOverflow.ellipsis,
              ),
            ),
          ],
        ),
      );

  List<Widget> _flagChips(int flags) {
    const names = {
      CompanionStatusFlag.recording: 'RECORDING',
      CompanionStatusFlag.gpsPhone: 'GPS_PHONE',
      CompanionStatusFlag.gpsInternal: 'GPS_INTERNAL',
      CompanionStatusFlag.storageFull: 'STORAGE_FULL',
      CompanionStatusFlag.fsBusy: 'FS_BUSY',
      CompanionStatusFlag.usbMtpBusy: 'USB_MTP_BUSY',
      CompanionStatusFlag.moving: 'MOVING',
      CompanionStatusFlag.sensorHr: 'SENSOR_HR',
      CompanionStatusFlag.sensorCsc: 'SENSOR_CSC',
      CompanionStatusFlag.sensorCps: 'SENSOR_CPS',
    };

    final active = <Widget>[
      for (final entry in names.entries)
        if (flags & entry.key != 0)
          Chip(
            label: Text(entry.value),
            visualDensity: VisualDensity.compact,
          ),
    ];

    if (active.isEmpty) {
      active.add(
        const Chip(
          label: Text('无置位'),
          visualDensity: VisualDensity.compact,
        ),
      );
    }

    return active;
  }

  String _stateLabel(CompanionConnState s) => switch (s) {
        CompanionConnState.disconnected => '未连接',
        CompanionConnState.connecting => '连接中…',
        CompanionConnState.discovering => '发现服务中…',
        CompanionConnState.ready => '就绪',
        CompanionConnState.versionMismatch => '协议版本不匹配',
        CompanionConnState.error => '出错',
      };
}

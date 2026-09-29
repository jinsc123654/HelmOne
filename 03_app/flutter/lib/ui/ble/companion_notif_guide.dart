import 'dart:async';

import 'package:flutter/material.dart';
import 'package:sifli_companion/app/app_keys.dart';
import 'package:sifli_companion/app/app_theme.dart';
import 'package:sifli_companion/ble/companion_client.dart';
import 'package:sifli_companion/ble/companion_notif_relay.dart';

/// 通知使用权引导：打开系统设置 → 返回后自动重绑。不依赖 adb。
class CompanionNotifGuideCard extends StatefulWidget {
  /// 创建引导卡片。
  const CompanionNotifGuideCard({super.key});

  @override
  State<CompanionNotifGuideCard> createState() => _CompanionNotifGuideCardState();
}

class _CompanionNotifGuideCardState extends State<CompanionNotifGuideCard>
    with WidgetsBindingObserver {
  final _relay = CompanionNotifRelay.instance;

  NotifAccessState _access = NotifAccessState.needsPermission;
  bool _forward = true;
  bool _busy = false;
  String _hint = '';
  Timer? _heardTick;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    _heardTick = Timer.periodic(const Duration(seconds: 1), (_) {
      if (mounted) setState(() {});
    });
    _refresh(fromResume: false);
  }

  @override
  void dispose() {
    _heardTick?.cancel();
    WidgetsBinding.instance.removeObserver(this);
    super.dispose();
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    if (state == AppLifecycleState.resumed) {
      _refresh(fromResume: true);
    }
  }

  Future<void> _refresh({required bool fromResume}) async {
    if (!mounted) return;
    setState(() => _busy = true);
    try {
      final pref = await _relay.loadEnabledPref();
      final access = await _relay.prepareAccess();
      if (!mounted) return;
      setState(() {
        _forward = pref;
        _access = access;
        _busy = false;
        _hint = switch (access) {
          NotifAccessState.unsupported => '当前平台没有系统通知监听。',
          NotifAccessState.needsPermission => fromResume
              ? '还没看到授权。请在列表里打开「Helm One」，然后返回。'
              : '需要在系统设置中打开通知使用权，App 无法自己勾选。',
          NotifAccessState.needsToggle =>
            '权限已开，但监听没连上（覆盖安装后常见）。请再进设置，把「Helm One」关掉再打开。',
          NotifAccessState.ready => fromResume
              ? '监听已就绪。发一条 QQ 或点「系统测试通知」。'
              : '监听已就绪。',
        };
      });
    } catch (e, st) {
      if (!mounted) return;
      setState(() {
        _busy = false;
        _hint = AppTheme.failText('NotifGuide', e, st);
      });
    }
  }

  Future<void> _goAuthorize() async {
    setState(() {
      _busy = true;
      _hint = '请在打开的页面中找到 Helm One 并打开开关，然后返回本页。';
    });
    await _relay.openSettings();
    if (mounted) setState(() => _busy = false);
  }

  Future<void> _setForward(bool v) async {
    await _relay.saveEnabledPref(v);
    if (v) {
      await _relay.start();
    } else {
      await _relay.stop();
    }
    if (mounted) setState(() => _forward = v);
  }

  @override
  Widget build(BuildContext context) {
    final ready = _access == NotifAccessState.ready;
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(12),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            Text('通知转发', style: Theme.of(context).textTheme.titleMedium),
            const SizedBox(height: 8),
            const Text(
              '把手机通知发到码表。授权只能在系统设置里打开，回来后 App 会自动连接监听，不用 adb。',
            ),
            const SizedBox(height: 12),
            _step(
              index: '1',
              title: '通知使用权',
              done: _access == NotifAccessState.ready ||
                  _access == NotifAccessState.needsToggle,
              detail: _access == NotifAccessState.needsPermission
                  ? '未授权'
                  : '已授权',
            ),
            _step(
              index: '2',
              title: '监听服务',
              done: ready,
              detail: ready ? '已绑定' : '未绑定',
            ),
            Row(
              children: [
                const Expanded(
                  child: Text('3. 转发到码表', style: TextStyle(fontSize: 14)),
                ),
                Switch(value: _forward, onChanged: _busy ? null : _setForward),
              ],
            ),
            const SizedBox(height: 8),
            if (_hint.isNotEmpty)
              Text(
                _hint,
                style: TextStyle(
                  color: ready
                      ? Theme.of(context).colorScheme.primary
                      : Theme.of(context).colorScheme.error,
                ),
              ),
            const SizedBox(height: 8),
            if (_busy) const LinearProgressIndicator(),
            const SizedBox(height: 8),
            if (!ready)
              FilledButton.icon(
                onPressed: _busy ? null : _goAuthorize,
                icon: const Icon(Icons.settings),
                label: Text(
                  _access == NotifAccessState.needsToggle
                      ? '去设置里关掉再打开'
                      : '去系统设置授权',
                ),
              ),
            if (!ready)
              TextButton(
                onPressed: _busy ? null : () => _refresh(fromResume: false),
                child: const Text('我已打开，重新检测'),
              ),
            _kv('最近一条', _relay.lastHeard),
            const SizedBox(height: 8),
            Wrap(
              spacing: 8,
              runSpacing: 8,
              children: [
                OutlinedButton(
                  onPressed: _relay.isSupported && !_busy
                      ? () => _relay.postTest()
                      : null,
                  child: const Text('系统测试通知'),
                ),
                FilledButton(
                  onPressed: CompanionClient.instance.isReady && !_busy
                      ? _sendDebug
                      : null,
                  child: const Text('直发一条到码表'),
                ),
              ],
            ),
            if (!CompanionClient.instance.isReady)
              const Padding(
                padding: EdgeInsets.only(top: 6),
                child: Text(
                  '「直发」要先扫描连接码表。授权只影响 QQ/微信等系统通知。',
                  style: TextStyle(fontSize: 12, color: Colors.black54),
                ),
              ),
          ],
        ),
      ),
    );
  }

  Future<void> _sendDebug() async {
    final pkg = await _relay.ownPackage();
    final usePkg = pkg.isEmpty ? AppKeys.androidPackage : pkg;
    final iconName = await _relay.cacheIconForPackage(
      packageName: usePkg,
      appName: AppKeys.appName,
    );
    await CompanionClient.instance.sendNotification(
      title: '微信',
      body: '测试：直接经 GATT 下发',
      appName: '调试',
      packageName: usePkg,
      iconFileName: iconName,
    );
  }

  Widget _step({
    required String index,
    required String title,
    required bool done,
    required String detail,
  }) {
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 4),
      child: Row(
        children: [
          Icon(
            done ? Icons.check_circle : Icons.radio_button_unchecked,
            size: 20,
            color: done ? Colors.green : Colors.grey,
          ),
          const SizedBox(width: 8),
          Expanded(child: Text('$index. $title')),
          Text(detail, style: const TextStyle(color: Colors.grey)),
        ],
      ),
    );
  }

  Widget _kv(String k, String v) => Padding(
        padding: const EdgeInsets.symmetric(vertical: 2),
        child: Row(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            SizedBox(
              width: 88,
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
}

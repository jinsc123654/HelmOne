import 'dart:async';
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:get/get.dart';
import 'package:sifli_companion/ble/companion_notif_relay.dart';
import 'package:sifli_companion/ble/companion_client.dart';
import 'package:sifli_companion/ble/companion_proto.dart';
import 'package:sifli_companion/ble/notif_icon_cache.dart';
import 'package:sifli_companion/ble/notif_settings.dart';
import 'package:sifli_companion/app/app_theme.dart';
import 'package:sifli_companion/i18n/locale_keys.dart';
import 'package:sifli_companion/transfer/transfer_center.dart';
import 'package:sifli_companion/ui/widgets/helm_chrome.dart';

/// 消息通知二级页（码表联调）：总开关、锁屏/骑行条件、同步或自定义应用。
class NotifSettingsPage extends StatefulWidget {
  /// 创建设置页。
  const NotifSettingsPage({super.key});

  @override
  State<NotifSettingsPage> createState() => _NotifSettingsPageState();
}

class _NotifSettingsPageState extends State<NotifSettingsPage>
    with WidgetsBindingObserver {
  final _settings = NotifSettings.instance;
  final _relay = CompanionNotifRelay.instance;
  final _search = TextEditingController();

  StreamSubscription<CompanionConnState>? _bleSub;
  NotifAccessState _access = NotifAccessState.needsPermission;
  final _icons = <String, Uint8List>{};
  List<InstalledNotifApp> _apps = const [];
  bool _ready = false;
  bool _bleReady = false;
  /// 服务发现里有没有 `0xFF17`。为 `false` 时安卓多半缓存了旧 GATT 表。
  bool _notifCharOk = false;
  bool _refreshingCache = false;
  bool _uploading = false;
  bool _uploadCancel = false;
  int _uploadDone = 0;
  int _uploadTotal = 0;

  /// 订阅全局任务表：上传可能在本页被销毁后还在跑，
  /// 重新进来时按钮必须反映真实状态，否则会一直卡在「取消」。
  StreamSubscription<void>? _taskSub;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    _bleReady = CompanionClient.instance.isReady;
    _notifCharOk = CompanionClient.instance.canRelayNotification;
    _bleSub = CompanionClient.instance.stateStream.listen((_) {
      if (mounted) {
        setState(() {
          _bleReady = CompanionClient.instance.isReady;
          _notifCharOk = CompanionClient.instance.canRelayNotification;
        });
      }
    });
    _uploading = TransferCenter.to.isKindRunning(_kind);
    _taskSub = TransferCenter.to.changes.listen((_) {
      if (!mounted) return;
      final running = TransferCenter.to.isKindRunning(_kind);
      if (running != _uploading) {
        setState(() => _uploading = running);
      }
    });
    _search.addListener(() {
      if (mounted) setState(() {});
    });
    _load();
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    // 不在这里置 _uploadCancel：需求要求退出页面后上传继续。
    // 取消只由用户显式点「取消」触发。
    _taskSub?.cancel();
    _bleSub?.cancel();
    _search.dispose();
    super.dispose();
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    if (state == AppLifecycleState.resumed) {
      _refreshAccess();
    }
  }

  Future<void> _load() async {
    await _settings.ensureLoaded();
    await _refreshAccess();
    var apps = const <InstalledNotifApp>[];
    try {
      apps = await _relay.listInstalledApps();
    } catch (_) {}
    await _settings.seedCustomFromInstalled(apps.map((e) => e.packageName));
    _sortApps(apps);
    if (mounted) {
      setState(() {
        _apps = apps;
        _ready = true;
      });
    }
    unawaited(_warmIcons(apps));
  }

  void _sortApps(List<InstalledNotifApp> apps) {
    apps.sort((a, b) {
      final ae = _settings.allowsPackage(a.packageName) ? 0 : 1;
      final be = _settings.allowsPackage(b.packageName) ? 0 : 1;
      if (ae != be) return ae - be;
      return a.appName.toLowerCase().compareTo(b.appName.toLowerCase());
    });
  }

  /// 已有缓存先上屏；缺的再向系统取 PNG，写入 `app_cache/notif_icons/`。
  Future<void> _warmIcons(List<InstalledNotifApp> apps) async {
    final cached = await NotifIconCache.instance.list();
    for (final e in cached) {
      if (e.packageName.isEmpty) continue;
      try {
        final bytes = await e.file.readAsBytes();
        if (bytes.isNotEmpty) _icons[e.packageName] = bytes;
      } catch (_) {}
    }
    if (mounted) setState(() {});

    var dirty = 0;
    for (final app in apps) {
      if (!mounted) return;
      if (_icons.containsKey(app.packageName)) continue;
      try {
        await _relay.cacheIconForPackage(
          packageName: app.packageName,
          appName: app.appName,
        );
        final bytes = await NotifIconCache.instance.readPng(
          CompanionNotif.iconFileName(app.packageName),
        );
        if (bytes == null || bytes.isEmpty) continue;
        _icons[app.packageName] = bytes;
        dirty++;
        if (dirty >= 10) {
          dirty = 0;
          if (mounted) setState(() {});
        }
      } catch (_) {}
    }
    if (mounted) setState(() {});
  }

  /// 手机侧 GATT 缓存陈旧时，清一次缓存重新发现服务。
  ///
  /// 不走重连：连接是好的，只是服务表是旧的。刷新成功且拿到 `0xFF17` 后，
  /// 通知转发立刻可用，不用等下一轮重连。
  Future<void> _refreshServiceCache() async {
    if (_refreshingCache) return;
    setState(() => _refreshingCache = true);
    try {
      final ok = await CompanionClient.instance.refreshGattCache();
      if (!mounted) return;
      setState(() => _notifCharOk = ok);
      AppTheme.snack(
        LocaleKeys.notifTitle.tr,
        ok
            ? LocaleKeys.notifStaleCacheOk.tr
            : LocaleKeys.notifStaleCacheStill.tr,
      );
    } catch (e) {
      if (!mounted) return;
      AppTheme.snack(LocaleKeys.notifTitle.tr, '$e');
    } finally {
      if (mounted) setState(() => _refreshingCache = false);
    }
  }

  Future<void> _uploadAllIcons() async {
    final center = TransferCenter.to;
    if (_uploading || center.isKindRunning(_kind)) {
      // 第二次点击 = 取消。走中心那条路，onCancel 会翻转 _uploadCancel。
      final t = center.runningOfKind(_kind);
      if (t != null) {
        center.cancel(t);
      } else {
        _uploadCancel = true;
      }
      return;
    }
    if (!_bleReady) {
      AppTheme.snack(
        LocaleKeys.notifTitle.tr,
        LocaleKeys.notifUploadNeedBle.tr,
      );
      return;
    }
    if (CompanionClient.instance.lastStatus?.isTransferBusy == true) {
      AppTheme.snack(
        LocaleKeys.notifTitle.tr,
        LocaleKeys.notifUploadMtpBusy.tr,
      );
      return;
    }

    _uploadCancel = false;
    setState(() {
      _uploading = true;
      _uploadDone = 0;
      _uploadTotal = 0;
    });
    final task = center.begin(
      title: LocaleKeys.notifTitle.tr,
      kind: _kind,
      cancellable: true,
      onCancel: () => _uploadCancel = true,
    );
    try {
      final result = await _relay.uploadCachedIconsToDevice(
        onProgress: (p) {
          center.progress(
            task,
            done: p.bytesDone,
            total: p.bytesTotal,
            transferred: p.bytesTransferred,
            phase: '${p.filesDone + 1}/${p.filesTotal}  ${p.name}',
          );
          if (!mounted) return;
          setState(() {
            _uploadDone = p.filesDone + 1;
            _uploadTotal = p.filesTotal;
          });
        },
        // 只看 _uploadCancel：退出页面不再中断上传。
        shouldStop: () => _uploadCancel,
      );
      if (result.stopped) {
        if (task.isRunning) center.cancel(task);
      } else {
        // 零图标时把「没有需要同步的图标」写进卡片 —— 否则悬浮球一闪而过，
        // 只留个 ✓，看不出是"同步完了"还是"压根没东西"。
        center.finish(
          task,
          message: (result.ok == 0 && result.fail == 0)
              ? LocaleKeys.notifUploadNone.tr
              : LocaleKeys.notifUploadDone.trParams({
                  'ok': '${result.ok}',
                  'fail': '${result.fail}',
                }),
        );
      }
      if (!mounted) return;
      if (result.ok == 0 && result.fail == 0 && !result.stopped) {
        AppTheme.snack(
          LocaleKeys.notifTitle.tr,
          LocaleKeys.notifUploadNone.tr,
        );
      } else {
        AppTheme.snack(
          LocaleKeys.notifTitle.tr,
          LocaleKeys.notifUploadDone.trParams({
            'ok': '${result.ok}',
            'fail': '${result.fail}',
          }),
        );
      }
    } catch (e, st) {
      center.fail(task, e);
      if (!mounted) return;
      final mtp = e is StateError && e.message == 'mtp';
      if (mtp) {
        AppTheme.snack(
          LocaleKeys.notifTitle.tr,
          LocaleKeys.notifUploadMtpBusy.tr,
        );
      } else {
        AppTheme.fail(LocaleKeys.notifTitle.tr, 'NotifUpload', e, st);
      }
    } finally {
      if (mounted) {
        setState(() => _uploading = false);
      }
    }
  }

  /// 任务类别（跨页面查重入用）。
  static const _kind = 'notif_icons';

  Future<void> _refreshAccess() async {
    final access = await _relay.prepareAccess();
    if (mounted) setState(() => _access = access);
  }

  Future<void> _setEnabled(bool v) async {
    await _settings.setEnabled(v);
    if (v) {
      await _relay.start();
      await _refreshAccess();
    } else {
      await _relay.stop();
    }
    if (mounted) setState(() {});
  }

  Future<void> _showHelp() async {
    await Get.dialog<void>(
      AlertDialog(
        title: Text(LocaleKeys.notifHelpTitle.tr),
        content: SingleChildScrollView(
          child: Text(LocaleKeys.notifHelpBody.tr),
        ),
        actions: [
          TextButton(
            onPressed: () => Get.back<void>(),
            child: Text(LocaleKeys.cancel.tr),
          ),
          FilledButton(
            onPressed: () async {
              Get.back<void>();
              await _relay.openSettings();
            },
            child: Text(LocaleKeys.notifGoAuthorize.tr),
          ),
        ],
      ),
    );
  }

  List<InstalledNotifApp> get _visibleApps {
    final q = _search.text.trim().toLowerCase();
    if (q.isEmpty) return _apps;
    return _apps
        .where(
          (a) =>
              a.appName.toLowerCase().contains(q) ||
              a.packageName.toLowerCase().contains(q),
        )
        .toList();
  }

  @override
  Widget build(BuildContext context) {
    final visible = _visibleApps;
    return Scaffold(
      appBar: AppBar(title: Text(LocaleKeys.notifTitle.tr)),
      body: !_ready
          ? const Center(child: CircularProgressIndicator())
          : ListView(
              padding: const EdgeInsets.fromLTRB(16, 8, 16, 24),
              children: [
                if (_access == NotifAccessState.needsPermission ||
                    _access == NotifAccessState.needsToggle) ...[
                  HelmCard(
                    padding: const EdgeInsets.fromLTRB(16, 14, 12, 14),
                    child: Row(
                      children: [
                        const HelmIconBadge(
                          color: AppTheme.iconOta,
                          icon: Icons.info_outline,
                        ),
                        const SizedBox(width: 12),
                        Expanded(
                          child: Text(
                            _access == NotifAccessState.needsToggle
                                ? LocaleKeys.notifNeedToggle.tr
                                : LocaleKeys.notifNeedPermission.tr,
                          ),
                        ),
                        FilledButton(
                          onPressed: _relay.openSettings,
                          child: Text(LocaleKeys.notifGoAuthorize.tr),
                        ),
                      ],
                    ),
                  ),
                  const SizedBox(height: 12),
                ],
                // 连接正常但没有 0xFF17：安卓缓存了旧服务表，给个就地补救的入口。
                if (_bleReady && !_notifCharOk) ...[
                  HelmCard(
                    padding: const EdgeInsets.fromLTRB(16, 14, 12, 14),
                    child: Row(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        const HelmIconBadge(
                          color: AppTheme.danger,
                          icon: Icons.bluetooth_disabled,
                        ),
                        const SizedBox(width: 12),
                        Expanded(
                          child: Column(
                            crossAxisAlignment: CrossAxisAlignment.start,
                            children: [
                              Text(
                                LocaleKeys.notifStaleCacheTitle.tr,
                                style: const TextStyle(
                                  fontWeight: FontWeight.w600,
                                ),
                              ),
                              const SizedBox(height: 4),
                              Text(
                                LocaleKeys.notifStaleCacheBody.tr,
                                style: const TextStyle(
                                  color: AppTheme.muted,
                                  fontSize: 13,
                                ),
                              ),
                              const SizedBox(height: 10),
                              Align(
                                alignment: Alignment.centerRight,
                                child: FilledButton(
                                  onPressed:
                                      _refreshingCache ? null : _refreshServiceCache,
                                  child: _refreshingCache
                                      ? const SizedBox(
                                          width: 16,
                                          height: 16,
                                          child: CircularProgressIndicator(
                                            strokeWidth: 2,
                                          ),
                                        )
                                      : Text(
                                          LocaleKeys.notifStaleCacheButton.tr,
                                        ),
                                ),
                              ),
                            ],
                          ),
                        ),
                      ],
                    ),
                  ),
                  const SizedBox(height: 12),
                ],
                HelmCard(
                  child: HelmTileGroup(
                    children: [
                      HelmSwitchTile(
                        color: AppTheme.iconNotif,
                        icon: Icons.notifications_outlined,
                        title: LocaleKeys.notifMaster.tr,
                        value: _settings.enabled,
                        onChanged: _setEnabled,
                        subtitleWidget: GestureDetector(
                          onTap: _showHelp,
                          child: Text.rich(
                            TextSpan(
                              text: '${LocaleKeys.notifHelpHint.tr} ',
                              style: const TextStyle(
                                color: AppTheme.muted,
                                fontSize: 13,
                              ),
                              children: [
                                TextSpan(
                                  text: LocaleKeys.notifHelpLink.tr,
                                  style: const TextStyle(
                                    color: AppTheme.accent,
                                    decoration: TextDecoration.underline,
                                  ),
                                ),
                              ],
                            ),
                          ),
                        ),
                      ),
                      HelmSwitchTile(
                        color: AppTheme.iconKeep,
                        icon: Icons.lock_outline,
                        title: LocaleKeys.notifLockOnly.tr,
                        subtitle: LocaleKeys.notifLockOnlySub.tr,
                        value: _settings.lockScreenOnly,
                        onChanged: _settings.enabled
                            ? (v) async {
                                await _settings.setLockScreenOnly(v);
                                if (mounted) setState(() {});
                              }
                            : null,
                      ),
                      HelmSwitchTile(
                        color: AppTheme.iconRide,
                        icon: Icons.pedal_bike_outlined,
                        title: LocaleKeys.notifRideOnly.tr,
                        subtitle: LocaleKeys.notifRideOnlySub.tr,
                        value: _settings.rideOnly,
                        onChanged: _settings.enabled
                            ? (v) async {
                                await _settings.setRideOnly(v);
                                if (mounted) setState(() {});
                              }
                            : null,
                      ),
                      HelmNavTile(
                        color: AppTheme.iconFile,
                        icon: Icons.upload_file,
                        title: LocaleKeys.notifUploadIcons.tr,
                        subtitle: _uploading && _uploadTotal > 0
                            ? LocaleKeys.notifUploadProgress.trParams({
                                'done': '$_uploadDone',
                                'total': '$_uploadTotal',
                              })
                            : !_bleReady
                                ? LocaleKeys.notifUploadNeedBle.tr
                                : LocaleKeys.notifUploadIconsSub.tr,
                        onTap: _uploading || !_bleReady
                            ? null
                            : _uploadAllIcons,
                        showChevron: !_uploading,
                        leading: HelmIconBadge(
                          color: AppTheme.iconFile,
                          icon: Icons.upload_file,
                          child: _uploading
                              ? const SizedBox(
                                  width: 16,
                                  height: 16,
                                  child: CircularProgressIndicator(
                                    strokeWidth: 2,
                                    color: Colors.white,
                                  ),
                                )
                              : null,
                        ),
                        trailing: _uploading
                            ? TextButton(
                                onPressed: () => _uploadCancel = true,
                                child: Text(LocaleKeys.cancel.tr),
                              )
                            : null,
                      ),
                    ],
                  ),
                ),
                Padding(
                  padding: const EdgeInsets.fromLTRB(4, 20, 4, 8),
                  child: Text(
                    LocaleKeys.notifModeHint.tr,
                    style: const TextStyle(
                      color: AppTheme.muted,
                      fontSize: 13,
                    ),
                  ),
                ),
                HelmCard(
                  child: RadioGroup<bool>(
                    groupValue: _settings.customMode,
                    onChanged: (v) async {
                      if (!_settings.enabled || v == null) return;
                      await _settings.setCustomMode(v);
                      if (mounted) setState(() {});
                    },
                    child: Column(
                      children: [
                        RadioListTile<bool>(
                          title: Text(LocaleKeys.notifModeSync.tr),
                          value: false,
                        ),
                        const HelmHairline(indent: 16),
                        RadioListTile<bool>(
                          title: Text(LocaleKeys.notifModeCustom.tr),
                          value: true,
                        ),
                      ],
                    ),
                  ),
                ),
                if (_settings.customMode) ...[
                  const SizedBox(height: 12),
                  TextField(
                    controller: _search,
                    enabled: _settings.enabled,
                    decoration: InputDecoration(
                      prefixIcon: const Icon(Icons.search),
                      hintText: LocaleKeys.notifAppSearch.tr,
                      isDense: true,
                    ),
                  ),
                  const SizedBox(height: 12),
                  if (visible.isEmpty)
                    HelmCard(
                      padding: const EdgeInsets.all(20),
                      child: Text(
                        LocaleKeys.notifAppEmpty.tr,
                        textAlign: TextAlign.center,
                        style: const TextStyle(color: AppTheme.muted),
                      ),
                    )
                  else
                    HelmCard(
                      child: HelmTileGroup(
                        children: [
                          for (final app in visible)
                            _appTile(
                              title: app.appName,
                              pkg: app.packageName,
                            ),
                        ],
                      ),
                    ),
                ],
              ],
            ),
    );
  }

  Widget _appTile({required String title, required String pkg}) {
    final icon = _icons[pkg];
    final on = _settings.allowsPackage(pkg);
    return Padding(
      padding: const EdgeInsets.fromLTRB(16, 6, 8, 6),
      child: Row(
        children: [
          CircleAvatar(
            radius: 18,
            backgroundColor: AppTheme.field,
            backgroundImage: icon != null ? MemoryImage(icon) : null,
            child: icon == null
                ? Text(
                    title.isNotEmpty ? title[0] : '?',
                    style: const TextStyle(fontSize: 14),
                  )
                : null,
          ),
          const SizedBox(width: 12),
          Expanded(
            child: Text(
              title,
              style: const TextStyle(color: Colors.white, fontSize: 16),
            ),
          ),
          Switch(
            value: on,
            onChanged: _settings.enabled
                ? (v) async {
                    await _settings.setPackageEnabled(pkg, v);
                    if (mounted) setState(() {});
                  }
                : null,
          ),
        ],
      ),
    );
  }
}

import 'dart:async';

import 'package:flutter/material.dart';
import 'package:get/get.dart';
import 'package:sifli_companion/app/app_assets.dart';
import 'package:sifli_companion/app/app_routes.dart';
import 'package:sifli_companion/app/app_theme.dart';
import 'package:sifli_companion/ble/companion_client.dart';
import 'package:sifli_companion/ble/companion_proto.dart';
import 'package:sifli_companion/gnss/eph_auto_sync.dart';
import 'package:sifli_companion/i18n/locale_keys.dart';
import 'package:sifli_companion/ride/mcu_sync.dart';
import 'package:sifli_companion/transfer/transfer_center.dart';
import 'package:sifli_companion/ui/shell/shell_controller.dart';
import 'package:sifli_companion/ui/widgets/helm_chrome.dart';
import 'package:sifli_companion/ui/widgets/helm_rings.dart';

/// 同步阶段文案：页面内的计量条和全局悬浮框共用一套，避免两处漂移。
String _syncPhaseLabel(McuSyncPhase? p) => switch (p) {
      McuSyncPhase.listing => LocaleKeys.deviceSyncList.tr,
      McuSyncPhase.pull => LocaleKeys.deviceSyncPull.tr,
      McuSyncPhase.pushFav || McuSyncPhase.pushNav =>
        LocaleKeys.deviceSyncPush.tr,
      _ => LocaleKeys.deviceSyncing.tr,
    };

class DevicePage extends GetView<ShellController> {
  /// 创建设备页。
  const DevicePage({super.key});

  /// 任务类别（跨页面查重入用）。
  static const _syncKind = 'mcu_sync';

  /// 未绑定才进扫描；已绑定只能回连，换机走「重新绑定码表」。
  Future<void> _scan() async {
    if (controller.hasBond.value) {
      AppTheme.snack(
        LocaleKeys.bleSnackbarTitle.tr,
        LocaleKeys.rebindNeedUnbind.tr,
      );
      return;
    }
    await Get.toNamed(AppRoutes.bleScan);
    if (controller.isReady) await controller.onConnected();
  }

  /// 主按钮：已绑定 → 静默回连；未绑定 → 扫描配对。
  Future<void> _connectOrScan() async {
    if (controller.hasBond.value) {
      await controller.retryConnect();
      return;
    }
    await _scan();
  }

  /// 请码表开配对窗口 30 s（之后别的手机才能扫到它并配对）。
  ///
  /// **2026-09-20 起 app 里不再有入口**：没配对的时候码表根本不广播，app 也连不上它，
  /// 这个按钮在"真的要配一台新手机"时反而按不动；配对窗口只能（也应该）在码表上开，
  /// 页面上改成 [pairGuide] 那几张引导卡。协议方法留着，将来要做"已连接的设备上再配一台
  /// 备用手机"可以直接用。
  Future<void> _sync() async {
    if (!controller.isReady) {
      AppTheme.snack(
        LocaleKeys.bleSnackbarTitle.tr,
        LocaleKeys.deviceNeedConnect.tr,
      );
      return;
    }
    final st = controller.status.value;
    if (st != null && st.isTransferBusy) {
      AppTheme.snack(
        LocaleKeys.deviceSync.tr,
        LocaleKeys.deviceMtpBusy.tr,
      );
      return;
    }
    final center = TransferCenter.to;
    if (center.isKindRunning(_syncKind)) return;

    controller.syncing.value = true;
    controller.syncProg.value =
        const McuSyncProgress(phase: McuSyncPhase.listing);
    controller.syncKbs.value = 0;

    final task = center.begin(
      title: LocaleKeys.deviceSync.tr,
      kind: _syncKind,
    );

    try {
      final n = await McuSync.pullRideRecords(
        controller.client,
        onProgress: (p) {
          // 页面内的计量条（ShellController）和全局悬浮框各要一份。
          controller.applySyncProgress(p);
          center.progress(
            task,
            done: p.done,
            total: p.total,
            transferred: p.transferred,
            phase: _syncPhaseLabel(p.phase),
          );
        },
      );
      center.finish(
        task,
        message: n > 0
            ? LocaleKeys.syncDone.trParams({'n': '$n'})
            : LocaleKeys.syncNone.tr,
      );
      controller.bumpRides();
      AppTheme.snack(
        LocaleKeys.deviceSync.tr,
        n > 0
            ? LocaleKeys.syncDone.trParams({'n': '$n'})
            : LocaleKeys.syncNone.tr,
      );
    } catch (e, st) {
      center.fail(task, e);
      AppTheme.fail(LocaleKeys.deviceSync.tr, 'DeviceSync', e, st);
    } finally {
      controller.clearSyncProgress();
    }
  }

  void _openDevice(String route) {
    if (!controller.isReady) {
      AppTheme.snack(
        LocaleKeys.bleSnackbarTitle.tr,
        LocaleKeys.deviceNeedConnect.tr,
      );
      return;
    }
    Get.toNamed(route);
  }

  @override
  Widget build(BuildContext context) {
    return SafeArea(
      child: Obx(() {
        final ready = controller.isReady;
        final mismatch =
            controller.conn.value == CompanionConnState.versionMismatch;
        final bound = controller.hasBond.value;
        final connecting = controller.conn.value ==
                CompanionConnState.connecting ||
            controller.conn.value == CompanionConnState.discovering ||
            (bound &&
                !ready &&
                !mismatch &&
                controller.linkError.value == null &&
                controller.client.autoReconnect);
        final st = controller.status.value;
        final name = controller.deviceName.isEmpty
            ? LocaleKeys.deviceNoName.tr
            : controller.deviceName;
        final hasBatt = st != null && st.hasBattery;

        final failed =
            !ready && !connecting && controller.linkError.value != null;
        String statusLine;
        if (mismatch) {
          statusLine = LocaleKeys.deviceMismatch.tr;
        } else if (connecting) {
          statusLine = bound
              ? LocaleKeys.deviceReconnecting.tr
              : LocaleKeys.deviceConnecting.tr;
        } else if (ready) {
          statusLine = LocaleKeys.deviceConnected.tr;
        } else if (failed) {
          statusLine = controller.linkError.value!;
        } else if (bound) {
          statusLine = LocaleKeys.deviceDisconnected.tr;
        } else {
          statusLine = LocaleKeys.deviceDisconnected.tr;
        }

        String? detailLine;
        if (ready && hasBatt) {
          detailLine = LocaleKeys.deviceBattery.trParams({
            'pct': '${st.batteryPct}',
          });
        }

        final actionLabel = controller.syncing.value
            ? LocaleKeys.deviceSyncing.tr
            : (ready
                ? LocaleKeys.deviceSync.tr
                : (connecting
                    ? LocaleKeys.deviceConnecting.tr
                    : (bound
                        ? LocaleKeys.deviceRetry.tr
                        : LocaleKeys.deviceConnect.tr)));

        return ListView(
          padding: const EdgeInsets.only(bottom: 24),
          children: [
            HelmTitleRow(
              title: LocaleKeys.tabDevice.tr,
              // 已绑定禁止再扫：换机只能走「重新绑定码表」。
              trailing: bound
                  ? null
                  : IconButton(
                      onPressed: _scan,
                      tooltip: LocaleKeys.add.tr,
                      icon: const Icon(
                        Icons.add_circle_outline,
                        color: Colors.white,
                      ),
                    ),
            ),
            Padding(
              padding: const EdgeInsets.fromLTRB(12, 8, 20, 8),
              child: Row(
                crossAxisAlignment: CrossAxisAlignment.center,
                children: [
                  HelmPulseGlow(
                    active: ready,
                    intensity: HelmPulseGlow.deviceGlowIntensity,
                    child: Image.asset(
                      AppAssets.bikeComputer,
                      width: 132,
                      height: 132,
                      filterQuality: FilterQuality.high,
                    ),
                  ),
                  const SizedBox(width: 8),
                  Expanded(
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        Text(
                          name,
                          maxLines: 2,
                          overflow: TextOverflow.ellipsis,
                          style: const TextStyle(
                            color: Colors.white,
                            fontSize: 18,
                            fontWeight: FontWeight.w600,
                            height: 1.25,
                          ),
                        ),
                        const SizedBox(height: 6),
                        AnimatedSwitcher(
                          duration: const Duration(milliseconds: 240),
                          // 只做位移：新状态从下方顶上来，旧的往上走。
                          transitionBuilder: (child, anim) => ClipRect(
                            child: SlideTransition(
                              position: Tween<Offset>(
                                begin: const Offset(0, 1),
                                end: Offset.zero,
                              ).animate(
                                CurvedAnimation(
                                  parent: anim,
                                  curve: Curves.easeOutCubic,
                                ),
                              ),
                              child: child,
                            ),
                          ),
                          child: Text(
                            statusLine,
                            key: ValueKey(statusLine),
                            style: TextStyle(
                              color: failed ? AppTheme.danger : AppTheme.muted,
                              fontSize: 13,
                            ),
                          ),
                        ),
                        if (detailLine != null) ...[
                          const SizedBox(height: 4),
                          Text(
                            detailLine,
                            maxLines: 2,
                            overflow: TextOverflow.ellipsis,
                            style: const TextStyle(
                              color: AppTheme.muted,
                              fontSize: 13,
                              height: 1.35,
                            ),
                          ),
                        ],
                        const SizedBox(height: 12),
                        FilledButton(
                          onPressed: controller.syncing.value || connecting
                              ? null
                              : (ready ? _sync : _connectOrScan),
                          style: FilledButton.styleFrom(
                            backgroundColor: AppTheme.sync,
                            foregroundColor: Colors.white,
                            disabledBackgroundColor:
                                AppTheme.sync.withValues(alpha: 0.55),
                            padding: const EdgeInsets.symmetric(
                              horizontal: 22,
                              vertical: 8,
                            ),
                            minimumSize: const Size(0, 36),
                            tapTargetSize: MaterialTapTargetSize.shrinkWrap,
                            shape: const StadiumBorder(),
                          ),
                          child: Text(actionLabel),
                        ),
                      ],
                    ),
                  ),
                ],
              ),
            ),
            if (controller.syncing.value)
              Padding(
                padding: const EdgeInsets.fromLTRB(16, 8, 16, 0),
                child: HelmCard(
                  padding: const EdgeInsets.fromLTRB(16, 14, 16, 14),
                  child: _syncMeter(
                    controller.syncProg.value,
                    controller.syncKbs.value,
                  ),
                ),
              ),
            if (!ready && !connecting && !bound) ...[
              Padding(
                padding: const EdgeInsets.fromLTRB(20, 4, 20, 0),
                child: Text(
                  LocaleKeys.deviceIdleHint.tr,
                  style: const TextStyle(
                    color: AppTheme.muted,
                    fontSize: 13,
                    height: 1.45,
                  ),
                ),
              ),
              // 仅未绑定时展示配对引导；已绑定后去「重新绑定码表」页看。
              Padding(
                padding: const EdgeInsets.fromLTRB(16, 10, 16, 0),
                child: HelmCard(
                  padding: const EdgeInsets.fromLTRB(16, 14, 16, 14),
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Row(
                        children: [
                          const Icon(
                            Icons.bluetooth,
                            size: 18,
                            color: AppTheme.iconBle,
                          ),
                          const SizedBox(width: 8),
                          Text(
                            LocaleKeys.pairGuideTitle.tr,
                            style: const TextStyle(
                              color: Colors.white,
                              fontSize: 14,
                              fontWeight: FontWeight.w600,
                            ),
                          ),
                        ],
                      ),
                      const SizedBox(height: 10),
                      _guideStep(1, LocaleKeys.pairGuideStep1.tr),
                      _guideStep(2, LocaleKeys.pairGuideStep2.tr),
                      _guideStep(3, LocaleKeys.pairGuideStep3.tr),
                      const SizedBox(height: 8),
                      Text(
                        LocaleKeys.pairGuideNote.tr,
                        style: const TextStyle(
                          color: AppTheme.muted,
                          fontSize: 12,
                          height: 1.4,
                        ),
                      ),
                    ],
                  ),
                ),
              ),
            ],
            if (!ready && bound && !connecting)
              Padding(
                padding: const EdgeInsets.fromLTRB(20, 4, 20, 0),
                child: Text(
                  LocaleKeys.deviceBoundHint.tr,
                  style: const TextStyle(
                    color: AppTheme.muted,
                    fontSize: 13,
                    height: 1.45,
                  ),
                ),
              ),
            if (ready && st != null && _hasStatusChips(st)) ...[
              const SizedBox(height: 8),
              Padding(
                padding: const EdgeInsets.symmetric(horizontal: 16),
                child: HelmCard(
                  child: Padding(
                    padding: const EdgeInsets.fromLTRB(16, 14, 16, 14),
                    child: Wrap(
                      spacing: 8,
                      runSpacing: 8,
                      children: [
                        if (st.isRecording)
                          _chip(
                            LocaleKeys.deviceRecording.tr,
                            AppTheme.ringOrange,
                          ),
                        if (st.isMoving)
                          _chip(
                            LocaleKeys.deviceMoving.tr,
                            AppTheme.iconRide,
                          ),
                        if (st.isRecording && !st.isMoving)
                          _chip(
                            LocaleKeys.devicePaused.tr,
                            AppTheme.muted,
                          ),
                        if (st.isUsingPhoneGnss)
                          _chip(LocaleKeys.deviceGpsPhone.tr, AppTheme.iconNav),
                        if (st.flags & CompanionStatusFlag.gpsInternal != 0)
                          _chip(
                            LocaleKeys.deviceGpsInternal.tr,
                            AppTheme.iconRide,
                          ),
                        if (st.flags & CompanionStatusFlag.sensorHr != 0)
                          _chip(LocaleKeys.deviceHr.tr, const Color(0xFFDC2626)),
                        if (st.flags & CompanionStatusFlag.sensorCsc != 0)
                          _chip(
                            LocaleKeys.deviceCadence.tr,
                            AppTheme.iconRide,
                          ),
                        if (st.flags & CompanionStatusFlag.sensorCps != 0)
                          _chip(LocaleKeys.devicePower.tr, AppTheme.iconFav),
                        if (st.isTransferBusy)
                          _chip(LocaleKeys.deviceMtpBusy.tr, AppTheme.muted),
                      ],
                    ),
                  ),
                ),
              ),
            ],
            const SizedBox(height: 16),
            Padding(
              padding: const EdgeInsets.symmetric(horizontal: 16),
              child: HelmCard(
                child: HelmTileGroup(
                  children: [
                    HelmNavTile(
                      color: AppTheme.iconNotif,
                      icon: Icons.notifications_outlined,
                      title: LocaleKeys.settingsNotif.tr,
                      onTap: () => Get.toNamed(AppRoutes.notifSettings),
                    ),
                    HelmNavTile(
                      color: AppTheme.iconOta,
                      icon: Icons.system_update_alt,
                      title: LocaleKeys.settingsOta.tr,
                      onTap: () => Get.toNamed(AppRoutes.companionOta),
                    ),
                    Obx(
                      () => HelmNavTile(
                        color: AppTheme.iconEph,
                        icon: Icons.satellite_alt_outlined,
                        title: LocaleKeys.settingsEph.tr,
                        // 「下次同步」在设备页也要能看到，不用点进星历页。
                        subtitle: EphAutoSync.instance.status.value.label,
                        onTap: () => _openDevice(AppRoutes.companionEph),
                      ),
                    ),
                    HelmNavTile(
                      color: AppTheme.iconCoredump,
                      icon: Icons.bug_report_outlined,
                      title: LocaleKeys.settingsCoredump.tr,
                      subtitle: LocaleKeys.settingsCoredumpSub.tr,
                      onTap: () => Get.toNamed(AppRoutes.companionCoredump),
                    ),
                    HelmNavTile(
                      color: AppTheme.iconBle,
                      icon: Icons.bluetooth_disabled_outlined,
                      title: LocaleKeys.rebindTitle.tr,
                      subtitle: LocaleKeys.rebindSub.tr,
                      onTap: () => Get.toNamed(AppRoutes.deviceRebind),
                    ),
                    HelmNavTile(
                      color: AppTheme.iconUsb,
                      icon: Icons.usb,
                      title: LocaleKeys.settingsMapUsb.tr,
                      onTap: () => Get.toNamed(AppRoutes.mapUsb),
                    ),
                  ],
                ),
              ),
            ),
            if (ready) ...[
              const SizedBox(height: 16),
              Padding(
                padding: const EdgeInsets.symmetric(horizontal: 16),
                child: HelmCard(
                  child: InkWell(
                    onTap: controller.disconnect,
                    child: Padding(
                      padding: const EdgeInsets.symmetric(vertical: 16),
                      child: Text(
                        LocaleKeys.deviceDisconnect.tr,
                        textAlign: TextAlign.center,
                        style: const TextStyle(
                          color: Color(0xFFEF4444),
                          fontSize: 16,
                          fontWeight: FontWeight.w600,
                        ),
                      ),
                    ),
                  ),
                ),
              ),
            ],
          ],
        );
      }),
    );
  }

  /// 引导步骤：数字圈 + 文案（配对引导卡用）。
  Widget _guideStep(int n, String text) {
    return Padding(
      padding: const EdgeInsets.only(bottom: 6),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Container(
            width: 18,
            height: 18,
            alignment: Alignment.center,
            decoration: const BoxDecoration(
              color: AppTheme.field,
              shape: BoxShape.circle,
            ),
            child: Text(
              '$n',
              style: const TextStyle(
                color: Colors.white,
                fontSize: 11,
                fontWeight: FontWeight.w600,
              ),
            ),
          ),
          const SizedBox(width: 8),
          Expanded(
            child: Text(
              text,
              style: const TextStyle(
                color: Colors.white70,
                fontSize: 13,
                height: 1.45,
              ),
            ),
          ),
        ],
      ),
    );
  }

  Widget _syncMeter(McuSyncProgress? prog, double kbs) {    final total = prog?.total ?? 0;
    final done = prog?.done ?? 0;
    final has = total > 0;
    final frac = has ? (done / total).clamp(0.0, 1.0) : null;
    final phase = _syncPhaseLabel(prog?.phase);
    final speed = kbs <= 0
        ? ''
        : LocaleKeys.deviceSyncSpeed.trParams({
            'n': kbs >= 100 ? kbs.toStringAsFixed(0) : kbs.toStringAsFixed(1),
          });
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Text(
          phase,
          style: const TextStyle(
            color: Colors.white,
            fontSize: 13,
            fontWeight: FontWeight.w600,
          ),
        ),
        const SizedBox(height: 10),
        ClipRRect(
          borderRadius: BorderRadius.circular(4),
          child: LinearProgressIndicator(
            value: frac,
            minHeight: 6,
            color: AppTheme.ringOrange,
            backgroundColor: AppTheme.field,
          ),
        ),
        const SizedBox(height: 8),
        Text(
          [
            if (has)
              LocaleKeys.deviceSyncBytes.trParams({
                'done': _fmtBytes(done),
                'total': _fmtBytes(total),
              }),
            if (speed.isNotEmpty) speed,
          ].join('  ·  '),
          style: const TextStyle(color: AppTheme.muted, fontSize: 12),
        ),
      ],
    );
  }

  String _fmtBytes(int n) {
    if (n < 1024) return '$n B';
    if (n < 1024 * 1024) return '${(n / 1024).toStringAsFixed(1)} KB';
    return '${(n / (1024 * 1024)).toStringAsFixed(1)} MB';
  }

  bool _hasStatusChips(CompanionStatus st) {
    return st.isRecording ||
        st.isMoving ||
        st.isUsingPhoneGnss ||
        st.isTransferBusy ||
        (st.flags & CompanionStatusFlag.gpsInternal) != 0 ||
        (st.flags & CompanionStatusFlag.sensorHr) != 0 ||
        (st.flags & CompanionStatusFlag.sensorCsc) != 0 ||
        (st.flags & CompanionStatusFlag.sensorCps) != 0;
  }

  Widget _chip(String label, Color color) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
      decoration: BoxDecoration(
        color: color.withValues(alpha: 0.18),
        borderRadius: BorderRadius.circular(20),
      ),
      child: Text(
        label,
        style: TextStyle(
          color: color,
          fontSize: 12,
          fontWeight: FontWeight.w600,
        ),
      ),
    );
  }
}

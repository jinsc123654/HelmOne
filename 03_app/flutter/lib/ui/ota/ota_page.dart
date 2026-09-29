import 'dart:async';

import 'package:flutter/material.dart';
import 'package:get/get.dart';
import 'package:sifli_companion/app/app_theme.dart';
import 'package:sifli_companion/ble/companion_client.dart';
import 'package:sifli_companion/ble/companion_foreground_service.dart';
import 'package:sifli_companion/ble/companion_proto.dart';
import 'package:sifli_companion/i18n/locale_keys.dart';
import 'package:sifli_companion/ota/ota_config.dart';
import 'package:sifli_companion/ota/ota_manifest.dart';
import 'package:sifli_companion/ota/ota_service.dart';
import 'package:sifli_companion/transfer/transfer_center.dart';
import 'package:sifli_companion/ui/widgets/helm_center_action.dart';
import 'package:sifli_companion/ui/widgets/helm_peek_sheet.dart';

/// 码表固件 OTA：拉云端清单、校验后经 BLE 写入设备。
class OtaPage extends StatefulWidget {
  /// 创建固件升级页。
  const OtaPage({super.key});

  @override
  State<OtaPage> createState() => _OtaPageState();
}

enum _OtaPhase { idle, checking, downloading, verifying, uploading, done }

class _OtaPageState extends State<OtaPage> with SingleTickerProviderStateMixin {
  final _client = CompanionClient.instance;
  final _ota = OtaService();

  StreamSubscription<CompanionConnState>? _stateSub;
  _OtaPhase _phase = _OtaPhase.idle;
  String? _error;
  OtaManifest? _manifest;
  List<CompanionFsEntry> _slot = [];
  bool _cached = false;
  int _done = 0;
  int _total = 0;
  bool _cancel = false;
  late final AnimationController _sheet;

  bool get _busy =>
      _phase == _OtaPhase.checking ||
      _phase == _OtaPhase.downloading ||
      _phase == _OtaPhase.verifying ||
      _phase == _OtaPhase.uploading;

  bool get _ready => _client.isReady;

  String? get _deviceVersion {
    final info = _client.lastDevInfo;
    if (info != null && info.swVersion.isNotEmpty) {
      return info.swVersion;
    }
    final name = OtaVersion.highestLabel(_slot.map((e) => e.name));
    if (name == null) return null;
    return OtaVersion.parseOrZero(name).toString();
  }

  bool get _updateAvailable {
    final m = _manifest;
    if (m == null) return false;
    final cur = _deviceVersion;
    if (cur == null) return true;
    return OtaVersion.parseOrZero(m.version) >
        OtaVersion.parseOrZero(cur);
  }

  @override
  void initState() {
    super.initState();
    _sheet = AnimationController(
      vsync: this,
      duration: const Duration(milliseconds: 280),
    );
    _stateSub = _client.stateStream.listen((_) {
      if (mounted) setState(() {});
    });
    unawaited(_bootstrap());
  }

  @override
  void dispose() {
    // 注意：**不**在这里置 _cancel，也**不**无条件关 http client。
    // 需求要求退出页面后抓取继续跑，而 _http.close(force: true) 会当场
    // 掐断在途下载。只有在没有传输时才回收 client。
    if (!TransferCenter.to.isKindRunning(_kind)) {
      _ota.close();
    }
    _sheet.dispose();
    _stateSub?.cancel();
    super.dispose();
  }

  /// 任务类别（跨页面查重入用）。
  static const _kind = 'ota';

  Future<void> _bootstrap() async {
    await _check(readSlot: true);
  }

  Future<void> _check({bool readSlot = true}) async {
    if (_busy) return;
    setState(() {
      _phase = _OtaPhase.checking;
      _error = null;
    });
    try {
      final hw = _client.lastDevInfo?.hwVersion;
      final manifest = await _ota.fetchManifest(
        hardware: (hw != null && hw.isNotEmpty)
            ? hw
            : OtaConfig.defaultHardware,
      );
      var slot = _slot;
      if (readSlot && _ready) {
        slot = await _ota.listSlot(_client);
      }
      final cached = await _ota.hasCache(manifest);
      if (!mounted) return;
      setState(() {
        _manifest = manifest;
        _slot = slot;
        _cached = cached;
        _phase = _OtaPhase.idle;
      });
    } catch (e, st) {
      if (!mounted) return;
      AppTheme.failText('OtaCheck', e, st);
      setState(() {
        _phase = _OtaPhase.idle;
        _error = LocaleKeys.otaCheckFail.tr;
      });
    }
  }

  Future<void> _install() async {
    final manifest = _manifest;
    final center = TransferCenter.to;
    if (manifest == null || _busy) return;
    // 页面重新进来时 _busy 已归零，靠中心拦重复发起。
    if (center.isKindRunning(_kind)) return;
    if (!_ready) {
      setState(() => _error = LocaleKeys.otaNeedBle.tr);
      return;
    }
    if (_client.lastStatus?.isTransferBusy == true) {
      setState(() => _error = LocaleKeys.otaMtpBusy.tr);
      return;
    }

    _cancel = false;
    _ota.lastFromCache = false;
    setState(() {
      _error = null;
      _phase = _OtaPhase.downloading;
      _done = 0;
      _total = manifest.size;
    });

    // 下载段可取消（HTTP 流按块检查 shouldStop）；上传段不可取消 ——
    // fsUpload 一旦开始只能让它跑完。
    final task = center.begin(
      title: LocaleKeys.otaTitle.tr,
      kind: _kind,
      cancellable: true,
      onCancel: () => _cancel = true,
    );

    try {
      await _ensureFgs();
      final bytes = await _ota.downloadFirmware(
        manifest,
        onProgress: (d, t) {
          center.progress(
            task,
            done: d,
            total: t,
            transferred: d,
            phase: LocaleKeys.otaDownloading.tr,
          );
          if (!mounted) return;
          setState(() {
            _done = d;
            _total = t;
          });
        },
        // 只看 _cancel：退出页面不再中断下载。
        shouldStop: () => _cancel,
      );
      if (_cancel) {
        if (task.isRunning) center.cancel(task);
        if (mounted) setState(() => _phase = _OtaPhase.idle);
        return;
      }

      if (mounted) {
        setState(() {
          _phase = _OtaPhase.verifying;
          _cached = true;
        });
      }
      // 校验阶段没有字节进度，total 置 0 让悬浮框画不确定进度条。
      center.progress(
        task,
        done: 0,
        total: 0,
        phase: LocaleKeys.otaVerifying.tr,
      );
      _ota.verify(manifest, bytes);

      if (mounted) {
        setState(() {
          _phase = _OtaPhase.uploading;
          _done = 0;
          _total = bytes.length;
        });
      }
      await CompanionForegroundService.update('OTA ${manifest.version}');
      final dlBytes = bytes.length;
      await _ota.uploadToDevice(
        _client,
        manifest,
        bytes,
        onProgress: (d, t) {
          center.progress(
            task,
            done: d,
            total: t,
            // 跨阶段累计：上传段的 transferred 从下载量起步，否则
            // 测速窗口会看到字节数「倒扣」，速度会僵住。
            transferred: dlBytes + d,
            phase: LocaleKeys.otaUploading.tr,
          );
          if (!mounted) return;
          setState(() {
            _done = d;
            _total = t;
          });
        },
      );

      List<CompanionFsEntry> slot = _slot;
      try {
        slot = await _ota.listSlot(_client);
      } catch (_) {}

      center.finish(task);
      if (!mounted) return;
      setState(() {
        _slot = slot;
        _phase = _OtaPhase.done;
      });
    } on OtaCancelledException {
      if (task.isRunning) center.cancel(task);
      if (mounted) setState(() => _phase = _OtaPhase.idle);
    } on OtaChecksumException catch (e, st) {
      center.fail(task, e);
      if (!mounted) return;
      AppTheme.failText('OtaInstall', e, st);
      setState(() {
        _phase = _OtaPhase.idle;
        _error = LocaleKeys.otaHashFail.tr;
      });
    } catch (e, st) {
      center.fail(task, e);
      if (!mounted) return;
      try {
        await _client.fsAbort();
      } catch (_) {}
      setState(() {
        _phase = _OtaPhase.idle;
        _error = AppTheme.failText('OtaInstall', e, st);
      });
    } finally {
      /* 升级期间通知栏被 pin 成"正在升级码表固件"，出口有四个（成功 / 取消 /
       * 校验失败 / 异常），在这里统一交还，免得漏一处让通知栏永久停在那句话。
       * 放在 finally 而不是 dispose：退出页面后台继续升级时，这条路径才是升级
       * 真正的终点。 */
      unawaited(CompanionForegroundService.unpin());
    }
  }

  Future<void> _ensureFgs() async {
    if (!CompanionForegroundService.isSupported) return;
    // 先占位：升级期间通知栏归这里，连接状态刷新让位（否则"正在升级码表固件"
    // 会被"正在连接码表…"顶掉，而升级中掉线恰恰是最需要看见的时刻）。
    await CompanionForegroundService.pin('正在升级码表固件');
    if (await CompanionForegroundService.isRunning()) return;
    await CompanionForegroundService.requestNotificationPermission();
    await CompanionForegroundService.start(text: '正在升级码表固件');
  }

  String _sizeLabel(int n) {
    if (n >= 1024 * 1024) {
      return '${(n / (1024 * 1024)).toStringAsFixed(1)} MB';
    }
    if (n >= 1024) return '${(n / 1024).toStringAsFixed(0)} KB';
    return '$n B';
  }

  Color get _orbColor => switch (_phase) {
        _OtaPhase.downloading ||
        _OtaPhase.checking ||
        _OtaPhase.verifying =>
          AppTheme.otaDownload,
        _OtaPhase.uploading => AppTheme.otaPush,
        _OtaPhase.done => AppTheme.ringLime,
        _OtaPhase.idle => AppTheme.iconOta,
      };

  IconData get _orbIcon {
    if (!_ready && _phase == _OtaPhase.idle) {
      return Icons.bluetooth_disabled;
    }
    return switch (_phase) {
      _OtaPhase.downloading => Icons.cloud_download_rounded,
      _OtaPhase.verifying => Icons.verified_rounded,
      _OtaPhase.uploading => Icons.unarchive_rounded,
      _OtaPhase.checking => Icons.cloud_sync_rounded,
      _OtaPhase.done => Icons.check_rounded,
      _OtaPhase.idle => Icons.system_update_alt,
    };
  }

  String _orbLabel({required bool primaryIsCheck}) {
    return switch (_phase) {
      _OtaPhase.downloading => LocaleKeys.otaDownloadAction.tr,
      _OtaPhase.verifying => LocaleKeys.otaVerifyAction.tr,
      _OtaPhase.uploading => LocaleKeys.otaPushAction.tr,
      _OtaPhase.checking => LocaleKeys.otaCheck.tr,
      _OtaPhase.done => LocaleKeys.otaInstall.tr,
      _OtaPhase.idle => primaryIsCheck
          ? LocaleKeys.otaCheck.tr
          : (_updateAvailable
              ? (_cached
                  ? LocaleKeys.otaInstallCached.tr
                  : LocaleKeys.otaInstall.tr)
              : LocaleKeys.otaReinstall.tr),
    };
  }

  String? _headlineOf(OtaManifest? m) {
    if (m != null) return m.version;
    if (_phase == _OtaPhase.checking) return LocaleKeys.otaChecking.tr;
    if (!_ready) return LocaleKeys.otaNeedBle.tr;
    return LocaleKeys.otaHint.tr;
  }

  String? _captionOf(OtaManifest? m) {
    if (m == null) return null;
    final bits = <String>[
      if (m.date.isNotEmpty) m.date,
      _sizeLabel(m.size),
      if (_phase != _OtaPhase.done)
        _updateAvailable
            ? LocaleKeys.otaUpdateAvailable.tr
            : LocaleKeys.otaUpToDate.tr,
    ];
    final device = _deviceVersion;
    if (device != null && device.isNotEmpty) {
      bits.add(LocaleKeys.otaDeviceVersion.trParams({'ver': device}));
    }
    return bits.join('  ·  ');
  }

  @override
  Widget build(BuildContext context) {
    final m = _manifest;
    final canInstall = !_busy && m != null && _ready;
    final primaryIsCheck = m == null;

    String? status;
    if (_busy) {
      status = _phaseLabel();
      if ((_phase == _OtaPhase.downloading ||
              _phase == _OtaPhase.uploading) &&
          _total > 0) {
        status = LocaleKeys.otaProgress.trParams({
          'done': _sizeLabel(_done),
          'total': _sizeLabel(_total),
          'pct': ((_done / _total) * 100).clamp(0, 100).toStringAsFixed(0),
        });
      }
    }

    final pct = (_phase == _OtaPhase.downloading ||
            _phase == _OtaPhase.uploading) &&
        _total > 0
        ? (_done / _total).clamp(0.0, 1.0)
        : null;

    final err = _error;
    final peek = HelmPeekSheet.reserve(context);

    return Scaffold(
      appBar: AppBar(title: Text(LocaleKeys.otaTitle.tr)),
      body: Stack(
        children: [
          Padding(
            padding: EdgeInsets.only(bottom: peek),
            child: HelmCenterAction(
              icon: _orbIcon,
              label: _orbLabel(primaryIsCheck: primaryIsCheck),
              color: _orbColor,
              onPressed: _busy
                  ? null
                  : (primaryIsCheck
                      ? () => _check(readSlot: true)
                      : (canInstall ? _install : null)),
              busy: _busy,
              progress: pct,
              headline: _headlineOf(m),
              caption: _captionOf(m),
              status: status,
              message: err ??
                  (_phase == _OtaPhase.done ? LocaleKeys.otaDone.tr : null),
              messageError: err != null,
            ),
          ),
          AnimatedBuilder(
            animation: _sheet,
            builder: (context, _) {
              return Align(
                alignment: Alignment.bottomCenter,
                child: _detailsSheet(),
              );
            },
          ),
        ],
      ),
    );
  }

  void _onSheetDragUpdate(double dy) {
    final bodyH = MediaQuery.sizeOf(context).height;
    final travel = bodyH * 0.48 - HelmPeekSheet.peekHeight;
    if (travel <= 8) return;
    if (_sheet.isAnimating) _sheet.stop();
    _sheet.value = (_sheet.value - dy / travel).clamp(0.0, 1.0);
  }

  void _onSheetDragEnd(double velocity) {
    final next = velocity < -350
        ? 1.0
        : velocity > 350
            ? 0.0
            : _sheet.value >= 0.45
                ? 1.0
                : 0.0;
    _sheet.animateTo(next, curve: Curves.easeOutCubic);
  }

  Widget _detailsSheet() {
    final m = _manifest;
    final device = _deviceVersion;
    const body = TextStyle(color: Colors.white, fontSize: 15, height: 1.45);
    const muted = TextStyle(color: AppTheme.muted, fontSize: 14, height: 1.45);
    return HelmPeekSheet(
      t: Curves.easeOutCubic.transform(_sheet.value),
      handle: m == null
          ? LocaleKeys.otaDetails.tr
          : [
              m.version,
              if (m.date.isNotEmpty) m.date,
            ].join('  ·  '),
      onHeaderTap: () {
        if (_sheet.value < 0.5) {
          _sheet.animateTo(1, curve: Curves.easeOutCubic);
        } else {
          _sheet.animateTo(0, curve: Curves.easeOutCubic);
        }
      },
      onDragDelta: _onSheetDragUpdate,
      onDragEnd: _onSheetDragEnd,
      children: [
        if (!_ready) ...[
          Text(LocaleKeys.otaNeedBle.tr, style: muted),
          const SizedBox(height: 12),
        ],
        Text(
          device == null
              ? LocaleKeys.otaDeviceUnknown.tr
              : LocaleKeys.otaDeviceVersion.trParams({'ver': device}),
          style: body,
        ),
        if (m != null) ...[
          const SizedBox(height: 6),
          Text(
            LocaleKeys.otaCloudVersion.trParams({'ver': m.version}),
            style: body,
          ),
          if (m.date.isNotEmpty) ...[
            const SizedBox(height: 6),
            Text(
              LocaleKeys.otaReleased.trParams({'date': m.date}),
              style: body,
            ),
          ],
          const SizedBox(height: 6),
          Text(
            LocaleKeys.otaSize.trParams({'size': _sizeLabel(m.size)}),
            style: muted,
          ),
          if (_cached) ...[
            const SizedBox(height: 6),
            Text(LocaleKeys.otaCached.tr, style: muted),
          ],
          if (m.note.isNotEmpty) ...[
            const SizedBox(height: 12),
            Text(
              LocaleKeys.otaNote.trParams({'note': m.note}),
              style: body,
            ),
          ],
        ],
        const SizedBox(height: 12),
        Text(LocaleKeys.otaHint.tr, style: muted.copyWith(fontSize: 13)),
        const SizedBox(height: 16),
        OutlinedButton(
          onPressed: _busy ? null : () => _check(readSlot: true),
          child: Text(LocaleKeys.otaCheck.tr),
        ),
      ],
    );
  }

  String _phaseLabel() => switch (_phase) {
        _OtaPhase.checking => LocaleKeys.otaChecking.tr,
        _OtaPhase.downloading => LocaleKeys.otaDownloading.tr,
        _OtaPhase.verifying => LocaleKeys.otaVerifying.tr,
        _OtaPhase.uploading => LocaleKeys.otaUploading.tr,
        _ => '',
      };
}

import 'dart:async';

import 'package:flutter/material.dart';
import 'package:get/get.dart';
import 'package:sifli_companion/app/app_theme.dart';
import 'package:sifli_companion/ble/companion_client.dart';
import 'package:sifli_companion/ble/companion_proto.dart';
import 'package:sifli_companion/gnss/eph_auto_sync.dart';
import 'package:sifli_companion/gnss/eph_service.dart';
import 'package:sifli_companion/gnss/mga_ubx.dart';
import 'package:sifli_companion/i18n/locale_keys.dart';
import 'package:sifli_companion/ride/gpx_util.dart';
import 'package:sifli_companion/transfer/transfer_center.dart';
import 'package:sifli_companion/ui/widgets/helm_center_action.dart';
import 'package:sifli_companion/ui/widgets/helm_peek_sheet.dart';

/// 从网上拉广播星历，生成后经 BLE 写入并热加载。
class EphPage extends StatefulWidget {
  /// 创建星历同步页。
  const EphPage({super.key});

  @override
  State<EphPage> createState() => _EphPageState();
}

enum _EphPhase { idle, syncing, done }

class _EphPageState extends State<EphPage> with SingleTickerProviderStateMixin {
  final _client = CompanionClient.instance;
  final _eph = EphService();

  StreamSubscription<CompanionConnState>? _stateSub;
  late final AnimationController _sheet;
  _EphPhase _phase = _EphPhase.idle;
  EphSyncStage? _stage;
  String? _error;
  List<CompanionFsEntry> _onDevice = [];
  bool _listed = false;
  int _done = 0;
  int _total = 0;
  EphSyncResult? _lastResult;

  bool get _busy => _phase == _EphPhase.syncing;

  bool get _ready => _client.isReady;

  CompanionFsEntry? get _latest {
    for (final e in _onDevice) {
      if (!e.isDir) return e;
    }
    return null;
  }

  DateTime? get _lastSync {
    DateTime? best;
    for (final e in _onDevice) {
      if (e.isDir) continue;
      final t = _entryTime(e);
      if (t != null && (best == null || t.isAfter(best))) best = t;
    }
    return best;
  }

  static DateTime? _entryTime(CompanionFsEntry e) {
    final utc = MgaUbx.parseFileNameUtc(e.name);
    if (utc != null) {
      return DateTime.fromMillisecondsSinceEpoch(
        utc * 1000,
        isUtc: true,
      ).toLocal();
    }
    if (e.mtime > 0) {
      return DateTime.fromMillisecondsSinceEpoch(e.mtime * 1000).toLocal();
    }
    return null;
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
      if (_client.isReady && !_listed && !_busy) {
        unawaited(_refreshDevice());
      }
    });
    if (_ready) {
      unawaited(_refreshDevice());
    }
    // 进来就先要一份「下次同步」（节流过的，不会额外压文件系统）。
    unawaited(EphAutoSync.instance.refreshStatus());
  }

  @override
  void dispose() {
    _sheet.dispose();
    _stateSub?.cancel();
    super.dispose();
  }

  Future<void> _refreshDevice() async {
    if (!_ready || _phase == _EphPhase.syncing) return;
    try {
      final list = await _eph.listOnDevice(_client);
      if (!mounted) return;
      setState(() {
        _onDevice = list;
        _listed = true;
      });
    } catch (e, st) {
      AppTheme.failText('EphList', e, st);
      if (!mounted) return;
      setState(() => _listed = true);
    }
  }

  Future<void> _sync() async {
    final center = TransferCenter.to;
    if (_busy) return;
    // 页面重建后 _busy 会丢，回中心问一次，避免重复发起把 BLE 通道踩乱。
    if (center.isKindRunning(_kind)) return;
    if (!_ready) {
      setState(() => _error = LocaleKeys.ephNeedBle.tr);
      return;
    }
    if (_client.lastStatus?.isTransferBusy == true) {
      setState(() => _error = LocaleKeys.ephMtpBusy.tr);
      return;
    }

    setState(() {
      _error = null;
      _phase = _EphPhase.syncing;
      _stage = EphSyncStage.download;
      _done = 0;
      _total = 0;
    });

    // 星历没有取消通道：上传中途只能靠 fsAbort 把整个 BLE 会话打断，
    // 代价比等它跑完更大，所以不给取消按钮。
    final task = center.begin(
      title: LocaleKeys.ephTitle.tr,
      kind: _kind,
    );

    try {
      final result = await _eph.syncToDevice(
        _client,
        onStage: (s) {
          center.progress(task, phase: _stageLabel(s));
          if (!mounted) return;
          setState(() => _stage = s);
        },
        onProgress: (d, t) {
          center.progress(
            task,
            done: d,
            total: t,
            transferred: d,
            phase: _stageLabel(EphSyncStage.upload),
          );
          if (!mounted) return;
          setState(() {
            _done = d;
            _total = t;
          });
        },
      );
      List<CompanionFsEntry> list = _onDevice;
      try {
        list = await _eph.listOnDevice(_client);
      } catch (_) {}
      center.finish(task);
      if (!mounted) return;
      setState(() {
        _onDevice = list;
        _listed = true;
        _lastResult = result;
        _phase = _EphPhase.done;
        _stage = null;
      });
      // 刚写进去的文件就是新的「上次同步」，下次同步时间跟着后移。
      unawaited(EphAutoSync.instance.refreshStatus(force: true));
    } catch (e, st) {
      center.fail(task, e);
      if (!mounted) return;
      try {
        await _client.fsAbort();
      } catch (_) {}
      setState(() {
        _phase = _EphPhase.idle;
        _stage = null;
        _error = AppTheme.failText('EphSync', e, st);
      });
    }
  }

  /// 任务类别（跨页面查重入用）。
  static const _kind = 'eph';

  String _stageLabel(EphSyncStage? s) => switch (s) {
        EphSyncStage.download => LocaleKeys.ephDownloading.tr,
        EphSyncStage.convert => LocaleKeys.ephConverting.tr,
        EphSyncStage.upload => LocaleKeys.ephUploading.tr,
        null => LocaleKeys.ephTitle.tr,
      };

  Color get _orbColor {
    if (_phase == _EphPhase.done) return AppTheme.ringLime;
    if (_stage == EphSyncStage.upload) return AppTheme.otaPush;
    if (_stage == EphSyncStage.download || _stage == EphSyncStage.convert) {
      return AppTheme.otaDownload;
    }
    return AppTheme.iconEph;
  }

  IconData get _orbIcon {
    if (!_ready && _phase == _EphPhase.idle) {
      return Icons.bluetooth_disabled;
    }
    return switch (_stage) {
      EphSyncStage.download => Icons.cloud_download_rounded,
      EphSyncStage.convert => Icons.auto_fix_high_rounded,
      EphSyncStage.upload => Icons.unarchive_rounded,
      null => _phase == _EphPhase.done
          ? Icons.check_rounded
          : Icons.satellite_alt,
    };
  }

  String get _orbLabel {
    return switch (_stage) {
      EphSyncStage.download => LocaleKeys.ephDownloadAction.tr,
      EphSyncStage.convert => LocaleKeys.ephConvertAction.tr,
      EphSyncStage.upload => LocaleKeys.ephPushAction.tr,
      null => LocaleKeys.ephSync.tr,
    };
  }

  String? get _headline {
    final last = _lastSync;
    if (last != null) return GpxUtil.formatStamp(last);
    if (!_ready) return LocaleKeys.ephNeedBle.tr;
    if (_listed) return LocaleKeys.ephNever.tr;
    return LocaleKeys.ephTitle.tr;
  }

  String? get _caption {
    final file = _latest;
    final bits = <String>[
      if (file != null) file.name,
      if (file != null && file.size > 0) _sizeLabel(file.size),
      if (_lastResult != null && _lastResult!.parsed.mgaFrames > 0)
        LocaleKeys.ephFrames.trParams({
          'n': '${_lastResult!.parsed.mgaFrames}',
        }),
    ];
    if (bits.isNotEmpty) return bits.join('  ·  ');
    return LocaleKeys.ephHint.tr;
  }

  String _sizeLabel(int n) {
    if (n >= 1024) return '${(n / 1024).toStringAsFixed(1)} KB';
    return '$n B';
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
    final last = _lastSync;
    final file = _latest;
    const body = TextStyle(color: Colors.white, fontSize: 15, height: 1.45);
    const muted = TextStyle(color: AppTheme.muted, fontSize: 14, height: 1.45);
    return HelmPeekSheet(
      t: Curves.easeOutCubic.transform(_sheet.value),
      handle: last == null
          ? LocaleKeys.ephDetails.tr
          : GpxUtil.formatStamp(last),
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
          Text(LocaleKeys.ephNeedBle.tr, style: muted),
          const SizedBox(height: 12),
        ],
        Text(
          last == null
              ? LocaleKeys.ephNever.tr
              : LocaleKeys.ephLastSync.trParams({
                  'time': GpxUtil.formatStamp(last),
                }),
          style: body,
        ),
        if (file != null) ...[
          const SizedBox(height: 6),
          Text(
            LocaleKeys.ephFile.trParams({'name': file.name}),
            style: body,
          ),
          const SizedBox(height: 6),
          Obx(
            () => Text(EphAutoSync.instance.status.value.label, style: body),
          ),
          if (file.size > 0) ...[
            const SizedBox(height: 6),
            Text(
              LocaleKeys.otaSize.trParams({'size': _sizeLabel(file.size)}),
              style: muted,
            ),
          ],
        ],
        if (_lastResult != null && _lastResult!.parsed.mgaFrames > 0) ...[
          const SizedBox(height: 6),
          Text(
            LocaleKeys.ephFrames.trParams({
              'n': '${_lastResult!.parsed.mgaFrames}',
            }),
            style: muted,
          ),
        ],
        const SizedBox(height: 12),
        Text(LocaleKeys.ephHint.tr, style: muted.copyWith(fontSize: 13)),
      ],
    );
  }

  @override
  Widget build(BuildContext context) {
    final uploading = _stage == EphSyncStage.upload && _total > 0;
    final pct = uploading ? (_done / _total).clamp(0.0, 1.0) : null;
    final peek = HelmPeekSheet.reserve(context);

    String? status;
    if (_busy) {
      status = uploading
          ? LocaleKeys.ephProgress.trParams({
              'done': _sizeLabel(_done),
              'total': _sizeLabel(_total),
              'pct': ((_done / _total) * 100).clamp(0, 100).toStringAsFixed(0),
            })
          : switch (_stage) {
              EphSyncStage.download => LocaleKeys.ephDownloading.tr,
              EphSyncStage.convert => LocaleKeys.ephConverting.tr,
              EphSyncStage.upload => LocaleKeys.ephUploading.tr,
              null => LocaleKeys.ephSync.tr,
            };
    }

    return Scaffold(
      appBar: AppBar(title: Text(LocaleKeys.ephTitle.tr)),
      body: Stack(
        children: [
          Padding(
            padding: EdgeInsets.only(bottom: peek),
            child: Obx(
              () => HelmCenterAction(
                icon: _orbIcon,
                label: _orbLabel,
                color: _orbColor,
                onPressed: _busy || !_ready ? null : _sync,
                busy: _busy,
                progress: pct,
                headline: _headline,
                caption: _caption,
                // 空闲时这行显示「下次同步 …」：一眼能看到下一次什么时候同步。
                status: status ?? EphAutoSync.instance.status.value.label,
                message: _error ??
                    (_phase == _EphPhase.done ? LocaleKeys.ephDone.tr : null),
                messageError: _error != null,
              ),
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
}

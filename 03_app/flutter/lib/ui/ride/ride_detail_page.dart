import 'dart:convert';
import 'dart:io';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter_map/flutter_map.dart';
import 'package:get/get.dart';
import 'package:latlong2/latlong.dart';
import 'package:path/path.dart' as p;
import 'package:path_provider/path_provider.dart';
import 'package:share_plus/share_plus.dart';
import 'package:sifli_companion/app/app_theme.dart';
import 'package:sifli_companion/i18n/locale_keys.dart';
import 'package:sifli_companion/map/osm_tiles.dart';
import 'package:sifli_companion/ride/gpx_library_store.dart';
import 'package:sifli_companion/ride/gpx_util.dart';
import 'package:sifli_companion/ride/ride_record_store.dart';
import 'package:sifli_companion/ui/ride/import_upload.dart';
import 'package:sifli_companion/ui/ride/ride_charts.dart';
import 'package:sifli_companion/ui/ride/ride_glance.dart';
import 'package:sifli_companion/ui/ride/ride_poster.dart';
import 'package:sifli_companion/ui/ride/ride_track_style.dart';
import 'package:sifli_companion/ui/shell/shell_controller.dart';
import 'package:sifli_companion/ui/widgets/helm_chrome.dart';

/// 一条本机骑行记录的详情：地图为主，底部条上滑把地图挤到半屏看数据。
class RideDetailPage extends StatefulWidget {
  /// 创建详情页。
  const RideDetailPage({super.key});

  @override
  State<RideDetailPage> createState() => _RideDetailPageState();
}

class _RideDetailPageState extends State<RideDetailPage>
    with TickerProviderStateMixin {
  final _shotKey = GlobalKey();
  final _map = MapController();
  final _shareMap = MapController();
  RideRecord? _rec;
  GpxSummary? _sum;
  List<LatLng> _pts = const [];
  List<LatLng> _disp = const [];
  bool _sharing = false;
  late final AnimationController _sheet;

  /// 轨迹回放：0..1 = 走完整条。进场自动播一次，最长 [_playMaxSec] 秒。
  late final AnimationController _play;

  /// 回放压缩上限：再长的骑行也在 10 秒内播完。
  static const _playMaxSec = 10.0;

  /// 每个点的累计骑行秒数（由「距离 / 速度」还原的真实节奏）。
  List<double> _rideT = const [];
  double _rideTotal = 0;

  /// 整条轨迹的色标。加载时算一次，回放只做切片（逐帧重算平滑会掉帧）。
  List<Color> _stops = const [];

  @override
  void initState() {
    super.initState();
    final name = Get.arguments as String?;
    if (name != null) {
      // 先看骑行记录；没有就去 GPX 库找 —— 那边（导入的、地图上画好的）文件
      // 不在这张表里，以前点开是空白页。
      _rec = RideRecordStore().find(name) ?? _fromLibrary(name);
    }
    _loadPts();
    _sheet = AnimationController(
      vsync: this,
      duration: const Duration(milliseconds: 280),
    )..addStatusListener((status) {
        if (status == AnimationStatus.completed ||
            status == AnimationStatus.dismissed) {
          _fitLive();
        }
      });
    _play = AnimationController(vsync: this);
  }

  /// 在 GPX 库里按文件名找一条（找不到返回 null）。
  static RideRecord? _fromLibrary(String fileName) {
    for (final rec in GpxLibraryStore().items) {
      if (rec.fileName == fileName) return rec;
    }
    return null;
  }

  @override
  void dispose() {
    _map.dispose();
    _shareMap.dispose();
    _sheet.dispose();
    _play.dispose();
    super.dispose();
  }

  Future<void> _loadPts() async {
    final rec = _rec;
    if (rec == null) return;
    final f = File(rec.localPath);
    if (!await f.exists()) return;
    final xml = utf8.decode(await f.readAsBytes(), allowMalformed: true);
    final sum = GpxUtil.parse(xml);
    final speeds = sum.pointSpeedsKmh;
    // 回放时间轴：按「距离 / 速度」累加，快的地方画得快。
    final rideT = GpxUtil.cumulativeRideSeconds(sum.points, speeds);
    final total = rideT.isEmpty ? 0.0 : rideT.last;
    if (!mounted) return;
    setState(() {
      _sum = sum;
      _pts = sum.points;
      _disp = HelmOsmTiles.toDisplayAll(sum.points);
      _rideT = rideT;
      _rideTotal = total;
      _stops = RideTrackStyle.bandStops(
        speeds,
        fallbackKmh: sum.avgSpeedKmh ?? 18,
      );
    });
    _startPlayback();
  }

  /// 进场自动回放一次。
  ///
  /// 把整段骑行时长等比压进 [_playMaxSec] 秒内播完：等比压缩保留了各段的快慢差别，
  /// 所以「以运动的速度绘制」这件事在压缩后依然成立。本来就比上限短的按实际时长播。
  void _startPlayback() {
    if (_disp.length < 2) return;
    final secs = _rideTotal > 0 ? _rideTotal : _playMaxSec;
    _play.duration = Duration(
      milliseconds: (secs.clamp(0.6, _playMaxSec) * 1000).round(),
    );
    _play.forward(from: 0);
  }

  /// 当前画到的小数下标（4.5 表示在第 4、5 点之间）。
  double get _playCut {
    if (_disp.length < 2) return 0;
    // 没有速度信息 → 没有时间轴，按点数均匀播。
    if (_rideT.length != _disp.length || _rideTotal <= 0) {
      return _play.value * (_disp.length - 1);
    }
    final sec = _play.value * _rideTotal;
    // 播完就整条。二进制查找找的是「第一个 >= sec 的下标」，如果尾部有一串
    // 零位移点（原地停留，累计时间相同）会停在那串的第一个上，线就画不到底、
    // 游标也不消失。
    if (sec >= _rideTotal) return _disp.length - 1;
    var lo = 0;
    var hi = _rideT.length - 1;
    while (lo < hi) {
      final mid = (lo + hi) >> 1;
      if (_rideT[mid] < sec) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    final i = lo < 1 ? 1 : lo;
    final t0 = _rideT[i - 1];
    final t1 = _rideT[i];
    final f = t1 - t0 <= 0 ? 1.0 : ((sec - t0) / (t1 - t0)).clamp(0.0, 1.0);
    return (i - 1) + f;
  }

  static const _mapPad = EdgeInsets.fromLTRB(40, 48, 40, 40);
  static const _mapPadPeek = EdgeInsets.fromLTRB(36, 80, 36, 100);
  static const _peekBody = 56.0;

  void _fitLive() {
    if (!mounted || _disp.length < 2) return;
    final fit = _fitOf(
      _disp,
      padding: _sheet.value > 0.5 ? _mapPad : _mapPadPeek,
    );
    if (fit == null) return;
    try {
      _map.fitCamera(fit);
    } catch (_) {}
  }

  void _onSheetDragUpdate(double dy, double bodyH) {
    final travel = bodyH * 0.5 - _peekBody;
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

  CameraFit? _fitOf(List<LatLng> pts, {EdgeInsets padding = _mapPad}) {
    if (pts.length < 2) return null;
    return CameraFit.bounds(
      bounds: LatLngBounds.fromPoints(pts),
      padding: padding,
      maxZoom: 16,
      minZoom: 3,
      forceIntegerZoomLevel: true,
    );
  }

  /// 用控件真实尺寸算出第一帧相机，避免先对着起点再跳到整条轨迹。
  ({LatLng center, double zoom})? _cameraFor(
    List<LatLng> pts,
    Size size, {
    EdgeInsets padding = _mapPad,
  }) {
    final fit = _fitOf(pts, padding: padding);
    if (fit == null || size.width < 8 || size.height < 8) return null;
    try {
      final fitted = fit.fit(
        MapCamera(
          crs: const Epsg3857(),
          center: pts.first,
          zoom: 10,
          rotation: 0,
          nonRotatedSize: size,
          minZoom: 3,
          maxZoom: 18,
        ),
      );
      return (center: fitted.center, zoom: fitted.zoom);
    } catch (_) {
      return null;
    }
  }

  void _fitTrack(MapController map) {
    final fit = _fitOf(_disp, padding: _mapPadPeek);
    if (fit == null) return;
    try {
      map.fitCamera(fit);
    } catch (_) {}
  }

  Future<void> _shareGpx() async {
    final rec = _rec;
    if (rec == null) return;
    await SharePlus.instance.share(
      ShareParams(
        files: [
          XFile(
            rec.localPath,
            mimeType: 'application/gpx+xml',
            name: '${rec.title}.gpx',
          ),
        ],
        text: rec.title,
      ),
    );
  }

  Future<void> _rename() async {
    final rec = _rec;
    if (rec == null) return;
    final name = await HelmPromptDialog.show(
      context,
      title: LocaleKeys.rideRename.tr,
      cancel: LocaleKeys.cancel.tr,
      confirm: LocaleKeys.confirm.tr,
      label: LocaleKeys.navRoutesName.tr,
      hint: LocaleKeys.rideRenameHint.tr,
      initial: rec.title,
    );
    if (name == null || name.isEmpty) return;
    final updated = await RideRecordStore().setDisplayName(rec.fileName, name);
    if (Get.isRegistered<ShellController>()) {
      Get.find<ShellController>().bumpRides();
    }
    if (!mounted) return;
    setState(() {
      if (updated != null) _rec = updated;
    });
  }

  Future<void> _uploadImport() async {
    final rec = _rec;
    if (rec == null) return;
    await ImportUpload.push(
      context: context,
      localPath: rec.localPath,
      defaultName: rec.title,
    );
  }

  Future<void> _shareImage() async {
    final rec = _rec;
    if (rec == null || _sharing) return;
    setState(() => _sharing = true);
    try {
      await WidgetsBinding.instance.endOfFrame;
      if (!mounted) return;
      if (_pts.length >= 2) {
        _fitTrack(_shareMap);
        await Future<void>.delayed(const Duration(milliseconds: 700));
      }
      if (!mounted) return;
      final boundary =
          _shotKey.currentContext?.findRenderObject() as RenderRepaintBoundary?;
      if (boundary == null) {
        throw StateError('map not ready');
      }
      final dpr = MediaQuery.devicePixelRatioOf(context).clamp(2.0, 3.0);
      final image = await boundary.toImage(pixelRatio: dpr);
      final bytes = await image.toByteData(format: ui.ImageByteFormat.png);
      if (bytes == null) {
        throw StateError('empty png');
      }
      final dir = await getTemporaryDirectory();
      final stem = rec.fileName.toLowerCase().endsWith('.gpx')
          ? rec.fileName.substring(0, rec.fileName.length - 4)
          : rec.fileName;
      final file = File(p.join(dir.path, 'helm_$stem.png'));
      await file.writeAsBytes(bytes.buffer.asUint8List(), flush: true);
      final caption = RidePoster.shareCaption(rec, _sum);
      await SharePlus.instance.share(
        ShareParams(
          files: [
            XFile(file.path, mimeType: 'image/png', name: '${rec.title}.png'),
          ],
          text: caption,
        ),
      );
    } catch (e, st) {
      if (!mounted) return;
      AppTheme.fail(LocaleKeys.rideShareImage.tr, 'RideShare', e, st);
    } finally {
      if (mounted) setState(() => _sharing = false);
    }
  }

  Future<void> _delete() async {
    final rec = _rec;
    if (rec == null) return;
    final ok = await Get.dialog<bool>(
      AlertDialog(
        title: Text(LocaleKeys.rideDelete.tr),
        content: Text(
          LocaleKeys.deleteConfirm.trParams({'name': rec.title}),
        ),
        actions: [
          TextButton(
            onPressed: () => Get.back(result: false),
            child: Text(LocaleKeys.cancel.tr),
          ),
          TextButton(
            onPressed: () => Get.back(result: true),
            child: Text(LocaleKeys.confirm.tr),
          ),
        ],
      ),
    );
    if (ok != true) return;
    await RideRecordStore().remove(rec.fileName);
    if (Get.isRegistered<ShellController>()) {
      Get.find<ShellController>().bumpRides();
    }
    if (mounted) Get.back();
  }

  @override
  Widget build(BuildContext context) {
    final rec = _rec;
    return Scaffold(
      appBar: AppBar(
        title: Text(rec?.title ?? LocaleKeys.rideDetailTitle.tr),
        actions: [
          IconButton(
            tooltip: LocaleKeys.rideRename.tr,
            onPressed: rec == null || _sharing ? null : _rename,
            icon: const Icon(Icons.drive_file_rename_outline),
          ),
          IconButton(
            tooltip: LocaleKeys.rideShareImage.tr,
            onPressed: rec == null || _sharing ? null : _shareImage,
            icon: const Icon(Icons.ios_share),
          ),
          PopupMenuButton<String>(
            enabled: rec != null && !_sharing,
            onSelected: (v) {
              if (v == 'import') _uploadImport();
              if (v == 'gpx') _shareGpx();
              if (v == 'del') _delete();
            },
            itemBuilder: (context) => [
              PopupMenuItem(
                value: 'import',
                child: Text(LocaleKeys.rideUploadImport.tr),
              ),
              PopupMenuItem(
                value: 'gpx',
                child: Text(LocaleKeys.rideShareGpx.tr),
              ),
              PopupMenuItem(
                value: 'del',
                child: Text(LocaleKeys.rideDelete.tr),
              ),
            ],
          ),
        ],
      ),
      body: rec == null
          ? Center(child: Text(LocaleKeys.rideEmpty.tr))
          : LayoutBuilder(
              builder: (context, box) {
                final h = box.maxHeight;
                final bottom = MediaQuery.paddingOf(context).bottom;
                return AnimatedBuilder(
                  animation: _sheet,
                  builder: (context, _) {
                    final t = Curves.easeOutCubic.transform(_sheet.value);
                    final peekH = _peekBody + bottom * (1 - t);
                    final sheetH = peekH + (h * 0.5 - peekH) * t;
                    return Stack(
                      children: [
                        Column(
                          children: [
                            Expanded(
                              child: Stack(
                                fit: StackFit.expand,
                                children: [
                                  _mapView(
                                    _map,
                                    fitPad: t < 0.5 ? _mapPadPeek : _mapPad,
                                    playable: true,
                                  ),
                                  if (t < 0.98)
                                    IgnorePointer(
                                      child: Opacity(
                                        opacity: (1 - t).clamp(0.0, 1.0),
                                        child: RideGlanceOverlay(
                                          rec: rec,
                                          summary: _sum,
                                        ),
                                      ),
                                    ),
                                ],
                              ),
                            ),
                            SizedBox(
                              height: sheetH.clamp(_peekBody, h * 0.5),
                              child: _RideDataSheet(
                                rec: rec,
                                summary: _sum,
                                expanded: t > 0.04,
                                sharing: _sharing,
                                onShare: _shareImage,
                                onDelete: _delete,
                                onDragUpdate: (dy) =>
                                    _onSheetDragUpdate(dy, h),
                                onDragEnd: _onSheetDragEnd,
                                onHeaderTap: () {
                                  if (_sheet.value < 0.5) {
                                    _sheet.animateTo(
                                      1,
                                      curve: Curves.easeOutCubic,
                                    );
                                  } else {
                                    _sheet.animateTo(
                                      0,
                                      curve: Curves.easeOutCubic,
                                    );
                                  }
                                },
                              ),
                            ),
                          ],
                        ),
                        if (_sharing)
                          Positioned.fill(
                            child: RepaintBoundary(
                              key: _shotKey,
                              child: RidePoster(
                                rec: rec,
                                summary: _sum,
                                map: _mapView(_shareMap, fitPad: _mapPadPeek),
                              ),
                            ),
                          ),
                        if (_sharing)
                          const Positioned.fill(
                            child: ColoredBox(
                              color: Color(0x88000000),
                              child: Center(
                                child: CircularProgressIndicator(),
                              ),
                            ),
                          ),
                      ],
                    );
                  },
                );
              },
            ),
    );
  }

  /// [playable] 为 true 时地图跟着回放进度重建（只有详情页的主地图要）。
  Widget _mapView(
    MapController controller, {
    EdgeInsets fitPad = _mapPad,
    bool playable = false,
  }) {
    if (_disp.isEmpty) {
      return const ColoredBox(
        color: Color(0xFFEDE8DF),
        child: Center(
          child: Icon(Icons.map_outlined, color: AppTheme.muted, size: 48),
        ),
      );
    }
    Widget build({required double? cut}) => LayoutBuilder(
      builder: (context, box) {
        final mapSize = Size(box.maxWidth, box.maxHeight);
        if (mapSize.width < 8 || mapSize.height < 8) {
          return const ColoredBox(color: Color(0xFFEDE8DF));
        }
        final cam = _cameraFor(_disp, mapSize, padding: fitPad);
        return FlutterMap(
          mapController: controller,
          options: MapOptions(
            initialCenter: cam?.center ?? _disp.first,
            initialZoom: cam?.zoom ?? 14,
            minZoom: 3,
            maxZoom: 18,
            interactionOptions: const InteractionOptions(
              flags: InteractiveFlag.all & ~InteractiveFlag.rotate,
            ),
            backgroundColor: const Color(0xFFEDE8DF),
          ),
          children: [
            HelmOsmTiles.layer(
              tileDisplay: const TileDisplay.instantaneous(),
            ),
            ...rideTrackLayers(
              display: _disp,
              summary: _sum,
              wgs: _pts,
              stops: _stops,
              cut: cut,
            ),
          ],
        );
      },
    );

    // 分享封面用完整轨迹，不参与回放。
    if (!playable) return build(cut: null);
    // 只让地图这一层跟着回放重建：整页 60 fps 重建会把面板和榜单一起带上。
    return ListenableBuilder(
      listenable: _play,
      builder: (context, _) => build(cut: _playCut),
    );
  }
}

class _RideDataSheet extends StatelessWidget {
  const _RideDataSheet({
    required this.rec,
    required this.summary,
    required this.expanded,
    required this.sharing,
    required this.onShare,
    required this.onDelete,
    required this.onDragUpdate,
    required this.onDragEnd,
    required this.onHeaderTap,
  });

  final RideRecord rec;
  final GpxSummary? summary;
  final bool expanded;
  final bool sharing;
  final VoidCallback onShare;
  final VoidCallback onDelete;
  final ValueChanged<double> onDragUpdate;
  final ValueChanged<double> onDragEnd;
  final VoidCallback onHeaderTap;

  String get _time {
    if (summary?.duration != null) {
      return GpxUtil.formatHms(summary!.duration!);
    }
    final t = summary?.startTime ??
        (rec.mtime > 0
            ? DateTime.fromMillisecondsSinceEpoch(rec.mtime * 1000)
            : null);
    return t == null ? '' : GpxUtil.formatStamp(t);
  }

  String get _km {
    final km = summary?.distanceKm ?? rec.distanceKm ?? 0;
    if (km < 0.001) return '';
    if (km < 1) {
      return LocaleKeys.rideDistM.trParams({
        'm': (km * 1000).round().toString(),
      });
    }
    return LocaleKeys.rideDistKm.trParams({
      'km': km < 10 ? km.toStringAsFixed(2) : km.toStringAsFixed(1),
    });
  }

  @override
  Widget build(BuildContext context) {
    final time = _time;
    final km = _km;
    return Material(
      color: const Color(0xFF141416),
      elevation: 0,
      shadowColor: Colors.transparent,
      surfaceTintColor: Colors.transparent,
      borderRadius: const BorderRadius.vertical(top: Radius.circular(20)),
      clipBehavior: Clip.antiAlias,
      child: Column(
        children: [
          GestureDetector(
            behavior: HitTestBehavior.opaque,
            onTap: onHeaderTap,
            onVerticalDragUpdate: (d) => onDragUpdate(d.delta.dy),
            onVerticalDragEnd: (d) => onDragEnd(d.primaryVelocity ?? 0),
            child: Padding(
              padding: EdgeInsets.fromLTRB(
                16,
                8,
                16,
                expanded ? 12 : 12 + MediaQuery.paddingOf(context).bottom,
              ),
              child: Column(
                children: [
                  Container(
                    width: 36,
                    height: 4,
                    decoration: BoxDecoration(
                      color: const Color(0xFF5C5C5E),
                      borderRadius: BorderRadius.circular(2),
                    ),
                  ),
                  const SizedBox(height: 10),
                  Text(
                    [
                      if (time.isNotEmpty) time,
                      if (km.isNotEmpty) km,
                    ].join('  ·  '),
                    maxLines: 1,
                    overflow: TextOverflow.ellipsis,
                    style: const TextStyle(
                      color: Colors.white,
                      fontSize: 16,
                      fontWeight: FontWeight.w700,
                      height: 1.2,
                    ),
                  ),
                ],
              ),
            ),
          ),
          if (expanded)
            Expanded(
              child: ListView(
                padding: const EdgeInsets.fromLTRB(12, 0, 12, 28),
                children: [
                  _SummaryCard(rec: rec, summary: summary),
                  // 没有心率也把卡片留着，卡里会写清「本次无心率」——
                  // 整块消失会让人以为是界面漏了。
                  const SizedBox(height: 12),
                  _HrCard(summary: summary),
                  const SizedBox(height: 12),
                  _SpeedCard(summary: summary),
                  const SizedBox(height: 16),
                  Row(
                    children: [
                      Expanded(
                        child: FilledButton.icon(
                          onPressed: sharing ? null : onShare,
                          icon: const Icon(Icons.image_outlined, size: 18),
                          label: Text(
                            sharing
                                ? LocaleKeys.rideSharePreparing.tr
                                : LocaleKeys.rideShareImage.tr,
                          ),
                        ),
                      ),
                      const SizedBox(width: 10),
                      Expanded(
                        child: FilledButton.icon(
                          onPressed: sharing ? null : onDelete,
                          style: FilledButton.styleFrom(
                            backgroundColor: const Color(0xFF3A1C1C),
                            foregroundColor: const Color(0xFFEF4444),
                          ),
                          icon: const Icon(Icons.delete_outline, size: 18),
                          label: Text(LocaleKeys.rideDelete.tr),
                        ),
                      ),
                    ],
                  ),
                  const SizedBox(height: 20),
                  Center(
                    child: Text(
                      LocaleKeys.appName.tr,
                      style: const TextStyle(
                        color: AppTheme.muted,
                        fontSize: 13,
                        fontWeight: FontWeight.w600,
                      ),
                    ),
                  ),
                ],
              ),
            ),
        ],
      ),
    );
  }
}

class _SummaryCard extends StatelessWidget {
  const _SummaryCard({required this.rec, required this.summary});

  final RideRecord rec;
  final GpxSummary? summary;

  @override
  Widget build(BuildContext context) {
    final km = summary?.distanceKm ?? rec.distanceKm ?? 0;
    final speedVals = summary?.pointSpeedsKmh ?? const <double>[];
    final when = summary?.startTime ??
        (rec.mtime > 0
            ? DateTime.fromMillisecondsSinceEpoch(rec.mtime * 1000)
            : null);
    final kcal = GpxUtil.estimateKcal(
      avgHrBpm: summary?.avgHrBpm,
      duration: summary?.duration,
      distanceKm: km,
    );
    final dist = km < 0.001
        ? '—'
        : km < 1
            ? LocaleKeys.rideDistM.trParams({
                'm': (km * 1000).round().toString(),
              })
            : LocaleKeys.rideDistKm.trParams({
                'km': km < 10 ? km.toStringAsFixed(2) : km.toStringAsFixed(1),
              });

    return HelmCard(
      padding: const EdgeInsets.fromLTRB(18, 16, 18, 16),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text(
            '${LocaleKeys.appName.tr}  |  ${LocaleKeys.rideOutdoor.tr}',
            style: const TextStyle(
              color: AppTheme.muted,
              fontSize: 13,
              fontWeight: FontWeight.w500,
            ),
          ),
          const SizedBox(height: 10),
          Text(
            dist,
            style: const TextStyle(
              color: Colors.white,
              fontSize: 40,
              fontWeight: FontWeight.w800,
              height: 1.05,
            ),
          ),
          if (when != null) ...[
            const SizedBox(height: 6),
            Text(
              GpxUtil.formatStamp(when),
              style: const TextStyle(color: AppTheme.muted, fontSize: 13),
            ),
          ],
          const SizedBox(height: 14),
          // 用逐点速度（和轨迹线同一份序列）+ 同一套归一化区间，
          // 「线」和「条」的同一段速度才会是同一个颜色。
          RidePaceStrip(
            speeds: speedVals,
            range: RideTrackStyle.bandRange(speedVals),
          ),
          const SizedBox(height: 6),
          Row(
            children: [
              Text(
                LocaleKeys.rideSlower.tr,
                style: const TextStyle(color: AppTheme.muted, fontSize: 11),
              ),
              const Spacer(),
              Text(
                LocaleKeys.rideFaster.tr,
                style: const TextStyle(color: AppTheme.muted, fontSize: 11),
              ),
            ],
          ),
          const SizedBox(height: 16),
          Row(
            children: [
              Expanded(
                child: _BigStat(
                  value: summary?.duration == null
                      ? '—'
                      : GpxUtil.formatHms(summary!.duration!),
                  label: LocaleKeys.rideMetricMoveTime.tr,
                ),
              ),
              Expanded(
                child: _BigStat(
                  value: kcal > 0 ? '$kcal kcal' : '—',
                  label: LocaleKeys.rideMetricKcal.tr,
                ),
              ),
              Expanded(
                child: _BigStat(
                  value: summary?.avgSpeedKmh == null
                      ? '—'
                      : '${summary!.avgSpeedKmh!.toStringAsFixed(1)} km/h',
                  label: LocaleKeys.rideMetricAvgSpeed.tr,
                ),
              ),
            ],
          ),
          const SizedBox(height: 16),
          _BigStat(
            value: summary?.avgHrBpm == null
                ? '—'
                : '${summary!.avgHrBpm} BPM',
            label: LocaleKeys.rideMetricAvgHr.tr,
          ),
        ],
      ),
    );
  }
}

class _HrCard extends StatelessWidget {
  const _HrCard({required this.summary});

  final GpxSummary? summary;

  static const _zoneColors = [
    Color(0xFF5B8DEF),
    Color(0xFF3DDC84),
    Color(0xFFFFD54A),
    Color(0xFFFF8A3D),
    Color(0xFFFF4D4D),
  ];

  @override
  Widget build(BuildContext context) {
    final sum = summary;
    if (sum == null || !sum.hasHr) return _empty(context);

    final zones = GpxUtil.hrZoneDurations(
      sum.hrPoints,
      total: sum.duration,
    );
    final maxMs = zones.fold<int>(0, (m, d) => d.inMilliseconds > m ? d.inMilliseconds : m);
    final names = [
      LocaleKeys.rideZoneWarmup.tr,
      LocaleKeys.rideZoneFat.tr,
      LocaleKeys.rideZoneAerobic.tr,
      LocaleKeys.rideZoneAnaerobic.tr,
      LocaleKeys.rideZoneExtreme.tr,
    ];
    final values = [for (final p in sum.hrPoints) p.value];

    return HelmCard(
      padding: const EdgeInsets.fromLTRB(18, 16, 18, 12),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          _CardTitle(
            icon: Icons.favorite,
            color: const Color(0xFFFF4D4D),
            title: '${LocaleKeys.rideHrCard.tr} (BPM)',
          ),
          const SizedBox(height: 12),
          Row(
            children: [
              Expanded(
                child: _InlineStat(
                  value: '${sum.avgHrBpm ?? '—'}',
                  label: LocaleKeys.rideMetricAvgHr.tr,
                ),
              ),
              Expanded(
                child: _InlineStat(
                  value: '${sum.maxHrBpm ?? '—'}',
                  label: LocaleKeys.rideMetricMaxHr.tr,
                ),
              ),
            ],
          ),
          const SizedBox(height: 12),
          SizedBox(
            height: 132,
            width: double.infinity,
            child: RideSeriesChart(
              values: values,
              color: const Color(0xFFFF5C4D),
              avg: sum.avgHrBpm?.toDouble(),
            ),
          ),
          const SizedBox(height: 8),
          for (var i = 0; i < 5; i++)
            RideZoneBar(
              label: names[i],
              color: _zoneColors[i],
              time: zones[i],
              maxMs: maxMs < 1 ? 1 : maxMs,
            ),
        ],
      ),
    );
  }

  /// 本次没有心率：卡片照留，把「为什么没有」说明白，别让人以为数据丢了。
  Widget _empty(BuildContext context) {
    return HelmCard(
      padding: const EdgeInsets.fromLTRB(18, 16, 18, 18),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          _CardTitle(
            icon: Icons.favorite_border,
            color: AppTheme.muted,
            title: '${LocaleKeys.rideHrCard.tr} (BPM)',
          ),
          const SizedBox(height: 12),
          Row(
            children: [
              Expanded(
                child: _InlineStat(
                  value: '—',
                  label: LocaleKeys.rideMetricAvgHr.tr,
                ),
              ),
              Expanded(
                child: _InlineStat(
                  value: '—',
                  label: LocaleKeys.rideMetricMaxHr.tr,
                ),
              ),
            ],
          ),
          const SizedBox(height: 12),
          Text(
            LocaleKeys.rideNoHr.tr,
            style: const TextStyle(
              color: AppTheme.muted,
              fontSize: 13,
              height: 1.4,
            ),
          ),
        ],
      ),
    );
  }
}

class _SpeedCard extends StatelessWidget {
  const _SpeedCard({required this.summary});

  final GpxSummary? summary;

  @override
  Widget build(BuildContext context) {
    final values = [
      for (final p in summary?.speedPoints ?? const <RideSeriesPoint>[]) p.value,
    ];
    final avg = summary?.avgSpeedKmh;
    final chart = values.isEmpty && avg != null ? [avg, avg] : values;

    return HelmCard(
      padding: const EdgeInsets.fromLTRB(18, 16, 18, 16),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          _CardTitle(
            icon: Icons.speed,
            color: const Color(0xFF3DDC84),
            title: '${LocaleKeys.rideSpeedCard.tr} (km/h)',
          ),
          const SizedBox(height: 12),
          Row(
            children: [
              Expanded(
                child: _InlineStat(
                  value: avg == null ? '—' : avg.toStringAsFixed(1),
                  label: LocaleKeys.rideMetricAvgSpeed.tr,
                ),
              ),
              Expanded(
                child: _InlineStat(
                  value: summary?.maxSpeedKmh == null
                      ? '—'
                      : summary!.maxSpeedKmh!.toStringAsFixed(1),
                  label: LocaleKeys.rideMetricMaxSpeed.tr,
                ),
              ),
            ],
          ),
          const SizedBox(height: 12),
          SizedBox(
            height: 132,
            width: double.infinity,
            child: chart.isEmpty
                ? const Center(
                    child: Icon(Icons.show_chart, color: AppTheme.muted, size: 36),
                  )
                : RideSeriesChart(
                    values: chart,
                    color: const Color(0xFF3DDC84),
                    fill: true,
                    avg: avg,
                  ),
          ),
        ],
      ),
    );
  }
}

class _CardTitle extends StatelessWidget {
  const _CardTitle({
    required this.icon,
    required this.color,
    required this.title,
  });

  final IconData icon;
  final Color color;
  final String title;

  @override
  Widget build(BuildContext context) {
    return Row(
      children: [
        Icon(icon, size: 18, color: color),
        const SizedBox(width: 6),
        Text(
          title,
          style: const TextStyle(
            color: Colors.white,
            fontSize: 16,
            fontWeight: FontWeight.w700,
          ),
        ),
      ],
    );
  }
}

class _BigStat extends StatelessWidget {
  const _BigStat({required this.value, required this.label});

  final String value;
  final String label;

  @override
  Widget build(BuildContext context) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Text(
          value,
          maxLines: 1,
          overflow: TextOverflow.ellipsis,
          style: const TextStyle(
            color: Colors.white,
            fontSize: 20,
            fontWeight: FontWeight.w800,
            height: 1.1,
          ),
        ),
        const SizedBox(height: 4),
        Text(
          label,
          style: const TextStyle(color: AppTheme.muted, fontSize: 12),
        ),
      ],
    );
  }
}

class _InlineStat extends StatelessWidget {
  const _InlineStat({required this.value, required this.label});

  final String value;
  final String label;

  @override
  Widget build(BuildContext context) {
    return Row(
      children: [
        Text(
          value,
          style: const TextStyle(
            color: Colors.white,
            fontSize: 26,
            fontWeight: FontWeight.w800,
            height: 1.1,
          ),
        ),
        const SizedBox(width: 8),
        Expanded(
          child: Text(
            label,
            maxLines: 2,
            overflow: TextOverflow.ellipsis,
            style: const TextStyle(
              color: AppTheme.muted,
              fontSize: 13,
              fontWeight: FontWeight.w500,
            ),
          ),
        ),
      ],
    );
  }
}

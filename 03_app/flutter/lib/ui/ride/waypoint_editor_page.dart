import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter_map/flutter_map.dart';
import 'package:get/get.dart';
import 'package:latlong2/latlong.dart';
import 'package:sifli_companion/app/app_theme.dart';
import 'package:sifli_companion/i18n/locale_keys.dart';
import 'package:sifli_companion/location/location_manager.dart';
import 'package:sifli_companion/map/osm_tiles.dart';
import 'package:sifli_companion/ride/gpx_library_store.dart';
import 'package:sifli_companion/ride/gpx_util.dart';
import 'package:sifli_companion/ride/nav_route_store.dart';
import 'package:sifli_companion/ride/road_router.dart';
import 'package:sifli_companion/ui/widgets/helm_chrome.dart';
import 'package:sifli_companion/ui/widgets/helm_map_edit.dart';

/// 地图途经点编辑。`mode=nav` 存坐标点库；`mode=gpx` 沿路网补点后写 GPX。
///
/// 交互：
/// - 点空白地图：吸附道路后加点。若当前选中的不是最后一站，则插在其后。
/// - 点已有钉子：选中。再点同一颗进入「挪点」，下一次点地图即新位置。
/// - 长按钉子：直接进入挪点。
/// - 底栏可改名称、撤销最后一点、保存；上拉列表可拖动排序、改名、删除。
class WaypointEditorPage extends StatefulWidget {
  /// 创建编辑页。
  const WaypointEditorPage({super.key});

  @override
  State<WaypointEditorPage> createState() => _WaypointEditorPageState();
}

class _WaypointEditorPageState extends State<WaypointEditorPage>
    with SingleTickerProviderStateMixin {
  static const _maxNav = 32;
  static const _pathGreen = Color(0xFF22C55E);

  /// 收起时底栏高度：表头 + 名称输入 + 里程。保存放在 AppBar，底栏不再常驻按钮，
  /// 所以这个值要贴着内容给（多出来的都会变成面板底部的一片空白）。
  static const _peekBody = 118.0;

  final _map = MapController();
  final _name = TextEditingController();
  final _stops = <NavStop>[];
  final _listCtrl = ScrollController();
  late final AnimationController _sheet;

  /// 准星当前压住的地理坐标。
  ///
  /// 用 notifier 而不是 `setState`：拖动时它每帧都变，整页重建会连带重建地图，
  /// 卡的正好是拖动本身。
  final _crosshairAt = ValueNotifier<LatLng?>(null);

  /// 地图是否正在被拖动（准星据此下沉）。
  final _mapMoving = ValueNotifier<bool>(false);

  List<LatLng> _routed = const [];
  String _mode = 'nav';
  String? _id;

  /// 保存 / 删记录进行中。只有这两件事锁交互（它们要写盘并离开页面）。
  bool _busy = false;
  bool _dirty = false;
  bool _moving = false;
  int? _selected;

  /// 准星模式：拖动地图，把屏幕中心放到想要的位置再确认。
  bool _crosshair = false;

  /// 在飞的网络工作数与其文案键。>0 时顶部显示带转圈的状态提示。
  int _inflight = 0;
  String _workKey = '';

  /// 路线重建代号：每次重建自增，回来时对不上就丢弃结果。
  ///
  /// 有了它，连点/连续拖排序都不需要靠禁用点击来防重入 —— 直接改，旧结果自己作废，
  /// 用户不会遇到「点了没反应」。
  int _routeGen = 0;

  bool get _isNav => _mode != 'gpx';

  String get _title =>
      _isNav ? LocaleKeys.navRouteEditorTitle.tr : LocaleKeys.gpxEditorTitle.tr;

  List<LatLng> get _points => [for (final s in _stops) s.point];

  List<LatLng> get _line {
    if (!_isNav && _routed.length >= 2) return _routed;
    return _points;
  }

  Color get _accent => _isNav ? AppTheme.accent : _pathGreen;

  @override
  void initState() {
    super.initState();
    _sheet = AnimationController(
      vsync: this,
      duration: const Duration(milliseconds: 280),
    );
    final args = (Get.arguments is Map)
        ? Map<String, Object?>.from(Get.arguments as Map)
        : <String, Object?>{};
    _mode = args['mode'] as String? ?? 'nav';
    _id = args['id'] as String?;
    if (_isNav && _id != null) {
      final rec = NavRouteStore().find(_id!);
      if (rec != null) {
        _name.text = rec.name;
        _stops.addAll(
          rec.stops.map((s) => NavStop(name: s.name, point: s.point)),
        );
        _routed = List<LatLng>.from(_points);
      }
    }
    _name.addListener(_markDirtyFromName);
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!mounted) return;
      if (_stops.isNotEmpty) {
        _fitStops();
      } else {
        _locate();
      }
    });
  }

  void _markDirtyFromName() {
    if (!_dirty) setState(() => _dirty = true);
  }

  @override
  void dispose() {
    _name.removeListener(_markDirtyFromName);
    _name.dispose();
    _listCtrl.dispose();
    _map.dispose();
    _sheet.dispose();
    _crosshairAt.dispose();
    _mapMoving.dispose();
    super.dispose();
  }

  void _touch() {
    _dirty = true;
  }

  /// 记一笔在飞的网络工作。计数而不是布尔：吸附与规划可能同时在飞。
  void _beginWork(String key) {
    _inflight++;
    _workKey = key;
    if (mounted) setState(() {});
  }

  void _endWork() {
    if (_inflight > 0) _inflight--;
    if (_inflight == 0) _workKey = '';
    if (mounted) setState(() {});
  }

  /// 把点击吸到路上。失败时按 [roadSnapKept] / [roadSnapOffline] 说明原因，
  /// 并保留用户点的位置 —— 编辑界面不该因为一次网络失败就吞掉他的操作。
  Future<LatLng> _snapOrKeep(LatLng wgs) async {
    final r = await RoadRouter.instance.snapOrKeep(wgs);
    if (r.kept && mounted) {
      final e = r.error;
      if (e != null) AppTheme.failText('Waypoint', e, r.stack);
      AppTheme.snack(
        _title,
        (e != null ? LocaleKeys.roadSnapOffline : LocaleKeys.roadSnapKept).tr,
      );
    }
    return r.point;
  }

  void _toggleCrosshair() {
    setState(() {
      _crosshair = !_crosshair;
      _moving = false;
    });
    if (_crosshair) {
      // 立刻读一次，别让读数空着等第一次拖动。
      try {
        _crosshairAt.value = HelmOsmTiles.fromDisplay(_map.camera.center);
      } catch (_) {
        _crosshairAt.value = null;
      }
    }
  }

  /// 准星确认：把屏幕中心那一点作为新点。
  Future<void> _placeCrosshair() async {
    LatLng? wgs;
    try {
      wgs = HelmOsmTiles.fromDisplay(_map.camera.center);
    } catch (_) {
      wgs = _crosshairAt.value;
    }
    if (wgs == null) return;
    setState(() => _crosshair = false);
    await _addAt(wgs);
  }

  Future<void> _locate() async {
    final pos = await LocationManager().getCurrentPosition();
    if (!mounted || pos == null) return;
    final ll = LatLng(pos.latitude, pos.longitude);
    helmAnimateCamera(_map, HelmOsmTiles.toDisplay(ll), zoom: 15);
  }

  void _fitStops() {
    if (_points.isEmpty) return;
    final disp = HelmOsmTiles.toDisplayAll(_points);
    try {
      if (disp.length == 1) {
        _map.move(disp.first, 15);
        return;
      }
      _map.fitCamera(
        CameraFit.bounds(
          bounds: LatLngBounds.fromPoints(disp),
          padding: const EdgeInsets.all(48),
          maxZoom: 16,
        ),
      );
    } catch (_) {}
  }

  Future<bool> _confirmLeave() async {
    if (!_dirty || (_stops.isEmpty && _name.text.trim().isEmpty)) {
      return true;
    }
    final ok = await Get.dialog<bool>(
      AlertDialog(
        title: Text(LocaleKeys.mapEditorDiscardTitle.tr),
        content: Text(LocaleKeys.mapEditorDiscardBody.tr),
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
    return ok == true;
  }

  int? _hitIndex(LatLng display) {
    try {
      final cam = _map.camera;
      final tap = cam.latLngToScreenOffset(display);
      var best = -1;
      var bestD = 36.0;
      for (var i = 0; i < _points.length; i++) {
        final p = cam.latLngToScreenOffset(HelmOsmTiles.toDisplay(_points[i]));
        final d = (p - tap).distance;
        if (d < bestD) {
          bestD = d;
          best = i;
        }
      }
      return best >= 0 ? best : null;
    } catch (_) {
      return null;
    }
  }

  void _select(int i, {bool move = false}) {
    setState(() {
      _selected = i;
      _moving = move;
    });
  }

  Future<void> _onTap(TapPosition _, LatLng display) async {
    if (_busy) return;
    // 准星模式下点一下地图 = 把准星挪到那里（相机带动准星），比拖更准。
    if (_crosshair) {
      final hit = _hitIndex(display);
      if (hit != null) {
        _select(hit);
        return;
      }
      helmAnimateCamera(_map, display);
      return;
    }
    if (_moving && _selected != null) {
      await _relocate(_selected!, HelmOsmTiles.fromDisplay(display));
      return;
    }
    final hit = _hitIndex(display);
    if (hit != null) {
      _select(hit, move: hit == _selected);
      return;
    }
    await _addAt(HelmOsmTiles.fromDisplay(display));
  }

  Future<void> _onLongPress(TapPosition _, LatLng display) async {
    if (_busy || _moving) return;
    final hit = _hitIndex(display);
    if (hit != null) _select(hit, move: true);
  }

  /// 在地图上加点。
  ///
  /// **先插点、再吸附**。吸附要走公网（实测 OSRM 单次 ~1.6 s），原来等它回来才插点，
  /// 用户点完屏幕上彻底没动静 —— 这就是「选点后像卡顿」的观感来源。现在立刻用点的
  /// 位置把钉子放上去（可以接着操作），吸附只是随后把它挪到路上。
  Future<void> _addAt(LatLng wgs) async {
    if (_isNav && _stops.length >= _maxNav) {
      AppTheme.snack(LocaleKeys.navRoutesTitle.tr, LocaleKeys.navRouteMaxPts.tr);
      return;
    }

    final insertAt = _insertIndex();
    final stop = NavStop(
      name: LocaleKeys.navRouteStopDefault.trParams({
        'n': '${_stops.length + 1}',
      }),
      point: wgs,
    );
    setState(() {
      _stops.insert(insertAt, stop);
      _selected = insertAt;
      _moving = false;
      _touch();
    });

    _beginWork(LocaleKeys.mapEditorSnapping.tr);
    try {
      final snapped = await _snapOrKeep(wgs);
      // 期间可能被撤销 / 删掉了：认对象本身，不认下标。
      if (mounted && _stops.contains(stop)) {
        setState(() => stop.point = snapped);
      }
    } finally {
      _endWork();
    }

    await _rebuildRouted();
  }

  int _insertIndex() {
    final sel = _selected;
    if (sel == null || sel >= _stops.length - 1) return _stops.length;
    return sel + 1;
  }

  Future<void> _relocate(int i, LatLng wgs) async {
    if (i < 0 || i >= _stops.length) return;
    // 挪位置同样是先落点再吸附：用户要立刻看到「放哪儿了」。
    final stop = _stops[i];
    setState(() {
      stop.point = wgs;
      _moving = false;
      _touch();
    });

    _beginWork(LocaleKeys.mapEditorSnapping.tr);
    try {
      final snapped = await _snapOrKeep(wgs);
      if (mounted && _stops.contains(stop)) {
        setState(() => stop.point = snapped);
      }
    } finally {
      _endWork();
    }

    await _rebuildRouted();
  }

  /// 按当前锚点重建沿路折线（gpx 模式）。
  ///
  /// 各段并行、结果带缓存，通常只等最慢的一段。代号对不上就丢结果：连点或连续
  /// 拖排序时，先发的旧结果不会覆盖新状态。
  Future<void> _rebuildRouted() async {
    if (_isNav || _stops.length < 2) {
      // 也要自增：作废可能还在飞的那次重建，否则它会用旧锚点把折线写回来。
      _routeGen++;
      _routed = List<LatLng>.from(_points);
      if (mounted) setState(() {});
      return;
    }

    final gen = ++_routeGen;
    _beginWork(LocaleKeys.mapEditorRouting.tr);
    try {
      final pts = await RoadRouter.instance.routeAnchors(_points);
      if (!mounted || gen != _routeGen) return;
      setState(() {
        _routed = pts;
        if (_routed.length >= 2) {
          _stops.first.point = _routed.first;
          _stops.last.point = _routed.last;
        }
      });
    } catch (e, st) {
      if (!mounted || gen != _routeGen) return;
      AppTheme.failText('Waypoint', e, st);
      AppTheme.snack(LocaleKeys.gpxEditorTitle.tr, LocaleKeys.roadRouteFail.tr);
    } finally {
      _endWork();
    }
  }

  Future<void> _undo() async {
    if (_busy || _stops.isEmpty) return;
    setState(() {
      _stops.removeLast();
      _selected = _stops.isEmpty ? null : _stops.length - 1;
      _moving = false;
      _touch();
    });
    await _rebuildRouted();
  }

  Future<void> _removeAt(int i) async {
    if (_busy || i < 0 || i >= _stops.length) return;
    setState(() {
      _stops.removeAt(i);
      if (_stops.isEmpty) {
        _selected = null;
      } else if (_selected != null) {
        if (_selected! >= _stops.length) {
          _selected = _stops.length - 1;
        } else if (_selected! > i) {
          _selected = _selected! - 1;
        }
      }
      _moving = false;
      _touch();
    });
    await _rebuildRouted();
  }

  Future<void> _reorder(int oldIndex, int newIndex) async {
    if (_busy) return;
    setState(() {
      var to = newIndex;
      if (to > oldIndex) to--;
      final item = _stops.removeAt(oldIndex);
      _stops.insert(to, item);
      _selected = to;
      _moving = false;
      _touch();
    });
    await _rebuildRouted();
  }

  /// 直接改某个途经点的经纬度。
  ///
  /// 改完走 [._relocate] —— 和在地图上拖动那个点完全是同一条路：导航模式重排
  /// 路线，GPX 模式重算沿路折线，撤销栈也跟着记一笔。
  Future<void> _editStopCoord(int i) async {
    if (_busy || i < 0 || i >= _stops.length) return;
    final next = await showLatLngDialog(
      context,
      title: LocaleKeys.mapEditorCoordTitle.trParams({'n': '${i + 1}'}),
      initial: _stops[i].point,
    );
    if (!mounted || next == null) return;
    // 打字给的是**精确坐标**，不再吸附到道路：吸附是"从地图上点"才需要的便利，
    // 用在这里会让人以为输入没生效（输入 32.123456 却跳到隔壁路口）。
    // 折线本身仍按路网重算，起点稍微离路也没关系。
    setState(() {
      _stops[i].point = next;
      _touch();
    });
    await _rebuildRouted();
  }

  /// 按经纬度**放点**：已知坐标时不用在地图上找位置。
  ///
  /// 和打字改坐标一样，落点就是输入的精确值，不吸附到道路（吸附是"从地图上点"
  /// 才需要的便利；输入的东西被挪走会让人以为没生效）。折线仍按路网重算。
  Future<void> _addByCoord() async {
    if (_isNav && _stops.length >= _maxNav) {
      AppTheme.snack(LocaleKeys.navRoutesTitle.tr, LocaleKeys.navRouteMaxPts.tr);
      return;
    }

    LatLng cur;
    try {
      cur = HelmOsmTiles.fromDisplay(_map.camera.center);
    } catch (_) {
      cur = _crosshairAt.value ?? const LatLng(0, 0);
    }

    final next = await showLatLngDialog(
      context,
      title: LocaleKeys.mapEditorCoordAdd.tr,
      initial: cur,
    );
    if (!mounted || next == null) return;

    final insertAt = _insertIndex();
    final stop = NavStop(
      name: LocaleKeys.navRouteStopDefault.trParams({
        'n': '${_stops.length + 1}',
      }),
      point: next,
    );
    setState(() {
      _stops.insert(insertAt, stop);
      _selected = insertAt;
      _moving = false;
      _touch();
    });
    await _rebuildRouted();
    if (mounted) helmAnimateCamera(_map, HelmOsmTiles.toDisplay(next), zoom: 16);
  }

  Future<void> _renameStop(int i) async {
    if (!_isNav || _busy || i < 0 || i >= _stops.length) return;
    final typed = await HelmPromptDialog.show(
      context,
      title: LocaleKeys.navRouteStopTitle.trParams({'n': '${i + 1}'}),
      cancel: LocaleKeys.cancel.tr,
      confirm: LocaleKeys.confirm.tr,
      label: LocaleKeys.navRouteStopLabel.tr,
      hint: LocaleKeys.navRouteStopHint.tr,
      initial: _stops[i].name,
    );
    if (!mounted || typed == null || typed.isEmpty) return;
    setState(() {
      _stops[i].name = typed;
      _touch();
    });
  }

  void _focusStop(int i) {
    _select(i);
    helmAnimateCamera(_map, HelmOsmTiles.toDisplay(_stops[i].point));
  }

  Future<void> _save() async {
    final name = _name.text.trim();
    // 提示要能照做：原来标题写「保存」、正文只有「名称」，看着像在报字段名。
    if (name.isEmpty) {
      AppTheme.snack(_title, LocaleKeys.mapEditorNeedName.tr);
      return;
    }
    if (_stops.isEmpty || (!_isNav && _stops.length < 2)) {
      AppTheme.snack(
        _title,
        _isNav ? LocaleKeys.navRouteNeedOne.tr : LocaleKeys.navRouteNeedTwo.tr,
      );
      return;
    }
    if (_isNav) {
      for (final s in _stops) {
        if (s.name.trim().isEmpty) {
          AppTheme.snack(_title, LocaleKeys.navRouteStopNeed.tr);
          return;
        }
      }
    }
    setState(() => _busy = true);
    try {
      if (_isNav) {
        final rec = NavRouteRecord(
          id: _id ?? DateTime.now().millisecondsSinceEpoch.toString(),
          name: name,
          stops: [
            for (final s in _stops)
              NavStop(name: s.name.trim(), point: s.point),
          ],
          updatedMs: DateTime.now().millisecondsSinceEpoch,
        );
        await NavRouteStore().upsert(rec);
      } else {
        var track = _routed;
        if (track.length < 2) {
          track = await RoadRouter.instance.routeAnchors(_points);
        }
        if (track.length < 2) {
          throw const RoadRouterException('no route');
        }
        final xml = GpxUtil.writeTrack(name: name, points: track);
        await GpxLibraryStore().putBytes(
          GpxUtil.safeFileName(name),
          utf8.encode(xml),
        );
      }
      _dirty = false;
      if (mounted) Get.back();
    } catch (e, st) {
      if (!mounted) return;
      AppTheme.failText('Waypoint', e, st);
      AppTheme.snack(LocaleKeys.save.tr, LocaleKeys.roadRouteFail.tr);
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  Future<void> _deleteRecord() async {
    if (_id == null) {
      Get.back();
      return;
    }
    final ok = await Get.dialog<bool>(
      AlertDialog(
        title: Text(LocaleKeys.navRouteDelete.tr),
        content: Text(
          LocaleKeys.deleteConfirm.trParams({'name': _name.text.trim()}),
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
    if (ok == true) {
      await NavRouteStore().remove(_id!);
      _dirty = false;
      if (mounted) Get.back();
    }
  }

  String get _hint {
    if (_crosshair) return LocaleKeys.mapEditorCrosshairHint.tr;
    if (_moving) return LocaleKeys.mapEditorMoving.tr;
    final sel = _selected;
    if (sel != null && sel < _stops.length - 1) {
      return LocaleKeys.mapEditorInsertAfter.trParams({'n': '${sel + 1}'});
    }
    return _isNav
        ? LocaleKeys.mapEditorHintNav.tr
        : LocaleKeys.mapEditorHintGpx.tr;
  }

  String get _stats {
    final n = _stops.length;
    final km = GpxUtil.distanceKm(_line);
    final dist = km < 0.001
        ? '—'
        : km < 1
        ? LocaleKeys.rideDistM.trParams({'m': (km * 1000).round().toString()})
        : LocaleKeys.rideDistKm.trParams({
            'km': km < 10 ? km.toStringAsFixed(2) : km.toStringAsFixed(1),
          });
    return LocaleKeys.mapEditorStats.trParams({'n': '$n', 'dist': dist});
  }

  void _onSheetDragUpdate(double dy, double h) {
    final range = h * 0.52 - _peekBody;
    if (range <= 0) return;
    _sheet.value = (_sheet.value - dy / range).clamp(0.0, 1.0);
  }

  void _onSheetDragEnd(double v) {
    if (v < -400) {
      _sheet.animateTo(1, curve: Curves.easeOutCubic);
    } else if (v > 400) {
      _sheet.animateTo(0, curve: Curves.easeOutCubic);
    } else {
      _sheet.animateTo(_sheet.value > 0.45 ? 1 : 0, curve: Curves.easeOutCubic);
    }
  }

  @override
  Widget build(BuildContext context) {
    final center = _points.isNotEmpty
        ? HelmOsmTiles.toDisplay(_points.last)
        : HelmOsmTiles.toDisplay(const LatLng(32.255, 118.32));
    return PopScope(
      canPop: false,
      onPopInvokedWithResult: (didPop, _) async {
        if (didPop) return;
        if (await _confirmLeave() && mounted) Get.back();
      },
      child: Scaffold(
        appBar: AppBar(
          title: Text(
            _isNav
                ? LocaleKeys.navRouteEditorTitle.tr
                : LocaleKeys.gpxEditorTitle.tr,
          ),
          actions: [
            // 有未保存改动时才出现。常年挂一条「保存」等于没有提示：改完才冒出来，
            // 本身就是「你改过了、要不要保存」的信号。
            if (_dirty)
              TextButton.icon(
                onPressed: _busy ? null : _save,
                icon: const SizedBox.square(
                  dimension: 7,
                  child: DecoratedBox(
                    decoration: BoxDecoration(
                      color: AppTheme.accent,
                      shape: BoxShape.circle,
                    ),
                  ),
                ),
                label: Text(LocaleKeys.navRouteSave.tr),
                style: TextButton.styleFrom(
                  foregroundColor: AppTheme.accent,
                  textStyle: const TextStyle(
                    fontSize: 15,
                    fontWeight: FontWeight.w700,
                  ),
                ),
              ),
            if (_isNav && _id != null)
              IconButton(
                tooltip: LocaleKeys.navRouteDelete.tr,
                onPressed: _busy ? null : _deleteRecord,
                icon: const Icon(Icons.delete_outline),
              ),
          ],
        ),
        body: LayoutBuilder(
          builder: (context, box) {
            final h = box.maxHeight;
            return AnimatedBuilder(
              animation: _sheet,
              builder: (context, _) {
                final t = Curves.easeOutCubic.transform(_sheet.value);
                // 面板高度只在这里和 HelmEditSheet 内部各算一次，用的是同一个函数；
                // 准星模式有底栏，高度要多留 footerReserve。
                final sheetH = HelmEditSheet.heightOf(
                  t: t,
                  peekBody: _peekBody,
                  bodyHeight: h,
                  bottomInset: MediaQuery.paddingOf(context).bottom,
                  hasFooter: _crosshair,
                );
                return Stack(
                  children: [
                    Positioned.fill(
                      child: FlutterMap(
                        mapController: _map,
                        options: MapOptions(
                          initialCenter: center,
                          initialZoom: 15,
                          onTap: _busy ? null : _onTap,
                          onLongPress: _busy ? null : _onLongPress,
                          // 准星读数：拖动 / 动画期间实时更新，只喂 notifier，
                          // 不触发整页重建。
                          onPositionChanged: (cam, _) {
                            if (_crosshair) {
                              _crosshairAt.value = HelmOsmTiles.fromDisplay(
                                cam.center,
                              );
                            }
                          },
                          onMapEvent: (e) {
                            // 只有手势才算「用户在拖」；程序化移动不参与，
                            // 否则相机动画时准星会莫名其妙地沉下去。
                            final byUser =
                                e.source != MapEventSource.mapController;
                            if (e is MapEventMoveStart) {
                              _mapMoving.value = byUser;
                            } else if (e is MapEventMoveEnd) {
                              _mapMoving.value = false;
                            }
                          },
                          interactionOptions: const InteractionOptions(
                            flags:
                                InteractiveFlag.all & ~InteractiveFlag.rotate,
                          ),
                          backgroundColor: const Color(0xFFEDE8DF),
                        ),
                        children: [
                          HelmOsmTiles.layer(),
                          if (_line.length >= 2)
                            PolylineLayer(
                              polylines: [
                                Polyline(
                                  points: HelmOsmTiles.toDisplayAll(_line),
                                  color: Colors.white.withValues(alpha: 0.9),
                                  strokeWidth: 8,
                                ),
                                Polyline(
                                  points: HelmOsmTiles.toDisplayAll(_line),
                                  color: _accent,
                                  strokeWidth: 5,
                                ),
                              ],
                            ),
                          MarkerLayer(
                            markers: [
                              for (var i = 0; i < _points.length; i++)
                                Marker(
                                  point: HelmOsmTiles.toDisplay(_points[i]),
                                  width: _selected == i ? 40 : 32,
                                  height: _selected == i ? 40 : 32,
                                  child: IgnorePointer(
                                    ignoring: _moving || _busy,
                                    child: GestureDetector(
                                      onTap: () =>
                                          _select(i, move: _selected == i),
                                      onLongPress: () => _select(i, move: true),
                                      child: HelmMapPin(
                                        index: i + 1,
                                        selected: _selected == i,
                                        isStart: i == 0,
                                        isEnd:
                                            i == _points.length - 1 &&
                                            _points.length > 1,
                                        accent: _accent,
                                      ),
                                    ),
                                  ),
                                ),
                            ],
                          ),
                        ],
                      ),
                    ),
                    // 准星层：钉尖与十字交点严格落在屏幕中心，压在地图上不挡手势。
                    if (_crosshair)
                      Positioned.fill(
                        child: IgnorePointer(
                          child: Stack(
                            children: [
                              Center(
                                child: ValueListenableBuilder<bool>(
                                  valueListenable: _mapMoving,
                                  builder: (context, moving, _) =>
                                      HelmMapCrosshair(active: moving),
                                ),
                              ),
                              Align(
                                alignment: const Alignment(0, 0.34),
                                child: ValueListenableBuilder<LatLng?>(
                                  valueListenable: _crosshairAt,
                                  builder: (context, at, _) => at == null
                                      ? const SizedBox.shrink()
                                      : HelmMapHintChip(
                                          text:
                                              '${at.latitude.toStringAsFixed(5)}, '
                                              '${at.longitude.toStringAsFixed(5)}',
                                        ),
                                ),
                              ),
                            ],
                          ),
                        ),
                      ),
                    Positioned(
                      top: 10,
                      left: 16,
                      right: 16,
                      child: HelmMapHintChip(
                        text: _inflight > 0 ? _workKey : _hint,
                        emphasis: _inflight == 0 && _moving,
                        busy: _inflight > 0,
                      ),
                    ),
                    Positioned(
                      right: 12,
                      bottom: sheetH + 12,
                      child: Column(
                        children: [
                          HelmMapRoundBtn(
                            tooltip: LocaleKeys.mapEditorCrosshair.tr,
                            icon: Icons.center_focus_strong,
                            active: _crosshair,
                            onPressed: _busy ? null : _toggleCrosshair,
                          ),
                          const SizedBox(height: 10),
                          HelmMapRoundBtn(
                            tooltip: LocaleKeys.mapEditorLocate.tr,
                            icon: Icons.my_location,
                            onPressed: _busy ? null : _locate,
                          ),
                          const SizedBox(height: 10),
                          HelmMapRoundBtn(
                            tooltip: LocaleKeys.mapEditorCoordAdd.tr,
                            icon: Icons.add_location_alt_outlined,
                            onPressed: _busy ? null : _addByCoord,
                          ),
                          const SizedBox(height: 10),
                          // 挪位置是一种「地图模式」，和准星一样放在地图上比塞进底栏
                          // 更容易懂：点亮即进入，再点一次退出。
                          HelmMapRoundBtn(
                            tooltip: LocaleKeys.mapEditorMove.tr,
                            icon: Icons.open_with,
                            active: _moving,
                            onPressed: _busy || _selected == null
                                ? null
                                : () => _select(_selected!, move: !_moving),
                          ),
                          const SizedBox(height: 10),
                          HelmMapRoundBtn(
                            tooltip: LocaleKeys.mapEditorUndo.tr,
                            icon: Icons.undo,
                            onPressed: _busy || _stops.isEmpty ? null : _undo,
                          ),
                        ],
                      ),
                    ),
                    Positioned(
                      left: 0,
                      right: 0,
                      bottom: 0,
                      child: HelmEditSheet(
                        t: t,
                        peekBody: _peekBody,
                        maxHeight: h,
                        onHeaderTap: () {
                          if (_sheet.value < 0.5) {
                            _sheet.animateTo(1, curve: Curves.easeOutCubic);
                          } else {
                            _sheet.animateTo(0, curve: Curves.easeOutCubic);
                          }
                        },
                        onDragDelta: (dy) => _onSheetDragUpdate(dy, h),
                        onDragEnd: _onSheetDragEnd,
                        peek: Column(
                          crossAxisAlignment: CrossAxisAlignment.stretch,
                          children: [
                            TextField(
                              controller: _name,
                              enabled: !_busy,
                              textInputAction: TextInputAction.done,
                              decoration: InputDecoration(
                                hintText: _isNav
                                    ? LocaleKeys.navRoutesNameHint.tr
                                    : LocaleKeys.gpxEditorNameHint.tr,
                                labelText: LocaleKeys.navRoutesName.tr,
                              ),
                            ),
                            const SizedBox(height: 8),
                            Text(
                              _stats,
                              style: const TextStyle(
                                color: AppTheme.muted,
                                fontSize: 13,
                                fontWeight: FontWeight.w600,
                              ),
                            ),
                          ],
                        ),
                        list: _stops.isEmpty
                            ? Center(
                                child: Text(
                                  LocaleKeys.mapEditorEmptyStops.tr,
                                  style: const TextStyle(color: AppTheme.muted),
                                ),
                              )
                            : ReorderableListView.builder(
                                scrollController: _listCtrl,
                                padding: const EdgeInsets.fromLTRB(8, 4, 8, 8),
                                itemCount: _stops.length,
                                onReorder: (a, b) {
                                  if (!_busy) _reorder(a, b);
                                },
                                proxyDecorator: (child, i, a) {
                                  return Material(
                                    color: AppTheme.field,
                                    borderRadius: BorderRadius.circular(12),
                                    child: child,
                                  );
                                },
                                itemBuilder: (context, i) {
                                  final s = _stops[i];
                                  final on = _selected == i;
                                  final title =
                                      _isNav && s.name.trim().isNotEmpty
                                      ? s.name
                                      : LocaleKeys.mapEditorStopN.trParams({
                                          'n': '${i + 1}',
                                        });
                                  return Material(
                                    key: ObjectKey(s),
                                    color: on
                                        ? AppTheme.field
                                        : Colors.transparent,
                                    borderRadius: BorderRadius.circular(12),
                                    child: ListTile(
                                      onTap: () => _focusStop(i),
                                      leading: ReorderableDragStartListener(
                                        index: i,
                                        child: HelmMapPin(
                                          index: i + 1,
                                          selected: on,
                                          isStart: i == 0,
                                          isEnd:
                                              i == _stops.length - 1 &&
                                              _stops.length > 1,
                                          accent: _accent,
                                          size: 28,
                                        ),
                                      ),
                                      title: Text(title),
                                      subtitle: Text(
                                        '${s.point.latitude.toStringAsFixed(5)}, '
                                        '${s.point.longitude.toStringAsFixed(5)}',
                                      ),
                                      trailing: Row(
                                        mainAxisSize: MainAxisSize.min,
                                        children: [
                                          IconButton(
                                            tooltip:
                                                LocaleKeys.mapEditorCoordEdit.tr,
                                            onPressed: _busy
                                                ? null
                                                : () => _editStopCoord(i),
                                            icon: const Icon(
                                              Icons.edit_location_alt_outlined,
                                              size: 20,
                                            ),
                                          ),
                                          if (_isNav)
                                            IconButton(
                                              tooltip: LocaleKeys
                                                  .navRouteStopLabel
                                                  .tr,
                                              onPressed: _busy
                                                  ? null
                                                  : () => _renameStop(i),
                                              icon: const Icon(
                                                Icons.edit_outlined,
                                                size: 20,
                                              ),
                                            ),
                                          IconButton(
                                            tooltip:
                                                LocaleKeys.mapEditorRemove.tr,
                                            onPressed: _busy
                                                ? null
                                                : () => _removeAt(i),
                                            icon: const Icon(
                                              Icons.close,
                                              size: 20,
                                            ),
                                          ),
                                        ],
                                      ),
                                    ),
                                  );
                                },
                              ),
                        // 底栏只在准星模式下用：那时需要一个明确的「确认放这里」。
                        // 平时没有底栏 —— 保存和撤销都在别处，留一排大按钮只是占地方。
                        footer: _crosshair
                            ? Row(
                                children: [
                                  OutlinedButton(
                                    onPressed: _toggleCrosshair,
                                    child: Text(LocaleKeys.cancel.tr),
                                  ),
                                  const SizedBox(width: 10),
                                  Expanded(
                                    child: FilledButton(
                                      onPressed: _placeCrosshair,
                                      child: Text(
                                        LocaleKeys.mapEditorPlaceHere.tr,
                                      ),
                                    ),
                                  ),
                                ],
                              )
                            : null,
                      ),
                    ),
                  ],
                );
              },
            );
          },
        ),
      ),
    );
  }
}

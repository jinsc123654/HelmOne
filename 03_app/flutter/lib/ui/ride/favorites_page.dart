import 'package:flutter/material.dart';
import 'package:flutter_map/flutter_map.dart';
import 'package:get/get.dart';
import 'package:latlong2/latlong.dart';
import 'package:sifli_companion/app/app_theme.dart';
import 'package:sifli_companion/ble/companion_client.dart';
import 'package:sifli_companion/i18n/locale_keys.dart';
import 'package:sifli_companion/location/location_manager.dart';
import 'package:sifli_companion/map/osm_tiles.dart';
import 'package:sifli_companion/ride/favorite_store.dart';
import 'package:sifli_companion/ride/mcu_sync.dart';
import 'package:sifli_companion/ride/road_router.dart';
import 'package:sifli_companion/transfer/transfer_center.dart';
import 'package:sifli_companion/ui/widgets/helm_chrome.dart';
import 'package:sifli_companion/ui/widgets/helm_map_edit.dart';

/// 常用点点池。同步后覆盖码表常用点文件。
class FavoritesPage extends StatefulWidget {
  /// 创建常用点页。
  const FavoritesPage({super.key});

  @override
  State<FavoritesPage> createState() => _FavoritesPageState();
}

class _FavoritesPageState extends State<FavoritesPage> {
  final _store = FavoriteStore();
  bool _busy = false;

  /// 任务类别（跨页面查重入用）。
  static const _kind = 'fav_push';

  Future<void> _reload() async {
    await _store.load();
    if (mounted) setState(() {});
  }

  @override
  void initState() {
    super.initState();
    _reload();
  }

  Future<void> _edit({FavoritePlace? existing}) async {
    if (!mounted) return;
    final picked = await Navigator.of(context).push<_PickedFav>(
      MaterialPageRoute(builder: (_) => _PickPointPage(existing: existing)),
    );
    if (picked == null) return;

    await _store.upsert(
      FavoritePlace(
        id: existing?.id ?? DateTime.now().millisecondsSinceEpoch.toString(),
        name: picked.name,
        point: picked.point,
      ),
    );
    await _reload();
    AppTheme.snack(LocaleKeys.favsTitle.tr, LocaleKeys.favsSaved.tr);
  }

  bool _needBle() {
    if (CompanionClient.instance.isReady) {
      if (CompanionClient.instance.lastStatus?.isTransferBusy == true) {
        AppTheme.snack(LocaleKeys.favsTitle.tr, LocaleKeys.deviceMtpBusy.tr);
        return true;
      }
      return false;
    }
    AppTheme.snack(LocaleKeys.favsTitle.tr, LocaleKeys.gpxNeedBle.tr);
    return true;
  }

  Future<void> _push() async {
    final center = TransferCenter.to;
    if (_busy || center.isKindRunning(_kind) || _needBle()) {
      return;
    }
    setState(() => _busy = true);
    final task = center.begin(title: LocaleKeys.favsTitle.tr, kind: _kind);
    try {
      final n = await McuSync.pushFavorites(
        CompanionClient.instance,
        onProgress: (p) => center.progress(
          task,
          done: p.done,
          total: p.total,
          transferred: p.transferred,
          phase: LocaleKeys.deviceSyncPush.tr,
        ),
      );
      center.finish(task);
      if (!mounted) {
        return;
      }
      AppTheme.snack(
        LocaleKeys.favsTitle.tr,
        LocaleKeys.waypointSyncFavDone.trParams({'n': '$n'}),
      );
    } catch (e, st) {
      center.fail(task, e);
      AppTheme.fail(LocaleKeys.favsTitle.tr, 'FavSync', e, st);
    } finally {
      if (mounted) {
        setState(() => _busy = false);
      }
    }
  }

  Future<void> _delete(FavoritePlace p) async {
    final ok = await Get.dialog<bool>(
      AlertDialog(
        content: Text(LocaleKeys.deleteConfirm.trParams({'name': p.name})),
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
      await _store.remove(p.id);
      await _reload();
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: Text(LocaleKeys.favsTitle.tr),
        actions: [
          IconButton(
            tooltip: LocaleKeys.waypointSync.tr,
            onPressed: _busy ? null : _push,
            icon: _busy
                ? const SizedBox(
                    width: 20,
                    height: 20,
                    child: CircularProgressIndicator(strokeWidth: 2),
                  )
                : const Icon(Icons.sync),
          ),
          IconButton(
            tooltip: LocaleKeys.favsAdd.tr,
            onPressed: () => _edit(),
            icon: const Icon(Icons.add),
          ),
        ],
      ),
      body: ListView(
        padding: const EdgeInsets.fromLTRB(16, 8, 16, 24),
        children: [
          HelmPlaceholderNote(text: LocaleKeys.placeholderMcuPushFav.tr),
          const SizedBox(height: 12),
          if (_store.items.isEmpty)
            HelmCard(
              padding: const EdgeInsets.all(20),
              child: Text(
                LocaleKeys.favsEmpty.tr,
                style: const TextStyle(color: AppTheme.muted),
              ),
            )
          else
            HelmCard(
              child: HelmTileGroup(
                children: [
                  for (final p in _store.items)
                    HelmNavTile(
                      color: AppTheme.iconFav,
                      icon: Icons.star,
                      title: p.name,
                      subtitle:
                          '${p.point.latitude.toStringAsFixed(5)}, '
                          '${p.point.longitude.toStringAsFixed(5)}',
                      onTap: () => _edit(existing: p),
                      trailing: IconButton(
                        tooltip: LocaleKeys.mapEditorRemove.tr,
                        icon: const Icon(Icons.delete_outline),
                        onPressed: () => _delete(p),
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

class _PickedFav {
  const _PickedFav({required this.name, required this.point});

  final String name;
  final LatLng point;
}

class _PickPointPage extends StatefulWidget {
  const _PickPointPage({this.existing});

  final FavoritePlace? existing;

  @override
  State<_PickPointPage> createState() => _PickPointPageState();
}

class _PickPointPageState extends State<_PickPointPage>
    with SingleTickerProviderStateMixin {
  /// 收起时底栏高度：表头 + 名称输入。保存放在 AppBar，底栏不再常驻按钮，
  /// 所以这个值要贴着内容给（多出来的都会变成面板底部的一片空白）。
  static const _peekBody = 90.0;

  /// 落点钉的直径；高度由 [HelmMapDotPin] 的几何决定。
  static const _pinSize = 22.0;

  final _map = MapController();
  final _name = TextEditingController();
  late final AnimationController _sheet;

  /// 准星读数 / 是否正在拖动。拖动时每帧都变，走 notifier 免得整页重建。
  final _crosshairAt = ValueNotifier<LatLng?>(null);
  final _mapMoving = ValueNotifier<bool>(false);

  LatLng _center = const LatLng(31.2304, 121.4737);
  LatLng? _picked;
  bool _dirty = false;
  bool _crosshair = false;
  int _inflight = 0;
  String _workKey = '';

  String get _title => widget.existing == null
      ? LocaleKeys.favsPickMap.tr
      : LocaleKeys.favsEditTitle.tr;

  String get _hint {
    if (_crosshair) return LocaleKeys.mapEditorCrosshairHint.tr;
    return _picked == null
        ? LocaleKeys.favsPickHint.tr
        : LocaleKeys.favsMoveHint.tr;
  }

  @override
  void initState() {
    super.initState();
    _sheet = AnimationController(
      vsync: this,
      duration: const Duration(milliseconds: 280),
    );
    final existing = widget.existing;
    if (existing != null) {
      _center = existing.point;
      _picked = existing.point;
      _name.text = existing.name;
    }
    _name.addListener(_onName);
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!mounted) return;
      final pin = _picked;
      if (pin == null) return;
      helmAnimateCamera(_map, HelmOsmTiles.toDisplay(pin), zoom: 15);
    });
    LocationManager().getCurrentPosition().then((pos) {
      if (!mounted || pos == null) return;
      if (_picked != null) return;
      final ll = LatLng(pos.latitude, pos.longitude);
      setState(() => _center = ll);
      helmAnimateCamera(_map, HelmOsmTiles.toDisplay(ll), zoom: 15);
    });
  }

  void _onName() {
    if (!_dirty) setState(() => _dirty = true);
  }

  @override
  void dispose() {
    _name.removeListener(_onName);
    _name.dispose();
    _map.dispose();
    _sheet.dispose();
    _crosshairAt.dispose();
    _mapMoving.dispose();
    super.dispose();
  }

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

  Future<bool> _confirmLeave() async {
    if (!_dirty) return true;
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

  /// 落点：先把钉子放到点的位置（立刻可见），再吸到最近道路。
  ///
  /// 吸附要走公网（实测 ~1.6 s），等它回来才落点的话，点完屏幕上什么都不会发生 ——
  /// 这正是「选点后像卡顿」的来源。这里也让收藏点和途经点用同一套吸附规则，
  /// 两处选点的行为才一致。
  /// 按经纬度放点：已知坐标时不用在地图上找。
  ///
  /// 和打字改坐标一样，落点就是输入的精确值 —— **不吸附道路**：吸附是"从地图上
  /// 点"才需要的便利，输入的东西被挪走会让人以为没生效。真要走路线时，路网规划
  /// 会自己把端点吸到路上。
  Future<void> _addByCoord() async {
    LatLng cur;
    try {
      cur = HelmOsmTiles.fromDisplay(_map.camera.center);
    } catch (_) {
      cur = _picked ?? _center;
    }
    final next = await showLatLngDialog(
      context,
      title: LocaleKeys.mapEditorCoordAdd.tr,
      initial: cur,
    );
    if (!mounted || next == null) return;
    _setPick(next);
  }

  /// 改当前这个点的经纬度。
  Future<void> _editCoord(LatLng pin) async {
    final next = await showLatLngDialog(
      context,
      title: LocaleKeys.mapEditorCoordEdit.tr,
      initial: pin,
    );
    if (!mounted || next == null) return;
    _setPick(next);
  }

  /// 键盘输入的点：直接落下 + 把镜头带过去（不再走吸附那条路）。
  void _setPick(LatLng wgs) {
    setState(() {
      _picked = wgs;
      _dirty = true;
      _crosshair = false;
    });
    helmAnimateCamera(_map, HelmOsmTiles.toDisplay(wgs), zoom: 16);
  }

  Future<void> _drop(LatLng wgs) async {
    setState(() {
      _picked = wgs;
      _dirty = true;
    });

    _beginWork(LocaleKeys.mapEditorSnapping.tr);
    try {
      final r = await RoadRouter.instance.snapOrKeep(wgs);
      if (!mounted) return;
      if (r.kept) {
        final e = r.error;
        if (e != null) AppTheme.failText('FavPick', e, r.stack);
        AppTheme.snack(
          _title,
          (e != null ? LocaleKeys.roadSnapOffline : LocaleKeys.roadSnapKept).tr,
        );
      }
      setState(() => _picked = r.point);
    } finally {
      _endWork();
    }
  }

  void _toggleCrosshair() {
    setState(() => _crosshair = !_crosshair);
    if (_crosshair) {
      try {
        _crosshairAt.value = HelmOsmTiles.fromDisplay(_map.camera.center);
      } catch (_) {
        _crosshairAt.value = null;
      }
    }
  }

  Future<void> _placeCrosshair() async {
    LatLng? wgs;
    try {
      wgs = HelmOsmTiles.fromDisplay(_map.camera.center);
    } catch (_) {
      wgs = _crosshairAt.value;
    }
    if (wgs == null) return;
    setState(() => _crosshair = false);
    await _drop(wgs);
  }

  Future<void> _useHere() async {
    final pos = await LocationManager().getCurrentPosition();
    if (!mounted) return;
    if (pos == null) {
      AppTheme.snack(_title, LocaleKeys.favsNeedFix.tr);
      return;
    }
    final ll = LatLng(pos.latitude, pos.longitude);
    setState(() => _center = ll);
    helmAnimateCamera(_map, HelmOsmTiles.toDisplay(ll), zoom: 15);
    await _drop(ll);
  }

  void _save() {
    final name = _name.text.trim();
    final pin = _picked;
    if (pin == null) {
      AppTheme.snack(LocaleKeys.favsTitle.tr, LocaleKeys.favsNeedPin.tr);
      return;
    }
    if (name.isEmpty) {
      AppTheme.snack(LocaleKeys.favsTitle.tr, LocaleKeys.favsNeedName.tr);
      return;
    }
    Navigator.of(context).pop(_PickedFav(name: name, point: pin));
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

  /// 展开区里的坐标卡。收起时只留名称输入，展开才看完整读数 —— 拖动才算有目的。
  Widget _coordBlock(LatLng? pin) {
    if (pin == null) {
      return Text(
        LocaleKeys.favsPickHint.tr,
        style: const TextStyle(
          color: AppTheme.muted,
          fontSize: 13,
          height: 1.4,
        ),
      );
    }
    return Column(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Text(
          LocaleKeys.mapPickCoord.tr,
          style: const TextStyle(
            color: AppTheme.muted,
            fontSize: 12,
            fontWeight: FontWeight.w600,
          ),
        ),
        const SizedBox(height: 6),
        // 点坐标直接键盘输入（懒得在地图上拖到某条路口时特别有用）。
        InkWell(
          onTap: () => _editCoord(pin),
          borderRadius: BorderRadius.circular(8),
          child: Padding(
            padding: const EdgeInsets.symmetric(vertical: 2),
            child: Row(
              children: [
                Text(
                  '${pin.latitude.toStringAsFixed(6)}, '
                  '${pin.longitude.toStringAsFixed(6)}',
                  style: const TextStyle(
                    color: Colors.white,
                    fontSize: 17,
                    fontWeight: FontWeight.w700,
                    letterSpacing: 0.4,
                  ),
                ),
                const SizedBox(width: 8),
                const Icon(
                  Icons.edit_location_alt_outlined,
                  size: 18,
                  color: AppTheme.muted,
                ),
              ],
            ),
          ),
        ),
        const SizedBox(height: 12),
        Text(
          LocaleKeys.favsMoveHint.tr,
          style: const TextStyle(
            color: AppTheme.muted,
            fontSize: 13,
            height: 1.4,
          ),
        ),
      ],
    );
  }

  @override
  Widget build(BuildContext context) {
    final pin = _picked;
    return PopScope(
      canPop: false,
      onPopInvokedWithResult: (didPop, _) async {
        if (didPop) return;
        if (await _confirmLeave() && mounted) Navigator.of(context).pop();
      },
      child: Scaffold(
        appBar: AppBar(
          title: Text(_title),
          actions: [
            // 改过（选了点 / 改了名）才出现。常年挂着「保存」等于没有提示。
            if (_dirty)
              TextButton.icon(
                onPressed: _save,
                icon: const SizedBox.square(
                  dimension: 7,
                  child: DecoratedBox(
                    decoration: BoxDecoration(
                      color: AppTheme.accent,
                      shape: BoxShape.circle,
                    ),
                  ),
                ),
                label: Text(LocaleKeys.save.tr),
                style: TextButton.styleFrom(
                  foregroundColor: AppTheme.accent,
                  textStyle: const TextStyle(
                    fontSize: 15,
                    fontWeight: FontWeight.w700,
                  ),
                ),
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
                          initialCenter: HelmOsmTiles.toDisplay(_center),
                          initialZoom: 15,
                          onTap: (_, ll) {
                            // 准星模式下点地图 = 把准星挪过去（相机带动准星）。
                            if (_crosshair) {
                              helmAnimateCamera(_map, ll);
                              return;
                            }
                            _drop(HelmOsmTiles.fromDisplay(ll));
                          },
                          onPositionChanged: (cam, _) {
                            if (_crosshair) {
                              _crosshairAt.value = HelmOsmTiles.fromDisplay(
                                cam.center,
                              );
                            }
                          },
                          onMapEvent: (e) {
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
                          if (pin != null && !_crosshair)
                            MarkerLayer(
                              markers: [
                                Marker(
                                  point: HelmOsmTiles.toDisplay(pin),
                                  width: _pinSize,
                                  height: HelmMapDotPin.heightFor(_pinSize),
                                  alignment: Alignment.bottomCenter,
                                  child: const HelmMapDotPin(
                                    size: _pinSize,
                                  ),
                                ),
                              ],
                            ),
                        ],
                      ),
                    ),
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
                            onPressed: _toggleCrosshair,
                          ),
                          const SizedBox(height: 10),
                          // 「用当前位置」就是选点场景下的定位：直接把钉子放到我这儿。
                          // 单独再放一个只挪镜头的定位钮，在选点页里没什么用。
                          HelmMapRoundBtn(
                            tooltip: LocaleKeys.favsUseHere.tr,
                            icon: Icons.my_location,
                            onPressed: _useHere,
                          ),
                          const SizedBox(height: 10),
                          HelmMapRoundBtn(
                            tooltip: LocaleKeys.mapEditorCoordAdd.tr,
                            icon: Icons.add_location_alt_outlined,
                            onPressed: _addByCoord,
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
                        peek: TextField(
                          controller: _name,
                          textInputAction: TextInputAction.done,
                          onSubmitted: (_) => _save(),
                          decoration: InputDecoration(
                            labelText: LocaleKeys.favsName.tr,
                            hintText: LocaleKeys.favsNameHint.tr,
                          ),
                        ),
                        // 展开区要能滚：面板刚拉开一点点时列表区几乎没有高度，
                        // 固定高度的坐标卡会把面板挤到溢出（真机上是黄黑条纹）。
                        list: SingleChildScrollView(
                          padding: const EdgeInsets.fromLTRB(16, 12, 16, 8),
                          child: _coordBlock(pin),
                        ),
                        // 底栏只在准星模式下用（要一个明确的「确认放这里」）。
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

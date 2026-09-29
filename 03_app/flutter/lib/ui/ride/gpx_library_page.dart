import 'dart:async';

import 'package:file_selector/file_selector.dart';
import 'package:flutter/material.dart';
import 'package:get/get.dart';
import 'package:sifli_companion/app/app_routes.dart';
import 'package:sifli_companion/app/app_theme.dart';
import 'package:sifli_companion/i18n/locale_keys.dart';
import 'package:sifli_companion/ride/gpx_library_store.dart';
import 'package:sifli_companion/ride/gpx_util.dart';
import 'package:sifli_companion/ride/ride_record_store.dart';
import 'package:sifli_companion/ui/ride/import_upload.dart';
import 'package:sifli_companion/ui/ride/ride_track_thumb.dart';
import 'package:sifli_companion/ui/widgets/helm_chrome.dart';

/// 本机 GPX 库：导入文件、地图新建、发到码表。
class GpxLibraryPage extends StatefulWidget {
  /// 创建 GPX 库页。
  const GpxLibraryPage({super.key});

  @override
  State<GpxLibraryPage> createState() => _GpxLibraryPageState();
}

class _GpxLibraryPageState extends State<GpxLibraryPage> {
  final _store = GpxLibraryStore();
  bool _busy = false;

  Future<void> _reload() async {
    await _store.load();
    if (mounted) setState(() {});
    // 缩略轨迹要解析 GPX：放后台补，别让列表等文件 IO。
    unawaited(_hydrate());
  }

  Future<void> _hydrate() async {
    try {
      final changed = await _store.ensurePreviews();
      if (changed && mounted) setState(() {});
    } catch (e, st) {
      AppTheme.failText('GpxLibrary', e, st);
    }
  }

  /// 打开这条 GPX 查看（复用骑行详情页：地图、回放、图表都在那边）。
  void _open(RideRecord rec) {
    Get.toNamed(AppRoutes.rideDetail, arguments: rec.fileName);
  }

  @override
  void initState() {
    super.initState();
    _reload();
  }

  Future<void> _importFile() async {
    // 这里**故意不带 acceptedTypeGroups**。
    //
    // 原来写的是 XTypeGroup(label:'GPX', extensions:['gpx','xml'])，在 Android 上
    // 反而把 GPX 全部锁死：`file_selector_android` 用系统 MimeTypeMap 把扩展名翻
    // 成 MIME（FileSelectorApiImpl.tryConvertExtensionsToMimetypes），`gpx` 没有
    // 注册项 → 返回 null → 被丢弃，只剩 `xml` → `text/xml`。于是 allMimetypes
    // 只剩一项，走 `intent.setType("text/xml")` 这条分支，选择器只认 text/xml；
    // 而系统里 GPX 登记的 MIME 是 application/gpx+xml 或 application/octet-stream，
    // 于是文件变灰、点不动 —— 这就是"选不了 GPX"的原因。
    //
    // 不过滤 + 选完自己校验（GpxUtil.looksLikeGpx），行为可预期且跨机型一致。
    // 同一做法在 companion_fs_browser.dart 的上传入口一直是对的（它本来就没过滤）。
    final file = await openFile();
    if (file == null) return;

    final bytes = await file.readAsBytes();
    final name = file.name.isEmpty ? 'import.gpx' : file.name;

    if (!GpxUtil.looksLikeGpx(bytes)) {
      AppTheme.snack(LocaleKeys.gpxImportFile.tr, LocaleKeys.gpxNotGpx.tr);
      return;
    }

    final rec = await _store.putBytes(name, bytes);
    await _reload();

    // **导入的文件不改名。**
    //
    // 这里的文件是"别处传上来的"（别人的 GPX、轨迹网站导出的路线），文件名本身
    // 就是用户的命名；再拿它的点去查地图/取地名自动改名，只会把人家认得的名字
    // 换成我们不认识的（而且还要联网）。码表拉回来的**骑行记录**才做自动命名
    // ——那条在 `mcu_sync.autoNameRecords()` 里，属于"我们自己记的，没名字"。
    //
    // 仍然保留 `rec`（放进列表后要能立刻选中/导航），只是不再触发取名。
    assert(rec.fileName.isNotEmpty);
  }

  Future<void> _draw() async {
    await Get.toNamed(
      AppRoutes.waypointEditor,
      arguments: {'mode': 'gpx'},
    );
    await _reload();
  }

  Future<void> _upload(RideRecord rec) async {
    setState(() => _busy = true);
    try {
      await ImportUpload.push(
        context: context,
        localPath: rec.localPath,
        defaultName: rec.title,
      );
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  Future<void> _delete(RideRecord rec) async {
    final ok = await Get.dialog<bool>(
      AlertDialog(
        title: Text(LocaleKeys.gpxTitle.tr),
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
    if (ok == true) {
      await _store.remove(rec.fileName);
      await _reload();
    }
  }

  String _fmtTime(int unix) {
    if (unix <= 0) return '';
    final d = DateTime.fromMillisecondsSinceEpoch(unix * 1000);
    final y = d.year.toString().padLeft(4, '0');
    final m = d.month.toString().padLeft(2, '0');
    final day = d.day.toString().padLeft(2, '0');
    final h = d.hour.toString().padLeft(2, '0');
    final min = d.minute.toString().padLeft(2, '0');
    return '$y-$m-$day $h:$min';
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: Text(LocaleKeys.gpxTitle.tr),
        actions: [
          IconButton(
            tooltip: LocaleKeys.gpxImportFile.tr,
            onPressed: _busy ? null : _importFile,
            icon: const Icon(Icons.file_open_outlined),
          ),
          IconButton(
            tooltip: LocaleKeys.gpxNew.tr,
            onPressed: _busy ? null : _draw,
            icon: const Icon(Icons.add),
          ),
        ],
      ),
      body: ListView(
        padding: const EdgeInsets.fromLTRB(16, 8, 16, 24),
        children: [
          if (_store.items.isEmpty)
            HelmCard(
              padding: const EdgeInsets.all(20),
              child: Text(
                LocaleKeys.gpxEmpty.tr,
                style: const TextStyle(color: AppTheme.muted),
              ),
            )
          else
            HelmCard(
              child: HelmTileGroup(
                children: [
                  for (final rec in _store.items)
                    HelmNavTile(
                      color: AppTheme.iconGpx,
                      icon: Icons.timeline,
                      // 左侧直接画这条轨迹：一眼看出是哪条线，而不是一个通用图标。
                      leading: RideTrackThumb(
                        points: rec.preview ?? const [],
                      ),
                      title: rec.title,
                      subtitle: [
                        _fmtTime(rec.mtime),
                        if (rec.durationSec != null && rec.durationSec! > 0)
                          GpxUtil.formatElapsed(
                            Duration(seconds: rec.durationSec!),
                          ),
                        if (rec.distanceKm != null && rec.distanceKm! > 0)
                          LocaleKeys.rideDistance.trParams({
                            'km': rec.distanceKm!.toStringAsFixed(1),
                          }),
                      ].where((s) => s.isNotEmpty).join(' · '),
                      onTap: _busy ? null : () => _open(rec),
                      showChevron: false,
                      trailing: Row(
                        mainAxisSize: MainAxisSize.min,
                        children: [
                          IconButton(
                            tooltip: LocaleKeys.gpxUpload.tr,
                            onPressed: _busy ? null : () => _upload(rec),
                            icon: const Icon(Icons.upload_outlined),
                          ),
                          IconButton(
                            onPressed: _busy ? null : () => _delete(rec),
                            icon: const Icon(Icons.delete_outline),
                          ),
                        ],
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

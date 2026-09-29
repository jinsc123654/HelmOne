import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:get/get.dart';
import 'package:sifli_companion/app/app_routes.dart';
import 'package:sifli_companion/app/app_theme.dart';
import 'package:sifli_companion/ble/companion_client.dart';
import 'package:sifli_companion/i18n/locale_keys.dart';
import 'package:sifli_companion/ride/gpx_util.dart';
import 'package:sifli_companion/ride/mcu_sync.dart';
import 'package:sifli_companion/ride/nav_route_store.dart';
import 'package:sifli_companion/transfer/transfer_center.dart';
import 'package:sifli_companion/ui/ride/import_upload.dart';
import 'package:sifli_companion/ui/widgets/helm_chrome.dart';

/// 坐标点记录列表。同步后覆盖码表 `mtp/navpts/*.tsv`。
class NavRoutesPage extends StatefulWidget {
  /// 创建列表页。
  const NavRoutesPage({super.key});

  @override
  State<NavRoutesPage> createState() => _NavRoutesPageState();
}

class _NavRoutesPageState extends State<NavRoutesPage> {
  final _store = NavRouteStore();
  bool _busy = false;

  /// 任务类别（跨页面查重入用）。
  static const _kind = 'nav_push';

  Future<void> _reload() async {
    await _store.load();
    if (mounted) setState(() {});
  }

  Future<void> _open({String? id}) async {
    await Get.toNamed(
      AppRoutes.waypointEditor,
      arguments: {'mode': 'nav', 'id': id},
    );
    await _reload();
  }

  bool _needBle() {
    if (CompanionClient.instance.isReady) {
      if (CompanionClient.instance.lastStatus?.isTransferBusy == true) {
        AppTheme.snack(
          LocaleKeys.navRoutesTitle.tr,
          LocaleKeys.deviceMtpBusy.tr,
        );
        return true;
      }
      return false;
    }
    AppTheme.snack(LocaleKeys.navRoutesTitle.tr, LocaleKeys.gpxNeedBle.tr);
    return true;
  }

  Future<void> _pushAll() async {
    final center = TransferCenter.to;
    if (_busy || center.isKindRunning(_kind) || _needBle()) {
      return;
    }
    setState(() => _busy = true);
    final task = center.begin(title: LocaleKeys.navRoutesTitle.tr, kind: _kind);
    try {
      final n = await McuSync.pushNavRoutes(
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
        LocaleKeys.navRoutesTitle.tr,
        LocaleKeys.waypointSyncNavDone.trParams({'n': '$n'}),
      );
    } catch (e, st) {
      center.fail(task, e);
      AppTheme.fail(LocaleKeys.navRoutesTitle.tr, 'NavSync', e, st);
    } finally {
      if (mounted) {
        setState(() => _busy = false);
      }
    }
  }

  Future<void> _upload(NavRouteRecord rec) async {
    if (rec.points.length < 2) {
      AppTheme.snack(
        LocaleKeys.navUploadImport.tr,
        LocaleKeys.navUploadNeedPts.tr,
      );
      return;
    }
    setState(() => _busy = true);
    try {
      final xml = GpxUtil.writeRoute(
        name: rec.name,
        points: rec.points,
        labels: [for (final s in rec.stops) s.name],
      );
      await ImportUpload.push(
        context: context,
        bytes: utf8.encode(xml),
        defaultName: rec.name,
      );
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  @override
  void initState() {
    super.initState();
    _reload();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: Text(LocaleKeys.navRoutesTitle.tr),
        actions: [
          IconButton(
            tooltip: LocaleKeys.waypointSync.tr,
            onPressed: _busy ? null : _pushAll,
            icon: _busy
                ? const SizedBox(
                    width: 20,
                    height: 20,
                    child: CircularProgressIndicator(strokeWidth: 2),
                  )
                : const Icon(Icons.sync),
          ),
          IconButton(
            tooltip: LocaleKeys.navRoutesNew.tr,
            onPressed: () => _open(),
            icon: const Icon(Icons.add),
          ),
        ],
      ),
      body: ListView(
        padding: const EdgeInsets.fromLTRB(16, 8, 16, 24),
        children: [
          HelmPlaceholderNote(text: LocaleKeys.placeholderMcuPushNav.tr),
          const SizedBox(height: 12),
          if (_store.items.isEmpty)
            HelmCard(
              padding: const EdgeInsets.all(20),
              child: Text(
                LocaleKeys.navRoutesEmpty.tr,
                style: const TextStyle(color: AppTheme.muted),
              ),
            )
          else
            HelmCard(
              child: HelmTileGroup(
                children: [
                  for (final rec in _store.items)
                    HelmNavTile(
                      color: AppTheme.iconNav,
                      icon: Icons.place_outlined,
                      title: rec.name,
                      subtitle: LocaleKeys.navRoutesCount.trParams({
                        'n': '${rec.points.length}',
                      }),
                      onTap: () => _open(id: rec.id),
                      showChevron: false,
                      trailing: IconButton(
                        tooltip: LocaleKeys.navUploadImport.tr,
                        onPressed: _busy ? null : () => _upload(rec),
                        icon: const Icon(Icons.upload_outlined),
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

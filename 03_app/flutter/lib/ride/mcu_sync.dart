import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:sifli_companion/ble/companion_client.dart';
import 'package:sifli_companion/ble/companion_proto.dart';
import 'package:sifli_companion/log/app_log.dart';
import 'package:sifli_companion/ride/favorite_store.dart';
import 'package:sifli_companion/ride/gpx_util.dart';
import 'package:sifli_companion/ride/nav_route_store.dart';
import 'package:sifli_companion/ride/place_lookup.dart';
import 'package:sifli_companion/ride/ride_record_store.dart';
import 'package:sifli_companion/ride/waypoint_tsv.dart';

/// 设备同步阶段。
enum McuSyncPhase { listing, pull, pushFav, pushNav }

/// 字节进度，供界面画进度条和速度。
class McuSyncProgress {
  const McuSyncProgress({
    required this.phase,
    this.done = 0,
    this.total = 0,
    this.transferred = 0,
  });

  final McuSyncPhase phase;
  final int done;
  final int total;
  final int transferred;
}

/// 码表 LittleFS 上产品用得到的相对路径。
abstract class McuFsPaths {
  static const recordDir = 'mtp/record';
  static const importDir = 'mtp/import';
  static const navptsDir = CompanionFs.navptsDir;
  static const favoritesPath = CompanionFs.favoritesPath;
}

/// 骑行记录拉取、GPX 写入 import/。
class McuSync {
  /// 从 `mtp/record/` 拉新 GPX 到本机。返回新增或更新条数。
  static Future<int> pullRideRecords(
    CompanionClient client, {
    void Function(McuSyncProgress p)? onProgress,
  }) async {
    if (!client.isReady) {
      throw StateError('Companion 未就绪');
    }
    final status = client.lastStatus;
    if (status != null && status.isTransferBusy) {
      throw StateError('USB/MTP 占用中');
    }

    try {
      await client.sendControl(CompanionCtrl.syncPrep);
    } catch (_) {}

    onProgress?.call(const McuSyncProgress(phase: McuSyncPhase.listing));

    List<CompanionFsEntry> list;
    try {
      list = await client.fsList(McuFsPaths.recordDir);
    } catch (_) {
      list = const [];
    }

    final store = RideRecordStore();
    final pending = <CompanionFsEntry>[];
    for (final e in list) {
      if (e.isDir || !e.name.toLowerCase().endsWith('.gpx')) continue;
      final local = store.find(e.name);
      if (local != null &&
          local.remoteSize != null &&
          local.remoteSize == e.size) {
        continue;
      }
      pending.add(e);
    }

    final totalBytes = pending.fold<int>(0, (s, e) => s + e.size);
    var doneBytes = 0;
    var added = 0;
    final pulled = <RideRecord>[];
    onProgress?.call(
      McuSyncProgress(
        phase: McuSyncPhase.pull,
        done: 0,
        total: totalBytes,
      ),
    );

    for (final e in pending) {
      final bytes = await client.fsDownload(
        CompanionFs.joinRel(McuFsPaths.recordDir, e.name),
        length: e.size,
        onProgress: (d, _) {
          onProgress?.call(
            McuSyncProgress(
              phase: McuSyncPhase.pull,
              done: doneBytes + d,
              total: totalBytes,
              transferred: doneBytes + d,
            ),
          );
        },
      );
      final rec = await store.putFile(
        fileName: e.name,
        bytes: bytes,
        remoteSize: e.size,
      );
      pulled.add(rec);
      doneBytes += e.size;
      added++;
      onProgress?.call(
        McuSyncProgress(
          phase: McuSyncPhase.pull,
          done: doneBytes,
          total: totalBytes,
          transferred: doneBytes,
        ),
      );
    }

    // 名字要联网取地名，放到同步之后后台做：别让「文件都拿回来了」这条提示等网络。
    unawaited(autoNameRecords(store, pulled));
    // 顺手把以前取过的粗名字（城市级）重取一遍，换到街道级。
    unawaited(refreshAutoNames(store));
    return added;
  }

  /// 本次运行是否已经重取过地名。一次就够（结果进缓存），别每次同步都全量解析。
  static bool _refreshedThisRun = false;

  /// 把「自动命名」过的老记录重取一遍地名。
  ///
  /// 为什么需要：早先反查只要到城市级（`zoom=16` + `city` 优先级最高），而且离线
  /// 兜底的城市名还会被写进缓存，于是名字永远是「滁州→滁州」，且再也不会刷新。
  /// 现在反查细化到街道 / 绿道，缓存也换了文件名，所以要把老名字重算一遍。
  ///
  /// 两条安全阀：
  /// - 用户手动改过名的（[RideRecord.userRenamed]）永不碰；
  /// - 只认**在线**结果（[PlaceLookup.nameForOnline]）：离线时反查只给城市名，
  ///   拿它把之前取到的细名字盖掉是倒退。
  static Future<void> refreshAutoNames(RideRecordStore store) async {
    if (_refreshedThisRun) return;
    _refreshedThisRun = true;

    for (final rec in store.items) {
      if (rec.userRenamed) continue;
      final old = rec.displayName?.trim() ?? '';
      if (old.isEmpty) continue;
      try {
        final f = File(rec.localPath);
        if (!await f.exists()) continue;
        final sum = GpxUtil.parse(
          utf8.decode(await f.readAsBytes(), allowMalformed: true),
        );
        if (sum.points.length < 2) continue;

        final from =
            await PlaceLookup.instance.nameForOnline(sum.points.first) ??
                PlaceLookup.offlineCity(sum.points.first);
        final to = await PlaceLookup.instance.nameForOnline(sum.points.last) ??
            PlaceLookup.offlineCity(sum.points.last);
        if (from == null && to == null) continue;

        final renamed = TrackAutoName.compose(
          start: sum.startTime,
          from: from,
          to: to,
          first: sum.points.first,
          last: sum.points.last,
        );
        if (renamed == old) continue;
        await store.setAutoName(rec.fileName, renamed);
        await AppLog.i('McuSync', '重取地名 ${rec.fileName}: $old → $renamed');
      } catch (e) {
        await AppLog.w('McuSync', '重取地名 ${rec.fileName} 失败: $e');
      }
    }
  }

  /// 给刚同步下来的记录补「时间 + 起点 → 终点」的展示名。
  ///
  /// 时间取 GPX 里的骑行开始时间（文件自带的那份数据）。地名取不到会退到坐标，
  /// 名字结构不变；用户手动改过名的跳过（[RideRecord.userRenamed]）。
  static Future<void> autoNameRecords(
    RideRecordStore store,
    List<RideRecord> recs,
  ) async {
    for (final rec in recs) {
      if (rec.userRenamed) continue;
      try {
        final f = File(rec.localPath);
        if (!await f.exists()) continue;
        final sum = GpxUtil.parse(
          utf8.decode(await f.readAsBytes(), allowMalformed: true),
        );
        if (sum.points.length < 2) continue;
        final auto = await PlaceLookup.instance.trackName(
          sum.points,
          start: sum.startTime,
        );
        if (auto != null) await store.setAutoName(rec.fileName, auto);
      } catch (e) {
        await AppLog.w('McuSync', '自动命名 ${rec.fileName} 失败: $e');
      }
    }
  }

  /// 把本机 GPX 写到 `mtp/import/`。
  static Future<String> pushImportFile(
    CompanionClient client,
    String localPath,
    String fileName, {
    String? displayName,
    void Function(int done, int total)? onProgress,
  }) async {
    final bytes = await File(localPath).readAsBytes();
    return pushImportBytes(
      client,
      bytes,
      fileName,
      displayName: displayName,
      onProgress: onProgress,
    );
  }

  /// 把 GPX 字节写到 `mtp/import/`，可用新名字，不改本机文件。
  static Future<String> pushImportBytes(
    CompanionClient client,
    List<int> bytes,
    String fileName, {
    String? displayName,
    void Function(int done, int total)? onProgress,
  }) async {
    if (!client.isReady) {
      throw StateError('Companion 未就绪');
    }
    final status = client.lastStatus;
    if (status != null && status.isTransferBusy) {
      throw StateError('USB/MTP 占用中');
    }
    try {
      await client.fsMkdir(McuFsPaths.importDir);
    } catch (_) {}
    var out = bytes;
    final label = displayName?.trim();
    if (label != null && label.isNotEmpty) {
      out = utf8.encode(
        GpxUtil.setName(utf8.decode(bytes, allowMalformed: true), label),
      );
    }
    final name = GpxUtil.safeFileName(fileName);
    await client.fsUpload(
      CompanionFs.joinRel(McuFsPaths.importDir, name),
      out,
      onProgress: onProgress,
    );
    return name;
  }

  static void _ensureReady(CompanionClient client) {
    if (!client.isReady) {
      throw StateError('Companion 未就绪');
    }
    final status = client.lastStatus;
    if (status != null && status.isTransferBusy) {
      throw StateError('USB/MTP 占用中');
    }
  }

  /// 用本机常用点覆盖码表 `/mnt/kv/bicycle_favorites.tsv`。
  static Future<int> pushFavorites(
    CompanionClient client, {
    void Function(McuSyncProgress p)? onProgress,
    int transferred = 0,
  }) async {
    _ensureReady(client);
    final store = FavoriteStore();
    await store.load();
    final pts = [
      for (final p in store.items.take(WaypointTsv.maxPts))
        (name: p.name, point: p.point),
    ];
    var tsv = WaypointTsv.encode(pts);
    if (tsv.isEmpty) {
      tsv = '\n';
    }
    final raw = utf8.encode(tsv);
    onProgress?.call(
      McuSyncProgress(
        phase: McuSyncPhase.pushFav,
        done: 0,
        total: raw.length,
        transferred: transferred,
      ),
    );
    await client.fsUpload(
      McuFsPaths.favoritesPath,
      raw,
      resume: false,
      onProgress: (d, t) {
        onProgress?.call(
          McuSyncProgress(
            phase: McuSyncPhase.pushFav,
            done: d,
            total: t,
            transferred: transferred + d,
          ),
        );
      },
    );
    return pts.length;
  }

  /// 用本机坐标点记录覆盖码表 `mtp/navpts/*.tsv`。
  static Future<int> pushNavRoutes(
    CompanionClient client, {
    void Function(McuSyncProgress p)? onProgress,
    int transferred = 0,
  }) async {
    _ensureReady(client);
    try {
      await client.fsMkdir('mtp');
    } catch (_) {}
    try {
      await client.fsMkdir(McuFsPaths.navptsDir);
    } catch (_) {}

    final store = NavRouteStore();
    await store.load();
    final jobs = <({String name, List<int> bytes})>[];
    final keep = <String>{};
    for (final rec in store.items) {
      if (rec.points.isEmpty) continue;
      var fileName = GpxUtil.safeTsvFileName(rec.name);
      var i = 2;
      while (keep.contains(fileName)) {
        fileName = GpxUtil.safeTsvFileName('${rec.name}_$i');
        i++;
      }
      keep.add(fileName);
      final pts = [
        for (var k = 0; k < rec.stops.length && k < WaypointTsv.maxPts; k++)
          (
            name: rec.stops[k].name.trim().isEmpty
                ? '${k + 1}'
                : rec.stops[k].name,
            point: rec.stops[k].point,
          ),
      ];
      jobs.add((name: fileName, bytes: utf8.encode(WaypointTsv.encode(pts))));
      if (jobs.length >= 32) break;
    }

    final totalBytes = jobs.fold<int>(0, (s, j) => s + j.bytes.length);
    var doneBytes = 0;
    onProgress?.call(
      McuSyncProgress(
        phase: McuSyncPhase.pushNav,
        done: 0,
        total: totalBytes,
        transferred: transferred,
      ),
    );
    for (final j in jobs) {
      await client.fsUpload(
        CompanionFs.joinRel(McuFsPaths.navptsDir, j.name),
        j.bytes,
        resume: false,
        onProgress: (d, _) {
          onProgress?.call(
            McuSyncProgress(
              phase: McuSyncPhase.pushNav,
              done: doneBytes + d,
              total: totalBytes,
              transferred: transferred + doneBytes + d,
            ),
          );
        },
      );
      doneBytes += j.bytes.length;
    }

    List<CompanionFsEntry> listed;
    try {
      listed = await client.fsList(McuFsPaths.navptsDir);
    } catch (_) {
      listed = const [];
    }
    for (final e in listed) {
      if (e.isDir) {
        continue;
      }
      if (!keep.contains(e.name)) {
        try {
          await client.fsDelete(
            CompanionFs.joinRel(McuFsPaths.navptsDir, e.name),
          );
        } catch (_) {}
      }
    }
    return jobs.length;
  }

  /// 下发常用点 + 坐标点记录。
  static Future<({int favs, int routes})> pushWaypoints(
    CompanionClient client, {
    void Function(McuSyncProgress p)? onProgress,
    int transferred = 0,
  }) async {
    var xfer = transferred;
    void tick(McuSyncProgress p) {
      xfer = p.transferred;
      onProgress?.call(p);
    }

    final favs = await pushFavorites(
      client,
      onProgress: tick,
      transferred: xfer,
    );
    final routes = await pushNavRoutes(
      client,
      onProgress: tick,
      transferred: xfer,
    );
    return (favs: favs, routes: routes);
  }
}

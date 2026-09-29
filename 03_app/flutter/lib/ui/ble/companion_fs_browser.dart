import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'package:file_selector/file_selector.dart';
import 'package:flutter/material.dart';
import 'package:get/get.dart';
import 'package:path/path.dart' as p;
import 'package:path_provider/path_provider.dart';
import 'package:share_plus/share_plus.dart';
import 'package:sifli_companion/app/app_theme.dart';
import 'package:sifli_companion/ble/companion_client.dart';
import 'package:sifli_companion/ble/companion_proto.dart';
import 'package:sifli_companion/i18n/locale_keys.dart';
import 'package:sifli_companion/log/app_log.dart';
import 'package:sifli_companion/transfer/transfer_center.dart';
import 'package:sifli_companion/ui/widgets/helm_chrome.dart';
import 'package:sifli_companion/ui/widgets/helm_fs_chrome.dart';
import 'package:sifli_companion/util/byte_format.dart';

/// 码表文件管理器：点进目录、返回上一级；文件提供下载/重命名/移动/删除。
///
/// 全程 GATT `0xFF15` / `0xFF16`。
///
/// 形状与「App 后台缓存」页一致：单行条目（不再是「一行文件 + 一行 4 个按钮」），
/// 行尾「更多」弹操作菜单；进度和速度由全局悬浮框显示，页内只留一行简述。
class CompanionFsBrowser extends StatefulWidget {
  /// 创建文件浏览器。
  const CompanionFsBrowser({
    super.key,
    required this.gatt,
  });

  /// GATT 文件客户端。
  final CompanionClient gatt;

  @override
  State<CompanionFsBrowser> createState() => _CompanionFsBrowserState();
}

class _CompanionFsBrowserState extends State<CompanionFsBrowser> {
  static const _hugeDirs = {'vmap', 'map'};

  String _dir = '';
  List<CompanionFsEntry> _entries = [];
  bool _busy = false;
  String? _status;
  int _xferDone = 0;
  int _xferTotal = 0;
  DateTime? _xferPaintAt;
  DateTime? _xferT0;
  int _xferLogMs = 0;
  StreamSubscription<CompanionConnState>? _gattSub;

  /// 当前操作登记在全局中心的抓取任务；悬浮框靠它显示进度和速度。
  TransferTask? _task;

  /// 任务类别（跨页面查重入用）。
  static const _kind = 'fs';

  bool get _ready => widget.gatt.state == CompanionConnState.ready;

  @override
  void initState() {
    super.initState();
    _gattSub = widget.gatt.stateStream.listen((s) {
      if (!mounted) return;
      setState(() {});
      if (s == CompanionConnState.ready && _entries.isEmpty && !_busy) {
        _refresh();
      }
    });
    if (_ready) {
      _refresh();
    }
  }

  @override
  void dispose() {
    _gattSub?.cancel();
    super.dispose();
  }

  Future<List<CompanionFsEntry>> _fsList(String path) =>
      widget.gatt.fsList(path);

  void _clearXfer() {
    _xferDone = 0;
    _xferTotal = 0;
    _xferPaintAt = null;
    _xferT0 = null;
    _xferLogMs = 0;
  }

  void _onXferProgress(String label, int done, int total) {
    _xferDone = done;
    _xferTotal = total;
    _xferT0 ??= DateTime.now();
    final now = DateTime.now();
    final elapsed = now.difference(_xferT0!).inMilliseconds.clamp(1, 1 << 30);
    final kbs = done * 1000 / elapsed / 1024;
    if (done == total || elapsed - _xferLogMs >= 250) {
      _xferLogMs = elapsed;
      AppLog.i(
        'CompanionFs',
        '$label ${kbs.toStringAsFixed(1)} KB/s  $done/$total',
      );
    }
    // 全局悬浮框：本页自己的 kbs 是「从开始到现在的均值」，中心的
    // 是滑动窗口，更贴近当前速度，所以两边各留各的。
    final task = _task;
    if (task != null) {
      TransferCenter.to.progress(
        task,
        done: done,
        total: total,
        transferred: done,
        phase: label,
      );
    }
    if (done < total &&
        _xferPaintAt != null &&
        now.difference(_xferPaintAt!) < const Duration(milliseconds: 120)) {
      return;
    }
    _xferPaintAt = now;
    if (!mounted) return;
    setState(() => _status = _progressText(label, done, total, kbs));
  }

  String _progressText(String label, int done, int total, [double? kbs]) {
    if (total <= 0) return label;
    final pct = ((done / total) * 100).clamp(0, 100).toStringAsFixed(0);
    final speed = kbs == null ? '' : ' ${kbs.toStringAsFixed(1)} KB/s';
    return '$label ${formatBytes(done)} / ${formatBytes(total)}（$pct%）$speed';
  }

  Future<void> _fsDelete(String path) => widget.gatt.fsDelete(path);

  Future<void> _fsMkdir(String path) => widget.gatt.fsMkdir(path);

  Future<void> _fsRename(String from, String to) =>
      widget.gatt.fsRename(from, to);

  Future<void> _refresh() async {
    if (_busy || !_ready) return;
    setState(() {
      _busy = true;
      _status = null;
    });
    try {
      await _reload();
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  Future<void> _reload() async {
    if (!_ready) return;
    try {
      final entries = await _fsList(_dir);
      entries.sort((a, b) {
        if (a.isDir != b.isDir) return a.isDir ? -1 : 1;
        return a.name.toLowerCase().compareTo(b.name.toLowerCase());
      });
      if (!mounted) return;
      setState(() {
        _entries = entries;
        // 条目数不再挤在状态行里 —— 路径栏下面就是汇总，这里留空。
        _status = null;
      });
    } catch (e) {
      if (mounted) setState(() => _status = _fsErr(e));
    }
  }

  Future<void> _runOp(String doing, Future<void> Function() action) async {
    if (_busy) return;
    final center = TransferCenter.to;
    if (center.isKindRunning(_kind)) return;
    setState(() {
      _busy = true;
      _status = doing;
      _clearXfer();
    });
    final task = center.begin(title: doing, kind: _kind);
    _task = task;
    try {
      await action();
      await _reload();
      center.finish(task);
    } catch (e) {
      center.fail(task, e);
      if (mounted) setState(() => _status = _fsErr(e));
    } finally {
      _task = null;
      if (mounted) {
        setState(() {
          _busy = false;
          _clearXfer();
        });
      }
    }
  }

  String _fsErr(Object e) {
    if (e is CompanionFsException && e.status != 0) {
      return '${e.message}（${CompanionFs.errnoText(e.status)}）';
    }
    return '$e';
  }

  Future<void> _enter(CompanionFsEntry entry) async {
    if (!entry.isDir) return;
    if (_hugeDirs.contains(entry.name)) {
      final ok = await Get.dialog<bool>(
        AlertDialog(
          title: Text(
            LocaleKeys.fsEnterBigTitle.trParams({'name': entry.name}),
          ),
          content: Text(LocaleKeys.fsEnterBigBody.tr),
          actions: [
            TextButton(
              onPressed: () => Get.back(result: false),
              child: Text(LocaleKeys.cancel.tr),
            ),
            FilledButton(
              onPressed: () => Get.back(result: true),
              child: Text(LocaleKeys.fsEnter.tr),
            ),
          ],
        ),
      );
      if (ok != true) return;
    }
    setState(() => _dir = CompanionFs.joinRel(_dir, entry.name));
    await _refresh();
  }

  Future<void> _up() async {
    if (_dir.isEmpty) return;
    setState(() => _dir = CompanionFs.parentRel(_dir));
    await _refresh();
  }

  String _fullRel(CompanionFsEntry e) => CompanionFs.joinRel(_dir, e.name);

  Rect _shareOrigin() {
    final box = context.findRenderObject();
    if (box is RenderBox && box.hasSize) {
      return box.localToGlobal(Offset.zero) & box.size;
    }
    return const Rect.fromLTWH(0, 0, 1, 1);
  }

  Future<void> _download(CompanionFsEntry e) async {
    final center = TransferCenter.to;
    if (center.isKindRunning(_kind)) return;
    final label = LocaleKeys.actionDownload.tr;
    setState(() {
      _busy = true;
      _status = '$label ${e.name}…';
      _clearXfer();
    });
    final task = center.begin(title: '$label ${e.name}', kind: _kind);
    _task = task;
    File? saved;
    var bytes = 0;
    try {
      final data = await widget.gatt.fsDownload(
        _fullRel(e),
        length: e.size > 0 ? e.size : null,
        onProgress: (done, total) => _onXferProgress('$label ${e.name}', done, total),
      );
      bytes = data.length;
      final cache = await getTemporaryDirectory();
      final dir = Directory('${cache.path}/bike_fs');
      await dir.create(recursive: true);
      final file = File('${dir.path}/${e.name}');
      await file.writeAsBytes(data, flush: true);
      saved = file;
      center.finish(task);
      if (mounted) {
        setState(() {
          _status = LocaleKeys.fsDownloaded.trParams({
            'name': e.name,
            'size': formatBytes(bytes),
          });
        });
      }
    } catch (err) {
      center.fail(task, err);
      if (mounted) {
        setState(() {
          _status = LocaleKeys.fsDownloadFailed.trParams({'err': '$err'});
        });
      }
    } finally {
      _task = null;
      if (mounted) {
        setState(() {
          _busy = false;
          _clearXfer();
        });
      }
    }

    if (saved == null || !mounted) return;
    try {
      await SharePlus.instance.share(
        ShareParams(
          files: [XFile(saved.path, name: e.name)],
          title: e.name,
          sharePositionOrigin: _shareOrigin(),
        ),
      );
    } catch (err) {
      if (mounted) {
        setState(() {
          _status = LocaleKeys.fsShareFailed.trParams({'err': '$err'});
        });
      }
    }
  }

  Future<void> _delete(CompanionFsEntry e) async {
    final ok = await helmConfirmDelete(
      context,
      title: LocaleKeys.actionDelete.tr,
      body: e.isDir
          ? LocaleKeys.fsDeleteDirConfirm.trParams({'name': e.name})
          : LocaleKeys.fsDeleteFileConfirm.trParams({
              'name': e.name,
              'size': formatBytes(e.size),
            }),
    );
    if (!ok) return;
    await _runOp(
      '${LocaleKeys.actionDelete.tr} ${e.name}…',
      () => _fsDelete(_fullRel(e)),
    );
    if (mounted && _status == null) {
      AppTheme.snack(
        LocaleKeys.fsTitle.tr,
        LocaleKeys.fsDeleted.trParams({'name': e.name}),
      );
    }
  }

  /// 重命名 / 移动：设备侧是同一个 `rename`，只是提示词和默认值不同。
  Future<void> _renameOrMove(CompanionFsEntry e, {required bool move}) async {
    final current = _fullRel(e);
    final dest = await HelmPromptDialog.show(
      context,
      title: move ? LocaleKeys.fsMoveTitle.tr : LocaleKeys.actionRename.tr,
      cancel: LocaleKeys.cancel.tr,
      confirm: move
          ? LocaleKeys.fsMoveConfirm.tr
          : LocaleKeys.actionRename.tr,
      label: move ? LocaleKeys.fsMoveLabel.tr : LocaleKeys.newNameLabel.tr,
      initial: current,
    );
    if (dest == null || dest.isEmpty || dest == current) return;
    // 重命名只允许改最后一段名字；带斜杠的当成移动处理。
    if (!move && dest.contains('/')) {
      AppTheme.snack(
        LocaleKeys.fsTitle.tr,
        LocaleKeys.invalidName.tr,
      );
      return;
    }
    final target = move ? dest : CompanionFs.joinRel(_dir, dest);
    await _runOp(
      '${move ? LocaleKeys.actionMove.tr : LocaleKeys.actionRename.tr} '
      '${e.name}…',
      () => _fsRename(current, target),
    );
  }

  Future<void> _showActions(CompanionFsEntry e) async {
    await HelmActionSheet.show(
      context,
      title: e.name,
      actions: [
        if (!e.isDir)
          HelmAction(
            icon: Icons.download_outlined,
            label: LocaleKeys.actionDownload.tr,
            onTap: () => _download(e),
          ),
        HelmAction(
          icon: Icons.edit_outlined,
          label: LocaleKeys.actionRename.tr,
          onTap: () => _renameOrMove(e, move: false),
        ),
        HelmAction(
          icon: Icons.drive_file_move_outlined,
          label: LocaleKeys.actionMove.tr,
          onTap: () => _renameOrMove(e, move: true),
        ),
        HelmAction(
          icon: Icons.delete_outline,
          label: LocaleKeys.actionDelete.tr,
          destructive: true,
          onTap: () => _delete(e),
        ),
      ],
    );
  }

  String get _summary {
    if (_entries.isEmpty) return '';
    var bytes = 0;
    for (final e in _entries) {
      if (!e.isDir) bytes += e.size;
    }
    final count = LocaleKeys.itemCount.trParams({'n': '${_entries.length}'});
    final size = LocaleKeys.totalSize.trParams({'size': formatBytes(bytes)});
    return '$count · $size';
  }

  @override
  Widget build(BuildContext context) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        Padding(
          padding: const EdgeInsets.fromLTRB(20, 12, 20, 2),
          child: Text(
            _ready
                ? LocaleKeys.fsRootHint.tr
                : LocaleKeys.fsNotReady.tr,
            style: const TextStyle(color: AppTheme.muted, fontSize: 12),
          ),
        ),
        HelmPathBar(
          path: CompanionFs.displayPath(_dir),
          onUp: _busy || _dir.isEmpty || !_ready ? null : _up,
          summary: _summary,
          actions: [
            IconButton(
              tooltip: LocaleKeys.actionUpload.tr,
              onPressed: _busy || !_ready ? null : _upload,
              icon: const Icon(Icons.upload_file),
            ),
            IconButton(
              tooltip: LocaleKeys.refresh.tr,
              onPressed: _busy || !_ready ? null : _refresh,
              icon: const Icon(Icons.refresh),
            ),
            IconButton(
              tooltip: LocaleKeys.actionNewFolder.tr,
              onPressed: _busy || !_ready ? null : _newFolder,
              icon: const Icon(Icons.create_new_folder_outlined),
            ),
          ],
        ),
        if (_busy)
          LinearProgressIndicator(
            value: _xferTotal > 0
                ? (_xferDone / _xferTotal).clamp(0.0, 1.0)
                : null,
          ),
        if (_status != null)
          Padding(
            padding: const EdgeInsets.fromLTRB(20, 2, 20, 2),
            child: Text(
              _status!,
              maxLines: 3,
              overflow: TextOverflow.ellipsis,
              style: const TextStyle(color: AppTheme.muted, fontSize: 12),
            ),
          ),
        Expanded(
          child: _entries.isEmpty && !_busy
              ? Center(
                  child: Text(
                    _ready ? LocaleKeys.emptyDir.tr : LocaleKeys.fsNotReady.tr,
                    style: const TextStyle(color: AppTheme.muted),
                  ),
                )
              : Padding(
                  padding: const EdgeInsets.fromLTRB(16, 6, 16, 16),
                  child: HelmFileList(
                    count: _entries.length,
                    itemBuilder: (ctx, i) => _entryTile(_entries[i]),
                  ),
                ),
        ),
      ],
    );
  }

  Widget _entryTile(CompanionFsEntry e) {
    final time = _fmtTime(e.mtime);
    return HelmNavTile(
      color: e.isDir ? AppTheme.iconNav : AppTheme.iconFile,
      icon: e.isDir ? Icons.folder_outlined : Icons.insert_drive_file_outlined,
      title: e.name,
      subtitle: e.isDir
          ? LocaleKeys.dirLabel.tr
          : (time.isEmpty
              ? formatBytes(e.size)
              : '${formatBytes(e.size)}  ·  $time'),
      showChevron: false,
      onTap: e.isDir && !_busy ? () => _enter(e) : null,
      trailing: Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          if (e.isDir)
            const Padding(
              padding: EdgeInsets.only(right: 6),
              child: Icon(Icons.chevron_right, color: AppTheme.muted, size: 20),
            ),
          IconButton(
            tooltip: LocaleKeys.actionMore.tr,
            onPressed: _busy ? null : () => _showActions(e),
            icon: const Icon(Icons.more_horiz),
          ),
        ],
      ),
    );
  }

  /// 设备侧 `mtime` 是 UNIX 秒；为 0（设备没给）时干脆不显示，不编一个 1970 出来。
  static String _fmtTime(int mtime) {
    if (mtime <= 0) return '';
    final local =
        DateTime.fromMillisecondsSinceEpoch(mtime * 1000).toLocal();
    String two(int v) => v.toString().padLeft(2, '0');
    return '${local.year}-${two(local.month)}-${two(local.day)} '
        '${two(local.hour)}:${two(local.minute)}';
  }

  Future<void> _newFolder() async {
    final name = await HelmPromptDialog.show(
      context,
      title: LocaleKeys.fsMkdirTitle.tr,
      cancel: LocaleKeys.cancel.tr,
      confirm: LocaleKeys.fsCreate.tr,
      label: LocaleKeys.actionNewFolder.tr,
    );
    if (name == null || name.isEmpty) return;
    await _runOp(
      '${LocaleKeys.fsMkdirTitle.tr} $name…',
      () => _fsMkdir(CompanionFs.joinRel(_dir, name)),
    );
  }

  Future<void> _upload() async {
    final file = await openFile(confirmButtonText: LocaleKeys.actionUpload.tr);
    if (file == null) return;

    final Uint8List bytes;
    try {
      bytes = await file.readAsBytes();
    } catch (e) {
      if (mounted) {
        setState(() {
          _status = LocaleKeys.fsReadFailed.trParams({'err': '$e'});
        });
      }
      return;
    }

    var name = _uploadName(file, bytes);
    if (name.isEmpty || name == '.' || name == '..') {
      setState(() => _status = LocaleKeys.invalidName.tr);
      return;
    }

    if (bytes.length > 2 * 1024 * 1024) {
      if (!mounted) return;
      final ok = await Get.dialog<bool>(
        AlertDialog(
          title: Text(
            LocaleKeys.fsUploadBigTitle.trParams({'name': name}),
          ),
          content: Text(
            LocaleKeys.fsUploadBigBody.trParams({
              'size': formatBytes(bytes.length),
            }),
          ),
          actions: [
            TextButton(
              onPressed: () => Get.back(result: false),
              child: Text(LocaleKeys.cancel.tr),
            ),
            FilledButton(
              onPressed: () => Get.back(result: true),
              child: Text(LocaleKeys.actionUpload.tr),
            ),
          ],
        ),
      );
      if (ok != true) return;
    }

    await _runOp(
      '${LocaleKeys.actionUpload.tr} $name（${formatBytes(bytes.length)}）…',
      () async {
        await widget.gatt.fsUpload(
          CompanionFs.joinRel(_dir, name),
          bytes,
          resume: true,
          onProgress: (done, total) =>
              _onXferProgress('${LocaleKeys.actionUpload.tr} $name', done, total),
        );
      },
    );
  }

  /// Android SAF 常把任意文件的 [XFile.name] 报成 `.bin`，这里只找回原名/原扩展名。
  static const _placeholderExt = {'.bin', '.dat', '.tmp', '.file', '.download'};

  bool _isPlaceholderName(String name) {
    final ext = p.extension(name).toLowerCase();
    final stem = p.basenameWithoutExtension(name).toLowerCase();
    return ext.isEmpty ||
        _placeholderExt.contains(ext) ||
        stem.isEmpty ||
        stem == 'document' ||
        stem == 'content' ||
        stem == 'file' ||
        stem == 'octet-stream';
  }

  String? _extFromMime(String? mime) {
    final m = (mime ?? '').toLowerCase();
    if (m.isEmpty) return null;
    if (m.contains('gpx')) return '.gpx';
    if (m.contains('kml')) return '.kml';
    if (m.contains('json')) return '.json';
    if (m.contains('fit')) return '.fit';
    if (m == 'text/xml' || m == 'application/xml') return '.xml';
    if (m.startsWith('text/')) return '.txt';
    return null;
  }

  String? _extFromPath(String path) {
    if (path.isEmpty) return null;
    var raw = path;
    try {
      raw = Uri.decodeFull(raw);
    } catch (_) {}
    final seg = raw.split('/').last.split('?').first;
    final ext = p.extension(seg).toLowerCase();
    if (ext.isEmpty || _placeholderExt.contains(ext)) return null;
    return ext;
  }

  bool _looksLikeGpx(Uint8List bytes) {
    if (bytes.length < 8) return false;
    var start = 0;
    final limit = bytes.length < 8192 ? bytes.length : 8192;
    if (limit >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb && bytes[2] == 0xbf) {
      start = 3;
    }
    String head;
    if (limit >= 2 && bytes[0] == 0xff && bytes[1] == 0xfe) {
      final chars = <int>[];
      for (var i = 2; i + 1 < limit; i += 2) {
        final c = bytes[i];
        if (c != 0) chars.add(c);
      }
      head = String.fromCharCodes(chars);
    } else {
      head = String.fromCharCodes(bytes.sublist(start, limit));
    }
    final h = head.toLowerCase();
    return h.contains('<gpx') ||
        h.contains('topografix.com/gpx') ||
        h.contains('<trkpt');
  }

  String _uploadName(XFile file, Uint8List bytes) {
    final fromName = file.name.trim().isEmpty ? '' : p.basename(file.name);
    final fromPath = file.path.isEmpty ? '' : p.basename(file.path);

    var name = fromName.isNotEmpty ? fromName : fromPath;
    if (fromPath.isNotEmpty &&
        _isPlaceholderName(name) &&
        !_isPlaceholderName(fromPath)) {
      name = fromPath;
    }
    if (name.isEmpty) name = 'upload';

    if (_isPlaceholderName(name)) {
      // SAF 常把显示名改成 .bin；优先从真实路径/MIME/内容找回原来的类型。
      final recovered = _extFromPath(file.path) ??
          _extFromMime(file.mimeType) ??
          (_looksLikeGpx(bytes) ? '.gpx' : null);
      if (recovered != null) {
        var stem = p.basenameWithoutExtension(name);
        if (stem.isEmpty ||
            stem.toLowerCase() == 'document' ||
            stem.toLowerCase() == 'content' ||
            stem.toLowerCase() == 'file' ||
            stem.toLowerCase() == 'octet-stream') {
          final pathStem = p.basenameWithoutExtension(fromPath);
          stem = pathStem.isNotEmpty ? pathStem : 'upload';
        }
        name = '$stem$recovered';
      }
    }

    AppLog.i(
      'CompanionFs',
      '上传原名 name=${file.name} path=${file.path} mime=${file.mimeType} -> $name',
    );
    return name;
  }
}

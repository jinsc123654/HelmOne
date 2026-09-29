import 'dart:io';

import 'package:flutter/material.dart';
import 'package:get/get.dart';
import 'package:path/path.dart' as p;
import 'package:share_plus/share_plus.dart';
import 'package:sifli_companion/app/app_theme.dart';
import 'package:sifli_companion/cache/app_cache_store.dart';
import 'package:sifli_companion/i18n/locale_keys.dart';
import 'package:sifli_companion/ui/widgets/helm_chrome.dart';
import 'package:sifli_companion/ui/widgets/helm_fs_chrome.dart';
import 'package:sifli_companion/ui/widgets/log_text_view.dart';
import 'package:sifli_companion/util/byte_format.dart';

/// 浏览 App 持久化后台缓存，交互照抄文件管理器。
///
/// 根目录是 [AppCacheStore] 的 `app_cache/`。后续新的缓存类型只要落到这个
/// 根下的子目录，就能在这里看到。
///
/// 形状与「码表文件操作」保持一致：单行条目、行尾「更多」弹操作菜单、
/// 顶部路径栏带条目数与合计大小；预览文本直接复用 [LogTextView]。
class AppCacheBrowserPage extends StatefulWidget {
  /// 创建缓存浏览器。
  const AppCacheBrowserPage({super.key});

  @override
  State<AppCacheBrowserPage> createState() => _AppCacheBrowserPageState();
}

class _AppCacheBrowserPageState extends State<AppCacheBrowserPage> {
  static const _imageExt = {'.png', '.jpg', '.jpeg', '.webp', '.gif', '.bmp'};
  static const _textExt = {
    '.json',
    '.txt',
    '.md',
    '.log',
    '.xml',
    '.csv',
    '.html',
    '.gpx',
    '.tsv',
  };

  final _store = AppCacheStore.instance;

  String _dir = '';
  List<AppCacheEntry> _entries = const [];
  bool _loading = true;
  String? _error;

  @override
  void initState() {
    super.initState();
    _reload();
  }

  Future<void> _reload() async {
    setState(() {
      _loading = true;
      _error = null;
    });
    try {
      final abs = await _store.resolve(_dir);
      final d = Directory(abs);
      if (!await d.exists()) {
        await d.create(recursive: true);
      }
      final items = <AppCacheEntry>[];
      await for (final ent in d.list(followLinks: false)) {
        final name = p.basename(ent.path);
        if (name.startsWith('.')) {
          continue;
        }
        final stat = await ent.stat();
        items.add(
          AppCacheEntry(
            name: name,
            isDir: ent is Directory,
            size: ent is File ? stat.size : 0,
            mtime: stat.modified,
            entity: ent,
          ),
        );
      }
      items.sort((a, b) {
        if (a.isDir != b.isDir) {
          return a.isDir ? -1 : 1;
        }
        return a.name.toLowerCase().compareTo(b.name.toLowerCase());
      });
      if (!mounted) {
        return;
      }
      setState(() {
        _entries = items;
        _loading = false;
      });
    } catch (e, st) {
      if (!mounted) {
        return;
      }
      setState(() {
        _error = AppTheme.failText('AppCache', e, st);
        _loading = false;
      });
    }
  }

  String _rel(AppCacheEntry e) => AppCacheStore.joinRel(_dir, e.name);

  Future<void> _enter(AppCacheEntry e) async {
    if (!e.isDir) return;
    setState(() => _dir = _rel(e));
    await _reload();
  }

  Future<void> _up() async {
    if (_dir.isEmpty) return;
    setState(() => _dir = AppCacheStore.parentRel(_dir));
    await _reload();
  }

  bool _isPreviewable(AppCacheEntry e) {
    if (e.isDir || e.entity is! File) return false;
    final ext = p.extension(e.name).toLowerCase();
    return _imageExt.contains(ext) || _textExt.contains(ext);
  }

  Future<void> _preview(AppCacheEntry e) async {
    if (e.entity is! File) return;
    final file = e.entity as File;
    final ext = p.extension(e.name).toLowerCase();
    if (_imageExt.contains(ext)) {
      await Get.to<void>(() => _CacheImagePage(name: e.name, file: file));
      return;
    }
    await Get.to<void>(() => _CacheTextPage(name: e.name, file: file));
  }

  Future<void> _share(AppCacheEntry e) async {
    if (e.entity is! File) return;
    try {
      await SharePlus.instance.share(
        ShareParams(
          files: [XFile(e.entity.path, name: e.name)],
          subject: e.name,
        ),
      );
    } catch (err, st) {
      AppTheme.fail(LocaleKeys.cacheTitle.tr, 'AppCache', err, st);
    }
  }

  Future<void> _rename(AppCacheEntry e) async {
    final name = await HelmPromptDialog.show(
      context,
      title: LocaleKeys.actionRename.tr,
      cancel: LocaleKeys.cancel.tr,
      confirm: LocaleKeys.actionRename.tr,
      label: LocaleKeys.newNameLabel.tr,
      initial: e.name,
    );
    if (name == null || name.isEmpty || name == e.name) return;
    if (AppCacheStore.sanitizeName(name).isEmpty) {
      AppTheme.snack(LocaleKeys.cacheTitle.tr, LocaleKeys.invalidName.tr);
      return;
    }
    try {
      await _store.renameEntry(_rel(e), name);
    } catch (err, st) {
      AppTheme.fail(LocaleKeys.cacheTitle.tr, 'AppCache', err, st);
      return;
    }
    await _reload();
  }

  Future<void> _delete(AppCacheEntry e) async {
    final ok = await helmConfirmDelete(
      context,
      title: LocaleKeys.actionDelete.tr,
      body: e.isDir
          ? LocaleKeys.cacheDeleteDirConfirm.trParams({'name': e.name})
          : LocaleKeys.cacheDeleteFileConfirm.trParams({
              'name': e.name,
              'size': formatBytes(e.size),
            }),
    );
    if (!ok) return;
    try {
      await _store.deleteEntry(_rel(e));
    } catch (err, st) {
      AppTheme.fail(LocaleKeys.cacheTitle.tr, 'AppCache', err, st);
      return;
    }
    await _reload();
    if (mounted) {
      AppTheme.snack(
        LocaleKeys.cacheTitle.tr,
        LocaleKeys.cacheDeleted.trParams({'name': e.name}),
      );
    }
  }

  Future<void> _clearAll() async {
    if (_dir.isNotEmpty) {
      // 只在根上提供「清空」；子目录里清空会让人误判清了什么。
      return;
    }
    final ok = await helmConfirmDelete(
      context,
      title: LocaleKeys.cacheClearAll.tr,
      body: LocaleKeys.cacheClearAllConfirm.tr,
    );
    if (!ok) return;
    try {
      for (final e in _entries) {
        await _store.deleteEntry(e.name);
      }
    } catch (err, st) {
      AppTheme.fail(LocaleKeys.cacheTitle.tr, 'AppCache', err, st);
    }
    await _reload();
  }

  Future<void> _showActions(AppCacheEntry e) async {
    await HelmActionSheet.show(
      context,
      title: e.name,
      actions: [
        if (!e.isDir && _isPreviewable(e))
          HelmAction(
            icon: Icons.visibility_outlined,
            label: LocaleKeys.actionPreview.tr,
            onTap: () => _preview(e),
          ),
        if (!e.isDir)
          HelmAction(
            icon: Icons.ios_share,
            label: LocaleKeys.actionShare.tr,
            onTap: () => _share(e),
          ),
        HelmAction(
          icon: Icons.edit_outlined,
          label: LocaleKeys.actionRename.tr,
          onTap: () => _rename(e),
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

  int get _totalBytes {
    var n = 0;
    for (final e in _entries) {
      if (!e.isDir) n += e.size;
    }
    return n;
  }

  String get _summary {
    if (_entries.isEmpty) return '';
    final count = LocaleKeys.itemCount.trParams({'n': '${_entries.length}'});
    final size = LocaleKeys.totalSize.trParams({'size': formatBytes(_totalBytes)});
    return '$count · $size';
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: Text(LocaleKeys.cacheTitle.tr),
        actions: [
          if (_dir.isEmpty)
            IconButton(
              tooltip: LocaleKeys.cacheClearAll.tr,
              onPressed: _loading ? null : _clearAll,
              icon: const Icon(Icons.delete_sweep_outlined),
            ),
        ],
      ),
      body: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          Padding(
            padding: const EdgeInsets.fromLTRB(20, 12, 20, 2),
            child: Text(
              LocaleKeys.cacheSub.tr,
              style: const TextStyle(color: AppTheme.muted, fontSize: 12),
            ),
          ),
          HelmPathBar(
            path: AppCacheStore.displayPath(_dir),
            onUp: _dir.isEmpty || _loading ? null : _up,
            summary: _summary,
            actions: [
              IconButton(
                tooltip: LocaleKeys.refresh.tr,
                onPressed: _loading ? null : _reload,
                icon: const Icon(Icons.refresh),
              ),
            ],
          ),
          if (_loading) const LinearProgressIndicator(),
          if (_error != null)
            Padding(
              padding: const EdgeInsets.fromLTRB(20, 4, 20, 4),
              child: Text(
                _error!,
                style: const TextStyle(color: AppTheme.danger, fontSize: 13),
              ),
            ),
          Expanded(
            child: _entries.isEmpty && !_loading
                ? Center(
                    child: Text(
                      LocaleKeys.emptyDir.tr,
                      style: const TextStyle(color: AppTheme.muted),
                    ),
                  )
                : Padding(
                    padding: const EdgeInsets.fromLTRB(16, 8, 16, 16),
                    child: HelmFileList(
                      count: _entries.length,
                      itemBuilder: (ctx, i) => _tile(_entries[i]),
                    ),
                  ),
          ),
        ],
      ),
    );
  }

  Widget _tile(AppCacheEntry e) {
    final ext = p.extension(e.name).toLowerCase();
    final IconData icon;
    if (e.isDir) {
      icon = Icons.folder_outlined;
    } else if (_imageExt.contains(ext)) {
      icon = Icons.image_outlined;
    } else if (_textExt.contains(ext)) {
      icon = Icons.description_outlined;
    } else {
      icon = Icons.insert_drive_file_outlined;
    }

    return HelmNavTile(
      color: e.isDir ? AppTheme.iconCache : AppTheme.iconFile,
      icon: icon,
      title: e.name,
      subtitle: e.isDir
          ? LocaleKeys.dirLabel.tr
          : '${formatBytes(e.size)}  ·  ${_CacheBrowserFmt.time(e.mtime)}',
      showChevron: false,
      onTap: _loading
          ? null
          : () {
              if (e.isDir) {
                _enter(e);
              } else {
                _preview(e);
              }
            },
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
            onPressed: _loading ? null : () => _showActions(e),
            icon: const Icon(Icons.more_horiz),
          ),
        ],
      ),
    );
  }
}

/// 缓存里的文本文件查看页：正文交给 [LogTextView]（保留开头 —— 结构化文件
/// 开头才是重点，不像日志要看结尾）。
class _CacheTextPage extends StatelessWidget {
  const _CacheTextPage({required this.name, required this.file});

  final String name;
  final File file;

  @override
  Widget build(BuildContext context) {
    var size = 0;
    DateTime? mtime;
    try {
      size = file.lengthSync();
      mtime = file.statSync().modified;
    } catch (_) {}

    return Scaffold(
      appBar: AppBar(
        title: Text(name, maxLines: 1, overflow: TextOverflow.ellipsis),
        actions: [
          IconButton(
            onPressed: () => _share(context),
            icon: const Icon(Icons.ios_share),
          ),
        ],
      ),
      body: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          Padding(
            padding: const EdgeInsets.fromLTRB(16, 10, 16, 8),
            child: Text(
              '${formatBytes(size)}  ·  ${_CacheBrowserFmt.time(mtime)}',
              style: const TextStyle(color: AppTheme.muted, fontSize: 12),
            ),
          ),
          const Divider(height: 1, thickness: 0.5, color: AppTheme.hairline),
          Expanded(child: LogTextView(file: file, keepTail: false)),
        ],
      ),
    );
  }

  Future<void> _share(BuildContext context) async {
    try {
      await SharePlus.instance.share(
        ShareParams(files: [XFile(file.path, name: name)], subject: name),
      );
    } catch (err, st) {
      AppTheme.fail(LocaleKeys.cacheTitle.tr, 'AppCache', err, st);
    }
  }
}

/// 缓存里的图片查看页：支持缩放，可分享。
class _CacheImagePage extends StatelessWidget {
  const _CacheImagePage({required this.name, required this.file});

  final String name;
  final File file;

  @override
  Widget build(BuildContext context) {
    var size = 0;
    DateTime? mtime;
    try {
      size = file.lengthSync();
      mtime = file.statSync().modified;
    } catch (_) {}

    return Scaffold(
      appBar: AppBar(
        title: Text(name, maxLines: 1, overflow: TextOverflow.ellipsis),
        actions: [
          IconButton(
            onPressed: () => _share(context),
            icon: const Icon(Icons.ios_share),
          ),
        ],
      ),
      body: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          Padding(
            padding: const EdgeInsets.fromLTRB(16, 10, 16, 8),
            child: Text(
              '${formatBytes(size)}  ·  ${_CacheBrowserFmt.time(mtime)}',
              style: const TextStyle(color: AppTheme.muted, fontSize: 12),
            ),
          ),
          Expanded(
            child: InteractiveViewer(
              maxScale: 6,
              child: Image.file(
                file,
                fit: BoxFit.contain,
                filterQuality: FilterQuality.medium,
                errorBuilder: (context, error, stack) => Center(
                  child: Text(
                    LocaleKeys.cacheImageLoadFailed.tr,
                    style: const TextStyle(color: AppTheme.muted),
                  ),
                ),
              ),
            ),
          ),
        ],
      ),
    );
  }

  Future<void> _share(BuildContext context) async {
    try {
      await SharePlus.instance.share(
        ShareParams(files: [XFile(file.path, name: name)], subject: name),
      );
    } catch (err, st) {
      AppTheme.fail(LocaleKeys.cacheTitle.tr, 'AppCache', err, st);
    }
  }
}

/// 缓存页里复用的时间格式（页面级私有，避免再开一个工具文件）。
abstract final class _CacheBrowserFmt {
  static String time(DateTime? t) {
    if (t == null) return '';
    final local = t.toLocal();
    String two(int v) => v.toString().padLeft(2, '0');
    return '${local.year}-${two(local.month)}-${two(local.day)} '
        '${two(local.hour)}:${two(local.minute)}';
  }
}

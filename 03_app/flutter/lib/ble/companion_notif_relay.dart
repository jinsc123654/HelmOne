import 'dart:async';
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:sifli_companion/app/app_keys.dart';
import 'package:sifli_companion/ble/companion_client.dart';
import 'package:sifli_companion/ble/companion_proto.dart';
import 'package:sifli_companion/ble/notif_icon_cache.dart';
import 'package:sifli_companion/ble/notif_settings.dart';
import 'package:sifli_companion/log/app_log.dart';

/// 系统通知监听是否可用。引导卡片按此决定下一步。
enum NotifAccessState {
  /// 非 Android。
  unsupported,

  /// 系统设置里还没打开通知使用权。
  needsPermission,

  /// 已授权但 listener 没 bind（覆盖安装后常见）。
  needsToggle,

  /// 已授权且已 bind。
  ready,
}

/// Forwards Android notifications to the bike computer over GATT 0xFF17.
///
/// Icons are cached in the App's internal `notif_icons/` folder and
/// refreshed by MD5.  The wire carries only the filename; the device
/// pulls a missing file with `FS_NEED`.
class CompanionNotifRelay {
  CompanionNotifRelay._();

  static final CompanionNotifRelay instance = CompanionNotifRelay._();

  static const _tag = 'CompanionNotif';

  /// 「没转发」的汇总窗口。
  ///
  /// 系统通知是**高频事件**：bilibili 这类推送一分钟能来几十条，而且它们带
  /// `CATEGORY_TRANSPORT`（媒体类），按设计就该被滤掉。原来每一条都写一行 W ——
  /// W 是始终落盘的，于是持久化日志被这些正文冲干净，而通知正文本身也不该进
  /// 持久化日志（用户隐私）。现在只留计数和包名，一个窗口一行。
  static const _dropWindow = Duration(minutes: 5);

  final _dropReasons = <String, int>{};
  final _dropApps = <String, int>{};

  /// 其中「用户想转发、但码表没就绪」而真丢掉的条数：这种要留 W（能被导出）。
  int _dropLost = 0;
  DateTime? _dropSince;
  Timer? _dropTimer;
  static const _method = MethodChannel('com.sifli.sifli_companion/notif');
  static const _events = EventChannel('com.sifli.sifli_companion/notif_events');

  StreamSubscription<dynamic>? _sub;
  bool _running = false;
  Future<void> _inflight = Future.value();

  /// 调试页展示：最近一次系统通知（含被过滤 / 未连接）。
  String lastHeard = '尚未收到系统通知';

  bool get isRunning => _running;

  bool get isSupported => !kIsWeb && Platform.isAndroid;

  /// 让码表 `FS_NEED` 能读到本地缓存（连接后也应调用）。
  void attachIconNeed() {
    CompanionClient.instance.iconNeedLoader = loadBytesForNeed;
    unawaited(cacheOwnIcon());
  }

  /// 本应用包名；非 Android 为空。
  Future<String> ownPackage() async {
    if (!isSupported) return '';
    final v = await _method.invokeMethod<String>('ownPackage');
    return v ?? '';
  }

  /// 把某个应用的图标写入持久化缓存。返回线上文件名。
  Future<String> cacheIconForPackage({
    required String packageName,
    String appName = '',
  }) async {
    final iconName = CompanionNotif.iconFileName(packageName);
    if (iconName.isEmpty) return '';
    await _cacheIcon(packageName, appName, iconName);
    return iconName;
  }

  /// 缓存本应用图标（文件名来自当前 Android 包名）。
  Future<String> cacheOwnIcon() async {
    final pkg = await ownPackage();
    if (pkg.isEmpty) return '';
    return cacheIconForPackage(packageName: pkg, appName: AppKeys.appName);
  }

  Future<bool> isListenerEnabled() async {
    if (!isSupported) return false;
    final v = await _method.invokeMethod<bool>('isEnabled');
    return v ?? false;
  }

  Future<bool> isListenerBound() async {
    if (!isSupported) return false;
    final v = await _method.invokeMethod<bool>('isBound');
    return v ?? false;
  }

  Future<void> requestRebind() async {
    if (!isSupported) return;
    await _method.invokeMethod<void>('requestRebind');
  }

  /// 把当前通知栏里已有的会话拉一遍（listener 不补发旧通知）。
  Future<bool> pullActive() async {
    if (!isSupported) return false;
    final v = await _method.invokeMethod<int>('pullActive');
    return (v ?? 0) != 0;
  }

  Future<void> openSettings() async {
    if (!isSupported) return;
    await _method.invokeMethod<void>('openSettings');
  }

  Future<bool> isPhoneLocked() async {
    if (!isSupported) return false;
    final v = await _method.invokeMethod<bool>('isPhoneLocked');
    return v ?? false;
  }

  /// 本机桌面可启动应用（不含无图标入口的系统包）。
  Future<List<InstalledNotifApp>> listInstalledApps() async {
    if (!isSupported) return const [];
    final raw = await _method.invokeMethod<List<dynamic>>('listInstalledApps');
    if (raw == null) return const [];
    final out = <InstalledNotifApp>[];
    for (final item in raw) {
      if (item is! Map) continue;
      final pkg = '${item['package'] ?? ''}';
      if (pkg.isEmpty) continue;
      final name = '${item['appName'] ?? ''}';
      out.add(
        InstalledNotifApp(
          packageName: pkg,
          appName: name.isEmpty ? pkg : name,
        ),
      );
    }
    return out;
  }

  Future<void> postTest({
    String title = '测试通知',
    String text = '发给码表的一条测试消息',
  }) async {
    if (!isSupported) return;
    await _method.invokeMethod<void>('postTest', {
      'title': title,
      'text': text,
    });
  }

  Future<bool> loadEnabledPref() async {
    await NotifSettings.instance.ensureLoaded();
    return NotifSettings.instance.enabled;
  }

  Future<void> saveEnabledPref(bool enabled) async {
    await NotifSettings.instance.setEnabled(enabled);
  }

  /// 读缓存；没有则按文件名猜测包名再向系统要一次图标。
  Future<Uint8List?> loadBytesForNeed(String fileName) async {
    final cached = await NotifIconCache.instance.readPng(fileName);
    if (cached != null && cached.isNotEmpty) {
      return cached;
    }
    if (!isSupported) {
      return null;
    }
    var pkg = CompanionNotif.wireName(fileName);
    if (pkg.toLowerCase().endsWith('.png')) {
      pkg = pkg.substring(0, pkg.length - 4);
    }
    if (pkg.isEmpty) {
      return null;
    }
    try {
      final bytes = await _method.invokeMethod<Uint8List>('getAppIcon', {
        'package': pkg,
      });
      if (bytes == null || bytes.isEmpty) {
        return null;
      }
      await NotifIconCache.instance.putIfMd5Changed(
        fileName: fileName,
        bytes: bytes,
        packageName: pkg,
        appName: '',
      );
      return bytes;
    } catch (e) {
      await AppLog.w(_tag, '按需取图标失败 $pkg: $e');
      return null;
    }
  }

  /// App 启动后调用：引擎已起来，按偏好订阅系统通知。
  Future<void> boot() async {
    attachIconNeed();
    if (!isSupported) return;
    if (await loadEnabledPref()) {
      await start();
    }
  }

  Future<void> start() async {
    attachIconNeed();
    if (!isSupported) return;
    if (!_running) {
      _running = true;
      _sub = _events.receiveBroadcastStream().listen(
        _enqueueEvent,
        onError: (Object e) => AppLog.w(_tag, '通知流错误: $e'),
      );
      await AppLog.i(_tag, '通知转发已开启');
    }
    await requestRebind();
  }

  /// 引导用：打开 EventChannel、尽量 bind，返回还缺哪一步。
  Future<NotifAccessState> prepareAccess({
    Duration wait = const Duration(seconds: 4),
  }) async {
    if (!isSupported) return NotifAccessState.unsupported;
    if (await loadEnabledPref()) {
      await start();
    } else {
      attachIconNeed();
    }
    if (!await isListenerEnabled()) {
      return NotifAccessState.needsPermission;
    }
    if (await isListenerBound()) {
      return NotifAccessState.ready;
    }
    await requestRebind();
    if (await waitUntilBound(timeout: wait)) {
      return NotifAccessState.ready;
    }
    return NotifAccessState.needsToggle;
  }

  Future<bool> waitUntilBound({
    Duration timeout = const Duration(seconds: 4),
  }) async {
    if (!isSupported) return false;
    final end = DateTime.now().add(timeout);
    while (DateTime.now().isBefore(end)) {
      if (await isListenerBound()) return true;
      await Future<void>.delayed(const Duration(milliseconds: 300));
    }
    return isListenerBound();
  }

  Future<void> stop() async {
    _running = false;
    await _sub?.cancel();
    _sub = null;
    await _flushDrops();
  }

  /// 记一次「没转发」。[lost] 为真表示这是真丢内容（码表未就绪），要留痕。
  void _noteDrop(String reason, String who, {bool lost = false}) {
    _dropSince ??= DateTime.now();
    _dropReasons[reason] = (_dropReasons[reason] ?? 0) + 1;
    if (who.isNotEmpty) {
      _dropApps[who] = (_dropApps[who] ?? 0) + 1;
    }
    if (lost) {
      _dropLost++;
    }
    _dropTimer ??= Timer(_dropWindow, () => unawaited(_flushDrops()));
  }

  /// 把一个窗口内的「没转发」汇总成一行。
  ///
  /// 纯过滤（媒体类通知、自己发的、系统 UI 等）只进控制台：debug 构建能看到，
  /// release 不落盘，日志不被刷；因码表未就绪而真丢的才写 W，导出日志里能查到。
  Future<void> _flushDrops() async {
    _dropTimer?.cancel();
    _dropTimer = null;
    final since = _dropSince;
    _dropSince = null;
    if (since == null || _dropReasons.isEmpty) {
      _dropReasons.clear();
      _dropApps.clear();
      _dropLost = 0;
      return;
    }

    final mins = DateTime.now().difference(since).inMinutes;
    final total = _dropReasons.values.fold<int>(0, (a, b) => a + b);
    final reasons = _dropReasons.entries
        .map((e) => '${e.key}×${e.value}')
        .join('、');
    final apps = (_dropApps.entries.toList()
          ..sort((a, b) => b.value.compareTo(a.value)))
        .take(4)
        .map((e) => '${e.key}×${e.value}')
        .join('、');
    final lost = _dropLost;

    _dropReasons.clear();
    _dropApps.clear();
    _dropLost = 0;

    final msg = '未转发 $total 条（${mins < 1 ? '<1' : mins} 分钟）：$reasons'
        '${apps.isEmpty ? '' : '；$apps'}'
        '${lost > 0 ? '；码表未就绪 $lost 条' : ''}';
    if (lost > 0) {
      await AppLog.w(_tag, msg);
    } else {
      await AppLog.i(_tag, msg);
    }
  }

  void _enqueueEvent(dynamic raw) {
    _inflight = _inflight.then((_) => _onEvent(raw)).catchError((Object e) {
      AppLog.w(_tag, '通知处理失败: $e');
    });
  }

  Future<void> _onEvent(dynamic raw) async {
    if (raw is! Map) return;

    final skipped = raw['skipped'] == true;
    final reason = (raw['reason'] as String?) ?? '';
    final status = (raw['status'] as String?) ?? '';
    final pkg = (raw['package'] as String?) ?? '';
    final appName = (raw['appName'] as String?) ?? '';
    final title = (raw['title'] as String?) ?? '';
    final text = (raw['text'] as String?) ?? '';

    if (status.isNotEmpty) {
      lastHeard = status == 'connected' ? '监听已绑定' : '监听断开';
      await AppLog.w(_tag, 'listener $status');
      return;
    }

    if (skipped) {
      lastHeard = '过滤 $reason  ${appName.isEmpty ? pkg : appName}  $title';
      _noteDrop(reason.isEmpty ? '过滤' : reason, appName.isEmpty ? pkg : appName);
      return;
    }

    if (title.isEmpty && text.isEmpty) {
      lastHeard = '空内容  ${appName.isEmpty ? pkg : appName}';
      _noteDrop('空内容', appName.isEmpty ? pkg : appName);
      return;
    }

    lastHeard = '${appName.isEmpty ? pkg : appName}  $title  $text';

    final settings = NotifSettings.instance;
    await settings.ensureLoaded();
    if (!settings.enabled) {
      lastHeard = '$lastHeard  （转发已关）';
      return;
    }
    if (settings.lockScreenOnly && !await isPhoneLocked()) {
      lastHeard = '$lastHeard  （未锁屏，未下发）';
      return;
    }
    if (settings.rideOnly) {
      final riding = CompanionClient.instance.lastStatus?.isRecording ?? false;
      if (!riding) {
        lastHeard = '$lastHeard  （未骑行，未下发）';
        return;
      }
    }
    if (!settings.allowsPackage(pkg)) {
      lastHeard = '$lastHeard  （自定义未选中，未下发）';
      return;
    }

    final iconName = pkg.isEmpty ? '' : CompanionNotif.iconFileName(pkg);
    if (pkg.isNotEmpty && iconName.isNotEmpty) {
      await cacheIconForPackage(packageName: pkg, appName: appName);
    }

    final client = CompanionClient.instance;
    if (!client.isReady) {
      lastHeard = '$lastHeard  （码表未连接，未下发）';
      _noteDrop('码表未就绪', appName.isEmpty ? pkg : appName, lost: true);
      return;
    }

    try {
      await client.sendNotification(
        title: title.isEmpty ? appName : title,
        body: text.isEmpty ? title : text,
        appName: appName,
        packageName: pkg,
        iconFileName: iconName,
      );
      lastHeard = '$lastHeard  → 已下发';
      await AppLog.i(_tag, '已下发 pkg=$pkg title=$title');
    } catch (e) {
      lastHeard = '$lastHeard  （下发失败）';
      await AppLog.w(_tag, '通知下发失败: $e');
    }
  }

  /// 把 App 缓存里的通知图标全部写入码表 `notif_icons/`。
  Future<NotifIconUploadResult> uploadCachedIconsToDevice({
    void Function(NotifIconProgress p)? onProgress,
    bool Function()? shouldStop,
  }) async {
    final client = CompanionClient.instance;
    if (!client.isReady) {
      return const NotifIconUploadResult(ok: 0, fail: 0, stopped: true);
    }
    if (client.lastStatus?.isTransferBusy == true) {
      throw StateError('mtp');
    }

    final entries = await NotifIconCache.instance.list();
    final files = [
      for (final e in entries)
        if (e.fileName.toLowerCase().endsWith('.png')) e,
    ];
    if (files.isEmpty) {
      return const NotifIconUploadResult(ok: 0, fail: 0, stopped: false);
    }

    // 先算总字节：图标不多，stat 一遍很便宜，但没有它就没法算速度和百分比。
    final sizes = <String, int>{};
    var bytesTotal = 0;
    for (final e in files) {
      try {
        final n = await e.file.length();
        sizes[e.fileName] = n;
        bytesTotal += n;
      } catch (_) {
        sizes[e.fileName] = 0;
      }
    }

    try {
      await client.fsMkdir(CompanionNotif.iconDir);
    } catch (_) {}

    var ok = 0;
    var fail = 0;
    var bytesDone = 0;
    var bytesTransferred = 0;
    for (var i = 0; i < files.length; i++) {
      if (shouldStop?.call() == true) {
        return NotifIconUploadResult(ok: ok, fail: fail, stopped: true);
      }
      final e = files[i];
      final size = sizes[e.fileName] ?? 0;

      void report(int extra) => onProgress?.call(
            NotifIconProgress(
              filesDone: i,
              filesTotal: files.length,
              name: e.fileName,
              bytesDone: bytesDone + extra,
              bytesTotal: bytesTotal,
              bytesTransferred: bytesTransferred + extra,
            ),
          );

      report(0);
      try {
        final bytes = await e.file.readAsBytes();
        if (bytes.isEmpty) {
          fail++;
          bytesDone += size;
          continue;
        }
        final dest = CompanionNotif.iconFsPath(e.fileName);
        if (dest.isEmpty) {
          fail++;
          bytesDone += size;
          continue;
        }
        await client.fsUpload(
          dest,
          bytes,
          resume: false,
          onProgress: (d, _) => report(d),
        );
        bytesDone += size;
        bytesTransferred += bytes.length;
        ok++;
      } catch (err) {
        bytesDone += size;
        fail++;
        await AppLog.w(_tag, '批量上传图标失败 ${e.fileName}: $err');
        if (err is CompanionFsException && err.status == 16) {
          return NotifIconUploadResult(ok: ok, fail: fail, stopped: true);
        }
      }
    }
    return NotifIconUploadResult(ok: ok, fail: fail, stopped: false);
  }

  Future<void> _cacheIcon(String pkg, String appName, String iconName) async {
    try {
      final bytes = await _method.invokeMethod<Uint8List>('getAppIcon', {
        'package': pkg,
      });
      if (bytes == null || bytes.isEmpty) return;
      final changed = await NotifIconCache.instance.putIfMd5Changed(
        fileName: iconName,
        bytes: bytes,
        packageName: pkg,
        appName: appName,
      );
      if (changed) {
        await AppLog.i(_tag, '已缓存图标 $iconName (${bytes.length} B)');
      }
    } catch (e) {
      await AppLog.w(_tag, '图标缓存失败 $pkg: $e');
    }
  }
}

/// 桌面上能打开的已安装应用（自定义通知列表用）。
class InstalledNotifApp {
  /// 创建一条记录。
  const InstalledNotifApp({
    required this.packageName,
    required this.appName,
  });

  /// Android 包名。
  final String packageName;

  /// 系统显示名。
  final String appName;
}

/// 批量上传通知图标的字节级进度。
///
/// [bytesDone] / [bytesTotal] 给进度条（已跳过/已失败的文件也算完成）；
/// [bytesTransferred] 单调递增，只算真正写进去的字节，专供测速。
class NotifIconProgress {
  /// 创建进度快照。
  const NotifIconProgress({
    required this.filesDone,
    required this.filesTotal,
    required this.name,
    required this.bytesDone,
    required this.bytesTotal,
    required this.bytesTransferred,
  });

  /// 已完成文件数。
  final int filesDone;

  /// 文件总数。
  final int filesTotal;

  /// 当前文件名。
  final String name;

  /// 已完成字节（含跳过）。
  final int bytesDone;

  /// 总字节。
  final int bytesTotal;

  /// 真正写入的字节（测速用）。
  final int bytesTransferred;
}

/// 批量把缓存图标写到码表的结果。
class NotifIconUploadResult {
  /// 创建结果。
  const NotifIconUploadResult({
    required this.ok,
    required this.fail,
    required this.stopped,
  });

  /// 成功个数。
  final int ok;

  /// 失败个数。
  final int fail;

  /// 被取消或因 MTP 忙中止。
  final bool stopped;
}

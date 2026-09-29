import 'dart:io';

import 'package:archive/archive_io.dart';
import 'package:flutter/foundation.dart';
import 'package:path/path.dart' as p;
import 'package:path_provider/path_provider.dart';

/// 本地文件日志与崩溃落盘；可在 [LogViewerPage] 中查看。
///
/// 约束（避免乱存涨盘）：
/// - 仅写入应用私有目录 `…/logs/`，不上传、不写相册/外部存储
/// - 只有主动调用 [d]/[i]/[w]/[e]/[crash] 才落盘（不劫持全局 print）
/// - **release：仅 [w]/[e]/[crash] 落盘；[d]/[i] 只打控制台**
/// - 按日文件 `app_YYYYMMDD.log`；保留 [keepDays] 天
/// - 单日文件超过 [maxAppLogBytes] 则截断保留尾部
/// - 崩溃文件最多保留 [maxCrashFiles] 个（新覆盖旧）
class AppLog {
  AppLog._();

  /// 普通日志保留天数。
  static const keepDays = 7;

  /// 单日 `app_*.log` 最大字节数（约 2MB）。
  static const maxAppLogBytes = 2 * 1024 * 1024;

  /// 崩溃文件最多保留个数。
  static const maxCrashFiles = 20;

  static Directory? _dir;
  static IOSink? _sink;
  static String? _currentDay;

  /// 所有会碰 [_sink] 的操作都排在这一条链上。
  ///
  /// 之前没有串行化：多个错误同时上报时（FlutterError 通道 + Zone +
  /// PlatformDispatcher 会重复上报），两个 `_write()` 并发走到
  /// `_rollIfNeeded()` / `_trimIfOversized()`，对同一个 IOSink 重复
  /// close/flush，抛 `Bad state: StreamSink is bound to a stream` ——
  /// 后果是**崩溃详情整条丢失**，只剩一行摘要，`creator:` 和约束信息
  /// 全没了，线上排查等于瞎猜。
  static Future<void> _chain = Future<void>.value();

  /// 把 [action] 追加到串行链尾。
  ///
  /// 链条自身吞掉异常：一次失败不能让后续所有写入都跟着跳过。
  /// 注意不要在 [action] 里再调 [_serialize]（会自锁）。
  static Future<T> _serialize<T>(Future<T> Function() action) {
    final job = _chain.then((_) => action());
    _chain = job.then<void>((_) {}, onError: (Object _) {});
    return job;
  }

  /// 日志目录（`…/logs`）。
  static Directory? get directory => _dir;

  /// 初始化日志目录与当日文件；应在业务启动最早调用。
  static Future<void> init() async {
    if (kIsWeb) return;
    final root = await getApplicationSupportDirectory();
    _dir = Directory(p.join(root.path, 'logs'));
    if (!await _dir!.exists()) {
      await _dir!.create(recursive: true);
    }
    await _serialize(_prune);
    await _serialize(_rollIfNeeded);
    await i('AppLog', 'init dir=${_dir!.path} keepDays=$keepDays');
  }

  static Future<void> _prune() async {
    if (_dir == null || !await _dir!.exists()) return;
    final now = DateTime.now();
    final cutoff = DateTime(now.year, now.month, now.day)
        .subtract(Duration(days: keepDays));
    final crashes = <File>[];

    for (final entity in _dir!.listSync()) {
      if (entity is! File || !entity.path.endsWith('.log')) continue;
      final name = p.basename(entity.path);

      if (name.startsWith('app_') && name.length >= 12) {
        final stamp = name.substring(4, 12); // YYYYMMDD
        final day = _parseDay(stamp);
        if (day != null && day.isBefore(cutoff)) {
          try {
            await entity.delete();
          } catch (_) {}
          continue;
        }
        await _trimIfOversized(entity);
      } else if (name.startsWith('crash_')) {
        crashes.add(entity);
      }
    }

    crashes.sort((a, b) => b.path.compareTo(a.path));
    for (var i = maxCrashFiles; i < crashes.length; i++) {
      try {
        await crashes[i].delete();
      } catch (_) {}
    }
  }

  static DateTime? _parseDay(String yyyymmdd) {
    if (yyyymmdd.length != 8) return null;
    final y = int.tryParse(yyyymmdd.substring(0, 4));
    final m = int.tryParse(yyyymmdd.substring(4, 6));
    final d = int.tryParse(yyyymmdd.substring(6, 8));
    if (y == null || m == null || d == null) return null;
    return DateTime(y, m, d);
  }

  static Future<void> _trimIfOversized(File file) async {
    try {
      if (!await file.exists()) return;
      final len = await file.length();
      if (len <= maxAppLogBytes) return;

      if (_sink != null &&
          _currentDay != null &&
          file.path.endsWith('app_$_currentDay.log')) {
        // 先摘掉引用再关：否则 _rollIfNeeded() 里刚 close 过的那个 sink
        // 会被这里再 flush/close 一次，正是 StateError 的来源。
        final s = _sink;
        _sink = null;
        await s?.flush();
        await s?.close();
      }

      final raf = await file.open();
      await raf.setPosition(len - maxAppLogBytes);
      final bytes = await raf.read(maxAppLogBytes);
      await raf.close();
      await file.writeAsBytes(bytes, flush: true);
    } catch (e) {
      debugPrint('AppLog trim failed: $e');
    }
  }

  static Future<void> _rollIfNeeded() async {
    final day = _dayStamp(DateTime.now());
    if (_sink != null && _currentDay == day) return;
    // 顺序要紧：先摘引用、再更新 _currentDay，最后才关旧 sink。
    // 反过来的话，下面的 _trimIfOversized() 会看到 _currentDay 已更新
    // 而 _sink 还指着那个刚关掉的 IOSink，于是又 close 一次。
    final old = _sink;
    _sink = null;
    _currentDay = day;
    await old?.flush();
    await old?.close();
    final file = File(p.join(_dir!.path, 'app_$day.log'));
    await _trimIfOversized(file);
    _sink = file.openWrite(mode: FileMode.append);
  }

  static String _dayStamp(DateTime t) =>
      '${t.year.toString().padLeft(4, '0')}'
      '${t.month.toString().padLeft(2, '0')}'
      '${t.day.toString().padLeft(2, '0')}';

  static String _ts(DateTime t) =>
      '${_dayStamp(t)}-'
      '${t.hour.toString().padLeft(2, '0')}'
      '${t.minute.toString().padLeft(2, '0')}'
      '${t.second.toString().padLeft(2, '0')}.'
      '${t.millisecond.toString().padLeft(3, '0')}';

  static Future<void> _write(String level, String tag, String message) =>
      // 串行化：并发调用按落链顺序写入，日志顺序即调用顺序。
      _serialize(() => _writeLocked(level, tag, message));

  static Future<void> _writeLocked(
    String level,
    String tag,
    String message,
  ) async {
    if (kIsWeb || _dir == null) {
      debugPrint('[$level] $tag: $message');
      return;
    }
    final now = DateTime.now();
    final line = '${_ts(now)} [$level] $tag: $message\n';
    debugPrint(line.trimRight());
    try {
      await _rollIfNeeded();
      // 取本地引用：锁内不会被换掉，但显式一点更安全。
      final sink = _sink;
      if (sink == null) return;
      sink.write(line);
      // 不每行 flush，降低 IO；关键节点仍 flush
      if (level == 'E' || level == 'CRASH' || level == 'W') {
        await sink.flush();
      }
    } catch (e) {
      debugPrint('AppLog write failed: $e');
    }
  }

  /// Debug（仅 debug 构建落盘，release 只打控制台）。
  static Future<void> d(String tag, String message) async {
    if (kReleaseMode) {
      debugPrint('[D] $tag: $message');
      return;
    }
    await _write('D', tag, message);
  }

  /// Info：debug 落盘；release 只打控制台，避免 I 刷满本地日志。
  static Future<void> i(String tag, String message) async {
    if (kReleaseMode) {
      debugPrint('[I] $tag: $message');
      return;
    }
    await _write('I', tag, message);
  }

  /// Warning（始终落盘）。
  static Future<void> w(String tag, String message) =>
      _write('W', tag, message);

  /// Error（始终落盘；可选堆栈）。
  static Future<void> e(
    String tag,
    Object error, [
    StackTrace? stack,
  ]) async {
    final msg = stack == null ? '$error' : '$error\n$stack';
    await _write('E', tag, msg);
  }

  static String? _lastCrashKey;
  static DateTime? _lastCrashAt;

  /// 网络抖动等可恢复错误：只记 [e]，不单独建 crash 文件。
  static bool _isTransient(Object error) {
    final s = error.toString();
    return s.contains('HandshakeException') ||
        s.contains('SocketException') ||
        s.contains('ClientException') ||
        s.contains('HttpException') ||
        s.contains('NetworkImageLoadException') ||
        s.contains('Connection closed') ||
        s.contains('Connection terminated') ||
        s.contains('Failed host lookup');
  }

  /// 短时相同异常去重（多通道 FlutterError/Zone/PlatformDispatcher 会重复上报）。
  static bool _shouldWriteCrashFile(String key) {
    final now = DateTime.now();
    if (_lastCrashKey == key &&
        _lastCrashAt != null &&
        now.difference(_lastCrashAt!) < const Duration(seconds: 5)) {
      return false;
    }
    _lastCrashKey = key;
    _lastCrashAt = now;
    return true;
  }

  /// 未捕获异常统一入口：瞬时网络错误降级为 [e]；其余写一条 crash（去重）。
  static Future<void> reportUncaught(
    String source,
    Object error,
    StackTrace? stack,
  ) async {
    if (_isTransient(error)) {
      await e(source, error, stack);
      return;
    }
    await crash(source, error, stack);
  }

  /// 崩溃/未捕获异常：写入独立 `crash_*.log`（5 秒内同文案只写 1 份），并记入当日 app 日志。
  static Future<void> crash(
    String source,
    Object error,
    StackTrace? stack,
  ) async {
    final key = error.toString();
    final writeFile = _shouldWriteCrashFile(key);
    await _write(
      'CRASH',
      source,
      writeFile ? key : '$key (deduped, skip file)',
    );
    if (!writeFile || kIsWeb || _dir == null) return;
    try {
      final body =
          'source=$source\nerror=$error\nstack=\n${stack ?? StackTrace.current}\n';
      final name = 'crash_${_ts(DateTime.now())}.log';
      final file = File(p.join(_dir!.path, name));
      await file.writeAsString(body);
      await _serialize(_prune);
    } catch (e) {
      debugPrint('AppLog crash file failed: $e');
    }
  }

  /// 列出日志/崩溃文件（新→旧）。
  static Future<List<File>> listFiles() async {
    if (_dir == null || !await _dir!.exists()) return [];
    final files = _dir!
        .listSync()
        .whereType<File>()
        .where((f) => f.path.endsWith('.log'))
        .toList()
      ..sort((a, b) => b.path.compareTo(a.path));
    return files;
  }

  /// 删除单个日志文件。
  ///
  /// 删的可能是**正在写的当日文件**，所以要先摘掉并关掉 sink：
  /// 句柄还开着的话，删除在部分平台会失败，或者文件立刻被重新创建出来。
  /// 下一次 [_write] 会经 [_rollIfNeeded] 重新打开，不影响后续落盘。
  static Future<void> deleteFile(File file) async {
    await _serialize(() async {
      if (_sink != null &&
          _currentDay != null &&
          file.path.endsWith('app_$_currentDay.log')) {
        final s = _sink;
        _sink = null;
        try {
          await s?.flush();
        } catch (_) {}
        await s?.close();
      }
    });
    try {
      if (await file.exists()) {
        await file.delete();
      }
    } catch (e) {
      debugPrint('AppLog delete failed: $e');
    }
  }

  /// 读取文本；过大时截断尾部保留。
  static Future<String> readFile(File file, {int maxChars = 200000}) async {
    if (!await file.exists()) return '';
    final text = await file.readAsString();
    if (text.length <= maxChars) return text;
    return '…(truncated)\n${text.substring(text.length - maxChars)}';
  }

  /// 导出前刷新当日日志缓冲。
  static Future<void> flush() => _serialize(() async {
        try {
          await _sink?.flush();
        } catch (_) {}
      });

  /// 将全部日志打成 zip（位于临时目录），供系统分享/另存。
  ///
  /// 无日志时返回 `null`。
  static Future<File?> exportZip() async {
    if (kIsWeb || _dir == null) return null;
    await flush();
    final files = await listFiles();
    if (files.isEmpty) return null;

    final tmp = await getTemporaryDirectory();
    final stamp = _ts(DateTime.now()).replaceAll('.', '-');
    final out = File(p.join(tmp.path, 'sifli_logs_$stamp.zip'));
    if (await out.exists()) {
      await out.delete();
    }

    final encoder = ZipFileEncoder();
    encoder.create(out.path);
    for (final f in files) {
      encoder.addFile(f);
    }
    await encoder.close();
    return out;
  }

  /// 清空全部日志文件。
  static Future<void> clearAll() async {
    // 关 sink 的动作必须进锁；但后面的删文件、重建、写「cleared」不能再进锁
    //（_serialize 不可重入），所以拆成两段。
    await _serialize(() async {
      final sink = _sink;
      _sink = null;
      _currentDay = null;
      try {
        await sink?.flush();
      } catch (_) {}
      await sink?.close();
    });
    if (_dir == null) return;
    if (await _dir!.exists()) {
      for (final entity in _dir!.listSync()) {
        if (entity is File && entity.path.endsWith('.log')) {
          await entity.delete();
        }
      }
    }
    await _serialize(_rollIfNeeded);
    await i('AppLog', 'cleared');
  }
}

import 'package:sifli_companion/ble/companion_client.dart';
import 'package:sifli_companion/ble/companion_proto.dart';
import 'package:sifli_companion/log/captured_log_store.dart';

/// 日志通道。
enum LogCaptureStage {
  /// 崩溃转储。
  coredump,

  /// 诊断日志。
  diag,
}

/// 抓取进度。
///
/// [bytesDone] / [bytesTotal] 是**进度条**用的（已在本机的文件也算完成，
/// 否则批量重抓时进度条会倒退）；[bytesTransferred] 是**真正下载的字节**，
/// 单调递增，专门给测速用 —— 否则跳过已存在文件时速度会瞬间冲到天上。
class LogCaptureProgress {
  /// 创建进度快照。
  const LogCaptureProgress({
    required this.stage,
    required this.filesDone,
    required this.filesTotal,
    required this.bytesDone,
    required this.bytesTotal,
    required this.bytesTransferred,
  });

  /// 当前通道。
  final LogCaptureStage stage;

  /// 该通道已完成文件数。
  final int filesDone;

  /// 该通道文件总数。
  final int filesTotal;

  /// 两条通道合计的已完成字节（含跳过）。
  final int bytesDone;

  /// 两条通道合计总字节。
  final int bytesTotal;

  /// 真正从设备下载的字节（测速用）。
  final int bytesTransferred;
}

/// 一次抓取的结果。
class LogCaptureResult {
  /// 创建结果。
  const LogCaptureResult({
    required this.coredump,
    required this.diag,
    required this.stopped,
  });

  /// 新写入的崩溃转储条数。
  final int coredump;

  /// 新写入的诊断日志条数。
  final int diag;

  /// 是否被用户中途停掉。
  final bool stopped;

  /// 合计新增条数。
  int get total => coredump + diag;
}

/// 从码表抓取日志到本机。
///
/// 两条通道一次抓完（需求：「一个按键」），先各自 LIST 拿到 bsize 总量，
/// 这样速度才有分母。设备侧目录不存在（从没写过日志）当空处理。
class LogCaptureSync {
  LogCaptureSync._();

  static final List<_Stage> _stages = [
    _Stage(
      stage: LogCaptureStage.coredump,
      store: CapturedLogStore.coredump,
      remoteDir: CompanionFs.coredumpDir,
    ),
    _Stage(
      stage: LogCaptureStage.diag,
      store: CapturedLogStore.diag,
      remoteDir: CompanionFs.diagDir,
    ),
  ];

  /// 抓取两条通道。返回各通道新增条数。
  ///
  /// [onProgress] 在文件边界和每个下载分片都会回调，调用方自己节流。
  /// [shouldStop] 在文件边界被检查 —— 单个文件下载中不打断（与
  /// `CompanionNotifRelay.uploadCachedIconsToDevice` 同样的粒度）。
  static Future<LogCaptureResult> pull(
    CompanionClient client, {
    void Function(LogCaptureProgress p)? onProgress,
    bool Function()? shouldStop,
  }) async {
    if (!client.isReady) {
      throw StateError('Companion 未就绪');
    }
    if (client.lastStatus?.isTransferBusy == true) {
      throw StateError('USB/MTP 占用中');
    }

    // 先全部列出来：既拿到总量（速度的分母），也避免边下边列让进度条乱跳。
    final plans = <_Plan>[];
    for (final s in _stages) {
      final files = await _listFiles(client, s.store, s.remoteDir);
      plans.add(_Plan(s, files));
    }
    final bytesTotal = plans.fold<int>(0, (a, p) => a + p.bytes);

    var bytesDone = 0;
    var bytesTransferred = 0;
    var stopped = false;
    final added = <LogCaptureStage, int>{};

    for (final plan in plans) {
      var n = 0;
      final files = plan.files;
      for (var i = 0; i < files.length; i++) {
        if (shouldStop?.call() == true) {
          stopped = true;
          break;
        }
        final e = files[i];

        void report(int extra) => onProgress?.call(
              LogCaptureProgress(
                stage: plan.stage.stage,
                filesDone: i,
                filesTotal: files.length,
                bytesDone: bytesDone + extra,
                bytesTotal: bytesTotal,
                bytesTransferred: bytesTransferred + extra,
              ),
            );

        report(0);

        final local = await plan.stage.store.fileFor(e.name);
        if (await local.exists() && await local.length() == e.size) {
          // 已在本机：进度要算，但不计入测速。
          bytesDone += e.size;
          continue;
        }

        final bytes = await client.fsDownload(
          CompanionFs.joinRel(plan.stage.remoteDir, e.name),
          length: e.size,
          onProgress: (d, _) => report(d),
        );
        await local.writeAsBytes(bytes, flush: true);
        await plan.stage.store.markPulled(e.name, DateTime.now());
        bytesDone += e.size;
        bytesTransferred += bytes.length;
        n++;
      }
      added[plan.stage.stage] = n;
      if (stopped) break;
    }

    // 收尾：把进度钉到 100%，否则悬浮框会停在 99%。
    onProgress?.call(
      LogCaptureProgress(
        stage: _stages.last.stage,
        filesDone: 0,
        filesTotal: 0,
        bytesDone: bytesTotal,
        bytesTotal: bytesTotal,
        bytesTransferred: bytesTransferred,
      ),
    );

    return LogCaptureResult(
      coredump: added[LogCaptureStage.coredump] ?? 0,
      diag: added[LogCaptureStage.diag] ?? 0,
      stopped: stopped,
    );
  }

  /// LIST 整个目录，翻页取尽。目录不存在返回空表。
  ///
  /// 只保留**长相是日志**的文件：目录里还有一个 `seq` 序号计数器，
  /// 它不是日志（见 [CapturedLogStore.isLogFile]）。
  static Future<List<CompanionFsEntry>> _listFiles(
    CompanionClient client,
    CapturedLogStore store,
    String dir,
  ) async {
    try {
      final out = <CompanionFsEntry>[];
      var cursor = 0;
      while (true) {
        final page = await client.fsList(dir, cursor: cursor);
        if (page.isEmpty) break;
        out.addAll(page);
        cursor += page.length;
        if (page.length < 4) break;
      }
      return [
        for (final e in out)
          if (!e.isDir && !e.name.startsWith('.') && store.isLogFile(e.name))
            e,
      ];
    } on CompanionFsException catch (e) {
      // 没有该目录 = 设备还没写过这类日志，不是错误。
      if (e.status == 2) return [];
      rethrow;
    }
  }
}

class _Stage {
  const _Stage({
    required this.stage,
    required this.store,
    required this.remoteDir,
  });

  final LogCaptureStage stage;
  final CapturedLogStore store;
  final String remoteDir;
}

class _Plan {
  _Plan(this.stage, this.files);

  final _Stage stage;
  final List<CompanionFsEntry> files;

  int get bytes => files.fold<int>(0, (a, e) => a + e.size);
}

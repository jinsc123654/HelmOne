import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:get/get.dart';

/// 一次数据抓取 / 传输的生命周期状态。
enum TransferStatus {
  /// 进行中。
  running,

  /// 正常结束。
  done,

  /// 出错结束。
  failed,

  /// 用户取消。
  cancelled,
}

/// 全局悬浮框里的一条抓取任务。
///
/// 由 [TransferCenter.begin] 创建，调用方持有它并回调 [TransferCenter.progress] /
/// [TransferCenter.finish] 等。字段是刻意可变的：进度回调在 BLE 的
/// 高频路径上（每个 notify 页都来一次），不想每次都分配新对象。
class TransferTask {
  /// 创建任务；一般走 [TransferCenter.begin]，不要直接 new。
  TransferTask({
    required this.id,
    required this.title,
    this.kind = '',
    this.total = 0,
    this.cancellable = false,
    this.onCancel,
  });

  /// 唯一 id。
  final String id;

  /// 标题（调用方传入，已翻译）。
  final String title;

  /// 任务类别，用于跨页面查「这类活儿是不是已经在跑」。
  ///
  /// 进度状态现在不属于任何页面了，页面重新进入时没法靠自己的
  /// `_busy` 判断重入 —— 得回来问 [TransferCenter]。
  final String kind;

  /// 是否支持取消（决定悬浮框显不显示取消按钮）。
  final bool cancellable;

  /// 点取消时回调；由发起方翻转自己的 stop 标志。
  final VoidCallback? onCancel;

  /// 阶段 / 明细文案，例如「崩溃记录 2/5」。调用方给，已翻译。
  String phase = '';

  /// 已完成量（单位由调用方定：字节或条目数）。
  int done = 0;

  /// 总量；0 表示不确定进度（画成转圈而不是进度条）。
  int total = 0;

  /// 累计已传字节，用于测速。
  int transferred = 0;

  /// 滑动窗口测得的 KB/s。
  double kbs = 0;

  /// 当前状态。
  TransferStatus status = TransferStatus.running;

  /// 失败原因（status == failed 时有值）。
  String? error;

  /// 开始时间。
  final DateTime startedAt = DateTime.now();

  final Stopwatch _sw = Stopwatch()..start();
  int _lastBytes = 0;
  int _lastMs = 0;

  /// 是否还在跑。
  bool get isRunning => status == TransferStatus.running;

  /// 进度比例 0..1；[total] 为 0 时返回 null（不确定进度）。
  double? get fraction {
    if (total <= 0) return null;
    return (done / total).clamp(0.0, 1.0);
  }

  /// 百分比整数。
  int get percent => ((fraction ?? 0) * 100).round();

  /// 刷新测速窗口，返回是否有新的速度值。
  bool _tickSpeed(int nowBytes) {
    final ms = _sw.elapsedMilliseconds;
    final dt = ms - _lastMs;
    if (dt < 280) return false;
    final db = nowBytes - _lastBytes;
    if (db >= 0) {
      kbs = db * 1000 / (dt < 1 ? 1 : dt) / 1024;
    }
    _lastBytes = nowBytes;
    _lastMs = ms;
    return true;
  }
}

/// 全 App 统一的抓取任务登记处：谁在抓、抓到哪了、多快。
///
/// 存在的理由是需求「页面退出不知道是不是还在抓取」——进度不能挂在
/// 某个页面的 `State` 上，否则退出页面就看不到了。这里只存状态，
/// 悬浮框 [TransferOverlay] 通过 `GetMaterialApp.builder` 挂在所有路由之外，
/// 所以切页、返回都不影响它。
///
/// 注意：这里**不**负责发请求，也不管串行化。Companion 的 BLE FS 通道
/// 本来就是串行的，并发发起会互相踩；调用方自己保证不重入
///（现有的 `_pulling` / `_busy` 标志继续保留）。
class TransferCenter extends GetxService {
  /// 取全局单例。已在 `main()` 里 `Get.put(..., permanent: true)`。
  static TransferCenter get to => Get.find<TransferCenter>();

  /// 当前任务列表，新的在前。
  final tasks = <TransferTask>[].obs;

  final _changes = StreamController<void>.broadcast();

  /// 任务表变化流。
  ///
  /// 给「页面已经重建过、本地标志丢了」的场景用：`tasks` 是 `RxList`，
  /// 没法用 `ever<bool>` 订阅，而把整个 build 包进 `Obx` 又太贵。
  Stream<void> get changes => _changes.stream;

  int _seq = 0;

  /// 是否已有任务在跑（用于拦重入）。
  bool get isBusy => tasks.any((t) => t.isRunning);

  /// 当前正在跑的任务条数。
  int get runningCount => tasks.where((t) => t.isRunning).length;

  /// 正在跑的任务。
  List<TransferTask> get runningTasks =>
      [for (final t in tasks) if (t.isRunning) t];

  /// 悬浮球该不该露头。
  ///
  /// 判据是**任务表里还有没有东西**，而不是"还在不在跑"—— 结束后的任务会
  /// 靠 [_scheduleDrop] 的延时留在表里一会儿，正是为了让用户看见结果。
  /// 早先这里只算 running/failed，于是「没东西要同步」那种几十毫秒就结束的
  /// 活儿，`finish()` 一落状态球就滑走了，压根来不及看，见 [_minVisible]。
  ///
  /// 任务失败后**故意**不设超时：静默把错误滑走，用户就永远不知道刚才那次
  /// 抓取挂了。失败的条目要等人点了关闭才消失。
  bool get hasWork => tasks.isNotEmpty;

  /// 某一类任务是否正在跑。
  ///
  /// 页面被销毁重建后 `_busy` 之类的本地标志就没了，重入判断得回到这里来问。
  bool isKindRunning(String kind) =>
      tasks.any((t) => t.isRunning && t.kind == kind);

  /// 取第一条第 [kind] 类且还在跑的任务。
  TransferTask? runningOfKind(String kind) {
    for (final t in tasks) {
      if (t.isRunning && t.kind == kind) return t;
    }
    return null;
  }

  DateTime _lastNotify = DateTime.fromMillisecondsSinceEpoch(0);

  /// 登记一条新任务。
  TransferTask begin({
    required String title,
    String kind = '',
    int total = 0,
    bool cancellable = false,
    VoidCallback? onCancel,
  }) {
    final t = TransferTask(
      id: 'T${DateTime.now().millisecondsSinceEpoch}_${_seq++}',
      title: title,
      kind: kind,
      total: total,
      cancellable: cancellable,
      onCancel: onCancel,
    );
    tasks.insert(0, t);
    _notify(force: true);
    return t;
  }

  /// 上报进度。
  ///
  /// [done] / [total] 单位由调用方定；[transferred] 是累计**字节**，
  /// 只用来算速度（条目计数的流程可以不给）。
  /// [phase] 是阶段文案，变了就立刻刷新，避免"卡住"的错觉。
  void progress(
    TransferTask t, {
    int? done,
    int? total,
    int? transferred,
    String? phase,
  }) {
    if (!t.isRunning) return;
    final phaseChanged = phase != null && phase != t.phase;
    if (done != null) t.done = done;
    if (total != null) t.total = total;
    if (transferred != null) t.transferred = transferred;
    if (phase != null) t.phase = phase;
    t._tickSpeed(t.transferred);
    _notify(force: phaseChanged);
  }

  /// 正常结束。默认成功后从悬浮框移除（失败/取消会留着让人看见）。
  ///
  /// [message] 是可选的结果文案，会覆盖卡片上的明细行。给「没东西要同步」
  /// 这类**什么也没做**的收尾用：光一个 ✓ 看不出为什么这么快就结束了。
  void finish(TransferTask t, {String? message}) {
    if (!t.isRunning) return;
    t.status = TransferStatus.done;
    t.kbs = 0;
    if (message != null && message.isNotEmpty) t.phase = message;
    if (t.total > 0) t.done = t.total;
    _notify(force: true);
    // 成功不留痕迹：悬浮框只在"还在抓"时有意义。
    _scheduleDrop(t);
  }

  /// 出错结束，保留在悬浮框里直到用户点掉。
  void fail(TransferTask t, Object error) {
    if (!t.isRunning) return;
    t.status = TransferStatus.failed;
    t.kbs = 0;
    t.error = error.toString();
    _notify(force: true);
  }

  /// 用户取消。
  void cancel(TransferTask t) {
    if (!t.isRunning) return;
    t.onCancel?.call();
    t.status = TransferStatus.cancelled;
    t.kbs = 0;
    _notify(force: true);
    _scheduleDrop(t);
  }

  /// 从列表里手动移除（悬浮框上的关闭按钮）。
  void dismiss(TransferTask t) {
    tasks.removeWhere((e) => e.id == t.id);
    _notify(force: true);
  }

  /// 清掉所有已结束的任务。
  void clearFinished() {
    tasks.removeWhere((t) => !t.isRunning);
    _notify(force: true);
  }

  /// 任务从**登记那一刻**起，至少要在屏幕上待这么久。
  ///
  /// 「没东西要同步」这类活儿几十毫秒就跑完了。如果按「结束即开始收」，悬浮球
  /// 滑进来（280 ms）还没停稳就开始滑出去，用户只看见一道影子，更别说看清跑的是
  /// 哪一条、结果是成功还是没内容。所以可见时长从 [TransferTask.startedAt] 起算，
  /// 不足的补足。
  static const _minVisible = Duration(milliseconds: 2600);

  /// 正常结束（成功 / 取消）后再多停留一会儿，让 ✓ 和结果文案能被读到。
  ///
  /// 失败的条目不走这里 —— 它们一直留着，等人点关闭。
  static const _doneHold = Duration(milliseconds: 1800);

  void _scheduleDrop(TransferTask t) {
    // 已经显示够久了就只加 [_doneHold]；刚开始就结束的补到 [_minVisible]。
    final elapsed = DateTime.now().difference(t.startedAt);
    final toMinVisible = _minVisible - elapsed;
    final wait = toMinVisible > _doneHold ? toMinVisible : _doneHold;
    Timer(wait, () {
      if (t.isRunning) return;
      tasks.removeWhere((e) => e.id == t.id);
      _notify(force: true);
    });
  }

  /// 节流刷新：BLE 进度回调很密，没必要每帧都重建悬浮框。
  void _notify({bool force = false}) {
    final now = DateTime.now();
    if (!force &&
        now.difference(_lastNotify) < const Duration(milliseconds: 80)) {
      return;
    }
    _lastNotify = now;
    tasks.refresh();
    if (!_changes.isClosed) _changes.add(null);
  }
}

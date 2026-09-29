import 'dart:async';

import 'package:get/get.dart';
import 'package:sifli_companion/ble/companion_client.dart';
import 'package:sifli_companion/ble/companion_proto.dart';
import 'package:sifli_companion/gnss/eph_service.dart';
import 'package:sifli_companion/gnss/mga_ubx.dart';
import 'package:sifli_companion/i18n/locale_keys.dart';
import 'package:sifli_companion/log/app_log.dart';
import 'package:sifli_companion/transfer/transfer_center.dart';

/// 辅助星历的自动同步。
///
/// 为什么需要它：星历只有 2–4 小时有效，而这颗模组**没有 V_BCKP 备份**（掉电即全丢，
/// 固件里那条备份电容还是未做的 TODO），所以「用户手动点一次」不够 —— 长骑行中途就会
/// 过期，之后模组只能靠手里那份旧数据。这里补三件事：
///
/// 1. 连上后按需补一次；
/// 2. 连接期间每 2 小时一次（骑行可能很长）；
/// 3. 回前台补一次（后台定时器会被系统冻住）。
///
/// 不重复下发：先列 `eph/` 看最新那份文件的时间戳再决定 —— App 下发的星历和模组自己的
/// DBD dump 同目录同名规则，都算数；DBD dump 里也带星历，它新就说明模组手里的是新的。
class EphAutoSync {
  EphAutoSync._();

  /// 单例。
  static final EphAutoSync instance = EphAutoSync._();

  static const _tag = 'EphAuto';

  /// 星历有效 2–4 h，阈值取一半留余量。
  static const maxAge = Duration(hours: 2);

  /// 连接期间的复查周期，同 [maxAge]。
  static const _period = Duration(hours: 2);

  /// 与星历页共用一个任务类别，避免两处同时跑把 BLE 通道踩乱。
  static const _kind = 'eph';

  /// 星历页 / 设备页共用的「下次同步」状态。
  ///
  /// 只有这里知道同步节奏（[maxAge] + 连上就补），所以状态也算在这里，
  /// 界面各处只管显示 —— 不然两个页面各推一次，迟早对不上。
  final status = const EphSyncStatus.unknown().obs;

  final _eph = EphService();
  Timer? _timer;
  bool _busy = false;

  /// 状态刷新的最小间隔（秒）：切页、重连都可以放心调。
  static const _probeMin = Duration(seconds: 10);

  /// 「还有几小时」是相对时间，界面得自己走字：有倒计时可显示时开一个 30 秒心跳，
  /// 到期 / 断开 / 没有星历就停 —— 没人看的时候不空转。
  static const _tickPeriod = Duration(seconds: 30);

  DateTime? _probeAt;
  DateTime? _lastNewest;
  Timer? _tick;
  bool _wired = false;

  /// 订阅链路状态：连上就查一次，之后按 [maxAge] 复查。
  void init() {
    if (_wired) return;
    _wired = true;
    CompanionClient.instance.stateStream.listen((s) {
      if (s == CompanionConnState.ready) {
        unawaited(refreshStatus(force: true));
        unawaited(maybeSync());
        _arm();
      } else {
        _timer?.cancel();
        _timer = null;
        /* 断开了：状态退回「连接后自动同步」，不能让界面停在旧时间上。 */
        unawaited(refreshStatus(force: true));
      }
    });
  }

  void _arm() {
    _timer?.cancel();
    _timer = Timer.periodic(_period, (_) => unawaited(maybeSync()));
  }

  /// 回前台时补一次。
  Future<void> kick() => maybeSync();

  /// 每个「跳过」的分支都留日志：出问题时能一眼看出是跳过了、还是失败了。
  Future<void> maybeSync({bool force = false}) async {
    if (_busy) return;
    final client = CompanionClient.instance;
    if (!client.isReady) return;
    if (client.lastStatus?.isTransferBusy == true) {
      await AppLog.i(_tag, '码表 USB/MTP 占用，跳过自动同步');
      return;
    }
    if (TransferCenter.to.isKindRunning(_kind)) return;

    _busy = true;
    try {
      if (!force) {
        /* 同步决策和界面状态用**同一次**目录查询，省一次文件系统往返。 */
        final newest = await newestTime(client);
        _publish(newest);
        final age = newest == null ? null : _ageOf(newest);
        if (age != null && age < maxAge) {
          await AppLog.i(_tag, '星历还新（${age.inMinutes} 分钟前），跳过');
          return;
        }
        await AppLog.i(
          _tag,
          age == null ? '码表上没有星历，自动同步' : '星历已 ${age.inMinutes} 分钟，自动同步',
        );
      }
      await _run(client);
    } catch (e, st) {
      // 后台任务不打扰用户：失败只记日志，星历页上能看到结果。
      await AppLog.e(_tag, '自动同步星历失败: $e', st);
    } finally {
      _busy = false;
    }
  }

  /// 刷新 [status]（界面调）。节流过；查不到就退回「没查过」，
  /// 不把界面钉在「码表上没有星历」上。
  Future<void> refreshStatus({bool force = false}) async {
    final client = CompanionClient.instance;
    if (!client.isReady || client.lastStatus?.isTransferBusy == true) {
      _setStatus(const EphSyncStatus.unknown());
      return;
    }
    if (TransferCenter.to.isKindRunning(_kind)) return;
    final now = DateTime.now();
    if (!force && _probeAt != null && now.difference(_probeAt!) < _probeMin) {
      return;
    }
    _probeAt = now;
    try {
      _publish(await newestTime(client));
    } catch (e) {
      await AppLog.w(_tag, '读星历状态失败（按未查询处理）: $e');
      // 连着但没读出来：显示中性文案，下一次刷新会自愈。
      _setStatus(const EphSyncStatus.unknown(connected: true));
    }
  }

  /// 码表上最新一份星历的时间（本地时区）。列不出来返回 `null`。
  Future<DateTime?> newestTime(CompanionClient client) async {
    final list = await _eph.listOnDevice(client);
    DateTime? newest;
    for (final e in list) {
      final t = _entryTime(e);
      if (t != null && (newest == null || t.isAfter(newest))) newest = t;
    }
    return newest;
  }

  void _publish(DateTime? newest) {
    _probeAt = DateTime.now();
    _lastNewest = newest;
    _setStatus(
      EphSyncStatus(newest: newest, listed: true, connected: true),
    );
  }

  /// 发状态并据此决定心跳开关（状态的一切出口都走这里，别漏了心跳）。
  void _setStatus(EphSyncStatus s) {
    status.value = s;
    _armTick();
  }

  void _armTick() {
    final st = status.value;
    final want = st.connected && st.listed && st.nextAt != null && !st.due;
    if (!want) {
      _tick?.cancel();
      _tick = null;
      return;
    }
    _tick ??= Timer.periodic(_tickPeriod, (_) {
      // 只重发一次状态（**不碰文件系统**）：让「X 小时后」随时间往下走。
      _setStatus(
        EphSyncStatus(newest: _lastNewest, listed: true, connected: true),
      );
    });
  }

  static Duration _ageOf(DateTime t) {
    final d = DateTime.now().difference(t);
    return d.isNegative ? Duration.zero : d;
  }

  static DateTime? _entryTime(CompanionFsEntry e) {
    final sec = MgaUbx.parseFileNameUtc(e.name);
    if (sec != null) {
      return DateTime.fromMillisecondsSinceEpoch(sec * 1000, isUtc: true)
          .toLocal();
    }
    if (e.mtime > 0) {
      return DateTime.fromMillisecondsSinceEpoch(e.mtime * 1000).toLocal();
    }
    return null;
  }

  Future<void> _run(CompanionClient client) async {
    final center = TransferCenter.to;
    final task = center.begin(title: LocaleKeys.ephTitle.tr, kind: _kind);
    try {
      final r = await _eph.syncToDevice(
        client,
        onStage: (s) => center.progress(
          task,
          done: 0,
          total: 0,
          phase: _stageLabel(s),
        ),
        onProgress: (d, t) => center.progress(
          task,
          done: d,
          total: t,
          transferred: d,
          phase: LocaleKeys.ephPushAction.tr,
        ),
      );
      center.finish(task);
      await AppLog.i(_tag, '自动同步完成 ${r.path}（${r.bytes.length} B）');
      await refreshStatus(force: true);
    } catch (e) {
      center.fail(task, e);
      rethrow;
    }
  }

  static String _stageLabel(EphSyncStage s) => switch (s) {
    EphSyncStage.download => LocaleKeys.ephDownloading.tr,
    EphSyncStage.convert => LocaleKeys.ephConverting.tr,
    EphSyncStage.upload => LocaleKeys.ephUploading.tr,
  };
}

/// 「下次同步」状态：星历页和设备页共用一份，避免两处各算各的、文案漂移。
class EphSyncStatus {
  /// 创建状态。
  const EphSyncStatus({
    this.newest,
    this.listed = false,
    this.connected = false,
  });

  /// 还没查过（没连上码表 / 目录列不出来）。
  const EphSyncStatus.unknown({this.connected = false})
      : newest = null,
        listed = false;

  /// 码表上最新一份星历的时间（本地时区）。[listed] 为真时它才有意义。
  final DateTime? newest;

  /// 是否成功列过码表目录（false = 本次结果不可信，按「没查过」显示）。
  final bool listed;

  /// 刷新这一刻码表是否在线。
  final bool connected;

  /// 下次自动同步的时间点：最新星历时间 + [EphAutoSync.maxAge]。
  ///
  /// 没有星历时为 null —— 那是「一连上就同步」。
  DateTime? get nextAt => newest?.add(EphAutoSync.maxAge);

  /// 是否已经到期（没有星历，或已过 [nextAt]）。
  bool get due {
    final at = nextAt;
    return at == null || DateTime.now().isAfter(at);
  }

  /// 给界面显示的一句话。倒计时用相对时间（`2 小时后`），比绝对时刻更直观。
  String get label {
    if (!connected) return LocaleKeys.ephNextOnConnect.tr;
    /* 连着但目录没读出来：别假装知道（更不能显示"已到期"），只说会同步。 */
    if (!listed) return LocaleKeys.ephNextSyncing.tr;
    final at = nextAt;
    if (at == null) return LocaleKeys.ephNextNone.tr;

    final left = at.difference(DateTime.now());
    if (left <= Duration.zero) return LocaleKeys.ephNextDue.tr;

    final mins = left.inMinutes;
    if (mins < 1) return LocaleKeys.ephNextSyncing.tr;
    if (mins < 60) return LocaleKeys.ephNextInMin.trParams({'n': '$mins'});

    final h = left.inHours;
    final m = mins % 60;
    if (m == 0) return LocaleKeys.ephNextInHours.trParams({'h': '$h'});
    return LocaleKeys.ephNextInHoursMin.trParams({'h': '$h', 'm': '$m'});
  }
}

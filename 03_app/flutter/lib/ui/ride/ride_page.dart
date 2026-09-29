import 'package:flutter/material.dart';
import 'package:get/get.dart';
import 'package:sifli_companion/app/app_routes.dart';
import 'package:sifli_companion/app/app_theme.dart';
import 'package:sifli_companion/i18n/locale_keys.dart';
import 'package:sifli_companion/ride/gpx_util.dart';
import 'package:sifli_companion/ride/ride_record_store.dart';
import 'package:sifli_companion/ride/ride_week_goals.dart';
import 'package:sifli_companion/ui/ride/import_upload.dart';
import 'package:sifli_companion/ride/ride_period_stats.dart';
import 'package:sifli_companion/ui/ride/ride_month_picker.dart';
import 'package:sifli_companion/ui/ride/ride_period_data.dart';
import 'package:sifli_companion/ui/ride/ride_period_header.dart';
import 'package:sifli_companion/ui/ride/ride_track_thumb.dart';
import 'package:sifli_companion/ui/widgets/helm_rise.dart';
import 'package:sifli_companion/ui/ride/ride_week_card.dart';
import 'package:sifli_companion/ui/shell/shell_controller.dart';
import 'package:sifli_companion/ui/widgets/helm_chrome.dart';

/// 骑行 Tab：本周三环 + 周 / 月 / 统计列表，支持重命名和批量操作。
class RidePage extends StatefulWidget {
  /// 创建骑行页。
  const RidePage({super.key});

  @override
  State<RidePage> createState() => _RidePageState();
}

class _RidePageState extends State<RidePage> {
  RidePeriod _period = RidePeriod.week;

  /// 月视图正在看的月份（可往前翻）；周固定本周、统计固定全部。
  DateTime _monthAnchor = DateTime.now();

  final _search = TextEditingController();
  bool _selecting = false;
  final _picked = <String>{};
  bool _busy = false;
  bool _hydrating = false;
  RideWeekGoals _goals = RideWeekGoals.defaults;

  ShellController get _shell => Get.find<ShellController>();

  @override
  void initState() {
    super.initState();
    _loadGoals();
  }

  @override
  void dispose() {
    _search.dispose();
    super.dispose();
  }

  Future<void> _loadGoals() async {
    final goals = await RideWeekGoals.load();
    if (!mounted) return;
    setState(() => _goals = goals);
  }

  void _maybeHydrate(List<RideRecord> all) {
    if (_hydrating) return;
    if (all.every((r) => r.durationSec != null && r.preview != null)) return;
    _hydrating = true;
    WidgetsBinding.instance.addPostFrameCallback((_) async {
      final changed = await RideRecordStore().ensureDurations();
      _hydrating = false;
      if (changed && mounted) setState(() {});
    });
  }

  Future<void> _editWeekPlan() async {
    final next = await showRideWeekPlanSheet(context, _goals);
    if (next == null || !mounted) return;
    await next.save();
    setState(() => _goals = next);
  }

  String _fmtSize(int n) {
    if (n < 1024) return '$n B';
    if (n < 1024 * 1024) return '${(n / 1024).toStringAsFixed(1)} KB';
    return '${(n / (1024 * 1024)).toStringAsFixed(1)} MB';
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

  Future<bool> _confirmDelete(String message) async {
    final ok = await Get.dialog<bool>(
      AlertDialog(
        title: Text(LocaleKeys.rideDelete.tr),
        content: Text(message),
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
    return ok == true;
  }

  Future<void> _rename(RideRecord ride) async {
    final name = await HelmPromptDialog.show(
      context,
      title: LocaleKeys.rideRename.tr,
      cancel: LocaleKeys.cancel.tr,
      confirm: LocaleKeys.confirm.tr,
      label: LocaleKeys.navRoutesName.tr,
      hint: LocaleKeys.rideRenameHint.tr,
      initial: ride.title,
    );
    if (name == null || name.isEmpty) return;
    await RideRecordStore().setDisplayName(ride.fileName, name);
    _shell.bumpRides();
    if (mounted) setState(() {});
  }

  Future<void> _deletePicked() async {
    if (_picked.isEmpty || _busy) return;
    final ok = await _confirmDelete(
      LocaleKeys.rideBatchDeleteConfirm.trParams({'n': '${_picked.length}'}),
    );
    if (!ok) return;
    await RideRecordStore().removeMany(Set<String>.of(_picked));
    _picked.clear();
    _selecting = false;
    _shell.bumpRides();
    if (mounted) setState(() {});
  }

  Future<void> _uploadPicked(List<RideRecord> visible) async {
    if (_picked.isEmpty || _busy) return;
    final items = [
      for (final r in visible)
        if (_picked.contains(r.fileName)) (path: r.localPath, title: r.title),
    ];
    if (items.length == 1) {
      setState(() => _busy = true);
      try {
        await ImportUpload.push(
          context: context,
          localPath: items.first.path,
          defaultName: items.first.title,
        );
      } finally {
        if (mounted) setState(() => _busy = false);
      }
      return;
    }
    setState(() => _busy = true);
    try {
      await ImportUpload.pushMany(context, items);
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  void _toggleSelect(String fileName) {
    setState(() {
      if (_picked.contains(fileName)) {
        _picked.remove(fileName);
      } else {
        _picked.add(fileName);
      }
    });
  }

  void _exitSelect() {
    setState(() {
      _selecting = false;
      _picked.clear();
    });
  }

  @override
  Widget build(BuildContext context) {
    return SafeArea(
      child: Obx(() {
        _shell.rideTick.value;
        final all = RideRecordStore().items;
        _maybeHydrate(all);
        final rides = _visibleRides(all);
        final week = RidePeriodFilter.apply(all, RidePeriod.week);
        var weekKm = 0.0;
        var weekSec = 0;
        for (final r in week) {
          weekKm += r.distanceKm ?? 0;
          weekSec += r.durationSec ?? 0;
        }
        // 周视图保持原样；月 / 统计是数据视图（大数字 + 柱状图 + 可搜索记录）。
        // 「导航入口」只属于周视图；逐条列表的删除 / 多选周、月都有，统计只给总账。
        // 月那批记录可能是空的（翻到还没骑过的月份、或搜索没命中），那就不给「选择」。
        final isWeek = _period == RidePeriod.week;
        final canSelect = all.isNotEmpty &&
            _period != RidePeriod.all &&
            (isWeek || rides.isNotEmpty);

        return Column(
          children: [
            Expanded(
              child: ListView(
                padding: const EdgeInsets.only(bottom: 24),
                children: [
                  HelmTitleRow(
                    title: _selecting
                        ? LocaleKeys.rideSelectedN.trParams({
                            'n': '${_picked.length}',
                          })
                        : LocaleKeys.rideTitle.tr,
                    trailing: !canSelect
                        ? null
                        : TextButton(
                            onPressed: _selecting
                                ? _exitSelect
                                : () => setState(() => _selecting = true),
                            child: Text(
                              _selecting
                                  ? LocaleKeys.rideSelectDone.tr
                                  : LocaleKeys.rideSelect.tr,
                            ),
                          ),
                  ),
                  // 切换视图时让周环平滑收起，而不是"啪"地消失。
                  AnimatedSize(
                    duration: const Duration(milliseconds: 260),
                    curve: Curves.easeOutCubic,
                    alignment: Alignment.topCenter,
                    child: isWeek
                        ? RideWeekCard(
                            rideCount: week.length,
                            durationSec: weekSec,
                            km: weekKm,
                            goals: _goals,
                            onEdit: _editWeekPlan,
                          )
                        : const SizedBox(width: double.infinity),
                  ),
                  Padding(
                    padding: const EdgeInsets.fromLTRB(16, 8, 16, 0),
                    child: SegmentedButton<RidePeriod>(
                      segments: [
                        ButtonSegment(
                          value: RidePeriod.week,
                          label: Text(LocaleKeys.ridePeriodWeek.tr),
                        ),
                        ButtonSegment(
                          value: RidePeriod.month,
                          label: Text(LocaleKeys.ridePeriodMonth.tr),
                        ),
                        ButtonSegment(
                          value: RidePeriod.all,
                          label: Text(LocaleKeys.ridePeriodStats.tr),
                        ),
                      ],
                      selected: {_period},
                      showSelectedIcon: false,
                      onSelectionChanged: (next) {
                        // 换视图时清掉搜索：免得"月里搜的关键词"悄悄筛着统计视图。
                        _search.clear();
                        setState(() {
                          _period = next.first;
                          _picked.clear();
                          _selecting = false;
                        });
                      },
                    ),
                  ),
                  // 三套内容切换只做**尺寸过渡**：内容直接换，但卡片高度是"长"
                  // 过去的 —— 淡入淡出在深色底上看着像闪，位移/形变才是物理感。
                  AnimatedSize(
                    duration: const Duration(milliseconds: 260),
                    curve: Curves.easeOutCubic,
                    alignment: Alignment.topCenter,
                    child: KeyedSubtree(
                      key: ValueKey(
                        _period == RidePeriod.month
                            ? 'm-${_monthAnchor.year}-${_monthAnchor.month}'
                            : _period.name,
                      ),
                      child: _periodSection(rides),
                    ),
                  ),
                ],
              ),
            ),
            if (_selecting) _batchBar(rides),
          ],
        );
      }),
    );
  }

  /// 分段控件下面那块内容。
  ///
  /// 周 = 「导航入口 + 记录列表」原样；月 / 统计 = 数据视图：大数字摘要 +
  /// 两张柱状图 + 可搜索的记录列表。[visible] 是当前视图里看得见的那批记录
  /// （周 = 本周，月 = 正在看的月 + 搜索词），列表和批量操作都以它为准。
  Widget _periodSection(List<RideRecord> visible) {
    if (_period == RidePeriod.week) {
      return Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          const SizedBox(height: 16),
          if (!_selecting)
            Padding(
              padding: const EdgeInsets.symmetric(horizontal: 16),
              child: _weekChart(),
            ),
          if (!_selecting) ...[
            const SizedBox(height: 16),
            Padding(
              padding: const EdgeInsets.symmetric(horizontal: 16),
              child: HelmCard(
                child: HelmTileGroup(
                  children: [
                    HelmNavTile(
                      color: AppTheme.iconGpx,
                      icon: Icons.timeline,
                      title: LocaleKeys.rideGpx.tr,
                      subtitle: LocaleKeys.rideGpxSub.tr,
                      onTap: () => Get.toNamed(AppRoutes.gpxLibrary),
                    ),
                    HelmNavTile(
                      color: AppTheme.iconNav,
                      icon: Icons.place_outlined,
                      title: LocaleKeys.rideNavRoutes.tr,
                      subtitle: LocaleKeys.rideNavRoutesSub.tr,
                      onTap: () => Get.toNamed(AppRoutes.navRoutes),
                    ),
                    HelmNavTile(
                      color: AppTheme.iconFav,
                      icon: Icons.star_outline,
                      title: LocaleKeys.rideFavorites.tr,
                      subtitle: LocaleKeys.rideFavoritesSub.tr,
                      onTap: () => Get.toNamed(AppRoutes.favorites),
                    ),
                  ],
                ),
              ),
            ),
          ],
          _weekListSection(visible),
        ],
      );
    }

    final all = RideRecordStore().items;
    final range = _dataRange(all);
    if (range == null) {
      return Padding(
        padding: const EdgeInsets.fromLTRB(16, 16, 16, 0),
        child: _emptyCard(LocaleKeys.rideEmpty.tr),
      );
    }

    final isMonth = _period == RidePeriod.month;
    // 月看周、统计看月：格子和「本周」是同一套周界，数字能对上。
    final span = isMonth ? RideBucketSpan.week : RideBucketSpan.month;
    final grid = RidePeriodStats.slots(range, span: span);
    final buckets = RidePeriodStats.buckets(all, range, span: span);
    final total = RidePeriodStats.total(buckets);
    final n = buckets.isEmpty ? 1 : buckets.length;
    final label = _rangeLabel(range);
    final q = _search.text.trim().toLowerCase();
    final rides = visible;
    final avgKey =
        isMonth ? LocaleKeys.rideStatAvgWeek : LocaleKeys.rideStatAvgMonth;
    final kmAvg = total.km / n;
    final minAvg = total.durationSec / 60.0 / n;
    final highlight = RidePeriodStats.slotOfToday(grid);
    final labels = rideChartLabels(grid, span);

    return Padding(
      padding: const EdgeInsets.fromLTRB(16, 12, 16, 0),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          if (isMonth) ...[
            RidePeriodHeader(
              label: label,
              // 往前永远点得动：空的月份给张"还没有骑行记录"的卡，比一个点不动的
              // 箭头清楚 —— 用户看到置灰只会当成坏了。
              canBack: true,
              canForward: RidePeriodStats.canForward(
                RidePeriod.month,
                _monthAnchor,
              ),
              onStep: _stepMonth,
              onNow: range.isCurrent ? null : _backToMonth,
              onTapLabel: () => _pickMonth(all),
            ),
            const SizedBox(height: 10),
          ],
          RideStatSummaryCard(
            km: total.km,
            durationSec: total.durationSec,
            rides: total.rides,
            caption: [
              avgKey.tr,
              '${kmAvg.toStringAsFixed(1)} km',
              RideWeekGoals.formatSeconds((total.durationSec / n).round()),
            ].join(' '),
          ),
          const SizedBox(height: 12),
          RideStatChartCard(
            title: (isMonth
                    ? LocaleKeys.rideChartKmWeek
                    : LocaleKeys.rideChartKmMonth)
                .tr,
            subtitle: [
              LocaleKeys.rideStatsTotal.tr,
              '${total.km.toStringAsFixed(1)} km',
              '· ${avgKey.tr} ${kmAvg.toStringAsFixed(1)} km',
            ].join(' '),
            values: [for (final b in buckets) b.km],
            labels: labels,
            color: AppTheme.ringCyan,
            avg: kmAvg,
            avgText: kmAvg.toStringAsFixed(1),
            highlight: highlight,
          ),
          const SizedBox(height: 12),
          RideStatChartCard(
            title: (isMonth
                    ? LocaleKeys.rideChartTimeWeek
                    : LocaleKeys.rideChartTimeMonth)
                .tr,
            subtitle: [
              LocaleKeys.rideStatsTotal.tr,
              RideWeekGoals.formatSeconds(total.durationSec),
              '· ${avgKey.tr} ${RideWeekGoals.formatMinutes(minAvg.round())}',
            ].join(' '),
            values: [for (final b in buckets) b.durationSec / 60.0],
            labels: labels,
            color: AppTheme.ringLime,
            avg: minAvg,
            avgText: RideWeekGoals.formatMinutes(minAvg.round()),
            highlight: highlight,
          ),
          const SizedBox(height: 16),
          // 统计只给总账：逐条记录留在周 / 月视图里。
          if (!isMonth)
            _summaryCard(all, total)
          else ...[
            TextField(
              controller: _search,
              // 换了关键词就是换了一批记录：原先选中的可能已经不在眼前，清掉。
              onChanged: (_) => setState(_picked.clear),
              decoration: InputDecoration(
                prefixIcon: const Icon(Icons.search),
                hintText: LocaleKeys.rideSearchHint.tr,
                isDense: true,
                suffixIcon: _search.text.isEmpty
                    ? null
                    : IconButton(
                        onPressed: () => setState(_search.clear),
                        icon: const Icon(Icons.close),
                        tooltip: LocaleKeys.cancel.tr,
                      ),
              ),
            ),
            const SizedBox(height: 12),
            // 这里在 16px 外 padding 里，用和 HelmSectionTitle 同级的小标题，
            // 免得左边距变成 36 和卡片对不齐。
            Padding(
              padding: const EdgeInsets.fromLTRB(2, 4, 2, 8),
              child: Text(
                q.isEmpty
                    ? LocaleKeys.rideRecentIn.trParams({'range': label})
                    : LocaleKeys.rideSearchCount.trParams({
                        'n': '${rides.length}',
                      }),
                style: const TextStyle(
                  color: Colors.white,
                  fontSize: 16,
                  fontWeight: FontWeight.w600,
                ),
              ),
            ),
            _statList(rides, total.km, label, q),
          ],
        ],
      ),
    );
  }

  /// 周视图的一张图：一…日 7 格，每日里程。
  ///
  /// 格子口径和月/统计里的「周」一致（同一套周界），所以这里的某根柱子等于
  /// 月视图里那一周的合计。只留里程 —— 时长在上面的三环里已经给过了。
  Widget _weekChart() {
    final all = RideRecordStore().items;
    final range = RidePeriodStats.range(RidePeriod.week, DateTime.now())!;
    final grid = RidePeriodStats.slots(range);
    final buckets = RidePeriodStats.buckets(all, range);
    final total = RidePeriodStats.total(buckets);
    final kmAvg = total.km / 7;

    return RideStatChartCard(
      title: LocaleKeys.rideChartKmDay.tr,
      subtitle: [
        LocaleKeys.rideStatsTotal.tr,
        '${total.km.toStringAsFixed(1)} km',
        '· ${LocaleKeys.rideStatAvgDay.tr} ${kmAvg.toStringAsFixed(1)} km',
      ].join(' '),
      values: [for (final b in buckets) b.km],
      labels: rideChartLabels(grid, RideBucketSpan.day),
      color: AppTheme.ringCyan,
      avg: kmAvg,
      avgText: kmAvg.toStringAsFixed(1),
      highlight: RidePeriodStats.slotOfToday(grid),
      chartHeight: 64,
    );
  }

  /// 统计视图的总账（健康 App 那种"拉个总结"）：不给逐条记录，只给结论。
  Widget _summaryCard(List<RideRecord> all, RideBucket total) {
    RideRecord? farthest;
    RideRecord? longest;
    final days = <String>{};
    DateTime? first;
    DateTime? last;

    for (final r in all) {
      if (r.mtime <= 0) continue;
      final t = DateTime.fromMillisecondsSinceEpoch(r.mtime * 1000);
      if (first == null || t.isBefore(first)) first = t;
      if (last == null || t.isAfter(last)) last = t;
      days.add('${t.year}-${t.month}-${t.day}');
      if ((r.distanceKm ?? 0) > (farthest?.distanceKm ?? 0)) farthest = r;
      if ((r.durationSec ?? 0) > (longest?.durationSec ?? 0)) longest = r;
    }

    final n = total.rides == 0 ? 1 : total.rides;
    final farT = farthest?.mtime ?? 0;
    final longT = longest?.mtime ?? 0;

    return HelmCard(
      child: Padding(
        padding: const EdgeInsets.fromLTRB(18, 16, 18, 12),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                const Icon(
                  Icons.insights_rounded,
                  size: 16,
                  color: AppTheme.muted,
                ),
                const SizedBox(width: 6),
                Text(
                  LocaleKeys.rideSummaryTitle.tr,
                  style: const TextStyle(
                    color: Colors.white,
                    fontSize: 13,
                    fontWeight: FontWeight.w600,
                  ),
                ),
              ],
            ),
            const SizedBox(height: 8),
            _kv(
              LocaleKeys.rideSummarySpan.tr,
              '${_fmtDay(first)} – ${_fmtDay(last)}',
            ),
            _kv(
              LocaleKeys.rideSummaryAvgRide.tr,
              '${(total.km / n).toStringAsFixed(1)} km · '
                  '${RideWeekGoals.formatSeconds((total.durationSec / n).round())}',
            ),
            if (farthest?.distanceKm != null)
              _kv(
                LocaleKeys.rideSummaryBestKm.tr,
                '${farthest!.distanceKm!.toStringAsFixed(1)} km · '
                    '${_fmtDay(DateTime.fromMillisecondsSinceEpoch(farT * 1000))}',
              ),
            if (longest?.durationSec != null)
              _kv(
                LocaleKeys.rideSummaryBestDur.tr,
                '${RideWeekGoals.formatSeconds(longest!.durationSec!)} · '
                    '${_fmtDay(DateTime.fromMillisecondsSinceEpoch(longT * 1000))}',
              ),
            _kv(LocaleKeys.rideSummaryDays.tr, '${days.length}'),
          ],
        ),
      ),
    );
  }

  Widget _kv(String label, String value) {
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 7),
      child: Row(
        children: [
          Text(
            label,
            style: const TextStyle(color: AppTheme.muted, fontSize: 13),
          ),
          const Spacer(),
          // 值可能挺长（`2026/5/20 – 2026/9/17`）：装不下就缩，不给省略号。
          Flexible(
            child: FittedBox(
              fit: BoxFit.scaleDown,
              alignment: Alignment.centerRight,
              child: Text(
                value,
                maxLines: 1,
                style: const TextStyle(
                  color: Colors.white,
                  fontSize: 14,
                  fontWeight: FontWeight.w600,
                  fontFeatures: [FontFeature.tabularFigures()],
                ),
              ),
            ),
          ),
        ],
      ),
    );
  }

  /// `2026/5/20`；没记录时给个破折号。
  static String _fmtDay(DateTime? t) =>
      t == null ? '—' : '${t.year}/${t.month}/${t.day}';

  /// 月视图里的记录列表：和本周那张一样可多选、可左滑删除，副标题多了里程占比。
  Widget _statList(
    List<RideRecord> rides,
    double periodKm,
    String label,
    String q,
  ) {
    if (rides.isEmpty) {
      return _emptyCard(
        q.isEmpty
            ? LocaleKeys.rideEmptyIn.trParams({'range': label})
            : LocaleKeys.rideSearchEmpty.trParams({'q': q}),
      );
    }
    return HelmCard(
      child: HelmTileGroup(
        children: [
          // 逐条从下方落位（延迟封顶），列表一眼"铺"开而不是整块跳出。
          for (var i = 0; i < rides.length; i++)
            HelmRise(
              delay: HelmRise.stagger(i),
              child: _rideTile(rides[i], _statSubtitle(rides[i], periodKm)),
            ),
        ],
      ),
    );
  }

  /// 周视图那条记录的副标题：时间 / 时长 / 里程 / 文件大小。
  String _weekSubtitle(RideRecord ride) => [
        _fmtTime(ride.mtime),
        if (ride.durationSec != null && ride.durationSec! > 0)
          GpxUtil.formatElapsed(Duration(seconds: ride.durationSec!)),
        if (ride.distanceKm != null && ride.distanceKm! > 0)
          LocaleKeys.rideDistance.trParams({
            'km': ride.distanceKm!.toStringAsFixed(1),
          }),
        LocaleKeys.rideSize.trParams({'size': _fmtSize(ride.size)}),
      ].where((s) => s.isNotEmpty).join(' · ');

  /// 数据视图那条记录的副标题：换成"占本区间多少公里"。
  String _statSubtitle(RideRecord ride, double periodKm) => [
        _fmtTime(ride.mtime),
        if (ride.durationSec != null && ride.durationSec! > 0)
          GpxUtil.formatElapsed(Duration(seconds: ride.durationSec!)),
        if (ride.distanceKm != null && ride.distanceKm! > 0)
          LocaleKeys.rideDistance.trParams({
            'km': ride.distanceKm!.toStringAsFixed(1),
          }),
        if ((ride.distanceKm ?? 0) > 0 && periodKm > 0)
          LocaleKeys.rideShareOf.trParams({
            'p': '${((ride.distanceKm! / periodKm) * 100).round()}',
          }),
      ].where((s) => s.isNotEmpty).join(' · ');

  /// 列表里的一条记录（周 / 月共用）：缩略轨迹 + 名称 + 副标题。
  ///
  /// 长按进多选、多选态点一下勾选、平时左滑删除 —— 两个视图行为完全一致。
  Widget _rideTile(RideRecord ride, String subtitle) {
    return Dismissible(
      key: ValueKey(ride.fileName),
      direction: _selecting ? DismissDirection.none : DismissDirection.endToStart,
      background: const ColoredBox(
        color: Color(0xFF3A1C1C),
        child: Align(
          alignment: Alignment.centerRight,
          child: Padding(
            padding: EdgeInsets.only(right: 20),
            child: Icon(Icons.delete_outline, color: Color(0xFFEF4444)),
          ),
        ),
      ),
      confirmDismiss: (_) => _confirmDelete(
        LocaleKeys.deleteConfirm.trParams({'name': ride.title}),
      ),
      onDismissed: (_) async {
        await RideRecordStore().remove(ride.fileName);
        _picked.remove(ride.fileName);
        _shell.bumpRides();
      },
      child: HelmNavTile(
        color: AppTheme.iconRide,
        icon: Icons.pedal_bike,
        leading: _selecting
            ? Checkbox(
                value: _picked.contains(ride.fileName),
                onChanged: (_) => _toggleSelect(ride.fileName),
              )
            : RideTrackThumb(points: ride.preview ?? const []),
        showChevron: !_selecting,
        title: ride.title,
        subtitle: subtitle,
        onLongPress: () {
          setState(() {
            _selecting = true;
            _picked.add(ride.fileName);
          });
        },
        onTap: () {
          if (_selecting) {
            _toggleSelect(ride.fileName);
            return;
          }
          Get.toNamed(
            AppRoutes.rideDetail,
            arguments: ride.fileName,
          );
        },
      ),
    );
  }

  /// 当前视图里看得见的那批记录。
  ///
  /// 周 = 本周；月 = 正在看的那个月，再叠加搜索词；统计 = 全部。列表、批量条、
  /// 「选择」按钮都按它算 —— 月视图翻页 / 搜索之后，"全选"只能选中眼前这些。
  List<RideRecord> _visibleRides(List<RideRecord> all) {
    if (_period != RidePeriod.month) {
      return RidePeriodFilter.apply(all, _period);
    }
    final range = RidePeriodStats.range(RidePeriod.month, _monthAnchor);
    if (range == null) return const [];
    final inRange = RidePeriodStats.inRange(all, range);
    final q = _search.text.trim().toLowerCase();
    if (q.isEmpty) return inRange;
    return [for (final r in inRange) if (_matches(r, q)) r];
  }

  /// 当前视图对应的区间。周 = 本周（不走这里），月 = 正在看的月，统计 = 全部。
  RideRange? _dataRange(List<RideRecord> all) => switch (_period) {
        RidePeriod.week => RidePeriodStats.range(RidePeriod.week, DateTime.now()),
        RidePeriod.month => RidePeriodStats.range(RidePeriod.month, _monthAnchor),
        RidePeriod.all => RidePeriodStats.everything(all),
      };

  /// 区间名：本月写「本月」，别的月份写 `2026/8`。
  String _rangeLabel(RideRange range) => range.isCurrent
      ? LocaleKeys.rideThisMonth.tr
      : '${range.start.year}/${range.start.month}';

  /// 往前 / 往后翻一个月（-1 = 更早），不允许翻到未来。
  void _stepMonth(int delta) {
    final next = RidePeriodStats.shift(RidePeriod.month, _monthAnchor, delta);
    final r = RidePeriodStats.range(RidePeriod.month, next);
    if (r == null) return;
    final now = DateTime.now();
    if (delta > 0 && r.start.isAfter(DateTime(now.year, now.month, now.day))) {
      return;
    }
    _gotoMonth(next);
  }

  void _backToMonth() => _gotoMonth(DateTime.now());

  /// 换一个正在看的月份：翻页和月份选择都走这里。
  void _gotoMonth(DateTime target) {
    _search.clear();
    // 换月份后选中的那批就看不见了，别让它们还挂在批量条上。
    setState(() {
      _monthAnchor = target;
      _picked.clear();
    });
  }

  /// 点区间名弹出月份选择 —— 一个月一个月地点箭头，回到去年某次骑行要点十几下。
  Future<void> _pickMonth(List<RideRecord> all) async {
    final picked = await showRideMonthPicker(
      context,
      selected: _monthAnchor,
      latest: DateTime.now(),
      rides: all,
    );
    if (picked == null || !mounted) return;
    _gotoMonth(picked);
  }

  /// 记录名 / 文件名 / 日期（`2026-08-17`、`08-17`、`2026/8` 都能搜到）。
  bool _matches(RideRecord r, String q) {
    if (r.title.toLowerCase().contains(q)) return true;
    if (r.fileName.toLowerCase().contains(q)) return true;
    final t = _fmtTime(r.mtime);
    return t.contains(q) || t.replaceAll('-', '/').contains(q);
  }

  /// 周视图的记录列表：可多选（长按进入）、可左滑删除。
  Widget _weekListSection(List<RideRecord> rides) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        HelmSectionTitle(LocaleKeys.rideRecentWeek.tr),
        Padding(
          padding: const EdgeInsets.symmetric(horizontal: 16),
          child: rides.isEmpty
              ? _emptyCard(LocaleKeys.rideEmptyWeek.tr)
              : HelmCard(
                  child: HelmTileGroup(
                    children: [
                      for (final ride in rides)
                        _rideTile(ride, _weekSubtitle(ride)),
                    ],
                  ),
                ),
        ),
      ],
    );
  }

  /// 空态卡：圆底骑行图标 + 一句说明。
  Widget _emptyCard(String text) {
    return HelmCard(
      padding: const EdgeInsets.fromLTRB(20, 28, 20, 28),
      child: Column(
        children: [
          Container(
            width: 56,
            height: 56,
            decoration: BoxDecoration(
              shape: BoxShape.circle,
              color: AppTheme.iconRide.withValues(alpha: 0.18),
            ),
            child: const Icon(
              Icons.pedal_bike,
              color: AppTheme.iconRide,
              size: 28,
            ),
          ),
          const SizedBox(height: 12),
          Text(
            text,
            textAlign: TextAlign.center,
            style: const TextStyle(color: AppTheme.muted, height: 1.45),
          ),
        ],
      ),
    );
  }

  Widget _batchBar(List<RideRecord> rides) {
    final n = _picked.length;
    final can = n > 0 && !_busy;
    return Material(
      color: AppTheme.card,
      child: SafeArea(
        top: false,
        child: Padding(
          padding: const EdgeInsets.fromLTRB(8, 6, 8, 8),
          child: Row(
            children: [
              TextButton(
                onPressed: rides.isEmpty
                    ? null
                    : () {
                        setState(() {
                          if (_picked.length == rides.length) {
                            _picked.clear();
                          } else {
                            _picked
                              ..clear()
                              ..addAll(rides.map((e) => e.fileName));
                          }
                        });
                      },
                child: Text(
                  _picked.length == rides.length && rides.isNotEmpty
                      ? LocaleKeys.rideSelectNone.tr
                      : LocaleKeys.rideSelectAll.tr,
                ),
              ),
              const Spacer(),
              if (n == 1)
                IconButton(
                  tooltip: LocaleKeys.rideRename.tr,
                  onPressed: can
                      ? () {
                          final one = RideRecordStore().find(_picked.first);
                          if (one != null) _rename(one);
                        }
                      : null,
                  icon: const Icon(Icons.drive_file_rename_outline),
                ),
              IconButton(
                tooltip: LocaleKeys.rideUploadImport.tr,
                onPressed: can ? () => _uploadPicked(rides) : null,
                icon: const Icon(Icons.upload_outlined),
              ),
              IconButton(
                tooltip: LocaleKeys.rideBatchDelete.tr,
                onPressed: can ? _deletePicked : null,
                icon: const Icon(Icons.delete_outline, color: Color(0xFFEF4444)),
              ),
            ],
          ),
        ),
      ),
    );
  }
}

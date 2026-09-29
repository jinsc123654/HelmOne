import 'dart:convert';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter_localizations/flutter_localizations.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:get/get.dart';
import 'package:sifli_companion/app/app_theme.dart';
import 'package:sifli_companion/i18n/app_locale.dart';
import 'package:sifli_companion/i18n/app_translations.dart';
import 'package:sifli_companion/ride/ride_record_store.dart';
import 'package:sifli_companion/ui/ride/ride_page.dart';
import 'package:sifli_companion/ui/ride/ride_period_header.dart';
import 'package:sifli_companion/ui/shell/shell_controller.dart';

// 平台实现包是 path_provider / shared_preferences 的传递依赖，测试里要顶掉真机实现，
// 否则会往宿主的 ~/Documents 里写记录。
// ignore: depend_on_referenced_packages
import 'package:path_provider_platform_interface/path_provider_platform_interface.dart';
// ignore: depend_on_referenced_packages
import 'package:shared_preferences_platform_interface/in_memory_shared_preferences_async.dart';
// ignore: depend_on_referenced_packages
import 'package:shared_preferences_platform_interface/shared_preferences_async_platform_interface.dart';

/// 骑行页月视图的回归测试：多选 / 批量删除 / 月份牌 / 区间标题。
///
/// 背景：月视图从「和本周一样的一张记录列表」改成「大数字 + 柱状图 + 可搜索记录」
/// 时（f41df44），删除和多选一起被砍掉了 —— 列表只剩只读，顶部「选择」按钮在
/// 非周视图直接藏起来，批量条永远出不来。这里把那条路钉住：月视图里要能长按进
/// 多选、要能批量删、搜索后的「全选」只作用于眼前这批；点区间名要能弹出带月度
/// 记录的月份牌；月份名要在整行正中，两侧箭头都点得动。
///
/// 两个测试环境的坑（都踩过）：
/// - 页面里的「本周三环」是常驻循环动画（`_pulse/_spin/_halo` 都在 repeat），
///   `pumpAndSettle` 永远不返回 ⇒ 一律用 [_settle] 有界推帧；
/// - widget 测试跑在假时钟里，`dart:io` 的 await 不会自己回来 ⇒ 读写记录库要包在
///   `tester.runAsync` 里（[_seed] 由调用方包，[`_settleIo`] 负责等落盘）。
/// 被顶成"应用文档目录"的临时目录。
late Directory docs;

void main() {
  setUpAll(() {
    // Store 是单例、目录只在第一次 _ensureDir 时定下来，所以整个文件共用一份。
    SharedPreferencesAsyncPlatform.instance =
        InMemorySharedPreferencesAsync.empty();
    docs = Directory.systemTemp.createTempSync('helm_ride_month_test');
    PathProviderPlatform.instance = _TempDocs(docs.path);
    Get.testMode = true;
  });

  setUp(() async {
    Get.reset();
    Get.put<ShellController>(_TestShell());
    await _seed(const []);
  });

  tearDownAll(() => docs.deleteSync(recursive: true));

  testWidgets('月视图能长按进多选、批量删除', (tester) async {
    final now = DateTime.now();
    await tester.runAsync(() async {
      await _seed([
        _ride('r1.gpx', '晨骑', DateTime(now.year, now.month, now.day, 8)),
        _ride('r2.gpx', '晚骑', DateTime(now.year, now.month, now.day, 18)),
      ]);
    });
    await _pumpPage(tester);

    await tester.tap(find.text('月'));
    await _settle(tester);
    expect(find.text('晨骑'), findsOneWidget);

    // 长按进多选：两条都变成勾选框。
    await tester.longPress(find.text('晨骑'));
    await _settle(tester);
    expect(find.text('已选 1 条'), findsOneWidget);
    expect(find.byType(Checkbox), findsNWidgets(2));

    await tester.tap(find.text('全选'));
    await _settle(tester);
    expect(find.text('已选 2 条'), findsOneWidget);

    await tester.tap(find.byTooltip('删除'));
    await _settle(tester);
    await tester.tap(find.text('确定'));
    await _settleIo(tester);

    expect(RideRecordStore().items, isEmpty);
    // 删完回到普通态：批量条收起来，月视图给空态。
    expect(find.text('全选'), findsNothing);
    expect(find.textContaining('还没有骑行记录'), findsOneWidget);
  });

  testWidgets('月视图搜索后的全选只作用于搜到的这几条', (tester) async {
    final now = DateTime.now();
    await tester.runAsync(() async {
      await _seed([
        _ride('r1.gpx', '晨骑', DateTime(now.year, now.month, now.day, 8)),
        _ride('r2.gpx', '晚骑', DateTime(now.year, now.month, now.day, 18)),
      ]);
    });
    await _pumpPage(tester);

    await tester.tap(find.text('月'));
    await _settle(tester);
    expect(find.text('选择'), findsOneWidget);

    await tester.enterText(find.byType(TextField), '晚骑');
    await _settle(tester);
    expect(find.text('晨骑'), findsNothing);

    await tester.tap(find.text('选择'));
    await _settle(tester);
    await tester.tap(find.text('全选'));
    await _settle(tester);
    expect(find.text('已选 1 条'), findsOneWidget);

    await tester.tap(find.byTooltip('删除'));
    await _settle(tester);
    await tester.tap(find.text('确定'));
    await _settleIo(tester);

    // 只删掉搜到的那条，「晨骑」还在。
    expect(RideRecordStore().items.map((r) => r.fileName), ['r1.gpx']);
  });

  testWidgets('点区间名弹出月份牌，牌上有那个月的大致记录', (tester) async {
    final now = DateTime.now();
    await tester.runAsync(() async {
      await _seed([
        _ride('jan.gpx', '开年', DateTime(now.year, 1, 15), km: 33.3),
        if (now.month != 1)
          _ride('now.gpx', '本月的', DateTime(now.year, now.month, 15)),
      ]);
    });
    await _pumpPage(tester);

    await tester.tap(find.text('月'));
    await _settle(tester);
    await tester.tap(find.text('本月'));
    await _settle(tester);

    expect(find.text('选择月份'), findsOneWidget);
    // 有记录的月份报里程和次数，一次没骑的写「无数据」。
    expect(find.text('33.3 km'), findsWidgets);
    expect(find.text('无数据'), findsWidgets);
    // 年份行下面给全年合计。
    expect(find.textContaining('总计'), findsOneWidget);

    if (now.month == 1) return;
    // 点一月那张牌：直接跳到一月，列表换成一月那条。
    await tester.tap(find.text('1月'));
    await _settle(tester);
    expect(find.text('${now.year}/1'), findsWidgets);
    expect(find.text('开年'), findsOneWidget);
    expect(find.text('本月的'), findsNothing);
  });

  testWidgets('月份名始终居中，两侧箭头都点得动', (tester) async {
    final now = DateTime.now();
    await tester.runAsync(() async {
      await _seed([
        // 只有"本月"这一条：以前往前没有记录时 ‹ 是置灰的，用户点不动。
        _ride('now.gpx', '本月的', DateTime(now.year, now.month, 15)),
      ]);
    });
    await _pumpPage(tester);

    await tester.tap(find.text('月'));
    await _settle(tester);

    // 本月：右侧没有「回到当前」，月份名在整行正中。
    _expectLabelCentered(tester);

    await tester.tap(find.byIcon(Icons.chevron_left));
    await _settle(tester);
    final prev = DateTime(now.year, now.month - 1);
    expect(find.text('${prev.year}/${prev.month}'), findsWidgets);
    // 上一月：多了「回到当前」，月份名仍然在正中（以前会被挤偏 7px）。
    _expectLabelCentered(tester);

    // 往后回到本月：› 点得动，且不会走进未来。
    await tester.tap(find.byIcon(Icons.chevron_right).first);
    await _settle(tester);
    expect(find.text('本月'), findsOneWidget);
  });

  testWidgets('月视图没有记录时不给「选择」', (tester) async {
    // 记录在别的月份：当前月是空的。
    final now = DateTime.now();
    await tester.runAsync(() async {
      await _seed([
        _ride(
          'old.gpx',
          '上个月',
          DateTime(now.year, now.month, 1).subtract(const Duration(days: 3)),
        ),
      ]);
    });
    await _pumpPage(tester);

    await tester.tap(find.text('月'));
    await _settle(tester);
    expect(find.text('选择'), findsNothing);
  });
}

/// 有界推帧。页面上有常驻循环动画，`pumpAndSettle` 不适用。
Future<void> _settle(WidgetTester tester, [int frames = 15]) async {
  for (var i = 0; i < frames; i++) {
    await tester.pump(const Duration(milliseconds: 120));
  }
}

/// 读写记录库要落盘，而 widget 测试的假时钟等不到真 I/O：
/// 交替跑"真事件循环一轮"和"推一帧"，让 `removeMany` 那串 await 走完。
Future<void> _settleIo(WidgetTester tester) async {
  for (var i = 0; i < 12; i++) {
    await tester.runAsync(
      () => Future<void>.delayed(const Duration(milliseconds: 10)),
    );
    await tester.pump(const Duration(milliseconds: 120));
  }
  await _settle(tester);
}

/// 月份名落在整行正中（不带「回到当前」和带它两种状态都要如此）。
void _expectLabelCentered(WidgetTester tester) {
  final label = tester.getRect(find.byKey(const ValueKey('ridePeriodLabel')));
  final row = tester.getRect(find.byType(RidePeriodHeader));
  expect((label.center.dx - row.center.dx).abs(), lessThan(1));
}

/// 页面本体：真主题 + 真多语言，和 app 里跑的是同一套。
Future<void> _pumpPage(WidgetTester tester) async {
  // 月视图内容是「摘要卡 + 两张图 + 列表」，默认 800 高的测试视口会把列表挤出
  // 构建范围（ListView 不建看不见的孩子），直接拉高视口省掉滚动。
  tester.view.physicalSize = const Size(1179, 7200);
  tester.view.devicePixelRatio = 3;
  addTearDown(tester.view.reset);

  await tester.pumpWidget(
    GetMaterialApp(
      theme: AppTheme.dark,
      themeMode: ThemeMode.dark,
      translations: AppTranslations(),
      locale: AppLocale.zhCN,
      fallbackLocale: AppLocale.fallback,
      supportedLocales: AppLocale.supported,
      localizationsDelegates: const [
        GlobalMaterialLocalizations.delegate,
        GlobalWidgetsLocalizations.delegate,
        GlobalCupertinoLocalizations.delegate,
      ],
      home: const Scaffold(body: RidePage()),
    ),
  );
  await _settle(tester);
}

/// 往记录库里放一批记录（直接写索引，省掉 GPX 解析）。
Future<void> _seed(List<Map<String, Object?>> rows) async {
  await RideRecordStore().load();
  final index = File('${docs.path}/rides/index.json');
  await index.writeAsString(jsonEncode(rows));
  await RideRecordStore().load();
}

/// 一条记录：带时长和空缩略轨迹，`_maybeHydrate` 就不会再去读 GPX。
Map<String, Object?> _ride(
  String fileName,
  String title,
  DateTime when, {
  double km = 12.5,
}) =>
    {
      'fileName': fileName,
      'localPath': '${docs.path}/rides/$fileName',
      'size': 2048,
      'mtime': when.millisecondsSinceEpoch ~/ 1000,
      'distanceKm': km,
      'pointCount': 30,
      'durationSec': 3600,
      'preview': <Object?>[],
      'displayName': title,
      'remoteSize': null,
      'userRenamed': true,
    };

/// 文档目录换成临时目录。
class _TempDocs extends PathProviderPlatform {
  _TempDocs(this.root);

  final String root;

  @override
  Future<String?> getApplicationDocumentsPath() async => root;

  @override
  Future<String?> getApplicationSupportPath() async => root;

  @override
  Future<String?> getTemporaryPath() async => root;
}

/// 只借 ShellController 的 `rideTick` 计数，不碰 BLE / 前台服务。
///
/// 只顶掉 onInit：它一上来就连 BLE、问前台服务，测试里全是 MissingPlugin。
/// onClose 不用动 —— 基类那两下（摘 observer、取消记事件的订阅）本来就是空转。
class _TestShell extends ShellController {
  @override
  void onInit() {}
}

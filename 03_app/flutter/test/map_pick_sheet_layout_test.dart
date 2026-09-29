import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:sifli_companion/app/app_theme.dart';
import 'package:sifli_companion/ui/widgets/helm_map_edit.dart';

/// 地图选点面板的高度回归测试。
///
/// 收起态面板高度是写死的常量（[HelmEditSheet.peekBody]），内容一变高就会溢出，
/// 而溢出在真机上只表现为黄黑条纹加一条 `RenderFlex overflowed` 日志，很容易漏。
/// 这里用真实主题把两种面板内容量出来，断言不溢出。
///
/// 背景：真机（20260917 版）报过 `A RenderFlex overflowed by 55 pixels on the
/// bottom.` —— 准星模式会显示底栏，但当时的面板高度没把那 60 px 算进去。
void main() {
  /// 面板所在页面的可用高度（真机约 873，取 800 更保守）。
  const bodyHeight = 800.0;

  Widget host(Widget child, {double bottomInset = 0, double textScale = 1}) {
    return MaterialApp(
      theme: AppTheme.dark,
      home: MediaQuery(
        data: MediaQueryData(
          size: const Size(393, bodyHeight),
          padding: EdgeInsets.only(bottom: bottomInset),
          textScaler: TextScaler.linear(textScale),
        ),
        child: Scaffold(body: SizedBox(height: bodyHeight, child: child)),
      ),
    );
  }

  Widget sheet({
    required Widget peek,
    required double peekBody,
    Widget? footer,
    double bottomInset = 0,
    double textScale = 1,
  }) {
    return host(
      Align(
        alignment: Alignment.bottomCenter,
        child: HelmEditSheet(
          t: 0,
          peekBody: peekBody,
          maxHeight: bodyHeight,
          footer: footer,
          onHeaderTap: () {},
          onDragDelta: (_) {},
          onDragEnd: (_) {},
          peek: peek,
        ),
      ),
      bottomInset: bottomInset,
      textScale: textScale,
    );
  }

  /// 编辑页的面板内容：名称 + 里程。
  Widget editorPeek() => Column(
    crossAxisAlignment: CrossAxisAlignment.stretch,
    children: const [
      TextField(
        decoration: InputDecoration(hintText: '例如 周末环城', labelText: '名称'),
      ),
      SizedBox(height: 8),
      Text(
        '12 个点 · 34.56 km',
        style: TextStyle(
          color: AppTheme.muted,
          fontSize: 13,
          fontWeight: FontWeight.w600,
        ),
      ),
    ],
  );

  /// 收藏页的面板内容：只有名称。
  Widget favPeek() => const TextField(
    decoration: InputDecoration(labelText: '名称', hintText: '例如 家、公司、常去咖啡馆'),
  );

  /// 准星模式的底栏。
  Widget crosshairFooter() => Row(
    children: [
      OutlinedButton(onPressed: () {}, child: const Text('取消')),
      const SizedBox(width: 10),
      Expanded(
        child: FilledButton(onPressed: () {}, child: const Text('放到这里')),
      ),
    ],
  );

  testWidgets('编辑页面板（无底栏）不溢出', (tester) async {
    await tester.pumpWidget(sheet(peek: editorPeek(), peekBody: 118));
    await tester.pumpAndSettle();
    expect(tester.takeException(), isNull);
  });

  testWidgets('收藏页面板（无底栏）不溢出', (tester) async {
    await tester.pumpWidget(sheet(peek: favPeek(), peekBody: 90));
    await tester.pumpAndSettle();
    expect(tester.takeException(), isNull);
  });

  testWidgets('编辑页准星底栏不溢出（回归：真机曾溢出 55 px）', (tester) async {
    await tester.pumpWidget(
      sheet(
        peek: editorPeek(),
        peekBody: 118,
        footer: crosshairFooter(),
        bottomInset: 34,
      ),
    );
    await tester.pumpAndSettle();
    expect(tester.takeException(), isNull);
  });

  testWidgets('收藏页准星底栏不溢出', (tester) async {
    await tester.pumpWidget(
      sheet(
        peek: favPeek(),
        peekBody: 90,
        footer: crosshairFooter(),
        bottomInset: 34,
      ),
    );
    await tester.pumpAndSettle();
    expect(tester.takeException(), isNull);
  });

  // 收起态面板按内容定高（peekBody 只是动画基准），所以字体放大也不该溢出。
  for (final scale in <double>[1.15, 1.5, 2.0]) {
    testWidgets('编辑页面板在大字体（${scale}x）下仍不溢出', (tester) async {
      await tester.pumpWidget(
        sheet(
          peek: editorPeek(),
          peekBody: 118,
          footer: crosshairFooter(),
          bottomInset: 34,
          textScale: scale,
        ),
      );
      await tester.pumpAndSettle();
      expect(tester.takeException(), isNull);
    });

    testWidgets('收藏页面板在大字体（${scale}x）下仍不溢出', (tester) async {
      await tester.pumpWidget(
        sheet(
          peek: favPeek(),
          peekBody: 90,
          footer: crosshairFooter(),
          bottomInset: 34,
          textScale: scale,
        ),
      );
      await tester.pumpAndSettle();
      expect(tester.takeException(), isNull);
    });
  }

  /// 展开态：列表区空间不足时也不能溢出。
  ///
  /// 收藏页展开后放的是坐标卡（固定高度的一小块），面板刚拉开一点点时列表区
  /// 几乎没有高度，正是最容易溢出的位置。
  Widget expandedSheet({
    required Widget peek,
    required Widget list,
    required double peekBody,
    required double t,
    double bottomInset = 34,
    double textScale = 1,
  }) {
    return host(
      Align(
        alignment: Alignment.bottomCenter,
        child: HelmEditSheet(
          t: t,
          peekBody: peekBody,
          maxHeight: bodyHeight,
          list: list,
          onHeaderTap: () {},
          onDragDelta: (_) {},
          onDragEnd: (_) {},
          peek: peek,
        ),
      ),
      bottomInset: bottomInset,
      textScale: textScale,
    );
  }

  Widget coordBlock() => Column(
    mainAxisSize: MainAxisSize.min,
    crossAxisAlignment: CrossAxisAlignment.start,
    children: const [
      Text('坐标', style: TextStyle(fontSize: 12)),
      SizedBox(height: 6),
      Text('32.26804, 118.40127', style: TextStyle(fontSize: 17)),
      SizedBox(height: 12),
      Text('点地图可改位置，填名称后保存', style: TextStyle(fontSize: 13)),
    ],
  );

  for (final t in <double>[0.05, 0.3, 1.0]) {
    testWidgets('收藏页展开态（t=$t）不溢出', (tester) async {
      await tester.pumpWidget(
        expandedSheet(
          peek: favPeek(),
          // 页面里这块是包在 SingleChildScrollView 里的，测试按同样结构渲染。
          list: SingleChildScrollView(child: coordBlock()),
          peekBody: 90,
          t: t,
        ),
      );
      await tester.pumpAndSettle();
      expect(tester.takeException(), isNull);
    });

    testWidgets('编辑页展开态（t=$t）不溢出', (tester) async {
      await tester.pumpWidget(
        expandedSheet(
          peek: editorPeek(),
          list: const Center(child: Text('还没有点。点地图开始。')),
          peekBody: 118,
          t: t,
        ),
      );
      await tester.pumpAndSettle();
      expect(tester.takeException(), isNull);
    });
  }
}

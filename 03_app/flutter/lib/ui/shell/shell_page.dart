import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:get/get.dart';
import 'package:sifli_companion/app/app_assets.dart';
import 'package:sifli_companion/app/app_theme.dart';
import 'package:sifli_companion/i18n/locale_keys.dart';
import 'package:sifli_companion/ui/device/device_page.dart';
import 'package:sifli_companion/ui/ride/ride_page.dart';
import 'package:sifli_companion/ui/settings/settings_page.dart';
import 'package:sifli_companion/ui/shell/shell_controller.dart';
import 'package:sifli_companion/ui/widgets/helm_chrome.dart';

/// 产品首页：骑行 / 设备 / 设置。
class ShellPage extends GetView<ShellController> {
  /// 创建产品壳。
  const ShellPage({super.key});

  @override
  Widget build(BuildContext context) {
    return AnnotatedRegion<SystemUiOverlayStyle>(
      value: const SystemUiOverlayStyle(
        statusBarColor: Colors.transparent,
        statusBarIconBrightness: Brightness.light,
        statusBarBrightness: Brightness.dark,
        systemNavigationBarColor: AppTheme.background,
        systemNavigationBarIconBrightness: Brightness.light,
      ),
      child: Obx(() {
        final i = controller.tab.value;
        return _ShellScaffold(
          index: i,
          onTab: (v) => controller.tab.value = v,
        );
      }),
    );
  }
}

/// 壳本体：三个 Tab + 底部导航。
///
/// 这里管两件"物理感"的事：
/// 1. 切 Tab 只做**位移**（不做淡入淡出 —— 那看着像闪一下）；
/// 2. 底栏跟着滚动走：向下滚收起、向上滚或滑到顶拉出（iOS 那种）。
class _ShellScaffold extends StatefulWidget {
  const _ShellScaffold({required this.index, required this.onTab});

  final int index;
  final ValueChanged<int> onTab;

  @override
  State<_ShellScaffold> createState() => _ShellScaffoldState();
}

class _ShellScaffoldState extends State<_ShellScaffold> {
  bool _navHidden = false;

  /// 累计位移：手指抖一下不该触发，攒够一截再切。
  double _acc = 0;

  /// 触发阈值（逻辑像素）。
  static const _fling = 28.0;

  @override
  void didUpdateWidget(covariant _ShellScaffold old) {
    super.didUpdateWidget(old);
    // 换 Tab 一定把底栏放出来，否则新页面"没有底栏"很懵。
    if (old.index != widget.index && _navHidden) {
      _navHidden = false;
      _acc = 0;
    }
  }

  bool _onScroll(ScrollNotification n) {
    // 只跟最外层滚动：内嵌的横向列表、地图不该把底栏收走。
    if (n.depth != 0 || n.metrics.axis != Axis.vertical) return false;
    final m = n.metrics;
    if (m.maxScrollExtent <= 0) {
      _want(false);
      return false;
    }
    if (n is ScrollUpdateNotification) {
      final d = n.scrollDelta ?? 0;
      if (m.pixels <= 0) {
        _acc = 0;
        _want(false);
      } else if (d > 0) {
        _acc += d;
        if (_acc >= _fling && m.pixels > 72) {
          _acc = 0;
          _want(true);
        }
      } else if (d < 0) {
        _acc += d;
        if (_acc <= -_fling * 0.7) {
          _acc = 0;
          _want(false);
        }
      }
    } else if (n is OverscrollNotification && m.pixels <= 0) {
      _acc = 0;
      _want(false);
    }
    return false;
  }

  void _want(bool hidden) {
    if (hidden == _navHidden) return;
    setState(() => _navHidden = hidden);
  }

  @override
  Widget build(BuildContext context) {
    final i = widget.index;
    return Scaffold(
      body: NotificationListener<ScrollNotification>(
        onNotification: _onScroll,
        child: _TabSlide(
          index: i,
          child: IndexedStack(
            index: i,
            children: [
              // 每个 Tab 自己管动画的开关。
              //
              // IndexedStack 给子页包的是 Visibility(maintainAnimation: true) ——
              // 未显示的 Tab 里**动画照常每帧跑**。设备页那块「已连接呼吸光晕」是
              // 无限动画，于是只要连着码表，哪怕你在骑行页，它也在持续重建、持续
              // 让 Android 侧走一遍视图遍历。真机 ANR 现场的堆栈就卡在这个遍历里
              // （主线程 21 秒用户态 CPU）。这里按当前 Tab 静音，切走即停。
              for (var k = 0; k < 3; k++)
                TickerMode(
                  enabled: i == k,
                  child: switch (k) {
                    0 => const RidePage(),
                    1 => const DevicePage(),
                    _ => const SettingsPage(),
                  },
                ),
            ],
          ),
        ),
      ),
      // 收起 = 高度收到 0（内容顺势铺满），不是把它推到屏幕外留一条空白。
      bottomNavigationBar: AnimatedSize(
        duration: const Duration(milliseconds: 240),
        curve: Curves.easeOutCubic,
        alignment: Alignment.topCenter,
        child: _navHidden
            ? const SizedBox(width: double.infinity)
            : HelmBottomNav(
                index: i,
                onChanged: widget.onTab,
                items: [
                  HelmBottomNavItem(
                    icon: Icons.pedal_bike_outlined,
                    label: LocaleKeys.tabRide.tr,
                  ),
                  HelmBottomNavItem(
                    asset: AppAssets.bikeComputerNav,
                    label: LocaleKeys.tabDevice.tr,
                  ),
                  HelmBottomNavItem(
                    icon: Icons.settings_outlined,
                    label: LocaleKeys.tabSettings.tr,
                  ),
                ],
              ),
      ),
    );
  }
}

/// 切 Tab 的过渡：**只位移，不透明变化** —— 淡入淡出看着像闪一下。
///
/// 不能用 AnimatedSwitcher —— 那会重建子树、把每个 Tab 的滚动位置和状态丢掉；
/// 这里只是给 IndexedStack 套一层动画，状态照旧保留。
class _TabSlide extends StatefulWidget {
  const _TabSlide({required this.index, required this.child});

  final int index;
  final Widget child;

  @override
  State<_TabSlide> createState() => _TabSlideState();
}

class _TabSlideState extends State<_TabSlide>
    with SingleTickerProviderStateMixin {
  late final AnimationController _c = AnimationController(
    vsync: this,
    duration: const Duration(milliseconds: 240),
    value: 1,
  );

  @override
  void didUpdateWidget(covariant _TabSlide old) {
    super.didUpdateWidget(old);
    if (old.index != widget.index) _c.forward(from: 0);
  }

  @override
  void dispose() {
    _c.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return SlideTransition(
      position: Tween<Offset>(
        begin: const Offset(0, 0.02),
        end: Offset.zero,
      ).animate(
        CurvedAnimation(parent: _c, curve: Curves.easeOutCubic),
      ),
      child: widget.child,
    );
  }
}

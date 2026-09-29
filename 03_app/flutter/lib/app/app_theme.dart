import 'dart:async';

import 'package:flutter/cupertino.dart' show CupertinoPageTransitionsBuilder;
import 'package:flutter/material.dart';
import 'package:get/get.dart';
import 'package:sifli_companion/i18n/locale_keys.dart';
import 'package:sifli_companion/log/app_log.dart';

/// Helm One：小米运动式纯黑底、深灰分组卡、无描边。
abstract class AppTheme {
  /// 主强调色（底栏选中、活动外环）。偏珊瑚，避免施工黄橙。
  static const accent = Color(0xFFFF5C33);

  /// 骑行环：外环次数 / 中环时长 / 内环里程。
  static const ringOrange = Color(0xFFFF7A18);
  static const ringLime = Color(0xFFC6E54B);
  static const ringCyan = Color(0xFF3ED4E0);

  /// 页面底。
  static const background = Color(0xFF000000);

  /// 卡片 / 列表容器。
  static const card = Color(0xFF1C1C1E);

  /// 输入框、开关轨道等略浅一层。
  static const field = Color(0xFF2C2C2E);

  /// 同步等次要胶囊按钮。
  static const sync = Color(0xFF4A2C2A);

  /// 危险操作 / 错误（删除按钮、失败提示）。
  static const danger = Color(0xFFEF4444);

  /// 次要文字。
  static const muted = Color(0xFF8E8E93);

  /// 列表内部分割线（深灰，不是白边）。
  static const hairline = Color(0xFF2C2C2E);

  /// 分组卡圆角。
  static const radius = 16.0;

  /// 圆形图标底色。
  static const iconNav = Color(0xFF2563EB);
  static const iconGpx = Color(0xFF7C3AED);
  static const iconFav = Color(0xFFEA580C);
  static const iconRide = Color(0xFF0D9488);
  static const iconNotif = Color(0xFF3B82F6);
  static const iconOta = Color(0xFFEF4444);

  /// OTA：从云端拉固件。
  static const otaDownload = Color(0xFF0EA5E9);

  /// OTA：经 BLE 下发到码表。
  static const otaPush = Color(0xFFFF7A18);
  static const iconEph = Color(0xFF7C3AED);
  static const iconLang = Color(0xFF64748B);
  static const iconFile = Color(0xFF0EA5E9);
  static const iconUsb = Color(0xFF78716C);
  static const iconKeep = Color(0xFF16A34A);
  static const iconBle = Color(0xFF0EA5E9);
  static const iconCache = Color(0xFF0EA5E9);
  static const iconAppUpdate = Color(0xFF2563EB);
  static const iconLog = Color(0xFF64748B);
  static const iconCoredump = Color(0xFFCA8A04);
  static const iconAbout = Color(0xFF78716C);

  static const _none = BorderSide(style: BorderStyle.none);

  static OutlineInputBorder get _fieldBorder => OutlineInputBorder(
        borderRadius: BorderRadius.circular(12),
        borderSide: _none,
      );

  static OverlayEntry? _toast;
  static Timer? _toastTimer;

  /// 屏幕中央轻提示，不拦截点击、不挡底部操作。
  static void snack(String title, String message) {
    final ctx = Get.overlayContext ?? Get.context;
    if (ctx == null) return;
    final overlay = Overlay.maybeOf(ctx, rootOverlay: true);
    if (overlay == null) return;

    _toastTimer?.cancel();
    _toast?.remove();
    _toast = OverlayEntry(
      builder: (_) => IgnorePointer(
        child: _HelmToast(title: title, message: message),
      ),
    );
    overlay.insert(_toast!);
    _toastTimer = Timer(const Duration(milliseconds: 2200), () {
      _toast?.remove();
      _toast = null;
      _toastTimer = null;
    });
  }

  /// 产品界面只提示「失败」；异常原文写入按日日志。
  static void fail(String title, String tag, Object error, [StackTrace? st]) {
    unawaited(AppLog.e(tag, error, st));
    snack(title, LocaleKeys.failed.tr);
  }

  /// 页内错误文案：只返回「失败」，细节落盘。
  static String failText(String tag, Object error, [StackTrace? st]) {
    unawaited(AppLog.e(tag, error, st));
    return LocaleKeys.failed.tr;
  }

  /// 暗色主题：Card / 输入框 / 开关都不走 Material 白边。
  static ThemeData get dark {
    const scheme = ColorScheme.dark(
      primary: accent,
      onPrimary: Colors.white,
      secondary: sync,
      onSecondary: Colors.white,
      secondaryContainer: field,
      onSecondaryContainer: Colors.white,
      tertiary: accent,
      surface: background,
      onSurface: Colors.white,
      onSurfaceVariant: muted,
      surfaceContainerLowest: background,
      surfaceContainerLow: card,
      surfaceContainer: card,
      surfaceContainerHigh: card,
      surfaceContainerHighest: field,
      outline: Colors.transparent,
      outlineVariant: hairline,
      error: Color(0xFFEF4444),
      onError: Colors.white,
      errorContainer: Color(0xFF3A1C1C),
      onErrorContainer: Color(0xFFFFB4AB),
      primaryContainer: Color(0xFF3A2418),
      onPrimaryContainer: Color(0xFFFFDCC8),
    );

    final shape = RoundedRectangleBorder(
      borderRadius: BorderRadius.circular(radius),
      side: _none,
    );

    return ThemeData(
      useMaterial3: true,
      brightness: Brightness.dark,
      // 所有平台统一用 iOS 式转场：新页从右侧推入、旧页轻微后移，可右滑返回。
      // GetX 的路由已经走 Transition.cupertino，这里补上 Navigator.push 那部分。
      pageTransitionsTheme: const PageTransitionsTheme(
        builders: {
          TargetPlatform.android: CupertinoPageTransitionsBuilder(),
          TargetPlatform.iOS: CupertinoPageTransitionsBuilder(),
          TargetPlatform.macOS: CupertinoPageTransitionsBuilder(),
          TargetPlatform.linux: CupertinoPageTransitionsBuilder(),
          TargetPlatform.windows: CupertinoPageTransitionsBuilder(),
        },
      ),
      colorScheme: scheme,
      scaffoldBackgroundColor: background,
      canvasColor: background,
      applyElevationOverlayColor: false,
      splashColor: accent.withValues(alpha: 0.12),
      highlightColor: Colors.white.withValues(alpha: 0.04),
      dividerColor: hairline,
      dividerTheme: const DividerThemeData(
        color: hairline,
        thickness: 0.5,
        space: 0.5,
      ),
      appBarTheme: const AppBarTheme(
        backgroundColor: background,
        foregroundColor: Colors.white,
        elevation: 0,
        scrolledUnderElevation: 0,
        surfaceTintColor: Colors.transparent,
        shadowColor: Colors.transparent,
        centerTitle: false,
        titleTextStyle: TextStyle(
          color: Colors.white,
          fontSize: 18,
          fontWeight: FontWeight.w600,
        ),
        iconTheme: IconThemeData(color: Colors.white),
      ),
      cardTheme: CardThemeData(
        color: card,
        elevation: 0,
        margin: EdgeInsets.zero,
        shadowColor: Colors.transparent,
        surfaceTintColor: Colors.transparent,
        shape: shape,
        clipBehavior: Clip.antiAlias,
      ),
      dialogTheme: DialogThemeData(
        backgroundColor: card,
        surfaceTintColor: Colors.transparent,
        elevation: 0,
        shape: shape,
        titleTextStyle: const TextStyle(
          color: Colors.white,
          fontSize: 18,
          fontWeight: FontWeight.w600,
        ),
        contentTextStyle: const TextStyle(color: Colors.white, fontSize: 15),
      ),
      bottomSheetTheme: const BottomSheetThemeData(
        backgroundColor: card,
        surfaceTintColor: Colors.transparent,
        elevation: 0,
        modalBackgroundColor: card,
        shape: RoundedRectangleBorder(
          borderRadius: BorderRadius.vertical(top: Radius.circular(radius)),
        ),
      ),
      popupMenuTheme: PopupMenuThemeData(
        color: card,
        surfaceTintColor: Colors.transparent,
        elevation: 0,
        shape: shape,
        textStyle: const TextStyle(color: Colors.white),
      ),
      listTileTheme: const ListTileThemeData(
        iconColor: muted,
        textColor: Colors.white,
        subtitleTextStyle: TextStyle(color: muted, fontSize: 13),
        tileColor: Colors.transparent,
        contentPadding: EdgeInsets.symmetric(horizontal: 16),
      ),
      switchTheme: SwitchThemeData(
        thumbColor: WidgetStateProperty.resolveWith((states) {
          return states.contains(WidgetState.selected)
              ? Colors.white
              : const Color(0xFFAEAEB2);
        }),
        trackColor: WidgetStateProperty.resolveWith((states) {
          return states.contains(WidgetState.selected) ? accent : field;
        }),
        trackOutlineColor: WidgetStateProperty.all(Colors.transparent),
        trackOutlineWidth: WidgetStateProperty.all(0),
      ),
      radioTheme: RadioThemeData(
        fillColor: WidgetStateProperty.resolveWith((states) {
          return states.contains(WidgetState.selected) ? accent : muted;
        }),
      ),
      checkboxTheme: CheckboxThemeData(
        fillColor: WidgetStateProperty.resolveWith((states) {
          return states.contains(WidgetState.selected)
              ? accent
              : Colors.transparent;
        }),
        checkColor: WidgetStateProperty.all(Colors.white),
        side: const BorderSide(color: muted, width: 1.4),
        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(4)),
      ),
      inputDecorationTheme: InputDecorationTheme(
        filled: true,
        fillColor: field,
        hintStyle: const TextStyle(color: muted),
        labelStyle: const TextStyle(color: muted),
        prefixIconColor: muted,
        suffixIconColor: muted,
        contentPadding: const EdgeInsets.symmetric(
          horizontal: 14,
          vertical: 12,
        ),
        border: _fieldBorder,
        enabledBorder: _fieldBorder,
        disabledBorder: _fieldBorder,
        focusedBorder: OutlineInputBorder(
          borderRadius: BorderRadius.circular(12),
          borderSide: const BorderSide(color: accent, width: 1),
        ),
        errorBorder: _fieldBorder,
        focusedErrorBorder: OutlineInputBorder(
          borderRadius: BorderRadius.circular(12),
          borderSide: const BorderSide(color: Color(0xFFEF4444), width: 1),
        ),
      ),
      filledButtonTheme: FilledButtonThemeData(
        style: FilledButton.styleFrom(
          backgroundColor: accent,
          foregroundColor: Colors.white,
          elevation: 0,
          shadowColor: Colors.transparent,
          disabledBackgroundColor: field,
          disabledForegroundColor: muted,
          minimumSize: const Size(0, 44),
          shape: const StadiumBorder(),
        ),
      ),
      elevatedButtonTheme: ElevatedButtonThemeData(
        style: ElevatedButton.styleFrom(
          backgroundColor: accent,
          foregroundColor: Colors.white,
          elevation: 0,
          shadowColor: Colors.transparent,
          shape: const StadiumBorder(),
        ),
      ),
      outlinedButtonTheme: OutlinedButtonThemeData(
        style: OutlinedButton.styleFrom(
          foregroundColor: Colors.white,
          backgroundColor: field,
          side: _none,
          elevation: 0,
          minimumSize: const Size(0, 44),
          shape: const StadiumBorder(),
        ),
      ),
      textButtonTheme: TextButtonThemeData(
        style: TextButton.styleFrom(foregroundColor: accent),
      ),
      iconButtonTheme: IconButtonThemeData(
        style: IconButton.styleFrom(foregroundColor: Colors.white),
      ),
      floatingActionButtonTheme: const FloatingActionButtonThemeData(
        backgroundColor: accent,
        foregroundColor: Colors.white,
        elevation: 0,
      ),
      progressIndicatorTheme: const ProgressIndicatorThemeData(
        color: accent,
        linearTrackColor: field,
        circularTrackColor: field,
      ),
      snackBarTheme: const SnackBarThemeData(
        backgroundColor: card,
        contentTextStyle: TextStyle(color: Colors.white),
        elevation: 0,
        behavior: SnackBarBehavior.floating,
        shape: RoundedRectangleBorder(
          borderRadius: BorderRadius.all(Radius.circular(radius)),
        ),
      ),
      chipTheme: ChipThemeData(
        backgroundColor: field,
        selectedColor: accent.withValues(alpha: 0.22),
        disabledColor: field,
        labelStyle: const TextStyle(color: Colors.white),
        secondaryLabelStyle: const TextStyle(color: Colors.white),
        padding: const EdgeInsets.symmetric(horizontal: 8),
        side: _none,
        shape: const StadiumBorder(side: _none),
        surfaceTintColor: Colors.transparent,
      ),
    );
  }
}

class _HelmToast extends StatefulWidget {
  const _HelmToast({required this.title, required this.message});

  final String title;
  final String message;

  @override
  State<_HelmToast> createState() => _HelmToastState();
}

class _HelmToastState extends State<_HelmToast> {
  double _opacity = 0;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (mounted) setState(() => _opacity = 1);
    });
  }

  @override
  Widget build(BuildContext context) {
    return SafeArea(
      child: Align(
        alignment: Alignment.center,
        child: AnimatedOpacity(
          opacity: _opacity,
          duration: const Duration(milliseconds: 160),
          child: ConstrainedBox(
            constraints: const BoxConstraints(maxWidth: 300),
            child: Material(
              color: AppTheme.card.withValues(alpha: 0.94),
              borderRadius: BorderRadius.circular(AppTheme.radius),
              child: Padding(
                padding: const EdgeInsets.symmetric(
                  horizontal: 18,
                  vertical: 14,
                ),
                child: Column(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    Text(
                      widget.title,
                      textAlign: TextAlign.center,
                      style: const TextStyle(
                        color: Colors.white,
                        fontWeight: FontWeight.w600,
                        fontSize: 15,
                      ),
                    ),
                    if (widget.message.isNotEmpty) ...[
                      const SizedBox(height: 4),
                      Text(
                        widget.message,
                        textAlign: TextAlign.center,
                        style: const TextStyle(
                          color: AppTheme.muted,
                          fontSize: 13,
                        ),
                      ),
                    ],
                  ],
                ),
              ),
            ),
          ),
        ),
      ),
    );
  }
}

import 'package:flutter/material.dart';
import 'package:flutter_map/flutter_map.dart';
import 'package:get/get.dart';
import 'package:latlong2/latlong.dart';
import 'package:sifli_companion/app/app_theme.dart';
import 'package:sifli_companion/i18n/locale_keys.dart';

/// 地图途经点钉子：起点绿、终点强调色、中间白底，选中时放大描边。
class HelmMapPin extends StatelessWidget {
  const HelmMapPin({
    super.key,
    required this.index,
    this.selected = false,
    this.isStart = false,
    this.isEnd = false,
    this.accent = AppTheme.accent,
    this.size,
  });

  /// 显示序号，从 1 起。
  final int index;
  final bool selected;
  final bool isStart;
  final bool isEnd;
  final Color accent;
  final double? size;

  @override
  Widget build(BuildContext context) {
    final dim = size ?? (selected ? 36.0 : 30.0);
    final Color fill;
    final Color fg;
    if (isStart) {
      fill = const Color(0xFF22C55E);
      fg = Colors.white;
    } else if (isEnd && !isStart) {
      fill = accent;
      fg = Colors.white;
    } else {
      fill = Colors.white;
      fg = const Color(0xFF111113);
    }
    return AnimatedContainer(
      duration: const Duration(milliseconds: 160),
      width: dim,
      height: dim,
      alignment: Alignment.center,
      decoration: BoxDecoration(
        color: fill,
        shape: BoxShape.circle,
        border: Border.all(
          color: selected ? accent : const Color(0xFF111113),
          width: selected ? 3 : 1.4,
        ),
      ),
      child: Text(
        '$index',
        style: TextStyle(
          fontSize: dim < 28 ? 10 : 12,
          fontWeight: FontWeight.w800,
          color: fg,
          height: 1,
        ),
      ),
    );
  }
}

/// 单个落点用的实心点钉（无序号），用于收藏点这类「只有一个位置」的场景。
///
/// 和 [HelmMapPin] 同一套语言（强调色 + 白环 + 投影），但没有编号，视觉上更轻。
/// 钉尖落在控件底边中点，所以 `Marker(alignment: Alignment.bottomCenter)` 时
/// 钉尖正好压在地理点上。
class HelmMapDotPin extends StatelessWidget {
  const HelmMapDotPin({
    super.key,
    this.color = AppTheme.accent,
    this.size = 22,
  });

  final Color color;
  final double size;

  /// 控件高度（到钉尖为止）。`Marker` 要按它开盒，钉尖才会落在 `point` 上。
  static double heightFor(double size) => size * 1.0453;

  @override
  Widget build(BuildContext context) {
    final tail = size * 0.46;
    return SizedBox(
      width: size,
      height: heightFor(size),
      child: Stack(
        alignment: Alignment.topCenter,
        children: [
          Positioned(
            top: size * 0.72 - tail / 2,
            child: Transform.rotate(
              angle: 0.7854,
              child: DecoratedBox(
                decoration: BoxDecoration(color: color),
                child: SizedBox.square(dimension: tail),
              ),
            ),
          ),
          Container(
            width: size,
            height: size,
            decoration: BoxDecoration(
              color: color,
              shape: BoxShape.circle,
              border: Border.all(color: Colors.white, width: 3),
              boxShadow: [
                BoxShadow(
                  color: Colors.black.withValues(alpha: 0.28),
                  blurRadius: 8,
                  offset: const Offset(0, 3),
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }
}

/// 浮在地图上的圆钮，深灰底、无阴影。[active] 时点亮成强调色（模式开关用）。
class HelmMapRoundBtn extends StatelessWidget {
  const HelmMapRoundBtn({
    super.key,
    required this.icon,
    this.onPressed,
    this.tooltip,
    this.active = false,
  });

  final IconData icon;
  final VoidCallback? onPressed;
  final String? tooltip;

  /// 当前处于该按钮对应的模式（例如准星选点已打开）。
  final bool active;

  @override
  Widget build(BuildContext context) {
    final btn = Material(
      color: active ? AppTheme.accent : AppTheme.card,
      shape: const CircleBorder(),
      clipBehavior: Clip.antiAlias,
      child: InkWell(
        customBorder: const CircleBorder(),
        onTap: onPressed,
        child: SizedBox.square(
          dimension: 44,
          child: Icon(
            icon,
            size: 22,
            color: onPressed == null ? AppTheme.muted : Colors.white,
          ),
        ),
      ),
    );
    if (tooltip == null || tooltip!.isEmpty) return btn;
    return Tooltip(message: tooltip!, child: btn);
  }
}

/// 地图顶部一条操作提示，不挡双指缩放。
class HelmMapHintChip extends StatelessWidget {
  const HelmMapHintChip({
    super.key,
    required this.text,
    this.emphasis = false,
    this.busy = false,
  });

  final String text;
  final bool emphasis;

  /// 前面带一个小转圈：用于「正在吸附到道路 / 规划路线」这类网络等待。
  ///
  /// 有转圈才算「说清了在忙什么」—— 原来只有一个 2 px 细条，用户看不出是在
  /// 算路、卡住、还是点了没反应。
  final bool busy;

  @override
  Widget build(BuildContext context) {
    if (text.isEmpty) return const SizedBox.shrink();
    return IgnorePointer(
      child: DecoratedBox(
        decoration: BoxDecoration(
          color: (emphasis ? AppTheme.accent : AppTheme.card).withValues(
            alpha: emphasis ? 0.94 : 0.92,
          ),
          borderRadius: BorderRadius.circular(20),
        ),
        child: Padding(
          padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 8),
          child: Row(
            mainAxisSize: MainAxisSize.min,
            children: [
              if (busy) ...[
                SizedBox.square(
                  dimension: 13,
                  child: CircularProgressIndicator(
                    strokeWidth: 2,
                    valueColor: AlwaysStoppedAnimation<Color>(
                      emphasis ? Colors.white : AppTheme.accent,
                    ),
                  ),
                ),
                const SizedBox(width: 8),
              ],
              Flexible(
                child: Text(
                  text,
                  textAlign: TextAlign.center,
                  style: TextStyle(
                    color: emphasis ? Colors.white : AppTheme.muted,
                    fontSize: 13,
                    fontWeight: emphasis ? FontWeight.w600 : FontWeight.w500,
                    height: 1.3,
                  ),
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}

/// 屏幕正中的选点准星：拖动地图选位置，而不是用手指去戳一个精确的点。
///
/// 钉尖、十字交点、地面投影中心三者都落在控件正中心，所以把它居中放在地图上，
/// 十字交点就严格对应 `MapController.camera.center` 的地理坐标 —— 调用方不用做
/// 任何坐标换算。拖动时钉头轻微下沉（[active]），「地图在动、准星不动」这件事
/// 才看得出来。
class HelmMapCrosshair extends StatelessWidget {
  const HelmMapCrosshair({
    super.key,
    this.active = false,
    this.color = AppTheme.accent,
  });

  /// 正在拖动地图 / 正在等吸附结果。
  final bool active;
  final Color color;

  /// 控件尺寸。水平中点与竖直中点都是地理落点，改动会同时影响调用方的居中假设。
  static const double width = 68;
  static const double height = 84;

  static const _dip = Duration(milliseconds: 140);

  @override
  Widget build(BuildContext context) {
    const cx = width / 2;
    const cy = height / 2;
    const headD = 26.0;
    const stem = 12.0;
    final headY = cy - stem - headD / 2;

    return IgnorePointer(
      child: SizedBox(
        width: width,
        height: height,
        child: Stack(
          clipBehavior: Clip.none,
          children: [
            // 地面投影：钉尖落在正中，压下时投影同时收一点，像是真的压近了。
            Positioned(
              left: cx - 12,
              top: cy,
              child: AnimatedContainer(
                duration: _dip,
                width: 24,
                height: 7,
                decoration: BoxDecoration(
                  borderRadius: BorderRadius.circular(20),
                  color: Colors.black.withValues(alpha: active ? 0.16 : 0.24),
                ),
              ),
            ),
            // 钉身整体下沉，投影不动。
            AnimatedSlide(
              duration: _dip,
              curve: Curves.easeOutCubic,
              offset: Offset(0, active ? 4 / height : 0),
              child: SizedBox(
                width: width,
                height: height,
                child: Stack(
                  children: [
                    // 十字交点即地理落点。
                    _bar(
                      left: cx - 13,
                      top: cy - 1,
                      w: 26,
                      h: 2,
                    ),
                    _bar(left: cx - 1, top: cy - 7, w: 2, h: 14),
                    // 竖杆把钉头连到落点。
                    _bar(
                      left: cx - 1,
                      top: headY,
                      w: 2,
                      h: cy,
                    ),
                    Positioned(
                      left: cx - headD / 2,
                      top: headY - headD / 2,
                      child: Container(
                        width: headD,
                        height: headD,
                        decoration: BoxDecoration(
                          color: color,
                          shape: BoxShape.circle,
                          border: Border.all(color: Colors.white, width: 3),
                          boxShadow: [
                            BoxShadow(
                              color: Colors.black.withValues(alpha: 0.28),
                              blurRadius: 8,
                              offset: const Offset(0, 3),
                            ),
                          ],
                        ),
                      ),
                    ),
                  ],
                ),
              ),
            ),
          ],
        ),
      ),
    );
  }

  Widget _bar({
    required double left,
    required double top,
    required double w,
    required double h,
  }) {
    return Positioned(
      left: left,
      top: top,
      child: Container(
        width: w,
        height: h,
        decoration: BoxDecoration(
          color: color,
          borderRadius: BorderRadius.circular(h / 2),
          // 浅色瓦片上给一条暗描边，细线才立得住。
          boxShadow: [
            BoxShadow(
              color: Colors.black.withValues(alpha: 0.22),
              blurRadius: 2,
              spreadRadius: 1,
            ),
          ],
        ),
      ),
    );
  }
}

/// 相机平滑移动（默认 320 ms easeOutCubic）。
///
/// `MapController.move` 是瞬移：编辑时焦点「跳」到别处，观感上像界面闪了一下，
/// 而不像自己挪过去。`MapControllerImpl.moveAnimatedRaw` 会先停掉上一段动画，
/// 连点也不会打架。非 Android/iOS 或还没挂载时退回瞬移。
void helmAnimateCamera(
  MapController map,
  LatLng target, {
  double? zoom,
  Duration duration = const Duration(milliseconds: 320),
  Curve curve = Curves.easeOutCubic,
}) {
  final double toZoom;
  try {
    toZoom = zoom ?? map.camera.zoom;
  } catch (_) {
    return; // 地图还没挂载
  }
  if (map is! MapControllerImpl) {
    try {
      map.move(target, toZoom);
    } catch (_) {}
    return;
  }
  map.moveAnimatedRaw(
    target,
    toZoom,
    duration: duration,
    curve: curve,
    hasGesture: false,
    source: MapEventSource.mapController,
  );
}

/// 地图编辑底栏：收起露名称/保存，上拉展开途经点列表。
class HelmEditSheet extends StatelessWidget {
  const HelmEditSheet({
    super.key,
    required this.t,
    required this.peek,
    required this.onHeaderTap,
    required this.onDragDelta,
    required this.onDragEnd,
    this.footer,
    this.list,
    this.peekBody = 132,
    this.maxHeight,
  });

  /// 0 收起，1 展开。
  final double t;
  final Widget peek;

  /// 底栏操作区。为 `null` 时整块不占高度 —— 保存放在 AppBar 上，底栏常驻一排
  /// 大按钮既占地方又没必要。
  final Widget? footer;
  final Widget? list;
  final VoidCallback onHeaderTap;
  final ValueChanged<double> onDragDelta;
  final ValueChanged<double> onDragEnd;
  final double peekBody;

  /// 底栏可用高度；缺省用整屏，地图编辑页应传入 body 高度。
  final double? maxHeight;

  /// 底栏有内容时，收起态要多占的高度（按钮 44 + 上下 4/12 留白）。
  ///
  /// 面板高度是个确定值（动画要有目标），所以「这次有没有底栏」必须算进高度里：
  /// 漏算就会把底栏挤成溢出 —— 真机上只表现为黄黑条纹加一条 RenderFlex 日志。
  static const footerReserve = 60.0;

  /// 面板在 [t] 处的高度。
  ///
  /// 面板自己和页面（要用它定位浮在地图上的圆钮）都调用这一个函数，
  /// 免得两边各算一套、日后改动只改一处就又对不上。
  static double heightOf({
    required double t,
    required double peekBody,
    required double bodyHeight,
    required double bottomInset,
    bool hasFooter = false,
  }) {
    final collapsed = peekBody + (hasFooter ? footerReserve : 0);
    final peek = collapsed + bottomInset * (1 - t);
    return (peek + (bodyHeight * 0.52 - peek) * t).clamp(
      collapsed,
      bodyHeight * 0.58,
    );
  }

  static double reserve(BuildContext context, {double peekBody = 132}) {
    return peekBody + MediaQuery.paddingOf(context).bottom;
  }

  @override
  Widget build(BuildContext context) {
    final bottom = MediaQuery.paddingOf(context).bottom;
    final bodyH = maxHeight ?? MediaQuery.sizeOf(context).height;
    final expanded = t > 0.04;
    final sheetH = heightOf(
      t: t,
      peekBody: peekBody,
      bodyHeight: bodyH,
      bottomInset: bottom,
      hasFooter: footer != null,
    );
    // 收起态：下限是动画基准（[peekBody] + 底栏预留），上限是屏幕的 58%。内容比下限
    // 高就自己长高，所以改文案、系统字体放大都不会溢出；比下限矮也不会缩得比基准小，
    // 展开动画的起点才稳定。
    // 展开态：锁定在动画目标高度，列表吃剩下的空间。
    final base = peekBody + (footer != null ? footerReserve : 0);
    final cap = bodyH * 0.58;
    return ConstrainedBox(
      constraints: expanded
          ? BoxConstraints(minHeight: sheetH, maxHeight: sheetH)
          : BoxConstraints(minHeight: base, maxHeight: cap < base ? base : cap),
      child: Material(
        color: AppTheme.card,
        elevation: 0,
        shadowColor: Colors.transparent,
        surfaceTintColor: Colors.transparent,
        borderRadius: const BorderRadius.vertical(top: Radius.circular(20)),
        clipBehavior: Clip.antiAlias,
        child: Column(
          // 恒为 min：靠外层约束的 minHeight 兜底。展开时约束是紧的，Column 会被
          // 撑满，所以列表的 Expanded 依然拿得到剩余空间。
          mainAxisSize: MainAxisSize.min,
          children: [
            GestureDetector(
              behavior: HitTestBehavior.opaque,
              onTap: onHeaderTap,
              onVerticalDragUpdate: (d) => onDragDelta(d.delta.dy),
              onVerticalDragEnd: (d) => onDragEnd(d.primaryVelocity ?? 0),
              child: const Padding(
                padding: EdgeInsets.only(top: 10, bottom: 8),
                child: Center(
                  child: SizedBox(
                    width: 36,
                    height: 4,
                    child: DecoratedBox(
                      decoration: BoxDecoration(
                        color: Color(0xFF5C5C5E),
                        borderRadius: BorderRadius.all(Radius.circular(2)),
                      ),
                    ),
                  ),
                ),
              ),
            ),
            Padding(
              padding: const EdgeInsets.fromLTRB(16, 4, 16, 0),
              child: peek,
            ),
            if (expanded && list != null)
              Expanded(child: list!)
            else
              const SizedBox(height: 8),
            if (footer != null)
              Padding(
                padding: EdgeInsets.fromLTRB(
                  16,
                  4,
                  16,
                  expanded ? 12 + bottom : 12 + bottom,
                ),
                child: footer!,
              )
            else if (bottom > 0)
              // 没有底栏时底部安全区得有人消化，否则那片空白就白留在面板里。
              SizedBox(height: bottom),
          ],
        ),
      ),
    );
  }
}

/// 直接输入经纬度（WGS-84）。确认返回坐标，取消返回 null。
///
/// 地图拖不准的时候（从别人那儿抄来的坐标、要精确到某条路口），打字比拖图快得多。
Future<LatLng?> showLatLngDialog(
  BuildContext context, {
  required String title,
  required LatLng initial,
}) {
  return showDialog<LatLng>(
    context: context,
    builder: (_) => _LatLngDialog(title: title, initial: initial),
  );
}

class _LatLngDialog extends StatefulWidget {
  const _LatLngDialog({required this.title, required this.initial});

  final String title;
  final LatLng initial;

  @override
  State<_LatLngDialog> createState() => _LatLngDialogState();
}

class _LatLngDialogState extends State<_LatLngDialog> {
  late final _lat = TextEditingController(
    text: widget.initial.latitude.toStringAsFixed(6),
  );
  late final _lng = TextEditingController(
    text: widget.initial.longitude.toStringAsFixed(6),
  );
  String? _error;

  @override
  void dispose() {
    _lat.dispose();
    _lng.dispose();
    super.dispose();
  }

  /// 宽松一点：顺手把中文逗号/全角句点也认了，别让人为了一个标点重打。
  static double? _parse(String raw) {
    final t = raw
        .trim()
        .replaceAll('，', '.')
        .replaceAll('。', '.')
        .replaceAll(',', '.');
    if (t.isEmpty) return null;
    return double.tryParse(t);
  }

  void _confirm() {
    final lat = _parse(_lat.text);
    final lng = _parse(_lng.text);
    if (lat == null ||
        lng == null ||
        lat < -90 ||
        lat > 90 ||
        lng < -180 ||
        lng > 180) {
      setState(() => _error = LocaleKeys.mapEditorCoordBad.tr);
      return;
    }
    Navigator.of(context).pop(LatLng(lat, lng));
  }

  @override
  Widget build(BuildContext context) {
    return AlertDialog(
      title: Text(widget.title),
      content: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          TextField(
            controller: _lat,
            autofocus: true,
            keyboardType: const TextInputType.numberWithOptions(
              decimal: true,
              signed: true,
            ),
            textInputAction: TextInputAction.next,
            decoration: InputDecoration(
              labelText: LocaleKeys.mapEditorLat.tr,
              hintText: '39.904200',
            ),
          ),
          const SizedBox(height: 12),
          TextField(
            controller: _lng,
            keyboardType: const TextInputType.numberWithOptions(
              decimal: true,
              signed: true,
            ),
            textInputAction: TextInputAction.done,
            onSubmitted: (_) => _confirm(),
            decoration: InputDecoration(
              labelText: LocaleKeys.mapEditorLng.tr,
              hintText: '116.407400',
            ),
          ),
          const SizedBox(height: 10),
          Text(
            _error ?? LocaleKeys.mapEditorCoordRange.tr,
            style: TextStyle(
              color: _error == null ? AppTheme.muted : AppTheme.danger,
              fontSize: 12,
            ),
          ),
        ],
      ),
      actions: [
        TextButton(
          onPressed: () => Navigator.of(context).pop(),
          child: Text(LocaleKeys.cancel.tr),
        ),
        TextButton(
          onPressed: _confirm,
          child: Text(LocaleKeys.confirm.tr),
        ),
      ],
    );
  }
}

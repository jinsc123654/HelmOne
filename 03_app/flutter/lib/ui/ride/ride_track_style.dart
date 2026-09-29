import 'package:flutter/material.dart';
import 'package:flutter_map/flutter_map.dart';
import 'package:latlong2/latlong.dart' hide Path;
import 'package:sifli_companion/map/osm_tiles.dart';
import 'package:sifli_companion/ride/gpx_util.dart';

/// 与码表一致：慢绿 / 巡航青 / 快红。
abstract class RideTrackStyle {
  /// 速度色带三档色。与总览速度条（`RidePaceStrip`）共用，两边颜色才对得上。
  static const bandSlow = Color(0xFF3DDC84);
  static const bandMid = Color(0xFFFFD54A);
  static const bandFast = Color(0xFFFF5C33);

  /// 码表上那条线的分档配色（按绝对速度：0–18 绿、18–32 青、32+ 红）。
  ///
  /// App 里的轨迹线和速度条改走 [speedBandColor]（按本次骑行归一化，和速度条一致），
  /// 这个函数保留下来只作为与码表配色对照的依据。
  static Color speedColor(double kph) {
    var v = kph;
    if (v < 0) v = 0;
    if (v < 18) {
      final t = v / 18;
      return Color.fromARGB(
        255,
        (20 + 20 * t).round(),
        (168 + 42 * t).round(),
        (72 + 28 * t).round(),
      );
    }
    if (v < 32) {
      final t = (v - 18) / 14;
      return Color.fromARGB(
        255,
        (40 * (1 - t)).round(),
        (210 - 50 * t).round(),
        (100 + 120 * t).round(),
      );
    }
    final t = ((v - 32) / 16).clamp(0.0, 1.0);
    return Color.fromARGB(
      255,
      (255 * t).round(),
      (160 * (1 - t)).round(),
      (220 * (1 - t) + 32 * t).round(),
    );
  }

  /// 速度 → 色带颜色，[value] 在 [minV]..[maxV] 之间归一化（绿 → 黄 → 红）。
  ///
  /// 轨迹线和速度条都调这一个函数。以前轨迹线按绝对速度分档、速度条按本次骑行
  /// 归一化，同一条轨迹上两条东西的颜色是对不上的。
  static Color speedBandColor(double value, double minV, double maxV) {
    var lo = minV;
    var hi = maxV;
    if (hi - lo < 0.4) {
      lo -= 0.5;
      hi += 0.5;
    }
    final t = ((value - lo) / (hi - lo)).clamp(0.0, 1.0);
    return t < 0.5
        ? Color.lerp(bandSlow, bandMid, t * 2)!
        : Color.lerp(bandMid, bandFast, (t - 0.5) * 2)!;
  }

  /// 速度区间，供色带归一化用。
  ///
  /// **必须用原始序列算，不能用平滑后的。** 实测某条轨迹原始是 1.6–79.1 km/h，
  /// 被 ±48 点的滑动平均压成 16.0–31.4；中位速度 22.8 在原始区间里是 t=0.27（绿），
  /// 在压缩区间里变成 t=0.44（黄橙）—— 同一条轨迹上「线」和「速度条」就差出半条色带，
  /// 也就是用户看到的「上下颜色没对上」。
  ///
  /// 全无有效值时返回 `null`。
  static ({double lo, double hi})? bandRange(List<double> speedsAtPoint) {
    var lo = double.infinity;
    var hi = double.negativeInfinity;
    for (final v in speedsAtPoint) {
      if (!(v > 0.2)) continue;
      if (v < lo) lo = v;
      if (v > hi) hi = v;
    }
    if (lo == double.infinity) return null;
    return (lo: lo, hi: hi);
  }

  /// 把整条轨迹的速度抽成色标（给轨迹线的 `gradientColors`）。
  ///
  /// 取值方式和速度条一致：**按位置取样，不做平滑**（速度条的 `downsample` 同样是
  /// 取样）。120 个色标本身就是「每段距离一个」，已经足够平滑；再叠一层滑动平均
  /// 会把峰值削掉 —— 实测某条轨迹原始峰值 79 km/h，±2 点平滑后就只剩 49，线到不了
  /// 红色，而速度条那时已经是红的。
  ///
  /// 加载时算一次就够：逐帧重算是「点数 × 窗口」的量级，回放会掉帧。
  static List<Color> bandStops(
    List<double> speedsAtPoint, {
    double fallbackKmh = 18,
    int maxStops = 120,
    ({double lo, double hi})? range,
  }) {
    final n = speedsAtPoint.length;
    // 完全没有速度信息时给一条纯色：画成绿到红会被误读成「有快有慢」。
    if (n == 0 || !speedsAtPoint.any((v) => v > 0.2)) {
      return const [bandSlow, bandSlow];
    }
    final r = range ?? bandRange(speedsAtPoint)!;
    final stops = maxStops < 2 ? 2 : (maxStops > n ? n : maxStops);
    return [
      for (var i = 0; i < stops; i++)
        speedBandColor(
          _speedAt(speedsAtPoint, ((i / (stops - 1)) * (n - 1)).round(), fallbackKmh),
          r.lo,
          r.hi,
        ),
    ];
  }

  /// 第 [i] 个点的速度；越界或缺失（<=0.2）时用 [fallback]。
  static double _speedAt(List<double> speeds, int i, double fallback) {
    if (i < 0 || i >= speeds.length) return fallback;
    final v = speeds[i];
    return v > 0.2 ? v : fallback;
  }

  /// [stops] 里对应「画到小数下标 [cut]」的那一段。总数不足则原样返回。
  static List<Color> stopsUpTo(List<Color> stops, double cut, int totalPoints) {
    if (stops.length < 2 || totalPoints < 2) return stops;
    if (cut >= totalPoints - 1) return stops;
    final frac = (cut / (totalPoints - 1)).clamp(0.0, 1.0);
    final m = (frac * (stops.length - 1)).round().clamp(1, stops.length - 1);
    return stops.sublist(0, m + 1);
  }

  /// 小数下标 [cut] 对应的位置（回放游标用）。
  static LatLng? pointAt(List<LatLng> disp, double cut) {
    final pts = revealPoints(disp, cut);
    return pts.isEmpty ? null : pts.last;
  }

  /// 画到小数下标 [cut]：取整段，再在最后一段上插一个点。
  static List<LatLng> revealPoints(List<LatLng> disp, double cut) {
    if (disp.isEmpty) return const [];
    final i = cut.floor().clamp(0, disp.length - 1);
    final f = (cut - i).clamp(0.0, 1.0);
    final out = disp.sublist(0, i + 1);
    if (f > 0 && i + 1 < disp.length) {
      final a = disp[i];
      final b = disp[i + 1];
      out.add(
        LatLng(
          a.latitude + (b.latitude - a.latitude) * f,
          a.longitude + (b.longitude - a.longitude) * f,
        ),
      );
    }
    return out;
  }

  /// 轨迹线。[cut] 为 `null` 画整条；给小数下标则只画到那里（逐段回放）。
  static List<Polyline> speedPolylines(
    List<LatLng> disp, {
    required List<Color> stops,
    double? cut,
  }) {
    if (disp.length < 2 || stops.isEmpty) return const [];
    const casing = Color(0xEEFFFFFF);
    final c = cut;
    if (c == null || c >= disp.length - 1) {
      return [
        Polyline(points: disp, strokeWidth: 9, color: casing),
        Polyline(
          points: disp,
          strokeWidth: 5,
          color: stops[stops.length ~/ 2],
          gradientColors: stops,
        ),
      ];
    }
    final pts = revealPoints(disp, c);
    if (pts.length < 2) return const [];
    // 色标只在整条上算一次，这里切片 —— 回放中颜色不会随进度漂移。
    final colors = stopsUpTo(stops, c, disp.length);
    return [
      Polyline(points: pts, strokeWidth: 9, color: casing),
      Polyline(
        points: pts,
        strokeWidth: 5,
        color: colors[colors.length ~/ 2],
        gradientColors: colors,
      ),
    ];
  }

}

/// 起点：F1 式黑白格圆。
class RideStartGrid extends StatelessWidget {
  const RideStartGrid({super.key});

  @override
  Widget build(BuildContext context) {
    return const CustomPaint(
      painter: _StartGridPainter(),
      child: SizedBox.expand(),
    );
  }
}

class _StartGridPainter extends CustomPainter {
  const _StartGridPainter();

  @override
  void paint(Canvas canvas, Size size) {
    final c = Offset(size.width / 2, size.height / 2);
    final r = size.shortestSide / 2;
    canvas.drawCircle(
      c,
      r,
      Paint()
        ..color = const Color(0x66000000)
        ..maskFilter = const MaskFilter.blur(BlurStyle.normal, 1.4),
    );
    canvas.save();
    canvas.clipPath(
      Path()..addOval(Rect.fromCircle(center: c, radius: r - 1.6)),
    );
    const n = 4;
    final cell = (r * 2) / n;
    final origin = Offset(c.dx - r, c.dy - r);
    for (var y = 0; y < n; y++) {
      for (var x = 0; x < n; x++) {
        canvas.drawRect(
          Rect.fromLTWH(
            origin.dx + x * cell,
            origin.dy + y * cell,
            cell + 0.4,
            cell + 0.4,
          ),
          Paint()..color = (x + y).isEven ? Colors.black : Colors.white,
        );
      }
    }
    canvas.restore();
    canvas.drawCircle(
      c,
      r - 0.9,
      Paint()
        ..style = PaintingStyle.stroke
        ..strokeWidth = 2.2
        ..color = Colors.white,
    );
    canvas.drawCircle(
      c,
      r - 0.2,
      Paint()
        ..style = PaintingStyle.stroke
        ..strokeWidth = 1
        ..color = Colors.black87,
    );
  }

  @override
  bool shouldRepaint(covariant CustomPainter oldDelegate) => false;
}

/// 终点：F1 方格旗。
class RideFinishFlag extends StatelessWidget {
  const RideFinishFlag({super.key});

  @override
  Widget build(BuildContext context) {
    return const CustomPaint(
      painter: _FinishFlagPainter(),
      child: SizedBox.expand(),
    );
  }
}

class _FinishFlagPainter extends CustomPainter {
  const _FinishFlagPainter();

  @override
  void paint(Canvas canvas, Size size) {
    final poleX = size.width * 0.18;
    final base = Offset(poleX, size.height - 1);
    canvas.drawLine(
      Offset(poleX, 2),
      base,
      Paint()
        ..color = const Color(0xFF1A1A1A)
        ..strokeWidth = 2.2
        ..strokeCap = StrokeCap.round,
    );
    const cols = 5;
    const rows = 4;
    final flag = Rect.fromLTWH(
      poleX + 1,
      3,
      size.width * 0.72,
      size.height * 0.52,
    );
    final cw = flag.width / cols;
    final ch = flag.height / rows;
    canvas.save();
    canvas.clipRRect(
      RRect.fromRectAndRadius(flag, const Radius.circular(1.5)),
    );
    for (var y = 0; y < rows; y++) {
      for (var x = 0; x < cols; x++) {
        canvas.drawRect(
          Rect.fromLTWH(
            flag.left + x * cw,
            flag.top + y * ch,
            cw + 0.3,
            ch + 0.3,
          ),
          Paint()..color = (x + y).isEven ? Colors.black : Colors.white,
        );
      }
    }
    canvas.restore();
    canvas.drawRRect(
      RRect.fromRectAndRadius(flag, const Radius.circular(1.5)),
      Paint()
        ..style = PaintingStyle.stroke
        ..strokeWidth = 1
        ..color = Colors.black87,
    );
    canvas.drawCircle(base, 2.2, Paint()..color = Colors.black87);
  }

  @override
  bool shouldRepaint(covariant CustomPainter oldDelegate) => false;
}

/// 整公里圆标，数字居中。
class RideKmBadge extends StatelessWidget {
  const RideKmBadge({super.key, required this.km});

  final int km;

  @override
  Widget build(BuildContext context) {
    return Container(
      alignment: Alignment.center,
      decoration: BoxDecoration(
        color: const Color(0xFF141416),
        shape: BoxShape.circle,
        border: Border.all(color: Colors.white, width: 2),
        boxShadow: const [
          BoxShadow(
            color: Color(0x66000000),
            blurRadius: 4,
            offset: Offset(0, 1),
          ),
        ],
      ),
      child: Text(
        '$km',
        style: TextStyle(
          color: Colors.white,
          fontSize: km >= 100 ? 8 : 11,
          fontWeight: FontWeight.w800,
          height: 1,
        ),
      ),
    );
  }
}

/// 轨迹层。
///
/// 公里牌和起终点是**线画到哪儿才在哪儿浮现**的：回放中牌子随着线经过依次长出来，
/// 而不是一开始全摆在地图上。浮现进度直接由回放进度算（见 [_Emerging]），不另跑动画。
/// [cut] 为 `null`（分享封面等）时全部按已浮现处理。
List<Widget> rideTrackLayers({
  required List<LatLng> display,
  required GpxSummary? summary,
  required List<LatLng> wgs,
  required List<Color> stops,
  double? cut,
}) {
  if (display.length < 2) return const [];
  final done = cut == null || cut >= display.length - 1;
  final head = done ? null : RideTrackStyle.pointAt(display, cut);

  // 浮现窗口：线越过标记之后再走这么多「点」才完全显形（约全程 3%）。
  final fade = ((display.length - 1) * 0.03).clamp(1.0, 40.0);
  double emerge(double at) => done ? 1.0 : ((cut - at) / fade).clamp(0.0, 1.0);

  final markers = <Marker>[];
  for (final m in GpxUtil.kmMarkers(wgs)) {
    final t = emerge(m.at);
    if (t <= 0) continue;
    markers.add(
      Marker(
        point: HelmOsmTiles.toDisplay(m.ll),
        width: 22,
        height: 22,
        child: _Emerging(t: t, child: RideKmBadge(km: m.km)),
      ),
    );
  }

  // 起点从第一帧起长出来。
  final startT = emerge(0);
  if (startT > 0) {
    markers.add(
      Marker(
        point: display.first,
        width: 26,
        height: 26,
        child: _Emerging(t: startT, child: const RideStartGrid()),
      ),
    );
  }
  // 终点：「画到」正好是回放结束那一刻，窗口往前挪一点，否则只能在结束瞬间硬弹出来。
  final finishT = emerge(display.length - 1 - fade);
  if (finishT > 0) {
    markers.add(
      Marker(
        point: display.last,
        width: 30,
        height: 38,
        alignment: Alignment.bottomCenter,
        child: _Emerging(t: finishT, child: const RideFinishFlag()),
      ),
    );
  }

  // 游标画在最后，压在标记之上。
  if (head != null) {
    markers.add(
      Marker(point: head, width: 22, height: 22, child: const RidePlayHead()),
    );
  }

  return [
    PolylineLayer(
      polylines: RideTrackStyle.speedPolylines(
        display,
        stops: stops,
        cut: done ? null : cut,
      ),
    ),
    if (markers.isNotEmpty) MarkerLayer(markers: markers),
  ];
}

/// 回放浮现：线越过标记之后，在 [t]（0→1）里放大淡入。
///
/// 不插一个「入场动画」：地图每帧都在重建，入场动画会反复重播。这里直接用回放进度
/// 算，帧帧一致，往回拖也停在正确的中间态。
class _Emerging extends StatelessWidget {
  const _Emerging({required this.t, required this.child});

  final double t;
  final Widget child;

  @override
  Widget build(BuildContext context) {
    return Opacity(
      opacity: t,
      // 从底边长出来：旗子和圆牌都是下沿先落地。
      child: Transform.scale(
        scale: 0.55 + 0.45 * t,
        alignment: Alignment.bottomCenter,
        child: child,
      ),
    );
  }
}

/// 回放游标：当前轨迹画到的位置。
class RidePlayHead extends StatelessWidget {
  const RidePlayHead({super.key});

  @override
  Widget build(BuildContext context) {
    return Center(
      child: Container(
        width: 16,
        height: 16,
        decoration: BoxDecoration(
          color: RideTrackStyle.bandFast,
          shape: BoxShape.circle,
          border: Border.all(color: Colors.white, width: 2.5),
          boxShadow: [
            BoxShadow(
              color: Colors.black.withValues(alpha: 0.32),
              blurRadius: 6,
              offset: const Offset(0, 2),
            ),
          ],
        ),
      ),
    );
  }
}

import 'package:flutter_test/flutter_test.dart';
import 'package:latlong2/latlong.dart';
import 'package:sifli_companion/ride/gpx_util.dart';
import 'package:sifli_companion/ui/ride/ride_track_style.dart';

/// 轨迹回放的数学：时间轴、按进度截断、色标切片。
///
/// 这些是「进去自动回放 + 线段按速度着色」的底座。算错了在真机上只表现为
/// 「画到一半不动」或者颜色不对，来回复现很费时间，所以在这儿锁住。
void main() {
  // 纬度方向约 1 km：1° ≈ 111.2 km。用 32.0 → 32.009。
  const a = LatLng(32.0, 118.0);
  const b = LatLng(32.009, 118.0);
  const c = LatLng(32.018, 118.0);

  /// 一条 40 点的直线轨迹，用于测试截断与色标切片。
  final track = List<LatLng>.generate(40, (i) => LatLng(32.0 + i * 0.001, 118.0));

  group('kmMarkers 的 at（回放浮现靠它判断线画到没有）', () {
    test('标记落在两点之间时给出小数下标', () {
      // 0.009° 纬度 ≈ 1 km，所以 1 km 标记落在第 0、1 点之间。
      final marks = GpxUtil.kmMarkers(const [a, b, c]);
      expect(marks, isNotEmpty);
      expect(marks.first.km, 1);
      expect(marks.first.at, greaterThan(0));
      expect(marks.first.at, lessThan(1));
    });

    test('点不足时为空', () {
      expect(GpxUtil.kmMarkers(const []), isEmpty);
      expect(GpxUtil.kmMarkers(const [a]), isEmpty);
    });
  });

  group('cumulativeRideSeconds', () {
    test('按距离除以速度累加（20 km/h 跑 1 km ≈ 180 s）', () {
      final t = GpxUtil.cumulativeRideSeconds(const [a, b], const [20, 20]);
      expect(t.length, 2);
      expect(t.first, 0);
      expect(t.last, closeTo(180, 2));
    });

    test('速度快的那段用时更少', () {
      final slow = GpxUtil.cumulativeRideSeconds(const [a, b], const [10, 10]);
      final fast = GpxUtil.cumulativeRideSeconds(const [a, b], const [30, 30]);
      expect(fast.last, lessThan(slow.last));
      expect(fast.last, closeTo(slow.last / 3, 3));
    });

    test('速度缺失或异常时用兜底速度（18 km/h ≈ 200 s）', () {
      final t = GpxUtil.cumulativeRideSeconds(
        const [a, b],
        const [0, 0],
        fallbackKmh: 18,
      );
      expect(t.last, closeTo(200, 3));
    });

    test('点数多于速度表时用兜底速度且不越界', () {
      final t = GpxUtil.cumulativeRideSeconds(const [a, b, c], const [20]);
      expect(t.length, 3);
      expect(t[2], greaterThan(t[1]));
    });
  });

  group('revealPoints / pointAt', () {
    const pts = [a, b, c];

    test('整数下标就是取到那一点', () {
      expect(RideTrackStyle.revealPoints(pts, 1).length, 2);
      expect(RideTrackStyle.revealPoints(pts, 2).length, 3);
    });

    test('小数下标在最后一段上插一个点', () {
      final out = RideTrackStyle.revealPoints(pts, 1.5);
      expect(out.length, 3);
      expect(out.last.latitude, closeTo(32.0135, 1e-9));
      expect(out.last.longitude, closeTo(118.0, 1e-9));
    });

    test('pointAt 与截断后的末点一致；空轨迹返回 null', () {
      expect(
        RideTrackStyle.pointAt(pts, 1.5)!.latitude,
        closeTo(32.0135, 1e-9),
      );
      expect(RideTrackStyle.pointAt(const [], 1), isNull);
    });

    test('超出范围时收敛到端点', () {
      expect(RideTrackStyle.revealPoints(pts, -3).length, 1);
      expect(RideTrackStyle.revealPoints(pts, 99).length, 3);
    });
  });

  group('色标', () {
    test('没有速度信息时给纯色，不假装有快慢', () {
      final stops = RideTrackStyle.bandStops(const [0, 0, 0]);
      expect(stops.length, 2);
      expect(stops[0], stops[1]);
    });

    test('归一化区间取原始序列，尖峰不能被平滑掉', () {
      // 真机上「上下颜色没对上」就是这个：区间若取自平滑后的序列，±48 点窗口会把
      // 峰值和低速一起抹平（实测 1.6–79.1 被压成 16.0–31.4），中位速度就从 t=0.27
      // 漂到 t=0.44，差出半条色带。
      const speeds = <double>[20, 20, 20, 60, 20, 20, 20];
      final r = RideTrackStyle.bandRange(speeds)!;
      expect(r.lo, 20);
      expect(r.hi, 60);
    });

    test('传入的区间被真正采用（线、条共用同一套）', () {
      const speeds = <double>[10, 20, 30, 40, 50];
      final own = RideTrackStyle.bandStops(speeds, maxStops: 5);
      final wide = RideTrackStyle.bandStops(
        speeds,
        maxStops: 5,
        range: (lo: 0, hi: 100),
      );
      expect(own[2], RideTrackStyle.speedBandColor(30, 10, 50));
      expect(wide[2], RideTrackStyle.speedBandColor(30, 0, 100));
      expect(own[2], isNot(wide[2]));
    });

    test('全无有效速度时区间为 null', () {
      expect(RideTrackStyle.bandRange(const [0, 0.1]), isNull);
    });

    test('有色标时首绿尾红', () {
      // 400 点缓升，避开滑动平均把短序列抹平。
      final ramp = List<double>.generate(400, (i) => 8 + i * 0.1);
      final stops = RideTrackStyle.bandStops(ramp);
      expect(stops.length, greaterThan(10));
      // 绿：g 明显大于 r；红：r 明显大于 g。
      expect(stops.first.g, greaterThan(stops.first.r));
      expect(stops.last.r, greaterThan(stops.last.g));
    });

    test('切片：刚开始只给头两个，走完给全部', () {
      final ramp = List<double>.generate(40, (i) => 12 + i.toDouble());
      final stops = RideTrackStyle.bandStops(ramp, maxStops: 8);
      expect(stops.length, 8);
      expect(RideTrackStyle.stopsUpTo(stops, 0, 40).length, 2);
      expect(RideTrackStyle.stopsUpTo(stops, 3, 40).length, lessThan(8));
      expect(RideTrackStyle.stopsUpTo(stops, 39, 40).length, 8);
      expect(RideTrackStyle.stopsUpTo(stops, 99, 40).length, 8);
    });
  });

  group('speedPolylines', () {
    final stops = RideTrackStyle.bandStops(
      List<double>.generate(40, (i) => 12 + i.toDouble()),
      maxStops: 8,
    );

    test('整条：两个图层、点数不变、色标全给', () {
      final out = RideTrackStyle.speedPolylines(track, stops: stops);
      expect(out.length, 2);
      expect(out.last.points.length, track.length);
      expect(out.last.gradientColors!.length, stops.length);
    });

    test('截断：点数变短、色标切片', () {
      final out = RideTrackStyle.speedPolylines(track, stops: stops, cut: 3.0);
      expect(out.length, 2);
      expect(out.last.points.length, 4);
      expect(out.last.gradientColors!.length, lessThan(stops.length));
    });

    test('进度为 0 时还没有可画的线', () {
      final out = RideTrackStyle.speedPolylines(track, stops: stops, cut: 0.0);
      expect(out, isEmpty);
    });

    test('点太少或没有色标时直接返回空，不抛异常', () {
      expect(RideTrackStyle.speedPolylines(const [a], stops: stops), isEmpty);
      expect(RideTrackStyle.speedPolylines(track, stops: const []), isEmpty);
    });
  });
}

import 'package:flutter_test/flutter_test.dart';
import 'package:latlong2/latlong.dart';
import 'package:sifli_companion/ride/place_lookup.dart';

/// 导入轨迹自动命名的纯逻辑部分：名字拼装 + 反查结果解析 + 离线城市兜底。
///
/// 只测不联网的部分 —— 真正的反查请求在真机上验证。
void main() {
  const a = LatLng(32.0, 118.0);
  const b = LatLng(32.009, 118.0);

  group('TrackAutoName.compose', () {
    test('时间 + 起点 → 终点', () {
      expect(
        TrackAutoName.compose(
          start: DateTime(2026, 9, 17, 8, 30),
          from: '玄武湖',
          to: '南京南站',
          first: a,
          last: b,
        ),
        '9/17 08:30 玄武湖→南京南站',
      );
    });

    test('地名过长时裁到 8 个字', () {
      final name = TrackAutoName.compose(
        start: null,
        from: '中南玄武湖乐园听水桥',
        to: '南京',
        first: a,
        last: b,
      );
      expect(name, '中南玄武湖乐园听水→南京');
    });

    test('没有地名时退到坐标（坐标不裁）', () {
      final name = TrackAutoName.compose(
        start: null,
        from: null,
        to: null,
        first: a,
        last: b,
      );
      expect(name, '32.000,118.000→32.009,118.000');
    });

    test('没有时间时只留起点终点', () {
      final name = TrackAutoName.compose(
        start: null,
        from: '家',
        to: '公司',
        first: a,
        last: b,
      );
      expect(name, '家→公司');
    });
  });

  group('环线', () {
    test('起终点同名时不写「X→X」', () {
      final name = TrackAutoName.compose(
        start: DateTime(2026, 9, 17, 8, 30),
        from: '西涧北路',
        to: '西涧北路',
        first: const LatLng(32.3, 118.3),
        last: const LatLng(32.31, 118.31),
      );
      expect(name, contains('西涧北路'));
      expect(name, isNot(contains('→')));
    });

    test('起终点不同名仍旧用箭头', () {
      final name = TrackAutoName.compose(
        start: DateTime(2026, 9, 17, 8, 30),
        from: '西涧北路',
        to: '琅琊大道',
        first: const LatLng(32.3, 118.3),
        last: const LatLng(32.35, 118.35),
      );
      expect(name, contains('西涧北路→琅琊大道'));
    });
  });

  group('PlaceLookup.parseName', () {
    test('道路名优先于城市（否则永远是「滁州→滁州」）', () {
      expect(
        PlaceLookup.parseName({
          'name': '滁州市',
          'address': {'road': '西涧北路', 'city': '滁州市', 'suburb': '南谯区'},
        }),
        '西涧北路',
      );
      expect(
        PlaceLookup.parseName({
          'address': {'cycleway': '滨江绿道', 'city': '南京市'},
        }),
        '滨江绿道',
      );
      // 只有城市时才是城市
      expect(
        PlaceLookup.parseName({
          'address': {'city': '滁州市', 'state': '安徽省'},
        }),
        '滁州市',
      );
      // 没有路名时退到街区
      expect(
        PlaceLookup.parseName({
          'address': {'suburb': '南谯区', 'city': '滁州市'},
        }),
        '南谯区',
      );
    });

    test('优先 POI / 道路名', () {
      expect(PlaceLookup.parseName({'name': '玄武湖'}), '玄武湖');
    });

    test('没有 name 时退到城市 / 区县', () {
      expect(
        PlaceLookup.parseName({
          'address': {'city': '南京市'},
        }),
        '南京市',
      );
      // 区县比市更细，优先给区县（「滁州」对哪一段路没有区分度）
      expect(
        PlaceLookup.parseName({
          'address': {'county': '来安县', 'city': '滁州市'},
        }),
        '来安县',
      );
    });

    test('再退到 display_name 的第一段', () {
      expect(
        PlaceLookup.parseName({
          'display_name': '中南玄武湖乐园, 听水桥, 玄武门街道',
        }),
        '中南玄武湖乐园',
      );
    });

    test('空白字段不算命中', () {
      expect(
        PlaceLookup.parseName({
          'name': '   ',
          'address': {'city': '  '},
        }),
        isNull,
      );
      expect(PlaceLookup.parseName(<String, Object?>{}), isNull);
      expect(PlaceLookup.parseName('not a map'), isNull);
      expect(PlaceLookup.parseName(null), isNull);
    });
  });

  group('PlaceLookup.offlineCity', () {
    test('命中的城市包围盒给出城市名', () {
      // 南京市包围盒：31.90–32.18 / 118.60–119.05
      expect(PlaceLookup.offlineCity(const LatLng(32.05, 118.78)), '南京');
    });

    test('不在任何城市里返回 null', () {
      expect(PlaceLookup.offlineCity(const LatLng(10.0, 10.0)), isNull);
    });
  });
}

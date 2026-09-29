import 'dart:math' as math;

import 'package:latlong2/latlong.dart';
import 'package:sifli_companion/ride/waypoint_tsv.dart';

/// GPX 1.1 最小读写：抽 trkpt/rtept，写 rte 或 trk。
abstract class GpxUtil {
  static final _ptRe = RegExp(
    r'<(?:trkpt|rtept)\s+[^>]*lat="([+-]?\d+(?:\.\d+)?)"[^>]*lon="([+-]?\d+(?:\.\d+)?)"',
    caseSensitive: false,
  );

  static final _ptReSwap = RegExp(
    r'<(?:trkpt|rtept)\s+[^>]*lon="([+-]?\d+(?:\.\d+)?)"[^>]*lat="([+-]?\d+(?:\.\d+)?)"',
    caseSensitive: false,
  );

  static final _nameRe = RegExp(
    r'<name>\s*([^<]+)\s*</name>',
    caseSensitive: false,
  );

  static final _timeRe = RegExp(
    r'<time>\s*([^<]+)\s*</time>',
    caseSensitive: false,
  );

  static final _trkptRe = RegExp(
    r'<trkpt\b([^>]*)>([\s\S]*?)</trkpt>',
    caseSensitive: false,
  );

  static final _trkptSelfRe = RegExp(
    r'<trkpt\b([^>]*)\s*/>',
    caseSensitive: false,
  );

  static final _metaOpenRe = RegExp(r'<metadata\b[^>]*>', caseSensitive: false);

  static final _gpxOpenRe = RegExp(r'<gpx\b[^>]*>', caseSensitive: false);

  /// 判断一段字节流像不像 GPX。
  ///
  /// 用途：Android 的文件选择器**没法按扩展名过滤** —— `file_selector_android`
  /// 用系统 `MimeTypeMap` 把 `extensions` 翻成 MIME，而 `gpx` 没有注册项会被
  /// 丢掉（插件日志 `Extension not supported: gpx`），只剩 `xml` → `text/xml`，
  /// 于是选择器被锁死在 `text/xml`，GPX 文件全部变灰选不中。所以选择时不过滤，
  /// 选完之后用这个函数自己认。
  ///
  /// 只看**前 64 KiB**：GPX 根元素一定在最前面，没必要把几十 MB 全解成字符串。
  static bool looksLikeGpx(List<int> bytes) {
    if (bytes.isEmpty) return false;

    final n = bytes.length > 65536 ? 65536 : bytes.length;
    // latin1 逐字节映射，不会因为多字节 UTF-8 截断而抛异常。
    final head = String.fromCharCodes(bytes.take(n));
    return _gpxOpenRe.hasMatch(head);
  }

  static final _openPtRe = RegExp(
    r'<(?:trkpt|rtept)\b([^>]*)>([\s\S]*?)</(?:trkpt|rtept)>',
    caseSensitive: false,
  );

  static final _latAttrRe = RegExp(
    r'\blat="([+-]?\d+(?:\.\d+)?)"',
    caseSensitive: false,
  );

  static final _lonAttrRe = RegExp(
    r'\blon="([+-]?\d+(?:\.\d+)?)"',
    caseSensitive: false,
  );

  static final _eleRe = RegExp(
    r'<ele>\s*([+-]?\d+(?:\.\d+)?)\s*</ele>',
    caseSensitive: false,
  );

  static final _hrRe = RegExp(
    r'<(?:[\w.]+:)?hr>\s*(\d{1,3})\s*</(?:[\w.]+:)?hr>',
    caseSensitive: false,
  );

  static final _extClimbRe = RegExp(
    r'<cumulativeClimb>\s*([+-]?\d+(?:\.\d+)?)\s*</cumulativeClimb>',
    caseSensitive: false,
  );

  static const _hrMinBpm = 30;
  static const _hrMaxBpm = 220;

  /// 解析名称、点列、以及文件内可信的起止时间。
  static GpxSummary parse(String xml) {
    var name = '';
    final m = _nameRe.firstMatch(xml);
    if (m != null) {
      name = m.group(1)!.trim();
    }

    final raw = _extractPts(xml);
    final points = [for (final p in raw) p.ll];
    final km = distanceKm(points);
    final times = plausibleTimes(xml);
    final start = times.isEmpty ? null : times.first;
    final end = times.isEmpty ? null : times.last;
    Duration? duration;
    if (start != null && end != null) {
      final d = end.difference(start);
      if (d.inSeconds >= 15) duration = d;
    }
    final speeds = _speedSeries(raw);
    final hr = _hrSeries(raw);
    final elev = _elev(raw);
    var gain = elev.gain;
    final extClimb = _extClimbRe.firstMatch(xml);
    if (gain == null && extClimb != null) {
      final v = double.tryParse(extClimb.group(1)!);
      if (v != null && v > 0.5) gain = v;
    }
    double? avg;
    if (duration != null && km > 0.05) {
      final hours = duration.inMilliseconds / 3600000.0;
      if (hours > 0) avg = km / hours;
    }
    if (avg == null && speeds.points.isNotEmpty) {
      var s = 0.0;
      for (final p in speeds.points) {
        s += p.value;
      }
      avg = s / speeds.points.length;
    }
    var atPoint = speeds.atPoint;
    if (avg != null && avg > 0 && atPoint.length > 1) {
      var any = false;
      for (final v in atPoint) {
        if (v > 0.2) {
          any = true;
          break;
        }
      }
      if (!any) {
        atPoint = List<double>.filled(atPoint.length, avg);
        atPoint[0] = 0;
      }
    }
    return GpxSummary(
      name: name,
      points: points,
      distanceKm: km,
      startTime: start,
      endTime: end,
      duration: duration,
      avgSpeedKmh: avg,
      maxSpeedKmh: speeds.max,
      elevGainM: gain,
      elevLossM: elev.loss,
      minEleM: elev.min,
      maxEleM: elev.max,
      avgHrBpm: hr.avg,
      maxHrBpm: hr.max,
      hrPoints: hr.points,
      speedPoints: speeds.points,
      pointSpeedsKmh: speeds.atPoint,
    );
  }

  static List<_GpxPt> _extractPts(String xml) {
    final out = <_GpxPt>[];
    for (final match in _openPtRe.allMatches(xml)) {
      final ll = _llFromAttrs(match.group(1)!);
      if (ll == null) continue;
      final inner = match.group(2)!;
      final eleM = _eleRe.firstMatch(inner);
      final timeM = _timeRe.firstMatch(inner);
      DateTime? t;
      if (timeM != null) {
        t = tryParseUtc(timeM.group(1)!);
        if (t != null && !isPlausibleTime(t)) t = null;
      }
      out.add((
        ll: ll,
        ele: eleM == null ? null : double.tryParse(eleM.group(1)!),
        time: t,
        hr: _hrFromInner(inner),
      ));
    }
    if (out.isNotEmpty) return out;
    for (final match in _ptRe.allMatches(xml)) {
      final lat = double.tryParse(match.group(1)!);
      final lon = double.tryParse(match.group(2)!);
      if (lat != null && lon != null) {
        out.add((ll: LatLng(lat, lon), ele: null, time: null, hr: null));
      }
    }
    if (out.isNotEmpty) return out;
    for (final match in _ptReSwap.allMatches(xml)) {
      final lon = double.tryParse(match.group(1)!);
      final lat = double.tryParse(match.group(2)!);
      if (lat != null && lon != null) {
        out.add((ll: LatLng(lat, lon), ele: null, time: null, hr: null));
      }
    }
    return out;
  }

  static int? _hrFromInner(String inner) {
    final m = _hrRe.firstMatch(inner);
    if (m == null) return null;
    final v = int.tryParse(m.group(1)!);
    if (v == null || v < _hrMinBpm || v > _hrMaxBpm) return null;
    return v;
  }

  static LatLng? _llFromAttrs(String attrs) {
    final lat = double.tryParse(_latAttrRe.firstMatch(attrs)?.group(1) ?? '');
    final lon = double.tryParse(_lonAttrRe.firstMatch(attrs)?.group(1) ?? '');
    if (lat == null || lon == null) return null;
    return LatLng(lat, lon);
  }

  static ({double? max, List<RideSeriesPoint> points, List<double> atPoint})
      _speedSeries(List<_GpxPt> pts) {
    var maxV = 0.0;
    final series = <RideSeriesPoint>[];
    final atPoint = List<double>.filled(pts.length, 0);
    var last = 0.0;
    for (var i = 1; i < pts.length; i++) {
      var v = last;
      final a = pts[i - 1].time;
      final b = pts[i].time;
      if (a != null && b != null) {
        final sec = b.difference(a).inMilliseconds / 1000.0;
        if (sec >= 0.8 && sec <= 60) {
          final km = _haversineKm(pts[i - 1].ll, pts[i].ll);
          final raw = km / (sec / 3600.0);
          if (raw > 0 && raw < 80) {
            v = raw;
            series.add(RideSeriesPoint(time: b, value: v));
            if (v > maxV) maxV = v;
          }
        }
      }
      atPoint[i] = v;
      last = v;
    }
    return (
      max: maxV > 0.5 ? maxV : null,
      points: series,
      atPoint: atPoint,
    );
  }

  /// 沿轨迹每整公里一个点。
  ///
  /// [at] 是它落在轨迹上的位置，用「点下标」表示（`4.5` = 第 4、5 点之间）。回放时
  /// 用它判断线画到没有 —— 缺了它就只能猜，牌子要么全出来要么全不出来。
  static List<({LatLng ll, int km, double at})> kmMarkers(
    List<LatLng> points, {
    double intervalKm = 1,
    int maxMarks = 120,
  }) {
    final out = <({LatLng ll, int km, double at})>[];
    if (points.length < 2 || intervalKm <= 0) return out;
    var acc = 0.0;
    var next = intervalKm;
    var n = 1;
    for (var i = 1; i < points.length; i++) {
      final d = _haversineKm(points[i - 1], points[i]);
      if (d <= 0) continue;
      while (acc + d >= next && n <= maxMarks) {
        final t = ((next - acc) / d).clamp(0.0, 1.0);
        final a = points[i - 1];
        final b = points[i];
        out.add((
          ll: LatLng(
            a.latitude + (b.latitude - a.latitude) * t,
            a.longitude + (b.longitude - a.longitude) * t,
          ),
          km: n,
          at: (i - 1) + t,
        ));
        n++;
        next += intervalKm;
      }
      acc += d;
    }
    return out;
  }

  /// 每个点的累计骑行时间（秒，从 0 起）。
  ///
  /// 用相邻两点的距离除以该点速度累加，还原「这段路实际花了多久」，回放时才能
  /// 按运动本身的速度推进（快的地方画得快）。比按点数平均更贴近真实节奏。
  /// 速度缺失或异常（<=0.2 km/h）时用 [fallbackKmh] 兜底。
  static List<double> cumulativeRideSeconds(
    List<LatLng> points,
    List<double> speedsAtPoint, {
    double fallbackKmh = 18,
  }) {
    final n = points.length;
    final out = List<double>.filled(n, 0);
    if (n < 2) return out;
    var acc = 0.0;
    for (var i = 1; i < n; i++) {
      final d = _haversineKm(points[i - 1], points[i]);
      var v = i < speedsAtPoint.length ? speedsAtPoint[i] : fallbackKmh;
      if (!(v > 0.2)) v = fallbackKmh;
      acc += d / v * 3600.0;
      out[i] = acc;
    }
    return out;
  }

  static ({int? avg, int? max, List<RideSeriesPoint> points}) _hrSeries(
    List<_GpxPt> pts,
  ) {
    final series = <RideSeriesPoint>[];
    var sum = 0;
    var maxV = 0;
    for (final p in pts) {
      final hr = p.hr;
      if (hr == null) continue;
      series.add(RideSeriesPoint(time: p.time, value: hr.toDouble()));
      sum += hr;
      if (hr > maxV) maxV = hr;
    }
    if (series.isEmpty) {
      return (avg: null, max: null, points: const []);
    }
    return (
      avg: (sum / series.length).round(),
      max: maxV,
      points: series,
    );
  }

  static ({double? gain, double? loss, double? min, double? max}) _elev(
    List<_GpxPt> pts,
  ) {
    double? minV;
    double? maxV;
    var gain = 0.0;
    var loss = 0.0;
    double? prev;
    var n = 0;
    for (final p in pts) {
      final e = p.ele;
      if (e == null) continue;
      n++;
      minV = minV == null ? e : math.min(minV, e);
      maxV = maxV == null ? e : math.max(maxV, e);
      if (prev != null) {
        final d = e - prev;
        if (d > 0.8) gain += d;
        if (d < -0.8) loss += -d;
      }
      prev = e;
    }
    if (n < 2) {
      return (gain: null, loss: null, min: minV, max: maxV);
    }
    return (
      gain: gain > 0.5 ? gain : null,
      loss: loss > 0.5 ? loss : null,
      min: minV,
      max: maxV,
    );
  }

  /// `1:33:20` 或不足一小时 `33:20`。
  static String formatElapsed(Duration d) {
    final h = d.inHours;
    final m = d.inMinutes.remainder(60).toString().padLeft(2, '0');
    final s = d.inSeconds.remainder(60).toString().padLeft(2, '0');
    if (h > 0) return '$h:$m:$s';
    return '$m:$s';
  }

  /// 始终 `00:04:16`。
  static String formatHms(Duration d) {
    final h = d.inHours.toString().padLeft(2, '0');
    final m = d.inMinutes.remainder(60).toString().padLeft(2, '0');
    final s = d.inSeconds.remainder(60).toString().padLeft(2, '0');
    return '$h:$m:$s';
  }

  /// `2024/8/31 09:49`
  static String formatStamp(DateTime t) {
    final l = t.toLocal();
    final hh = l.hour.toString().padLeft(2, '0');
    final mm = l.minute.toString().padLeft(2, '0');
    return '${l.year}/${l.month}/${l.day} $hh:$mm';
  }

  /// 心率区间累计（热身 / 燃脂 / 有氧 / 无氧 / 极限）。
  static List<Duration> hrZoneDurations(
    List<RideSeriesPoint> pts, {
    Duration? total,
  }) {
    final zones = List<Duration>.filled(5, Duration.zero);
    if (pts.isEmpty) return zones;
    final fallbackMs =
        (total?.inMilliseconds ?? pts.length * 1000) ~/ pts.length;
    for (var i = 0; i < pts.length; i++) {
      var ms = fallbackMs;
      if (i + 1 < pts.length) {
        final a = pts[i].time;
        final b = pts[i + 1].time;
        if (a != null && b != null) {
          final d = b.difference(a).inMilliseconds;
          if (d > 0 && d <= 120000) ms = d;
        }
      }
      if (ms <= 0) continue;
      final z = hrZoneIndex(pts[i].value.round());
      zones[z] += Duration(milliseconds: ms);
    }
    return zones;
  }

  /// 按默认最大心率 190 的五区。
  static int hrZoneIndex(int bpm) {
    if (bpm < 114) return 0;
    if (bpm < 133) return 1;
    if (bpm < 152) return 2;
    if (bpm < 171) return 3;
    return 4;
  }

  /// 无热量字段时的估算：有心率用 Keytel 简化，否则按里程。
  static int estimateKcal({
    int? avgHrBpm,
    Duration? duration,
    double distanceKm = 0,
  }) {
    final minutes = (duration?.inSeconds ?? 0) / 60.0;
    if (avgHrBpm != null && avgHrBpm > 0 && minutes > 0) {
      return math.max(1, (avgHrBpm * minutes * 0.03).round());
    }
    if (distanceKm > 0.001) {
      return math.max(1, (distanceKm * 30).round());
    }
    return 0;
  }

  /// 文件里按出现顺序的可信 UTC 时间（忽略码表未校时的 1970/2000）。
  static List<DateTime> plausibleTimes(String xml) {
    final out = <DateTime>[];
    for (final match in _timeRe.allMatches(xml)) {
      final t = tryParseUtc(match.group(1)!);
      if (t != null && isPlausibleTime(t)) {
        out.add(t);
      }
    }
    return out;
  }

  /// 解析 ISO-8601，失败返回 null。
  static DateTime? tryParseUtc(String raw) {
    try {
      return DateTime.parse(raw.trim()).toUtc();
    } catch (_) {
      return null;
    }
  }

  /// GNSS / 手机时间：2020 年之后、且不超过「现在 + 2 天」。
  static bool isPlausibleTime(DateTime t) {
    final u = t.toUtc();
    final now = DateTime.now().toUtc();
    return u.year >= 2020 && !u.isAfter(now.add(const Duration(days: 2)));
  }

  /// `2026-09-07T11:16:00Z`
  static String formatUtc(DateTime t) {
    final u = t.toUtc();
    String two(int n) => n.toString().padLeft(2, '0');
    return '${u.year.toString().padLeft(4, '0')}-'
        '${two(u.month)}-${two(u.day)}T'
        '${two(u.hour)}:${two(u.minute)}:${two(u.second)}Z';
  }

  static int unixSeconds(DateTime t) =>
      t.toUtc().millisecondsSinceEpoch ~/ 1000;

  /// 相邻点球面距离之和，千米。
  static double distanceKm(List<LatLng> points) {
    var sum = 0.0;
    for (var i = 1; i < points.length; i++) {
      sum += _haversineKm(points[i - 1], points[i]);
    }
    return sum;
  }

  static double _haversineKm(LatLng a, LatLng b) {
    const r = 6371.0;
    final dLat = _rad(b.latitude - a.latitude);
    final dLon = _rad(b.longitude - a.longitude);
    final x = math.sin(dLat / 2) * math.sin(dLat / 2) +
        math.cos(_rad(a.latitude)) *
            math.cos(_rad(b.latitude)) *
            math.sin(dLon / 2) *
            math.sin(dLon / 2);
    return 2 * r * math.asin(math.min(1, math.sqrt(x)));
  }

  static double _rad(double deg) => deg * math.pi / 180;

  /// 列表缩略图用：保留起终点，中间均匀抽点。
  static List<LatLng> previewTrack(List<LatLng> points, {int max = 48}) {
    if (points.length <= 2) return List<LatLng>.from(points);
    if (points.length <= max) return List<LatLng>.from(points);
    final last = points.length - 1;
    final out = <LatLng>[];
    for (var i = 0; i < max; i++) {
      final idx = ((last * i) / (max - 1)).round();
      if (out.isEmpty || out.last != points[idx]) {
        out.add(points[idx]);
      }
    }
    if (out.last != points.last) out.add(points.last);
    return out;
  }

  /// 稀疏途经点：route。写入 metadata 时间（手机时钟）。
  static String writeRoute({
    required String name,
    required List<LatLng> points,
    List<String>? labels,
    DateTime? timeUtc,
  }) {
    final metaTime = formatUtc(timeUtc ?? DateTime.now().toUtc());
    final buf = StringBuffer()
      ..writeln('<?xml version="1.0" encoding="UTF-8"?>')
      ..writeln(
        '<gpx version="1.1" creator="Helm One App" '
        'xmlns="http://www.topografix.com/GPX/1/1">',
      )
      ..writeln(
        '  <metadata><name>${_esc(name)}</name>'
        '<time>$metaTime</time></metadata>',
      )
      ..writeln('  <rte>')
      ..writeln('    <name>${_esc(name)}</name>');
    for (var i = 0; i < points.length; i++) {
      final p = points[i];
      final label = (labels != null && i < labels.length && labels[i].isNotEmpty)
          ? labels[i]
          : '${i + 1}';
      buf.writeln(
        '    <rtept lat="${p.latitude.toStringAsFixed(7)}" '
        'lon="${p.longitude.toStringAsFixed(7)}">'
        '<name>${_esc(label)}</name></rtept>',
      );
    }
    buf
      ..writeln('  </rte>')
      ..writeln('</gpx>');
    return buf.toString();
  }

  /// 密轨迹：track。点上写 `<time>`，默认 1 秒一点、最后一点为 [endUtc]。
  static String writeTrack({
    required String name,
    required List<LatLng> points,
    DateTime? endUtc,
    Duration pointInterval = const Duration(seconds: 1),
  }) {
    final end = (endUtc ?? DateTime.now()).toUtc();
    final n = points.length;
    final start = n <= 1
        ? end
        : end.subtract(pointInterval * (n - 1));
    final buf = StringBuffer()
      ..writeln('<?xml version="1.0" encoding="UTF-8"?>')
      ..writeln(
        '<gpx version="1.1" creator="Helm One App" '
        'xmlns="http://www.topografix.com/GPX/1/1">',
      )
      ..writeln(
        '  <metadata><name>${_esc(name)}</name>'
        '<time>${formatUtc(start)}</time></metadata>',
      )
      ..writeln('  <trk><name>${_esc(name)}</name><trkseg>');
    for (var i = 0; i < n; i++) {
      final p = points[i];
      final t = start.add(pointInterval * i);
      buf.writeln(
        '    <trkpt lat="${p.latitude.toStringAsFixed(7)}" '
        'lon="${p.longitude.toStringAsFixed(7)}">'
        '<time>${formatUtc(t)}</time></trkpt>',
      );
    }
    buf.writeln('  </trkseg></trk></gpx>');
    return buf.toString();
  }

  /// 码表 FS 的 mtime 不可信。文件里若没有可信 `<time>`，用手机时钟补上。
  /// 已有 GNSS/App 时间则原样返回，不改 ele / 扩展字段。
  static String ensureTimestamps(String xml, {DateTime? nowUtc}) {
    if (plausibleTimes(xml).isNotEmpty) {
      return xml;
    }
    final now = (nowUtc ?? DateTime.now()).toUtc();
    var n = _trkptRe.allMatches(xml).length;
    if (n == 0) {
      n = _trkptSelfRe.allMatches(xml).length;
    }
    final start = n <= 1 ? now : now.subtract(Duration(seconds: n - 1));
    var out = _stampTrkpts(xml, now);
    out = _ensureMetadataTime(out, start);
    return out;
  }

  static String _stampTrkpts(String xml, DateTime endUtc) {
    final blocks = _trkptRe.allMatches(xml).toList();
    if (blocks.isEmpty) {
      return _stampSelfClosing(xml, endUtc);
    }
    final n = blocks.length;
    final start = n <= 1
        ? endUtc
        : endUtc.subtract(Duration(seconds: n - 1));
    var out = xml;
    for (var i = n - 1; i >= 0; i--) {
      final m = blocks[i];
      final t = formatUtc(start.add(Duration(seconds: i)));
      out = out.replaceRange(m.start, m.end, _stampOneTrkpt(m.group(0)!, t));
    }
    return out;
  }

  static String _stampSelfClosing(String xml, DateTime endUtc) {
    final blocks = _trkptSelfRe.allMatches(xml).toList();
    if (blocks.isEmpty) return xml;
    final n = blocks.length;
    final start = n <= 1
        ? endUtc
        : endUtc.subtract(Duration(seconds: n - 1));
    var out = xml;
    for (var i = n - 1; i >= 0; i--) {
      final m = blocks[i];
      final attrs = m.group(1) ?? '';
      final t = formatUtc(start.add(Duration(seconds: i)));
      out = out.replaceRange(
        m.start,
        m.end,
        '<trkpt$attrs><time>$t</time></trkpt>',
      );
    }
    return out;
  }

  static String _stampOneTrkpt(String block, String iso) {
    if (_timeRe.hasMatch(block)) {
      return block.replaceFirst(_timeRe, '<time>$iso</time>');
    }
    return block.replaceFirst(
      RegExp(r'</trkpt>', caseSensitive: false),
      '<time>$iso</time></trkpt>',
    );
  }

  static String _ensureMetadataTime(String xml, DateTime t) {
    final iso = formatUtc(t);
    final meta = _metaOpenRe.firstMatch(xml);
    if (meta != null) {
      final closeIdx = xml.toLowerCase().indexOf('</metadata>', meta.end);
      if (closeIdx > meta.end) {
        final inner = xml.substring(meta.end, closeIdx);
        if (_timeRe.hasMatch(inner)) {
          final stamped = inner.replaceFirst(_timeRe, '<time>$iso</time>');
          return xml.substring(0, meta.end) + stamped + xml.substring(closeIdx);
        }
        return '${xml.substring(0, meta.end)}<time>$iso</time>${xml.substring(meta.end)}';
      }
    }
    final gpx = _gpxOpenRe.firstMatch(xml);
    if (gpx == null) return xml;
    return '${xml.substring(0, gpx.end)}\n'
        '  <metadata><time>$iso</time></metadata>'
        '${xml.substring(gpx.end)}';
  }

  /// 改 GPX 里的 `<name>`，没有则写入 metadata。分享 / 码表列表跟这个走。
  static String setName(String xml, String name) {
    final esc = _esc(name.trim());
    if (esc.isEmpty) return xml;
    if (_nameRe.hasMatch(xml)) {
      return xml.replaceAll(_nameRe, '<name>$esc</name>');
    }
    final meta = _metaOpenRe.firstMatch(xml);
    if (meta != null) {
      return '${xml.substring(0, meta.end)}<name>$esc</name>${xml.substring(meta.end)}';
    }
    final gpx = _gpxOpenRe.firstMatch(xml);
    if (gpx == null) return xml;
    return '${xml.substring(0, gpx.end)}\n'
        '  <metadata><name>$esc</name></metadata>'
        '${xml.substring(gpx.end)}';
  }

  /// 码表列表文件名上限约 31 字符（含 .gpx）。
  static String safeFileName(String raw, {int maxChars = 31}) {
    var stem = raw.trim();
    if (stem.toLowerCase().endsWith('.gpx')) {
      stem = stem.substring(0, stem.length - 4);
    }
    final buf = StringBuffer();
    for (final r in stem.runes) {
      final c = String.fromCharCode(r);
      if (RegExp(r'[A-Za-z0-9._\u4e00-\u9fff-]').hasMatch(c)) {
        buf.write(c);
      } else if (c == ' ') {
        buf.write('_');
      }
    }
    var out = buf.toString();
    if (out.isEmpty) {
      out = 'route';
    }
    const ext = '.gpx';
    final cap = maxChars - ext.length;
    if (out.length > cap) {
      out = out.substring(0, cap);
    }
    return '$out$ext';
  }

  /// 坐标点记录文件名（`.tsv`）。[maxChars] 按 UTF-8 字节计，给码表 `dirent` 留余量。
  static String safeTsvFileName(String raw, {int maxChars = 40}) {
    var stem = raw.trim();
    if (stem.toLowerCase().endsWith('.tsv')) {
      stem = stem.substring(0, stem.length - 4);
    }
    final buf = StringBuffer();
    for (final r in stem.runes) {
      final c = String.fromCharCode(r);
      if (RegExp(r'[A-Za-z0-9._\u4e00-\u9fff-]').hasMatch(c)) {
        buf.write(c);
      } else if (c == ' ') {
        buf.write('_');
      }
    }
    var out = buf.toString();
    if (out.isEmpty) {
      out = 'route';
    }
    const ext = '.tsv';
    final cap = maxChars - ext.length;
    out = WaypointTsv.capUtf8(out, cap > 1 ? cap : 1);
    return '$out$ext';
  }

  static String _esc(String s) => s
      .replaceAll('&', '&amp;')
      .replaceAll('<', '&lt;')
      .replaceAll('>', '&gt;')
      .replaceAll('"', '&quot;');
}

/// 轨迹上带时间的一维序列（心率、速度）。
class RideSeriesPoint {
  const RideSeriesPoint({this.time, required this.value});

  final DateTime? time;
  final double value;
}

typedef _GpxPt = ({LatLng ll, double? ele, DateTime? time, int? hr});

/// 解析结果。
class GpxSummary {
  const GpxSummary({
    required this.name,
    required this.points,
    required this.distanceKm,
    this.startTime,
    this.endTime,
    this.duration,
    this.avgSpeedKmh,
    this.maxSpeedKmh,
    this.elevGainM,
    this.elevLossM,
    this.minEleM,
    this.maxEleM,
    this.avgHrBpm,
    this.maxHrBpm,
    this.hrPoints = const [],
    this.speedPoints = const [],
    this.pointSpeedsKmh = const [],
  });

  final String name;
  final List<LatLng> points;
  final double distanceKm;

  /// 文件内第一个可信 `<time>`。
  final DateTime? startTime;

  /// 文件内最后一个可信 `<time>`（用于列表日期）。
  final DateTime? endTime;

  /// 起止时间差；过短则空。
  final Duration? duration;

  final double? avgSpeedKmh;
  final double? maxSpeedKmh;
  final double? elevGainM;
  final double? elevLossM;
  final double? minEleM;
  final double? maxEleM;
  final int? avgHrBpm;
  final int? maxHrBpm;
  final List<RideSeriesPoint> hrPoints;
  final List<RideSeriesPoint> speedPoints;

  /// 与 [points] 等长；`[i]` 是到达该点的瞬时速度。
  final List<double> pointSpeedsKmh;

  bool get hasHr => hrPoints.isNotEmpty && avgHrBpm != null;
}

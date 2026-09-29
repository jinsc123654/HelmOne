import 'dart:convert';
import 'dart:io';

import 'package:latlong2/latlong.dart';
import 'package:path/path.dart' as p;
import 'package:path_provider/path_provider.dart';

/// 坐标点记录里的一个途经点。
class NavStop {
  NavStop({required this.name, required this.point});

  String name;
  LatLng point;

  Map<String, Object?> toJson() => {
        'name': name,
        'lat': point.latitude,
        'lon': point.longitude,
      };

  factory NavStop.fromJson(Map<String, Object?> json, {String fallback = ''}) {
    return NavStop(
      name: (json['name'] as String?)?.trim() ?? fallback,
      point: LatLng(
        (json['lat'] as num?)?.toDouble() ?? 0,
        (json['lon'] as num?)?.toDouble() ?? 0,
      ),
    );
  }
}

/// 一条坐标点记录：有序途经点。
class NavRouteRecord {
  NavRouteRecord({
    required this.id,
    required this.name,
    required this.stops,
    required this.updatedMs,
  });

  final String id;
  String name;
  List<NavStop> stops;
  int updatedMs;

  List<LatLng> get points => [for (final s in stops) s.point];

  Map<String, Object?> toJson() => {
        'id': id,
        'name': name,
        'updatedMs': updatedMs,
        'points': [for (final s in stops) s.toJson()],
      };

  factory NavRouteRecord.fromJson(Map<String, Object?> json) {
    final raw = json['points'];
    final stops = <NavStop>[];
    if (raw is List) {
      var i = 0;
      for (final e in raw) {
        i++;
        if (e is Map) {
          stops.add(
            NavStop.fromJson(
              Map<String, Object?>.from(e),
              fallback: '$i',
            ),
          );
        }
      }
    }
    return NavRouteRecord(
      id: json['id'] as String? ?? '',
      name: json['name'] as String? ?? '',
      stops: stops,
      updatedMs: json['updatedMs'] as int? ?? 0,
    );
  }
}

/// 本机坐标点库。同步时覆盖码表 `mtp/navpts/`。
class NavRouteStore {
  factory NavRouteStore() => _instance;
  static final NavRouteStore _instance = NavRouteStore._();
  NavRouteStore._();

  File? _file;
  final List<NavRouteRecord> _items = [];

  List<NavRouteRecord> get items => List.unmodifiable(_items);

  Future<File> _ensure() async {
    if (_file != null) return _file!;
    final root = await getApplicationDocumentsDirectory();
    _file = File(p.join(root.path, 'nav_routes.json'));
    return _file!;
  }

  Future<void> load() async {
    final f = await _ensure();
    _items.clear();
    if (!await f.exists()) return;
    try {
      final raw = jsonDecode(await f.readAsString());
      if (raw is List) {
        for (final e in raw) {
          if (e is Map<String, dynamic>) {
            _items.add(NavRouteRecord.fromJson(Map<String, Object?>.from(e)));
          }
        }
      }
    } catch (_) {
      _items.clear();
    }
    _items.sort((a, b) => b.updatedMs.compareTo(a.updatedMs));
  }

  Future<void> _flush() async {
    final f = await _ensure();
    await f.writeAsString(
      jsonEncode(_items.map((e) => e.toJson()).toList()),
    );
  }

  NavRouteRecord? find(String id) {
    for (final e in _items) {
      if (e.id == id) return e;
    }
    return null;
  }

  Future<void> upsert(NavRouteRecord rec) async {
    rec.updatedMs = DateTime.now().millisecondsSinceEpoch;
    final i = _items.indexWhere((e) => e.id == rec.id);
    if (i >= 0) {
      _items[i] = rec;
    } else {
      _items.insert(0, rec);
    }
    _items.sort((a, b) => b.updatedMs.compareTo(a.updatedMs));
    await _flush();
  }

  Future<void> remove(String id) async {
    _items.removeWhere((e) => e.id == id);
    await _flush();
  }
}

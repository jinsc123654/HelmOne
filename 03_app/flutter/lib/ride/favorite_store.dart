import 'dart:convert';
import 'dart:io';

import 'package:latlong2/latlong.dart';
import 'package:path/path.dart' as p;
import 'package:path_provider/path_provider.dart';

/// 常用点池中的一个地点。
class FavoritePlace {
  FavoritePlace({
    required this.id,
    required this.name,
    required this.point,
  });

  final String id;
  String name;
  LatLng point;

  Map<String, Object?> toJson() => {
        'id': id,
        'name': name,
        'lat': point.latitude,
        'lon': point.longitude,
      };

  factory FavoritePlace.fromJson(Map<String, Object?> json) {
    return FavoritePlace(
      id: json['id'] as String? ?? '',
      name: json['name'] as String? ?? '',
      point: LatLng(
        (json['lat'] as num?)?.toDouble() ?? 0,
        (json['lon'] as num?)?.toDouble() ?? 0,
      ),
    );
  }
}

/// 本机常用点池。同步时覆盖码表常用点文件。
class FavoriteStore {
  factory FavoriteStore() => _instance;
  static final FavoriteStore _instance = FavoriteStore._();
  FavoriteStore._();

  File? _file;
  final List<FavoritePlace> _items = [];

  List<FavoritePlace> get items => List.unmodifiable(_items);

  Future<File> _ensure() async {
    if (_file != null) return _file!;
    final root = await getApplicationDocumentsDirectory();
    _file = File(p.join(root.path, 'favorites.json'));
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
            _items.add(FavoritePlace.fromJson(Map<String, Object?>.from(e)));
          }
        }
      }
    } catch (_) {
      _items.clear();
    }
  }

  Future<void> _flush() async {
    final f = await _ensure();
    await f.writeAsString(
      jsonEncode(_items.map((e) => e.toJson()).toList()),
    );
  }

  Future<void> upsert(FavoritePlace place) async {
    final i = _items.indexWhere((e) => e.id == place.id);
    if (i >= 0) {
      _items[i] = place;
    } else {
      _items.add(place);
    }
    await _flush();
  }

  Future<void> remove(String id) async {
    _items.removeWhere((e) => e.id == id);
    await _flush();
  }
}

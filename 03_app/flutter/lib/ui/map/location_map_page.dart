import 'package:flutter/material.dart';
import 'package:flutter_map/flutter_map.dart';
import 'package:get/get.dart';
import 'package:latlong2/latlong.dart';
import 'package:sifli_companion/i18n/locale_keys.dart';
import 'package:sifli_companion/location/location_manager.dart';
import 'package:sifli_companion/map/osm_tiles.dart';

/// 定位 + OpenStreetMap 地图页。
class LocationMapPage extends StatefulWidget {
  /// 创建地图定位页。
  const LocationMapPage({super.key});

  @override
  State<LocationMapPage> createState() => _LocationMapPageState();
}

class _LocationMapPageState extends State<LocationMapPage> {
  final _mapController = MapController();
  LatLng? _pos;
  String _status = '';
  bool _loading = false;
  double? _accuracy;

  @override
  void initState() {
    super.initState();
    _status = LocaleKeys.mapLocating.tr;
    _locate();
  }

  @override
  void dispose() {
    _mapController.dispose();
    super.dispose();
  }

  Future<void> _locate() async {
    setState(() {
      _loading = true;
      _status = LocaleKeys.mapLocating.tr;
    });

    final service = await LocationManager().isServiceEnabled();
    if (!service) {
      setState(() {
        _loading = false;
        _status = LocaleKeys.mapServiceOff.tr;
      });
      return;
    }

    final pos = await LocationManager().getCurrentPosition();
    if (!mounted) return;
    if (pos == null) {
      setState(() {
        _loading = false;
        _status = LocaleKeys.mapPermissionDenied.tr;
      });
      return;
    }

    final ll = LatLng(pos.latitude, pos.longitude);
    setState(() {
      _pos = ll;
      _accuracy = pos.accuracy;
      _loading = false;
      _status = LocaleKeys.mapCoord.trParams({
        'lat': pos.latitude.toStringAsFixed(6),
        'lng': pos.longitude.toStringAsFixed(6),
        'acc': pos.accuracy.toStringAsFixed(1),
      });
    });
    _mapController.move(HelmOsmTiles.toDisplay(ll), 16);
  }

  @override
  Widget build(BuildContext context) {
    final center = HelmOsmTiles.toDisplay(
      _pos ?? const LatLng(32.255, 118.32),
    );
    return Scaffold(
      appBar: AppBar(
        title: Text(LocaleKeys.mapTitle.tr),
        actions: [
          IconButton(
            tooltip: LocaleKeys.mapOpenSettings.tr,
            onPressed: () => LocationManager().openAppSettings(),
            icon: const Icon(Icons.settings),
          ),
        ],
      ),
      body: Column(
        children: [
          Material(
            elevation: 1,
            child: ListTile(
              dense: true,
              title: Text(_status),
              trailing: _loading
                  ? const SizedBox(
                      width: 20,
                      height: 20,
                      child: CircularProgressIndicator(strokeWidth: 2),
                    )
                  : null,
            ),
          ),
          Expanded(
            child: FlutterMap(
              mapController: _mapController,
              options: MapOptions(
                initialCenter: center,
                initialZoom: _pos == null ? 11 : 16,
              ),
              children: [
                HelmOsmTiles.layer(),
                if (_pos != null)
                  MarkerLayer(
                    markers: [
                      Marker(
                        point: HelmOsmTiles.toDisplay(_pos!),
                        width: 48,
                        height: 48,
                        child: const Icon(
                          Icons.location_on,
                          color: Colors.red,
                          size: 40,
                        ),
                      ),
                    ],
                  ),
                if (_pos != null && _accuracy != null && _accuracy! > 0)
                  CircleLayer(
                    circles: [
                      CircleMarker(
                        point: HelmOsmTiles.toDisplay(_pos!),
                        radius: _accuracy!,
                        useRadiusInMeter: true,
                        color: Colors.teal.withValues(alpha: 0.15),
                        borderStrokeWidth: 1,
                        borderColor: Colors.teal,
                      ),
                    ],
                  ),
              ],
            ),
          ),
        ],
      ),
      floatingActionButton: FloatingActionButton.extended(
        onPressed: _loading ? null : _locate,
        icon: const Icon(Icons.my_location),
        label: Text(LocaleKeys.mapLocate.tr),
      ),
    );
  }
}

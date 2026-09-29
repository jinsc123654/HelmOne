import 'package:flutter/material.dart';
import 'package:get/get.dart';
import 'package:sifli_companion/app/app_theme.dart';
import 'package:sifli_companion/i18n/locale_keys.dart';
import 'package:sifli_companion/map/osm_city_catalog.dart';
import 'package:sifli_companion/map/osm_city_store.dart';
import 'package:sifli_companion/ui/widgets/helm_chrome.dart';

/// 手动下载城市底图，以及查看哪些已经下完。
class OsmCitiesPage extends StatefulWidget {
  /// [initialTab] 0 = 下载目录，1 = 已下载。
  const OsmCitiesPage({super.key, this.initialTab = 0});

  final int initialTab;

  @override
  State<OsmCitiesPage> createState() => _OsmCitiesPageState();
}

class _OsmCitiesPageState extends State<OsmCitiesPage>
    with SingleTickerProviderStateMixin {
  late final TabController _tabs;
  final _search = TextEditingController();
  final _store = OsmCityStore.instance;
  List<OsmCityStatus> _local = const [];
  final _remoteStatus = <String, OsmCityStatus>{};
  bool _loadingLocal = true;

  @override
  void initState() {
    super.initState();
    _tabs = TabController(
      length: 2,
      vsync: this,
      initialIndex: widget.initialTab.clamp(0, 1),
    );
    _store.addListener(_onStore);
    _search.addListener(() => setState(() {}));
    _reload();
  }

  @override
  void dispose() {
    _store.removeListener(_onStore);
    _tabs.dispose();
    _search.dispose();
    super.dispose();
  }

  void _onStore() {
    if (mounted) setState(() {});
    if (!_store.running) {
      _reload();
    }
  }

  Future<void> _reload() async {
    final local = await _store.downloaded();
    if (!mounted) return;
    setState(() {
      _local = local;
      _loadingLocal = false;
      for (final s in local) {
        _remoteStatus[s.city.id] = s;
      }
    });
  }

  String _fmtBytes(int n) {
    if (n < 1024) return '$n B';
    if (n < 1024 * 1024) return '${(n / 1024).toStringAsFixed(1)} KB';
    return '${(n / (1024 * 1024)).toStringAsFixed(1)} MB';
  }

  Future<void> _start(OsmCity city) async {
    await _store.download(city);
    if (!mounted) return;
    if (_store.error != null) {
      AppTheme.snack(city.name, LocaleKeys.osmCitiesFailed.tr);
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: Text(LocaleKeys.osmCitiesTitle.tr),
        bottom: TabBar(
          controller: _tabs,
          indicatorColor: AppTheme.accent,
          labelColor: Colors.white,
          unselectedLabelColor: AppTheme.muted,
          tabs: [
            Tab(text: LocaleKeys.osmCitiesDownload.tr),
            Tab(text: LocaleKeys.osmCitiesLocal.tr),
          ],
        ),
      ),
      body: TabBarView(
        controller: _tabs,
        children: [
          _catalogTab(),
          _localTab(),
        ],
      ),
    );
  }

  Widget _catalogTab() {
    final q = _search.text;
    final cities =
        OsmCityCatalog.cities.where((c) => c.matches(q)).toList(growable: false);
    return Column(
      children: [
        Padding(
          padding: const EdgeInsets.fromLTRB(16, 12, 16, 8),
          child: TextField(
            controller: _search,
            decoration: InputDecoration(
              hintText: LocaleKeys.osmCitiesSearch.tr,
              prefixIcon: const Icon(Icons.search),
            ),
          ),
        ),
        Padding(
          padding: const EdgeInsets.fromLTRB(16, 0, 16, 8),
          child: Text(
            LocaleKeys.osmCitiesHint.tr,
            style: const TextStyle(color: AppTheme.muted, fontSize: 12),
          ),
        ),
        Expanded(
          child: ListView.builder(
            padding: const EdgeInsets.fromLTRB(16, 0, 16, 24),
            itemCount: cities.length,
            itemBuilder: (context, i) => Padding(
              padding: const EdgeInsets.only(bottom: 8),
              child: _cityCard(cities[i], showForget: false),
            ),
          ),
        ),
      ],
    );
  }

  Widget _localTab() {
    if (_loadingLocal) {
      return const Center(child: CircularProgressIndicator());
    }
    final active = _store.activeId == null
        ? null
        : OsmCityCatalog.byId(_store.activeId!);
    if (_local.isEmpty && active == null) {
      return Center(
        child: Text(
          LocaleKeys.osmCitiesLocalEmpty.tr,
          style: const TextStyle(color: AppTheme.muted),
        ),
      );
    }
    return ListView(
      padding: const EdgeInsets.fromLTRB(16, 12, 16, 24),
      children: [
        if (active != null)
          Padding(
            padding: const EdgeInsets.only(bottom: 8),
            child: _cityCard(active, showForget: false),
          ),
        for (final st in _local)
          if (st.city.id != _store.activeId)
            Padding(
              padding: const EdgeInsets.only(bottom: 8),
              child: _cityCard(st.city, showForget: true, known: st),
            ),
      ],
    );
  }

  Widget _cityCard(OsmCity city, {required bool showForget, OsmCityStatus? known}) {
    final st = known ?? _remoteStatus[city.id];
    final active = _store.activeId == city.id && _store.running;
    final have = active ? _store.done : (st?.have ?? 0);
    final expected = active ? _store.total : (st?.expected ?? city.expectedTiles);
    final complete = st?.isComplete ?? false;
    final partial = st?.isPartial ?? false;

    String sub;
    if (active) {
      sub = LocaleKeys.osmCitiesProgress.trParams({
        'have': '$have',
        'total': '$expected',
      });
    } else if (complete) {
      sub = '${LocaleKeys.osmCitiesComplete.tr} · ${_fmtBytes(st!.bytes)}';
    } else if (partial) {
      sub =
          '${LocaleKeys.osmCitiesPartial.tr} · $have / $expected · ${_fmtBytes(st!.bytes)}';
    } else {
      sub = LocaleKeys.osmCitiesAboutSize.trParams({
        'mb': (city.approxBytes / (1024 * 1024)).toStringAsFixed(0),
      });
    }

    return HelmCard(
      child: Padding(
        padding: const EdgeInsets.fromLTRB(16, 12, 8, 12),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                HelmIconBadge(
                  color: complete
                      ? AppTheme.iconKeep
                      : (active ? AppTheme.iconNav : AppTheme.iconUsb),
                  icon: complete
                      ? Icons.download_done
                      : Icons.location_city_outlined,
                ),
                const SizedBox(width: 12),
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Text(
                        city.name,
                        style: const TextStyle(
                          color: Colors.white,
                          fontWeight: FontWeight.w600,
                          fontSize: 16,
                        ),
                      ),
                      const SizedBox(height: 2),
                      Text(
                        '${city.nameEn} · $sub',
                        style: const TextStyle(
                          color: AppTheme.muted,
                          fontSize: 12,
                        ),
                      ),
                    ],
                  ),
                ),
                if (active)
                  TextButton(
                    onPressed: _store.cancel,
                    child: Text(LocaleKeys.osmCitiesCancel.tr),
                  )
                else if (complete)
                  showForget
                      ? TextButton(
                          onPressed: () async {
                            await _store.forget(city.id);
                            await _reload();
                          },
                          child: Text(LocaleKeys.osmCitiesDelete.tr),
                        )
                      : const Icon(Icons.check_circle, color: Color(0xFF22C55E))
                else
                  TextButton(
                    onPressed: _store.running ? null : () => _start(city),
                    child: Text(
                      partial
                          ? LocaleKeys.osmCitiesResume.tr
                          : LocaleKeys.osmCitiesDownload.tr,
                    ),
                  ),
              ],
            ),
            if (active || partial)
              Padding(
                padding: const EdgeInsets.only(top: 10, right: 8),
                child: LinearProgressIndicator(
                  value: expected <= 0 ? null : have / expected,
                  minHeight: 3,
                  color: AppTheme.accent,
                  backgroundColor: AppTheme.field,
                ),
              ),
          ],
        ),
      ),
    );
  }
}

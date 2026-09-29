import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:sifli_companion/gnss/rinex_mga.dart';
import 'package:sifli_companion/log/app_log.dart';

/// 从公开 IGS 镜像拉当天（或昨天）的 MIXED BRDC。
class EphBrdc {
  /// 创建下载器。
  EphBrdc({HttpClient? http}) : _http = http ?? HttpClient() {
    _http.userAgent = 'HelmOne/1.0 (companion GNSS BRDC)';
    _http.connectionTimeout = const Duration(seconds: 15);
  }

  static const _tag = 'EphBrdc';
  static const _bkgRoot = 'https://igs.bkg.bund.de/root_ftp/IGS/BRDC';

  final HttpClient _http;

  /// 下载并转成 UBX-MGA。
  Future<Uint8List> fetchAndBuild({
    DateTime? now,
    void Function(String url)? onUrl,
  }) async {
    final utc = (now ?? DateTime.now()).toUtc();
    final days = [utc, utc.subtract(const Duration(days: 1))];
    Object? last;
    for (final day in days) {
      final urls = <Uri>[
        ...await _listBkg(day),
        ...urisFor(day),
      ];
      final seen = <String>{};
      for (final url in urls) {
        if (!seen.add(url.toString())) continue;
        onUrl?.call(url.toString());
        try {
          final raw = await _get(url);
          final mga = RinexMga.convertBytes(raw, now: utc);
          await AppLog.i(_tag, 'BRDC $url -> ${mga.length} B');
          return mga;
        } catch (e) {
          last = e;
          await AppLog.w(_tag, 'BRDC skip $url: $e');
        }
      }
    }
    throw RinexMgaException('下载广播星历失败：${last ?? '无镜像'}');
  }

  /// 某日 BKG 上常见文件名（目录列表失败时的兜底）。
  static List<Uri> urisFor(DateTime utc) {
    final yyyy = utc.year.toString().padLeft(4, '0');
    final doy = _doy(utc);
    final tag = '$yyyy${doy}0000';
    const names = [
      'BRDC00IGS',
      'BRDC00WRD',
      'BRD400DLR',
      'BRDM00DLR',
    ];
    return [
      for (final n in names)
        for (final kind in ['R', 'S'])
          Uri.parse('$_bkgRoot/$yyyy/$doy/${n}_${kind}_${tag}_01D_MN.rnx.gz'),
    ];
  }

  static String _doy(DateTime utc) {
    final jan1 = DateTime.utc(utc.year);
    return (utc.difference(jan1).inDays + 1).toString().padLeft(3, '0');
  }

  Future<List<Uri>> _listBkg(DateTime utc) async {
    final yyyy = utc.year.toString().padLeft(4, '0');
    final doy = _doy(utc);
    final dir = Uri.parse('$_bkgRoot/$yyyy/$doy/');
    try {
      final html = utf8.decode(await _get(dir, allowHtml: true));
      final names = RegExp(
        r'href="([^"]+_01D_MN\.rnx\.gz)"',
        caseSensitive: false,
      ).allMatches(html).map((m) => m.group(1)!).toSet().toList();
      names.sort((a, b) => _score(b).compareTo(_score(a)));
      return [for (final n in names) dir.resolve(n)];
    } catch (e) {
      await AppLog.w(_tag, 'BRDC list $dir: $e');
      return const [];
    }
  }

  static int _score(String name) {
    var s = 0;
    if (name.contains('_R_')) s += 20;
    if (name.contains('IGS')) s += 10;
    if (name.contains('WRD')) s += 8;
    if (name.startsWith('BRDC')) s += 5;
    return s;
  }

  Future<Uint8List> _get(Uri uri, {bool allowHtml = false}) async {
    final req = await _http.getUrl(uri);
    req.followRedirects = true;
    req.maxRedirects = 5;
    final res = await req.close().timeout(const Duration(seconds: 25));
    if (res.statusCode < 200 || res.statusCode >= 300) {
      await res.drain<void>();
      throw HttpException('HTTP ${res.statusCode}', uri: uri);
    }
    final builder = BytesBuilder(copy: false);
    await for (final chunk in res) {
      builder.add(chunk);
      if (builder.length > 8 * 1024 * 1024) {
        throw const RinexMgaException('星历文件过大');
      }
    }
    final bytes = Uint8List.fromList(builder.takeBytes());
    if (bytes.length < 64) {
      throw const RinexMgaException('星历文件太小');
    }
    if (!allowHtml && bytes[0] == 0x3c) {
      throw const RinexMgaException('镜像返回了网页而不是星历');
    }
    return bytes;
  }
}

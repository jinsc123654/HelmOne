import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:crypto/crypto.dart';
import 'package:path/path.dart' as p;
import 'package:sifli_companion/app/app_keys.dart';
import 'package:sifli_companion/ble/companion_client.dart';
import 'package:sifli_companion/ble/companion_proto.dart';
import 'package:sifli_companion/cache/app_cache_store.dart';
import 'package:sifli_companion/log/app_log.dart';
import 'package:sifli_companion/ota/ota_config.dart';
import 'package:sifli_companion/ota/ota_manifest.dart';

/// 云端清单 + 下载校验 + BLE 写入 `fw/`。
class OtaService {
  OtaService({HttpClient? http}) : _http = http ?? HttpClient() {
    _http.userAgent = AppKeys.androidPackage;
  }

  static const _tag = 'Ota';

  final HttpClient _http;

  /// 最近一次 [downloadFirmware] 是否命中本地 `app_cache/ota/`。
  bool lastFromCache = false;

  void close() => _http.close(force: true);

  /// 缓存文件名：SHA256.bin，与 URL 无关。
  static String cacheFileName(OtaManifest manifest) =>
      '${manifest.sha256}.bin';

  Future<Directory> _cacheDir() => AppCacheStore.instance.subdir('ota');

  Future<File> _cacheFile(OtaManifest manifest) async {
    final dir = await _cacheDir();
    return File(p.join(dir.path, cacheFileName(manifest)));
  }

  /// 本地已有同 SHA256、同大小的 bin。
  Future<bool> hasCache(OtaManifest manifest) async {
    try {
      final f = await _cacheFile(manifest);
      if (!await f.exists()) return false;
      return await f.length() == manifest.size;
    } catch (_) {
      return false;
    }
  }

  Future<Uint8List?> _loadCache(OtaManifest manifest) async {
    final f = await _cacheFile(manifest);
    if (!await f.exists()) return null;
    final bytes = await f.readAsBytes();
    try {
      verify(manifest, bytes);
    } catch (e) {
      await AppLog.w(_tag, '本地缓存无效，删除后重下: $e');
      try {
        await f.delete();
      } catch (_) {}
      return null;
    }
    await AppLog.i(_tag, '命中缓存 ${f.path} ${bytes.length} B');
    return bytes;
  }

  Future<void> _saveCache(OtaManifest manifest, Uint8List bytes) async {
    final f = await _cacheFile(manifest);
    await f.writeAsBytes(bytes, flush: true);
    await AppLog.i(_tag, '已缓存 ${f.path}');
  }

  Future<OtaManifest> fetchManifest({
    String hardware = OtaConfig.defaultHardware,
  }) async {
    final uri = OtaConfig.manifestUri(hardware: hardware);
    await AppLog.i(_tag, 'GET $uri');
    final got = await _getString(uri);
    final decoded = jsonDecode(got.body);
    if (decoded is! Map) {
      throw const FormatException('firmware.json 不是对象');
    }
    var manifest = OtaManifest.fromJson(Map<String, dynamic>.from(decoded));
    if (manifest.date.isEmpty && got.modified != null) {
      manifest = manifest.copyWith(date: OtaManifest.formatDay(got.modified!));
    }
    return manifest;
  }

  Future<Uint8List> downloadFirmware(
    OtaManifest manifest, {
    void Function(int done, int total)? onProgress,
    bool Function()? shouldStop,
  }) async {
    lastFromCache = false;
    final cached = await _loadCache(manifest);
    if (cached != null) {
      lastFromCache = true;
      onProgress?.call(cached.length, cached.length);
      return cached;
    }

    final primary = manifest.downloadUri;
    try {
      return await _downloadAndCache(
        primary,
        manifest,
        onProgress: onProgress,
        shouldStop: shouldStop,
      );
    } catch (e) {
      final original = Uri.parse(manifest.url);
      if (primary != original) {
        await AppLog.w(_tag, 'https 下载失败，改走清单原地址: $e');
        return _downloadAndCache(
          original,
          manifest,
          onProgress: onProgress,
          shouldStop: shouldStop,
        );
      }
      rethrow;
    }
  }

  Future<Uint8List> _downloadAndCache(
    Uri uri,
    OtaManifest manifest, {
    void Function(int done, int total)? onProgress,
    bool Function()? shouldStop,
  }) async {
    final bytes = await _getBytes(
      uri,
      expected: manifest.size,
      onProgress: onProgress,
      shouldStop: shouldStop,
    );
    verify(manifest, bytes);
    try {
      await _saveCache(manifest, bytes);
    } catch (e) {
      await AppLog.w(_tag, '写本地缓存失败: $e');
    }
    return bytes;
  }

  /// SHA256 或大小对不上就抛 [OtaChecksumException]。
  void verify(OtaManifest manifest, List<int> bytes) {
    if (bytes.length != manifest.size) {
      throw OtaChecksumException(
        '大小 ${bytes.length} 与清单 ${manifest.size} 不一致',
      );
    }
    final got = sha256.convert(bytes).toString();
    if (got != manifest.sha256) {
      throw OtaChecksumException('SHA256 $got ≠ ${manifest.sha256}');
    }
  }

  /// 列码表 OTA 槽。目录不存在当作空。
  Future<List<CompanionFsEntry>> listSlot(CompanionClient client) async {
    try {
      final out = <CompanionFsEntry>[];
      var cursor = 0;
      while (true) {
        final page = await client.fsList(CompanionFs.otaDir, cursor: cursor);
        if (page.isEmpty) break;
        out.addAll(page);
        cursor += page.length;
        if (page.length < 4) break;
      }
      return out;
    } on CompanionFsException catch (e) {
      if (e.status == 2) return [];
      rethrow;
    }
  }

  Future<void> uploadToDevice(
    CompanionClient client,
    OtaManifest manifest,
    List<int> bytes, {
    void Function(int done, int total)? onProgress,
  }) async {
    final path = manifest.slotPath;
    await AppLog.i(
      _tag,
      '上传 $path ${bytes.length} B sha256=${manifest.sha256.substring(0, 12)}…',
    );
    await client.fsUpload(path, bytes, resume: true, onProgress: onProgress);
  }

  Future<({String body, DateTime? modified})> _getString(Uri uri) async {
    final resp = await _open(uri);
    if (resp.statusCode != 200) {
      unawaited(resp.drain<void>());
      throw HttpException('HTTP ${resp.statusCode}', uri: uri);
    }
    DateTime? modified;
    final raw = resp.headers.value(HttpHeaders.lastModifiedHeader);
    if (raw != null && raw.isNotEmpty) {
      try {
        modified = HttpDate.parse(raw);
      } catch (_) {}
    }
    final body = await resp.transform(utf8.decoder).join();
    return (body: body, modified: modified);
  }

  Future<Uint8List> _getBytes(
    Uri uri, {
    required int expected,
    void Function(int done, int total)? onProgress,
    bool Function()? shouldStop,
  }) async {
    await AppLog.i(_tag, 'GET $uri');
    final resp = await _open(uri);
    if (resp.statusCode != 200) {
      unawaited(resp.drain<void>());
      throw HttpException('HTTP ${resp.statusCode}', uri: uri);
    }
    final total = resp.contentLength > 0 ? resp.contentLength : expected;
    final out = BytesBuilder(copy: false);
    var done = 0;
    await for (final chunk in resp) {
      if (shouldStop?.call() == true) {
        throw const OtaCancelledException();
      }
      out.add(chunk);
      done += chunk.length;
      onProgress?.call(done, total);
    }
    return out.takeBytes();
  }

  Future<HttpClientResponse> _open(Uri uri) async {
    final req = await _http.getUrl(uri);
    req.followRedirects = true;
    req.maxRedirects = 5;
    return req.close();
  }
}

class OtaChecksumException implements Exception {
  OtaChecksumException(this.message);
  final String message;
  @override
  String toString() => message;
}

class OtaCancelledException implements Exception {
  const OtaCancelledException();
  @override
  String toString() => '已取消';
}

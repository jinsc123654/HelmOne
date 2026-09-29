import 'dart:convert';
import 'dart:io';

import 'package:path/path.dart' as p;
import 'package:sifli_companion/cache/app_cache_store.dart';
import 'package:sifli_companion/ride/gpx_util.dart';

/// 从设备抓下来的一条日志。
///
/// 崩溃转储与诊断日志共用这个模型：设备侧两者命名同构
///（`nNNN_YYYYMMDD_HHMMSS.txt` / `dNNN_YYYYMMDD_HHMMSS.txt`），
/// 只有目录和前缀字母不同 —— 见 `myvendor_diaglog.c` 的注释。
class CapturedLog {
  /// 创建一条记录。
  const CapturedLog({
    required this.name,
    required this.file,
    required this.size,
    required this.pulledAt,
    required this.capturedAt,
    required this.hasStamp,
  });

  /// 设备上的**原始文件名**，例如 `d003_20260916_143012.txt`。
  final String name;

  /// 本机文件。
  final File file;

  /// 字节数。
  final int size;

  /// App 把它拉到本机的时间。
  final DateTime pulledAt;

  /// 设备写下它的时间，从文件名解析；解析不出时退化为 [pulledAt]。
  final DateTime capturedAt;

  /// 文件名里是否真带得出时间戳（降级名 `d003.txt` 为 false）。
  final bool hasStamp;

  /// 列表标题：设备侧抓取时间。
  String get title => GpxUtil.formatStamp(capturedAt);
}

/// 落在 `app_cache/<dir>/` 的本机日志库。
///
/// 两个实例：[coredump] 与 [diag]。索引文件沿用 `.pulled.json`
///（原 `CoredumpStore` 的名字），所以升级后老数据的时间戳不丢。
class CapturedLogStore {
  CapturedLogStore._(this.dirName, this.prefix);

  /// 崩溃转储：设备 `/mnt/kv/coredump`，文件名 `nNNN_…`。
  static final CapturedLogStore coredump = CapturedLogStore._('coredump', 'n');

  /// 诊断日志：设备 `/mnt/kv/diag`，文件名 `dNNN_…`。
  static final CapturedLogStore diag = CapturedLogStore._('diag', 'd');

  /// 本机缓存子目录名（同时也是设备侧目录名）。
  final String dirName;

  /// 文件名前缀字母，用来校验解析出的时间戳确实属于这个目录。
  final String prefix;

  static const _indexName = '.pulled.json';

  /// 设备侧日志文件名：`nNNN_YYYYMMDD_HHMMSS.txt`，降级名 `nNNN.txt`。
  ///
  /// 宽度是设备决定的 —— `myvendor_coredump.c` 用
  /// `sscanf(name, "n%u_%4d%2d%2d_%2d%2d%2d.txt")`，diag 同构。
  late final RegExp _logName = RegExp(
    '^${RegExp.escape(prefix)}' r'\d+(_\d{8}_\d{6})?\.txt$',
    caseSensitive: false,
  );

  /// 该名字是否是本目录的**日志文件**。
  ///
  /// 两个目录里都还有一个 `seq` 序号计数器（`/mnt/kv/diag/seq`、
  /// `/mnt/kv/coredump/seq`，见 `myvendor_diaglog.c` 的 `DIAGLOG_DIR "/seq"`
  /// 与 `myvendor_coredump.c` 的 `COREDUMP_SEQ_PATH`），它既不该显示、
  /// 也不该抓 —— 之前只排除了隐藏文件，所以它被当成日志拉下来了。
  ///
  /// 这里按**命名规则**过滤而不是黑名单排除 `seq`：将来设备再加别的
  /// 簿记文件也不会漏，而且不会误伤用户自己叫 `seq` 的文件 ——
  /// 只有长相确实像日志的才收。
  bool isLogFile(String name) => _logName.hasMatch(name);

  Future<Directory> dir() => AppCacheStore.instance.subdir(dirName);

  Future<File> _indexFile() async {
    final d = await dir();
    return File(p.join(d.path, _indexName));
  }

  Future<Map<String, String>> _readIndex() async {
    try {
      final f = await _indexFile();
      if (!await f.exists()) return {};
      final raw = jsonDecode(await f.readAsString());
      if (raw is! Map) return {};
      final map = <String, String>{};
      raw.forEach((k, v) {
        if (k is String && v is String) map[k] = v;
      });
      return map;
    } catch (_) {
      return {};
    }
  }

  Future<void> _writeIndex(Map<String, String> map) async {
    final f = await _indexFile();
    await f.writeAsString(jsonEncode(map), flush: true);
  }

  DateTime _parsePulled(String? raw, DateTime fallback) {
    if (raw == null || raw.isEmpty) return fallback;
    return DateTime.tryParse(raw) ?? fallback;
  }

  /// 从 `n003_20260916_143012` 里抠出设备侧时间。
  ///
  /// 解析不出（降级名 `d003`、或格式变了）返回 null，调用方会退回 pulledAt。
  DateTime? _parseStamp(String baseNoExt) {
    final m = RegExp(
      '^$prefix\\d+_(\\d{4})(\\d{2})(\\d{2})_(\\d{2})(\\d{2})(\\d{2})\$',
    ).firstMatch(baseNoExt);
    if (m == null) return null;
    final y = int.parse(m.group(1)!);
    final mo = int.parse(m.group(2)!);
    final d = int.parse(m.group(3)!);
    final h = int.parse(m.group(4)!);
    final mi = int.parse(m.group(5)!);
    final s = int.parse(m.group(6)!);
    // 设备侧写的是本地墙钟时间，当本地时间比较才有意义。
    if (mo < 1 || mo > 12 || d < 1 || d > 31) return null;
    if (h > 23 || mi > 59 || s > 59) return null;
    return DateTime(y, mo, d, h, mi, s);
  }

  /// 列出本机已抓取的全部日志，**按抓取时间倒序（最新在前）**。
  ///
  /// 排序键是文件名里的设备侧时间，不是拉到本机的时间：一次抓取会把
  /// 好几条同时拉下来，pulledAt 几乎相同，用它排等于没排。
  Future<List<CapturedLog>> list() async {
    final d = await dir();
    final index = await _readIndex();
    var dirty = false;
    final out = <CapturedLog>[];

    await for (final ent in d.list(followLinks: false)) {
      if (ent is! File) continue;
      final name = p.basename(ent.path);
      if (name.startsWith('.')) continue;
      // 早期版本把 `seq` 也拉下来了；留着文件不动（不偷偷删用户可见的东西），
      // 但不再显示 —— 「清空本机」会连同它一起清掉。
      if (!isLogFile(name)) continue;
      final st = await ent.stat();
      final pulledAt = _parsePulled(index[name], st.modified);
      if (!index.containsKey(name)) {
        index[name] = pulledAt.toUtc().toIso8601String();
        dirty = true;
      }
      final stamp = _parseStamp(p.basenameWithoutExtension(name));
      out.add(
        CapturedLog(
          name: name,
          file: ent,
          size: st.size,
          pulledAt: pulledAt,
          capturedAt: stamp ?? pulledAt,
          hasStamp: stamp != null,
        ),
      );
    }

    if (dirty) {
      await _writeIndex(index);
    }

    out.sort((a, b) {
      final c = b.capturedAt.compareTo(a.capturedAt);
      // 同一秒内（批量抓取）用文件名兜底，保证顺序稳定、不抖动。
      return c != 0 ? c : b.name.compareTo(a.name);
    });
    return out;
  }

  /// 该名字在本机的目标文件。
  Future<File> fileFor(String name) async {
    final safe = AppCacheStore.sanitizeName(name);
    if (safe.isEmpty) {
      throw ArgumentError('invalid name');
    }
    final d = await dir();
    return File(p.join(d.path, safe));
  }

  /// 记下某条记录被拉到本机的时间。
  Future<void> markPulled(String name, DateTime at) async {
    final safe = AppCacheStore.sanitizeName(name);
    if (safe.isEmpty) return;
    final index = await _readIndex();
    index[safe] = at.toUtc().toIso8601String();
    await _writeIndex(index);
  }

  /// 只删 App 里的这条记录，设备上的文件不动。
  Future<void> delete(CapturedLog item) async {
    if (await item.file.exists()) {
      await item.file.delete();
    }
    final index = await _readIndex();
    if (index.remove(item.name) != null) {
      await _writeIndex(index);
    }
  }

  /// 清空本机已抓取的全部记录。
  Future<void> clearAll() async {
    final d = await dir();
    await for (final ent in d.list(followLinks: false)) {
      if (ent is File) {
        await ent.delete();
      }
    }
  }
}

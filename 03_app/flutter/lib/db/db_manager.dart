import 'package:path/path.dart';
import 'package:sifli_companion/app/app_keys.dart';
import 'package:sqflite/sqflite.dart';

/// SQLite 数据库管理器。
///
/// 启动时调用 [initDB]；库文件为 `sifli_companion.db`。
/// 建议在 [CacheData.initInfo] 之后打开。
class DBManager {
  /// 返回单例。
  factory DBManager() => _instance;

  static final DBManager _instance = DBManager._internal();

  /// 与 [DBManager.new] 相同的单例入口。
  static DBManager get instance => _instance;

  static late final Database _db;

  /// 已打开的数据库实例；须在 [initDB] 之后使用。
  Database get db => _db;

  DBManager._internal();

  static const int _version = 1;
  static const String _dbName = 'sifli_companion.db';

  /// 打开（或创建）数据库，并确保业务表存在。
  Future<void> initDB() async {
    _db = await openDatabase(
      join(await getDatabasesPath(), _dbName),
      version: _version,
      onCreate: (db, version) async {
        final batch = db.batch();
        batch.execute(_createBleDeviceSql);
        batch.execute(_createRideSessionSql);
        batch.execute(_createKvBackupSql);
        await batch.commit();
      },
      onUpgrade: (db, oldVersion, newVersion) async {
        // 后续 schema 升级写在这里。
      },
    );
    await _ensureTablesExist();
  }

  /// 兼容旧库：缺表时补建。
  Future<void> _ensureTablesExist() async {
    final tables = <MapEntry<String, String>>[
      MapEntry('ble_device', _createBleDeviceSql),
      MapEntry('ride_session', _createRideSessionSql),
      MapEntry('kv_backup', _createKvBackupSql),
    ];

    for (final entry in tables) {
      try {
        await _db.query(entry.key, limit: 1);
      } catch (_) {
        await _db.execute(entry.value);
      }
    }
  }

  static const String _createBleDeviceSql = '''
CREATE TABLE IF NOT EXISTS ble_device (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  device_id TEXT NOT NULL UNIQUE,
  name TEXT,
  last_connected_at INTEGER,
  extra TEXT
)
''';

  static const String _createRideSessionSql = '''
CREATE TABLE IF NOT EXISTS ride_session (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  started_at INTEGER NOT NULL,
  ended_at INTEGER,
  distance_m REAL,
  duration_s INTEGER,
  gpx_path TEXT,
  device_id TEXT
)
''';

  static const String _createKvBackupSql = '''
CREATE TABLE IF NOT EXISTS kv_backup (
  key TEXT PRIMARY KEY,
  value TEXT,
  updated_at INTEGER
)
''';

  /// 读取 DB 持久化自测计数；无记录时返回 `0`。
  Future<int> getPersistTestCounter() async {
    final rows = await _db.query(
      'kv_backup',
      columns: ['value'],
      where: 'key = ?',
      whereArgs: [AppKeys.persistDbCounterKey],
      limit: 1,
    );
    if (rows.isEmpty) return 0;
    return int.tryParse(rows.first['value'] as String? ?? '') ?? 0;
  }

  /// 读取 DB 持久化自测上次写入时间（毫秒时间戳）；无记录时返回 `null`。
  Future<int?> getPersistTestUpdatedAt() async {
    final rows = await _db.query(
      'kv_backup',
      columns: ['updated_at'],
      where: 'key = ?',
      whereArgs: [AppKeys.persistDbCounterKey],
      limit: 1,
    );
    if (rows.isEmpty) return null;
    return rows.first['updated_at'] as int?;
  }

  /// 将 DB 自测计数 +1 并写回，返回新值。
  Future<int> bumpPersistTestCounter() async {
    final next = (await getPersistTestCounter()) + 1;
    final now = DateTime.now().millisecondsSinceEpoch;
    await _db.insert(
      'kv_backup',
      {
        'key': AppKeys.persistDbCounterKey,
        'value': '$next',
        'updated_at': now,
      },
      conflictAlgorithm: ConflictAlgorithm.replace,
    );
    return next;
  }
}

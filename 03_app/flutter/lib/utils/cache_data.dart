import 'package:shared_preferences/shared_preferences.dart';
import 'package:sifli_companion/app/app_keys.dart';

/// KV 存储封装（[SharedPreferencesAsync]）。
///
/// 启动时由 [initInfo] 加载必要缓存（如语言环境）。
class CacheData {
  /// 返回单例。
  factory CacheData() => _instance;

  static final CacheData _instance = CacheData._internal();

  /// 与 [CacheData.new] 相同的单例入口。
  static CacheData get instance => _instance;

  SharedPreferencesAsync? _preferences;

  SharedPreferencesAsync get _prefs {
    return _preferences ??= SharedPreferencesAsync();
  }

  String _localeDesc = AppKeys.defaultLocale;
  bool _agreed = false;

  /// 当前语言环境描述（内存缓存，启动时从 KV 加载）。
  String get localeDesc => _localeDesc;

  /// 是否已同意首次启动协议。
  bool get hasAgreed => _agreed;

  CacheData._internal();

  /// 在 UI / 业务读取前初始化 KV（例如语言环境）。
  Future<void> initInfo() async {
    await _initLocaleDesc();
    await _initAgree();
  }

  Future<void> _initLocaleDesc() async {
    _localeDesc =
        await _prefs.getString(AppKeys.localeKey) ?? AppKeys.defaultLocale;
  }

  Future<void> _initAgree() async {
    _agreed = await _prefs.getBool(AppKeys.agreeKey) ?? false;
  }

  /// 设置并持久化语言环境。
  Future<void> setLocaleDesc(String locale) async {
    _localeDesc = locale;
    await setString(AppKeys.localeKey, locale);
  }

  /// 设置并持久化协议同意状态。
  Future<void> setAgreed(bool value) async {
    _agreed = value;
    await setBool(AppKeys.agreeKey, value);
  }

  /// 写入字符串。
  Future<void> setString(String key, String value) =>
      _prefs.setString(key, value);

  /// 写入整数。
  Future<void> setInt(String key, int value) => _prefs.setInt(key, value);

  /// 写入浮点数。
  Future<void> setDouble(String key, double value) =>
      _prefs.setDouble(key, value);

  /// 写入布尔值。
  Future<void> setBool(String key, bool value) => _prefs.setBool(key, value);

  /// 写入字符串列表。
  Future<void> setStringList(String key, List<String> value) =>
      _prefs.setStringList(key, value);

  /// 读取字符串；不存在时返回 `null`。
  Future<String?> getString(String key) => _prefs.getString(key);

  /// 读取整数；不存在时返回 `null`。
  Future<int?> getInt(String key) => _prefs.getInt(key);

  /// 读取浮点数；不存在时返回 `null`。
  Future<double?> getDouble(String key) => _prefs.getDouble(key);

  /// 读取布尔值；不存在时返回 `null`。
  Future<bool?> getBool(String key) => _prefs.getBool(key);

  /// 读取字符串列表；不存在时返回 `null`。
  Future<List<String>?> getStringList(String key) =>
      _prefs.getStringList(key);

  /// 删除指定键。
  Future<void> remove(String key) => _prefs.remove(key);

  /// 清空全部 KV。
  Future<void> clear() => _prefs.clear();
}

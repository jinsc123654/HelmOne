/// 应用级常量与 SharedPreferences / 持久化测试用的键名。
class AppKeys {
  /// 应用显示名称。
  static const String appName = 'Helm One';

  /// Android `applicationId` / OSM user-agent。
  static const String androidPackage = 'top.jinsc.helm_one';

  /// 默认语言环境描述，例如 `zh-CN`。
  static const String defaultLocale = 'zh-CN';

  /// 语言环境在 KV 中的键。
  static const String localeKey = 'locale';

  /// 最近连接的 BLE 设备 ID。
  static const String lastBleDeviceIdKey = 'connected_ble_device';

  /// 用户协议是否同意。
  static const String agreeKey = 'agree';

  /// KV 持久化自测：计数器键（点击 +1 → 杀进程 → 再启动验证）。
  static const String persistKvCounterKey = 'persist_test.kv_counter';

  /// KV 持久化自测：上次写入时间字符串。
  static const String persistKvUpdatedAtKey = 'persist_test.kv_updated_at';

  /// DB 持久化自测：在 `kv_backup` 表中的键名。
  static const String persistDbCounterKey = 'persist_test.db_counter';

  /// 是否把系统通知转发给码表。
  static const String notifForwardKey = 'companion.notif_forward';

  /// 是否后台保持连接（设置页「保持连接」开关）。
  ///
  /// 关掉后：退出/被杀之后不再自启动去连码表，也不拉后台前台服务。
  static const String keepAliveKey = 'companion.keep_alive';

  /// 周规划：每周骑行次数。
  static const String weekGoalRidesKey = 'ride.week_goal_rides';

  /// 周规划：每周骑行时长（分钟）。
  static const String weekGoalMinutesKey = 'ride.week_goal_minutes';

  /// 周规划：每周骑行里程（km）。
  static const String weekGoalKmKey = 'ride.week_goal_km';
}

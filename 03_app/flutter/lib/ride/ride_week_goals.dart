import 'package:sifli_companion/app/app_keys.dart';
import 'package:sifli_companion/utils/cache_data.dart';

/// 本周三环对照的周规划目标：次数、时长、里程。
class RideWeekGoals {
  const RideWeekGoals({
    this.rides = 4,
    this.minutes = 300,
    this.km = 50,
  });

  static const RideWeekGoals defaults = RideWeekGoals();

  final int rides;
  final int minutes;
  final double km;

  double get hours => minutes / 60.0;

  String get hoursText {
    if (minutes % 60 == 0) return '${minutes ~/ 60}';
    return (minutes / 60.0).toStringAsFixed(1);
  }

  static double progress(num value, num goal) {
    if (goal <= 0) return 0;
    return (value / goal).clamp(0.0, 1.0);
  }

  static String formatMinutes(int minutes) {
    if (minutes <= 0) return '0';
    if (minutes % 60 == 0) return '${minutes ~/ 60}h';
    final h = minutes ~/ 60;
    final m = minutes % 60;
    if (h == 0) return '${m}m';
    return '$h:${m.toString().padLeft(2, '0')}';
  }

  static String formatSeconds(int sec) {
    if (sec <= 0) return '0';
    return formatMinutes(sec ~/ 60);
  }

  RideWeekGoals clamped() => RideWeekGoals(
        rides: rides.clamp(1, 99),
        minutes: minutes.clamp(15, 10080),
        km: km.clamp(1, 2000),
      );

  /// 把「小时」输入转成分钟；无法解析时用 [fallbackMinutes]。
  static int minutesFromHoursText(String raw, {int fallbackMinutes = 300}) {
    final h = double.tryParse(raw.replaceAll(',', '.').trim());
    if (h == null) return fallbackMinutes.clamp(15, 10080);
    return (h * 60).round().clamp(15, 10080);
  }

  static Future<RideWeekGoals> load() async {
    final cache = CacheData();
    final rides = await cache.getInt(AppKeys.weekGoalRidesKey);
    final minutes = await cache.getInt(AppKeys.weekGoalMinutesKey);
    final km = await cache.getDouble(AppKeys.weekGoalKmKey);
    return RideWeekGoals(
      rides: rides ?? defaults.rides,
      minutes: minutes ?? defaults.minutes,
      km: km ?? defaults.km,
    ).clamped();
  }

  Future<void> save() async {
    final next = clamped();
    final cache = CacheData();
    await cache.setInt(AppKeys.weekGoalRidesKey, next.rides);
    await cache.setInt(AppKeys.weekGoalMinutesKey, next.minutes);
    await cache.setDouble(AppKeys.weekGoalKmKey, next.km);
  }
}

import 'package:flutter_test/flutter_test.dart';
import 'package:sifli_companion/app/app_keys.dart';

void main() {
  test('app keys are defined', () {
    expect(AppKeys.appName, 'Helm One');
    expect(AppKeys.defaultLocale, 'zh-CN');
  });
}

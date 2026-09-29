import 'package:flutter_test/flutter_test.dart';
import 'package:sifli_companion/ble/companion_proto.dart';
import 'package:sifli_companion/cache/app_cache_store.dart';

void main() {
  test('iconFileName is basename only', () {
    expect(CompanionNotif.iconFileName('com.tencent.mm'), 'com.tencent.mm.png');
    expect(CompanionNotif.iconFileName('a/b'), 'a_b.png');
  });

  test('package and icon names are length-capped', () {
    final long = 'com.${'a' * 80}.app';
    expect(CompanionNotif.sanitizePackage(long).length,
        CompanionNotif.packageMax);
    final icon = CompanionNotif.iconFileName(long);
    expect(icon.length, lessThanOrEqualTo(CompanionNotif.iconNameMax));
    expect(icon.endsWith('.png'), isTrue);
    expect(
      CompanionNotif.wireName('${'x' * 120}.png').length,
      CompanionNotif.iconNameMax,
    );
  });

  test('wireName strips directories and rejects dots', () {
    expect(CompanionNotif.wireName('notif_icons/com.foo.png'), 'com.foo.png');
    expect(CompanionNotif.wireName('com.foo.png'), 'com.foo.png');
    expect(CompanionNotif.wireName(r'a\b\c.png'), 'c.png');
    expect(CompanionNotif.wireName('../x.png'), 'x.png');
    expect(CompanionNotif.wireName('..'), '');
    expect(CompanionNotif.wireName('.'), '');
    expect(CompanionNotif.wireName(''), '');
  });

  test('iconFsPath is device-relative dest for NEED upload', () {
    expect(
      CompanionNotif.iconFsPath('com.foo.png'),
      'notif_icons/com.foo.png',
    );
    expect(CompanionNotif.iconFsPath('notif_icons/com.foo.png'),
        'notif_icons/com.foo.png');
  });

  test('AppCacheStore relative paths reject escape', () {
    expect(AppCacheStore.wirePath('notif_icons'), 'notif_icons');
    expect(AppCacheStore.wirePath('/notif_icons/a.png'), 'notif_icons/a.png');
    expect(AppCacheStore.wirePath('../etc'), '');
    expect(AppCacheStore.wirePath('a/../b'), '');
    expect(AppCacheStore.joinRel('', 'notif_icons'), 'notif_icons');
    expect(AppCacheStore.joinRel('notif_icons', 'a.png'), 'notif_icons/a.png');
    expect(AppCacheStore.parentRel('notif_icons/a.png'), 'notif_icons');
    expect(AppCacheStore.parentRel('notif_icons'), '');
    expect(AppCacheStore.displayPath(''), '/');
    expect(AppCacheStore.displayPath('notif_icons'), '/notif_icons');
    expect(AppCacheStore.sanitizeName('..'), '');
    expect(AppCacheStore.sanitizeName('a/b'), '');
  });
}

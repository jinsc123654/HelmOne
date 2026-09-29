/// Helm One 云端 OTA 入口。
///
/// 清单：`GET /ota/helm-one/firmware.json?token=…&hw=`
/// 固件槽：BLE 相对路径 `fw/<X.Y.Z-Helm-One>.bin` → 设备 `/mnt/kv/fw`。
abstract class OtaConfig {
  static const host = 'ota.jinsc.top';
  static const productPath = '/ota/helm-one/firmware.json';
  static const token = 'wkXudavMke95y-s67FrXKdxhjPutlbD9';
  static const defaultHardware = '1.0.0';

  /// 写入码表时的文件名中缀，与 `pack-fw` 产出 `1.0.0-Helm-One.bin` 一致。
  static const fileStem = 'Helm-One';

  static Uri manifestUri({String hardware = defaultHardware}) => Uri.https(
        host,
        productPath,
        {'token': token, 'hw': hardware},
      );

  /// 清单里的 bin 地址常常是 http；同主机改走 https。
  static Uri preferHttps(Uri uri) {
    if (uri.scheme == 'http' && uri.host == host) {
      return uri.replace(scheme: 'https');
    }
    return uri;
  }
}

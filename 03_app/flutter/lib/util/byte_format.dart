/// 字节数 → 人读文本。
///
/// 缓存页、码表文件管理、日志列表都要显示大小，之前每页各抄一份
/// `_sizeLabel`，位数还不一致（有的 KB 保留 0 位、有的 1 位）。统一到这里。
///
/// 千进制（不是 KiB）：`1536` → `1.5 KB`。日志/文件大小是给人看的量级参考，
/// 不是要参与运算的数，跟设备侧和文件管理器显示一致更好认。
String formatBytes(int n) {
  if (n < 0) return '0 B';
  if (n < 1024) return '$n B';
  if (n < 1024 * 1024) return '${(n / 1024).toStringAsFixed(1)} KB';
  if (n < 1024 * 1024 * 1024) {
    return '${(n / (1024 * 1024)).toStringAsFixed(1)} MB';
  }
  return '${(n / (1024 * 1024 * 1024)).toStringAsFixed(2)} GB';
}

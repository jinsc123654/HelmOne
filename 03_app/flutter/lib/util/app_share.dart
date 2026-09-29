import 'package:share_plus_platform_interface/share_plus_platform_interface.dart';
import 'package:sifli_companion/ble/companion_foreground_service.dart';
import 'package:sifli_companion/log/app_log.dart';

/// 包住真正的 share 实现：转发，并在**用户选中目标之后**问一句
/// 「回到码表 / 留在当前应用」（原生侧那条通知，见 `ShareReturnNotifier`）。
class _AskAfterShare extends SharePlatform {
  _AskAfterShare(this._inner);

  final SharePlatform _inner;

  @override
  Future<ShareResult> share(ShareParams params) async {
    /* 加一层超时：MainActivity 给选择器补了 `FLAG_ACTIVITY_NEW_TASK`（见那里的说明），
     * 而"带结果启动 + 新任务"时系统**不再把 onActivityResult 送回来**（框架会打
     * "launching as a new task, so cancelling activity result"）。现代 share_plus 主要靠
     * 选择器里的 `EXTRA_CHOSEN_COMPONENT_INTENT_SENDER` 广播回结果，与本条无关；这里
     * 只是兜底——万一那条通道也没回来，不能让调用方的 `await` 永久挂住。 */
    final result = await _inner.share(params).timeout(
          const Duration(seconds: 90),
          onTimeout: () {
            AppLog.w('Share', '等分享结果超时（90 s），按"未选中"处理');
            return ShareResult.unavailable;
          },
        );

    if (result.status == ShareResultStatus.success) {
      await CompanionForegroundService.notifyShared();
    }

    return result;
  }
}

/// 装上「分享后询问」。装一次即覆盖**所有** `SharePlus.instance.share(...)` 调用点
/// （包括以后新加的），所以各页面不需要各自记得调用 —— 这是选装饰器而不是包一层
/// `appShare()` 的原因。
///
/// **必须在任何代码读 `SharePlus.instance` 之前调用**：它是
/// `static final SharePlus._(SharePlatform.instance)` —— 也就是**第一次被读到时**
/// 把当时的实现拷了下来，装晚了就绕过去了。所以调用点放在 `main()` 最前面。
///
/// 为什么要问这一句：share_plus 用 `Activity.startActivity`（不带
/// `FLAG_ACTIVITY_NEW_TASK`）启动选择器 ⇒ 选择器和目标应用都落进**本 App 的任务**；
/// 而微信这类应用"返回"是把整个任务压到后台 ⇒ 按返回一路回到桌面、最近任务卡被顶成
/// "标题 Helm One、顶上却是微信"（2026-09-27 用 dumpsys + logcat 定位）。
/// 没有应用内对话框可用：分享成功那一刻前台是目标应用、我们在后台，Android 10+ 限制
/// 后台启动 Activity ⇒ 只能走通知。
///
/// 时机说明：`ShareResultStatus.success` 的含义是**用户选中了目标**（微信不会告诉我们
/// 是否真的发送出去了），所以这句询问出现在"选中之后"，不是"发送完"。
Future<void> installShareAsk() async {
  // 幂等：热重启、以及"进程被后台服务拉起"那条路径都会再跑一次 main()。
  if (SharePlatform.instance is _AskAfterShare) return;

  SharePlatform.instance = _AskAfterShare(SharePlatform.instance);
  await AppLog.i('Share', '已装「分享后询问」（回到码表 / 留在当前应用）');
}

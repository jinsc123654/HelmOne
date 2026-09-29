package com.sifli.sifli_companion

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.util.Log

/**
 * 分享完之后那条"问一句"的通知：**回到码表 / 留在当前应用**。
 *
 * ## 为什么是通知，而不是应用内对话框
 *
 * 分享成功那一刻前台是**目标应用**、我们自己的 App 在后台。Android 10+ 限制后台启动
 * Activity，弹不出对话框；通知是唯一稳定的通道（App 本来就有前台服务与通知机制）。
 *
 * ## 为什么需要这一句
 *
 * 分享会把选择器和目标应用都放进**本 App 的任务**里（share_plus 用 `Activity.startActivity`
 * 启动选择器、不带 `FLAG_ACTIVITY_NEW_TASK`），而微信这类应用"返回"时是**把整个任务压到
 * 后台**、并不结束自己 —— 于是按返回会一路回到桌面，最近任务卡也被顶成"标题 Helm One、
 * 顶上却是微信"。这条通知给出一条明确的路：点「回到码表」用
 * `NEW_TASK | CLEAR_TOP` 把我们自己的任务提到前台，不管它被压在几层下面。
 *
 * ## 时机
 *
 * `share_plus` 的"成功"= 用户**选中了目标**（微信不会告诉我们是否真的发送出去了），
 * 所以这条通知出现在"选中之后"，不是"发送完"。
 */
internal object ShareReturnNotifier {

    private const val TAG = "ShareReturn"

    private const val CHANNEL_ID = "share_return"

    /** 与前台服务那条（`companion_fgs`）分开：那条是常驻的，这条是一次性的。 */
    private const val NOTIFY_ID = 0x5757

    /** 没人搭理就自己收掉，免得在通知栏里留垃圾。 */
    private const val TIMEOUT_MS = 30_000L

    /** 「留在当前应用」这个按钮只把这个通知收掉。 */
    internal const val ACTION_STAY = "com.sifli.sifli_companion.SHARE_STAY"

    fun show(context: Context) {
        val app = context.applicationContext

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
            app.checkSelfPermission("android.permission.POST_NOTIFICATIONS") !=
            PackageManager.PERMISSION_GRANTED
        ) {
            Log.i(TAG, "没有通知权限，跳过")
            return
        }

        ensureChannel(app)

        // 「回到码表」：把自己的任务提到前台。
        // **NEW_TASK + REORDER_TO_FRONT，不是 CLEAR_TOP**：REORDER 只把我们的界面提到该任务
        // 的最前面，**不会结束压在它上面的别人**。用 CLEAR_TOP 会把压在我们任务里的微信界面
        // 直接 finish 掉 —— 那会让微信丢状态（2026-09-27 认领的坑）。
        val backIntent = Intent(app, MainActivity::class.java).apply {
            flags = Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_REORDER_TO_FRONT
        }
        val backPi = PendingIntent.getActivity(app, 0, backIntent, piFlags())

        val stayPi = PendingIntent.getBroadcast(
            /* 显式组件（不用 intent-filter）：exported=false 的 receiver 配隐式 action
             * 在 Android 12+ 上会被"必须显式"的规则绊住，而且 PendingIntent 本来就
             * 是按创建者身份执行的，不需要对系统暴露入口。 */
            app, 1, Intent(app, ShareStayReceiver::class.java).setAction(ACTION_STAY),
            piFlags(),
        )

        val notification = Notification.Builder(app, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_companion_bike)
            .setContentTitle("已分享")
            .setContentText("要回到码表，还是留在当前应用？")
            // 点正文 = 回码表（和左边那个按钮同一个动作）。
            .setContentIntent(backPi)
            .setAutoCancel(true)
            .setTimeoutAfter(TIMEOUT_MS)
            /* 用 `(int icon, …)` 这个重载（icon=0 = 不带图标）：API 23 那个
             * `(Icon?, …)` 重载传 null 会有重载歧义、编不过。 */
            .addAction(Notification.Action.Builder(0, "回到码表", backPi).build())
            .addAction(Notification.Action.Builder(0, "留在当前应用", stayPi).build())
            .build()

        (app.getSystemService(Context.NOTIFICATION_SERVICE) as? NotificationManager)
            ?.notify(NOTIFY_ID, notification)
        Log.i(TAG, "已发「已分享」通知（30 s 自动消失）")
    }

    internal fun cancel(context: Context) {
        (context.applicationContext
            .getSystemService(Context.NOTIFICATION_SERVICE) as? NotificationManager)
            ?.cancel(NOTIFY_ID)
    }

    private fun ensureChannel(app: Context) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return

        val channel = NotificationChannel(
            CHANNEL_ID,
            "分享后询问",
            /* DEFAULT：这条**是要用户看见的**（与前台服务那条 MIN 相反），
             * 但不要求它响 —— 分享刚结束，别再加一声提示音。 */
            NotificationManager.IMPORTANCE_DEFAULT,
        ).apply {
            description = "分享后提供「回到码表 / 留在当前应用」两个选择"
            setShowBadge(false)
            enableVibration(false)
            setSound(null, null)
        }

        (app.getSystemService(Context.NOTIFICATION_SERVICE) as? NotificationManager)
            ?.createNotificationChannel(channel)
    }

    private fun piFlags(): Int =
        PendingIntent.FLAG_UPDATE_CURRENT or
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
                PendingIntent.FLAG_IMMUTABLE
            } else {
                0
            }
}

/** 「留在当前应用」：什么都不做，只把那条通知收掉（必须声明在 manifest 里）。 */
class ShareStayReceiver : BroadcastReceiver() {

    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action == ShareReturnNotifier.ACTION_STAY) {
            ShareReturnNotifier.cancel(context)
        }
    }
}

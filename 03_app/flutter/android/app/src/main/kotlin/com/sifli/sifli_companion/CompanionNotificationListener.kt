package com.sifli.sifli_companion

import android.app.Notification
import android.content.ComponentName
import android.os.Build
import android.os.SystemClock
import android.service.notification.NotificationListenerService
import android.service.notification.StatusBarNotification
import android.util.Log

/**
 * System notification listener.  Forwards posted notifications to
 * [CompanionNotifPlugin] on the Flutter engine thread.
 */
class CompanionNotificationListener : NotificationListenerService() {

    override fun onListenerConnected() {
        super.onListenerConnected()
        instance = this
        connected = true
        Log.i(TAG, "listener connected")
        CompanionNotifPlugin.emitStatus("connected")

        /* 进程可能只是"为了通知监听"被系统拉起来的 —— 用户从最近任务划掉 App 之后就是
         * 这样：系统 1 s 内把监听绑回来，而前台服务那一步会被拖很久（实测 100 s 未投递）。
         * 那种启动没有 Activity ⇒ 没有 FlutterEngine ⇒ Dart 侧（连接逻辑所在）不会运行，
         * 表现就是"起来了但没连码表"。已配对过才建引擎，理由见 [CompanionEngine]。 */
        CompanionEngine.ensureStartedIfPaired(applicationContext)
    }

    override fun onListenerDisconnected() {
        super.onListenerDisconnected()
        connected = false
        instance = null
        Log.w(TAG, "listener disconnected, request rebind")
        CompanionNotifPlugin.emitStatus("disconnected")
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
            requestRebind(
                ComponentName(this, CompanionNotificationListener::class.java),
            )
        }
    }

    override fun onNotificationPosted(sbn: StatusBarNotification?) {
        if (sbn == null) return
        handlePosted(sbn)
    }

    fun replayActive(limit: Int = 8) {
        val list = try {
            activeNotifications
        } catch (e: Exception) {
            Log.w(TAG, "activeNotifications failed: ${e.message}")
            return
        } ?: return
        val keep = list.filter { sbn ->
            skipReason(sbn, NotifParse.extract(sbn.notification)) == null
        }.sortedBy { it.postTime }.takeLast(limit)
        Log.i(TAG, "replay active=${list.size} forward=${keep.size}")
        for (sbn in keep) {
            handlePosted(sbn)
        }
    }

    private fun handlePosted(sbn: StatusBarNotification) {
        val parsed = NotifParse.extract(sbn.notification)
        val reason = skipReason(sbn, parsed)
        if (reason != null) {
            // 过滤是**常态**（bilibili 这类推送带 CATEGORY_TRANSPORT，按设计就该滤掉），
            // 一分钟能来几十条。逐条打印、还带通知正文，会把 logcat 冲干净 ——
            // 这里只累计计数，满 SKIP_FLUSH_MS 汇总一行（不含正文）。
            noteSkip(reason)
            if (reason != SKIP_FGS && reason != SKIP_SYSTEM) {
                CompanionNotifPlugin.emitSkipped(
                    sbn.packageName,
                    appLabel(sbn.packageName),
                    reason,
                    parsed.title,
                    parsed.text,
                )
            }
            return
        }

        if (sameAsLast(sbn, parsed)) {
            Log.i(TAG, "dup pkg=${sbn.packageName} id=${sbn.id}")
            return
        }

        flushSkips()
        Log.i(
            TAG,
            "post pkg=${sbn.packageName} id=${sbn.id} " +
                "title=${parsed.title} text=${parsed.text}",
        )
        CompanionNotifPlugin.emitPosted(
            sbn,
            appLabel(sbn.packageName),
            parsed.title,
            parsed.text,
        )
    }

    /**
     * @return skip reason, or null to forward.
     */
    private fun skipReason(sbn: StatusBarNotification, parsed: NotifParse.Text): String? {
        val n = sbn.notification
        if (n.flags and Notification.FLAG_FOREGROUND_SERVICE != 0) {
            return SKIP_FGS
        }
        if (n.category == Notification.CATEGORY_TRANSPORT) {
            return "transport"
        }
        val pkg = sbn.packageName ?: return SKIP_SYSTEM
        if (pkg == "android" || pkg.startsWith("com.android.")) {
            return SKIP_SYSTEM
        }
        if (pkg == packageName &&
            sbn.id != CompanionNotifPlugin.TEST_NOTIFICATION_ID
        ) {
            return "self"
        }
        // QQ 常把会话做成 group summary；只有子通知已有正文时才丢掉摘要，避免整组被滤掉。
        if (n.flags and Notification.FLAG_GROUP_SUMMARY != 0 &&
            groupHasChildWithText(sbn)
        ) {
            return "group_summary"
        }
        if (parsed.isBlank) {
            return "empty"
        }
        return null
    }

    private fun groupHasChildWithText(sbn: StatusBarNotification): Boolean {
        val group = sbn.groupKey ?: return false
        val active = try {
            activeNotifications
        } catch (_: Exception) {
            return false
        } ?: return false
        return active.any { other ->
            other.key != sbn.key &&
                other.groupKey == group &&
                (other.notification.flags and Notification.FLAG_GROUP_SUMMARY == 0) &&
                !NotifParse.extract(other.notification).isBlank
        }
    }

    /** 记一条被过滤的通知；窗口到了就汇总一行。 */
    private fun noteSkip(reason: String) {
        skipCounts[reason] = (skipCounts[reason] ?: 0) + 1
        skipTotal++
        val now = SystemClock.elapsedRealtime()
        if (skipFlushAt == 0L) {
            skipFlushAt = now + SKIP_FLUSH_MS
        } else if (now >= skipFlushAt) {
            flushSkips()
        }
    }

    /** 把窗口内被过滤的条数汇总成一行（只有计数和原因，没有正文）。 */
    private fun flushSkips() {
        if (skipTotal == 0) return
        val detail = skipCounts.entries.joinToString(", ") { "${it.key} ${it.value}" }
        Log.i(TAG, "skip $skipTotal ($detail)")
        skipCounts.clear()
        skipTotal = 0
        skipFlushAt = 0L
    }

    private fun appLabel(pkg: String): String = try {
        val info = packageManager.getApplicationInfo(pkg, 0)
        packageManager.getApplicationLabel(info).toString()
    } catch (_: Exception) {
        pkg
    }

    private fun sameAsLast(sbn: StatusBarNotification, parsed: NotifParse.Text): Boolean {
        val finger = "${parsed.title}\n${parsed.text}"
        val key = sbn.key
        if (lastFinger[key] == finger) return true
        lastFinger.remove(key)
        lastFinger[key] = finger
        while (lastFinger.size > FINGER_MAX) {
            val oldest = lastFinger.keys.first()
            lastFinger.remove(oldest)
        }
        return false
    }

    companion object {
        private const val TAG = "CompanionNotif"
        const val SKIP_FGS = "fgs"
        const val SKIP_SYSTEM = "system"
        private const val FINGER_MAX = 64
        private const val SKIP_FLUSH_MS = 30_000L

        @Volatile
        var instance: CompanionNotificationListener? = null

        @Volatile
        var connected: Boolean = false
    }

    private val lastFinger = LinkedHashMap<String, String>()
    private val skipCounts = LinkedHashMap<String, Int>()
    private var skipTotal = 0
    private var skipFlushAt = 0L
}

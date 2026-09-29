package com.sifli.sifli_companion

import android.app.KeyguardManager
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.pm.ResolveInfo
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.drawable.BitmapDrawable
import android.graphics.drawable.Drawable
import android.content.pm.PackageManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import android.provider.Settings
import android.service.notification.NotificationListenerService
import android.service.notification.StatusBarNotification
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.EventChannel
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel
import java.io.ByteArrayOutputStream

/**
 * Android notification listener bridge for Companion 0xFF17.
 *
 * The system-bound [CompanionNotificationListener] posts here; Dart
 * caches icons locally by MD5 and forwards title/body plus the icon
 * filename over GATT.  The device pulls a missing file with FS_NEED.
 */
class CompanionNotifPlugin(
    private val context: Context,
) : MethodChannel.MethodCallHandler, EventChannel.StreamHandler {

    companion object {
        const val METHOD_CHANNEL = "com.sifli.sifli_companion/notif"
        const val EVENT_CHANNEL = "com.sifli.sifli_companion/notif_events"
        const val TEST_NOTIFICATION_ID = 9101
        private const val TEST_CHANNEL_ID = "companion_notif_test"
        private const val ICON_PX = 48

        @Volatile
        private var instance: CompanionNotifPlugin? = null

        private const val PENDING_MAX = 32
        private val pending = ArrayDeque<Map<String, Any>>()

        fun register(engine: FlutterEngine, context: Context) {
            val plugin = CompanionNotifPlugin(context.applicationContext)
            instance = plugin
            val messenger = engine.dartExecutor.binaryMessenger
            MethodChannel(messenger, METHOD_CHANNEL).setMethodCallHandler(plugin)
            EventChannel(messenger, EVENT_CHANNEL).setStreamHandler(plugin)
            plugin.flushPending()
        }

        fun emitStatus(status: String) {
            dispatch(
                hashMapOf("status" to status),
                queueIfNoSink = false,
            )
        }

        fun emitPosted(
            sbn: StatusBarNotification,
            appName: String,
            title: String,
            text: String,
        ) {
            dispatch(
                hashMapOf(
                    "package" to sbn.packageName,
                    "appName" to appName,
                    "title" to title,
                    "text" to text,
                    "id" to sbn.id,
                    "key" to sbn.key,
                    "postTime" to sbn.postTime,
                ),
                queueIfNoSink = true,
            )
        }

        fun emitSkipped(
            pkg: String,
            appName: String,
            reason: String,
            title: String,
            text: String,
        ) {
            dispatch(
                hashMapOf(
                    "skipped" to true,
                    "reason" to reason,
                    "package" to pkg,
                    "appName" to appName,
                    "title" to title,
                    "text" to text,
                ),
                queueIfNoSink = false,
            )
        }

        private fun dispatch(map: Map<String, Any>, queueIfNoSink: Boolean) {
            val plugin = instance
            if (plugin != null) {
                plugin.enqueueOrSend(map, queueIfNoSink)
            } else if (queueIfNoSink) {
                synchronized(pending) {
                    if (pending.size >= PENDING_MAX) pending.removeFirst()
                    pending.addLast(map)
                }
            }
        }
    }

    private val main = Handler(Looper.getMainLooper())
    private var events: EventChannel.EventSink? = null

    override fun onMethodCall(call: MethodCall, result: MethodChannel.Result) {
        when (call.method) {
            "isEnabled" -> result.success(isListenerEnabled())
            "openSettings" -> {
                context.startActivity(
                    Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS)
                        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
                )
                result.success(true)
            }
            "getAppIcon" -> {
                val pkg = call.argument<String>("package")
                if (pkg.isNullOrBlank()) {
                    result.error("bad_args", "package required", null)
                    return
                }
                try {
                    result.success(pngForPackage(pkg))
                } catch (e: Exception) {
                    result.error("icon_failed", e.message, null)
                }
            }
            "ownPackage" -> result.success(context.packageName)
            "isBound" -> result.success(CompanionNotificationListener.connected)
            "requestRebind" -> {
                ensureBound()
                result.success(CompanionNotificationListener.connected)
            }
            "pullActive" -> {
                val n = CompanionNotificationListener.instance?.let {
                    it.replayActive()
                    1
                } ?: 0
                result.success(n)
            }
            "postTest" -> {
                postTestNotification(
                    call.argument<String>("title") ?: "测试通知",
                    call.argument<String>("text") ?: "发给码表的一条测试消息",
                )
                result.success(true)
            }
            "isPhoneLocked" -> result.success(isPhoneLocked())
            "listInstalledApps" -> {
                Thread {
                    try {
                        val list = listLauncherApps()
                        main.post { result.success(list) }
                    } catch (e: Exception) {
                        main.post {
                            result.error("list_failed", e.message, null)
                        }
                    }
                }.start()
            }
            else -> result.notImplemented()
        }
    }

    override fun onListen(arguments: Any?, events: EventChannel.EventSink?) {
        this.events = events
        flushPending()
    }

    override fun onCancel(arguments: Any?) {
        this.events = null
    }

    private fun enqueueOrSend(map: Map<String, Any>, queueIfNoSink: Boolean) {
        main.post {
            val sink = events
            if (sink != null) {
                sink.success(map)
            } else if (queueIfNoSink) {
                synchronized(pending) {
                    if (pending.size >= PENDING_MAX) pending.removeFirst()
                    pending.addLast(map)
                }
            }
        }
    }

    private fun flushPending() {
        main.post {
            val sink = events ?: return@post
            val batch = ArrayList<Map<String, Any>>()
            synchronized(pending) {
                batch.addAll(pending)
                pending.clear()
            }
            for (item in batch) {
                sink.success(item)
            }
        }
    }

    private fun isListenerEnabled(): Boolean {
        val cn = ComponentName(context, CompanionNotificationListener::class.java)
        val enabled = Settings.Secure.getString(
            context.contentResolver,
            "enabled_notification_listeners",
        ) ?: return false
        return enabled.split(':').any {
            ComponentName.unflattenFromString(it) == cn || it.contains(cn.flattenToString())
        }
    }

    private fun isPhoneLocked(): Boolean {
        val km = context.getSystemService(Context.KEYGUARD_SERVICE) as KeyguardManager
        if (km.isKeyguardLocked) return true
        val pm = context.getSystemService(Context.POWER_SERVICE) as PowerManager
        return !pm.isInteractive
    }

    /**
     * 小米等机型：设置里已授权，但安装/热更后服务未真正 bind。
     * 先 requestRebind，仍未连上则开关一次组件。
     */
    private fun ensureBound() {
        if (!isListenerEnabled()) return
        val cn = ComponentName(context, CompanionNotificationListener::class.java)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
            NotificationListenerService.requestRebind(cn)
        }
        if (CompanionNotificationListener.connected) return
        try {
            val pm = context.packageManager
            pm.setComponentEnabledSetting(
                cn,
                PackageManager.COMPONENT_ENABLED_STATE_DISABLED,
                PackageManager.DONT_KILL_APP,
            )
            pm.setComponentEnabledSetting(
                cn,
                PackageManager.COMPONENT_ENABLED_STATE_ENABLED,
                PackageManager.DONT_KILL_APP,
            )
        } catch (e: Exception) {
            android.util.Log.w("CompanionNotif", "bounce listener failed: ${e.message}")
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
            NotificationListenerService.requestRebind(cn)
        }
    }

    /**
     * 桌面可启动的应用（用户能看到的那些），按显示名排序。
     * 不含无启动器的系统包，避免列表里塞满服务进程。
     */
    private fun listLauncherApps(): List<Map<String, String>> {
        val pm = context.packageManager
        val intent = Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER)
        val resolved: List<ResolveInfo> = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            pm.queryIntentActivities(
                intent,
                PackageManager.ResolveInfoFlags.of(0),
            )
        } else {
            @Suppress("DEPRECATION")
            pm.queryIntentActivities(intent, 0)
        }
        val seen = HashSet<String>()
        val out = ArrayList<Map<String, String>>()
        for (ri in resolved) {
            val pkg = ri.activityInfo?.packageName ?: continue
            if (!seen.add(pkg)) continue
            val label = ri.loadLabel(pm).toString().ifBlank { pkg }
            out.add(
                hashMapOf(
                    "package" to pkg,
                    "appName" to label,
                ),
            )
        }
        out.sortBy { it["appName"]?.lowercase().orEmpty() }
        return out
    }

    private fun pngForPackage(pkg: String): ByteArray {
        val drawable = context.packageManager.getApplicationIcon(pkg)
        return drawableToPng(drawable)
    }

    private fun drawableToPng(drawable: Drawable): ByteArray {
        val bitmap = when (drawable) {
            is BitmapDrawable -> {
                val src = drawable.bitmap
                Bitmap.createScaledBitmap(src, ICON_PX, ICON_PX, true)
            }
            else -> {
                val bmp = Bitmap.createBitmap(ICON_PX, ICON_PX, Bitmap.Config.ARGB_8888)
                val canvas = Canvas(bmp)
                drawable.setBounds(0, 0, ICON_PX, ICON_PX)
                drawable.draw(canvas)
                bmp
            }
        }
        val out = ByteArrayOutputStream()
        bitmap.compress(Bitmap.CompressFormat.PNG, 100, out)
        return out.toByteArray()
    }

    private fun postTestNotification(title: String, text: String) {
        val nm = context.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            nm.createNotificationChannel(
                NotificationChannel(
                    TEST_CHANNEL_ID,
                    "Companion 测试通知",
                    NotificationManager.IMPORTANCE_DEFAULT,
                ),
            )
        }
        val builder = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            Notification.Builder(context, TEST_CHANNEL_ID)
        } else {
            @Suppress("DEPRECATION")
            Notification.Builder(context)
        }
        val notification = builder
            .setContentTitle(title)
            .setContentText(text)
            .setSmallIcon(android.R.drawable.ic_dialog_info)
            .setAutoCancel(true)
            .build()
        nm.notify(TEST_NOTIFICATION_ID, notification)
    }
}

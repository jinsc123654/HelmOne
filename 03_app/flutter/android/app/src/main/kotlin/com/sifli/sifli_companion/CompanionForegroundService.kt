package com.sifli.sifli_companion

import android.Manifest
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.bluetooth.BluetoothManager
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import android.util.Log

/**
 * 保活前台服务：骑行时手机在兜里、屏幕黑着，进程一旦被回收 BLE 连接即断开。
 *
 * 服务本身不碰蓝牙。它只负责让进程（以及其中的 Flutter 引擎与 Dart 侧的
 * CompanionClient）保持存活，蓝牙与定位逻辑仍在 Dart 层。
 *
 * 但"保持存活"有个前提：**进程里得真有引擎**。系统用 START_STICKY 重启本服务时只创建
 * 服务、不创建 Activity，Dart 侧就无从运行，于是成了"活着但不连接"。所以这里在启动时
 * 检查一次，没有引擎就通过 [CompanionEngine] 拉起来（见它的注释）。
 *
 * 规格见固件仓库 vendor/my_vendor/docs/ble/companion_impl_plan.md。
 */
class CompanionForegroundService : Service() {

    companion object {
        private const val TAG = "CompanionFgs"

        /**
         * 安静通道：IMPORTANCE_MIN = 不响、不震、**不占状态栏图标**，只在通知栏下拉后
         * 底部折叠区里待一行。前台服务必须挂一条通知，这是平台允许的最安静形态。
         */
        private const val CHANNEL_ID = "companion_link_quiet"

        /** 2026-09 之前用的通道（LOW）。通道重要性一旦建好，App 自己改不了，只能换 id。 */
        private const val LEGACY_CHANNEL_ID = "companion_link"

        private const val NOTIFICATION_ID = 1001

        const val ACTION_START = "com.sifli.sifli_companion.action.START"
        const val ACTION_STOP = "com.sifli.sifli_companion.action.STOP"
        const val ACTION_UPDATE = "com.sifli.sifli_companion.action.UPDATE"

        /** 用户手动清掉了常驻通知（见 [dismissed]）。 */
        const val ACTION_DISMISSED = "com.sifli.sifli_companion.action.DISMISSED"

        const val EXTRA_TEXT = "text"

        /**
         * 指令是不是 Dart 侧（活着的引擎）发来的。
         *
         * 系统重启本服务时会把最后一次的 Intent 连 extras 一起重放，里面那句文案是
         * **上一次**的状态；照搬就会让通知栏声称"已连接 · 后台同步中"，而此刻
         * `GATT Client Map` 是空的（2026-09-25 现场就是这么被误读的）。
         */
        const val EXTRA_FROM_DART = "from_dart"

        /** 供 Dart 侧查询，避免重复启动。 */
        @Volatile
        var isRunning: Boolean = false
            private set
    }

    private var contentText: String = "正在保持与码表的连接"

    /** 已配对码表的名字，懒查一次（见 [companionDisplayName]）。 */
    private var companionName: String? = null

    /**
     * 用户手动清掉了常驻通知（Android 14+ 允许这样而服务照跑：连接不断）。
     *
     * 记住它，本轮不要再把它刷回来 —— 状态每次变化都重新弹一条，等于跟用户对着干。
     * 只有"明确的一次启动"（Dart 侧 `start()`，例如又从 App 切到后台）或进程重生才会
     * 重新显示（见 [onStartCommand] 的 else 分支）。
     */
    private var dismissed = false

    override fun onCreate() {
        super.onCreate()
        createNotificationChannel()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        // 只有 Dart 侧发来的指令才认它带的那句文案（理由见 EXTRA_FROM_DART）。
        val fromDart = intent?.getBooleanExtra(EXTRA_FROM_DART, false) == true

        when (intent?.action) {
            ACTION_DISMISSED -> {
                dismissed = true
                Log.i(TAG, "常驻通知被用户清掉，本轮不再刷新")
                return START_STICKY
            }

            ACTION_STOP -> {
                stopSelfAndForeground()
                return START_NOT_STICKY
            }

            ACTION_UPDATE -> {
                if (fromDart) intent.getStringExtra(EXTRA_TEXT)?.let { contentText = it }
                if (isRunning) {
                    if (!dismissed) {
                        notificationManager()?.notify(NOTIFICATION_ID, buildNotification())
                    }
                    return START_STICKY
                }
                // 还没起来就当作一次启动处理。
            }

            else -> {
                // 一次明确的启动 = 新一轮会话：用户先前清掉的那条允许重新出现。
                dismissed = false
                if (fromDart) intent?.getStringExtra(EXTRA_TEXT)?.let { contentText = it }
            }
        }

        startInForeground()

        /* 这条路径也可能是"系统为了本服务把进程重建"（START_STICKY）：它不创建 Activity，
         * 也就没有 FlutterEngine —— 而连接逻辑全在 Dart 侧，光有服务等于"活着但不连接"。
         * 所以缺引擎就补一份，剩下的（连码表、刷新通知栏）交给 Dart。 */
        if (isRunning && CompanionEngine.engineOrNull() == null) {
            try {
                CompanionEngine.start(this)
            } catch (e: Exception) {
                Log.e(TAG, "后台拉起 FlutterEngine 失败: ${e.message}", e)
            }
        }

        return START_STICKY
    }

    private fun startInForeground() {
        /* 逐个类型试：先按已授予的权限给全集，被系统拒了再退到"只保连接"。
         *
         * 为什么必须退：Android 14+ 规定**从后台启动**的前台服务不能用 location/camera/
         * microphone 这些"仅前台"类型。而本服务最常见的重生方式恰恰是系统从后台重启
         * （用户杀掉 App、进程被 START_STICKY 拉回来）—— 那时带上 location 会被
         * SecurityException 顶掉，服务 stopSelf 直接死掉，连"保活"都做不到。
         * 2026-09-25 实测原文："Foreground service started from background can not have
         * location/camera/microphone access"。 */
        for (type in serviceTypeCandidates()) {
            try {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                    startForeground(NOTIFICATION_ID, buildNotification(), type)
                } else {
                    startForeground(NOTIFICATION_ID, buildNotification())
                }
                isRunning = true
                return
            } catch (e: Exception) {
                Log.e(TAG, "startForeground(type=$type) 失败: ${e.message}")
            }
        }

        isRunning = false
        stopSelf()
    }

    /** 候选类型：全集 → 只保连接。[resolveServiceType] 已经退化过就不重复。 */
    private fun serviceTypeCandidates(): List<Int> {
        val full = resolveServiceType()
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return listOf(full)

        val deviceOnly = ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE
        return if (full == deviceOnly) listOf(full) else listOf(full, deviceOnly)
    }

    /**
     * 按**实际已授予的权限**挑选前台服务类型。
     *
     * 以 [ServiceInfo.FOREGROUND_SERVICE_TYPE_LOCATION] 启动但没有定位权限时，
     * 系统会直接抛异常，所以这里不能照搬 manifest 里声明的全集。
     */
    private fun resolveServiceType(): Int {
        var types = 0

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q && hasLocationPermission()) {
            types = types or ServiceInfo.FOREGROUND_SERVICE_TYPE_LOCATION
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && hasBluetoothPermission()) {
            types = types or ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE
        }

        // 一个类型都拿不到时退回 connectedDevice：仅保活、不取定位。
        if (types == 0 && Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            types = ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE
        }

        return types
    }

    private fun hasLocationPermission(): Boolean =
        checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION) ==
            PackageManager.PERMISSION_GRANTED ||
            checkSelfPermission(Manifest.permission.ACCESS_COARSE_LOCATION) ==
            PackageManager.PERMISSION_GRANTED

    private fun hasBluetoothPermission(): Boolean {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) return true
        return checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) ==
            PackageManager.PERMISSION_GRANTED
    }

    private fun buildNotification(): Notification {
        val launchIntent = Intent(this, MainActivity::class.java).apply {
            flags = Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TOP
        }

        val pendingFlags = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        } else {
            PendingIntent.FLAG_UPDATE_CURRENT
        }

        val contentIntent =
            PendingIntent.getActivity(this, 0, launchIntent, pendingFlags)

        /* 被划掉时回自己一条（ACTION_DISMISSED）：Android 14+ 允许用户清掉前台服务通知
         * 而服务照跑，那就别再刷回来。 */
        val dismissIntent = PendingIntent.getService(
            this,
            1,
            Intent(this, CompanionForegroundService::class.java).setAction(ACTION_DISMISSED),
            pendingFlags,
        )

        val builder = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            Notification.Builder(this, CHANNEL_ID)
        } else {
            @Suppress("DEPRECATION")
            Notification.Builder(this)
        }

        return builder
            /* 标题放**状态**——最有用的一行。Android 12+ 的通知头部本来就会显示 App 名，
             * 原标题再写一遍"Helm One"等于连着两行都在说同一件事。 */
            .setContentTitle(contentText)
            // 正文放码表名字，认清是哪一台。
            .setContentText(companionDisplayName())
            .setSmallIcon(R.drawable.ic_companion_bike)
            .setContentIntent(contentIntent)
            .setDeleteIntent(dismissIntent)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setShowWhen(false)
            // 静音不在这里设：API 26+ 的声音/震动由通道决定（CHANNEL 是 MIN ⇒ 不会响）。
            .setCategory(Notification.CATEGORY_SERVICE)
            .build()
    }

    /**
     * 已配对码表的名字（形如 `Helm One-77C8`），认不出来就返回空串。
     *
     * 查不到才重查：查一次会走一次蓝牙 binder，而这条通知每次状态变化都要重建。
     */
    private fun companionDisplayName(): String {
        companionName?.takeIf { it.isNotEmpty() }?.let { return it }

        val name = try {
            if (!hasBluetoothPermission()) {
                ""
            } else {
                val adapter =
                    (getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager)?.adapter

                adapter?.bondedDevices
                    ?.firstOrNull { device ->
                        val n = device.name?.lowercase() ?: return@firstOrNull false
                        CompanionEngine.COMPANION_NAME_HINTS.any { n.contains(it) }
                    }
                    ?.name
                    ?: ""
            }
        } catch (e: Exception) {
            Log.w(TAG, "查码表名字失败: ${e.message}")
            ""
        }

        companionName = name
        return name
    }

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return

        val channel = NotificationChannel(
            CHANNEL_ID,
            "码表连接",
            /* MIN：不响、不震、**不占状态栏图标**，只在下拉后的折叠区里待一行。
             * 前台服务必须挂一条通知（平台硬要求），这是能给到的最安静形态。 */
            NotificationManager.IMPORTANCE_MIN,
        ).apply {
            description = "保持与码表的蓝牙连接（不打扰）"
            setShowBadge(false)
            enableVibration(false)
            setSound(null, null)
        }

        notificationManager()?.apply {
            createNotificationChannel(channel)
            // 旧通道只会多一条设置项等着用户去问"这是干嘛的"，删掉。
            deleteNotificationChannel(LEGACY_CHANNEL_ID)
        }
    }

    private fun notificationManager(): NotificationManager? =
        getSystemService(Context.NOTIFICATION_SERVICE) as? NotificationManager

    private fun stopSelfAndForeground() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
            stopForeground(STOP_FOREGROUND_REMOVE)
        } else {
            @Suppress("DEPRECATION")
            stopForeground(true)
        }
        isRunning = false
        stopSelf()
    }

    override fun onDestroy() {
        isRunning = false
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null
}

package com.sifli.sifli_companion

import android.Manifest
import android.bluetooth.BluetoothManager
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.util.Log
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.embedding.engine.dart.DartExecutor
import io.flutter.plugin.common.MethodChannel
import io.flutter.plugins.GeneratedPluginRegistrant

/**
 * 进程内唯一的 FlutterEngine 持有者。
 *
 * 为什么要有这个东西：连接逻辑全在 Dart 侧，而 Dart 只在存在 FlutterEngine 时才运行。
 * 进程被系统"为了前台服务"重建时（START_STICKY 重启）不会创建 Activity，也就没人为它
 * 建引擎 —— 表现就是"进程起来了、常驻通知在，但永远连不上码表"。
 * 2026-09-25 现场实测：`Start proc … for service {…CompanionForegroundService}`，
 * 进程线程表里零个 Dart 线程、`GATT Client Map` 为空、板子侧只看到广播没人连。
 *
 * 所以引擎改成进程级持有：Activity 与前台服务两条路径都从这里取同一份，保证一个进程
 * 只有一个 Dart isolate —— 否则两个 isolate 会各自建一个 GATT client 抢同一条链路，
 * 通知转发也会重复。
 */
object CompanionEngine {

    private const val TAG = "CompanionEngine"

    private const val CHANNEL = "com.sifli.sifli_companion/companion_fgs"

    /** 码表广播名里的特征串（小写、子串匹配），与 Dart 侧保持一致。前台服务也用它认名字。 */
    internal val COMPANION_NAME_HINTS = listOf("helm one", "helmone", "mybike", "vela")

    /** 原生侧存的偏好文件（与 Dart 的 SharedPreferences 分开，免得耦合 Flutter 的键前缀）。 */
    private const val PREFS = "companion_native"

    /** 「保持连接」开关的镜像，见 [keepAliveWanted]。 */
    private const val KEY_KEEP_ALIVE = "keep_alive"

    @Volatile
    private var engine: FlutterEngine? = null

    /** 应用级 Context，供方法通道回调使用（Activity 可能已经不在了）。 */
    @Volatile
    private var appContext: Context? = null

    /** 本进程是否已经登记过插件与方法通道。 */
    @Volatile
    private var registered = false

    /** 进程内的引擎；返回 null 表示还没有，调用方自行决定是复用还是新建。 */
    fun engineOrNull(): FlutterEngine? = engine

    /**
     * 登记本 App 自己那部分（GeneratedPluginRegistrant 之外的东西）。
     *
     * 幂等：Activity 每次重建都会走 `configureFlutterEngine`，通道与插件只准登记一次。
     */
    fun registerOnce(context: Context, target: FlutterEngine) {
        synchronized(this) {
            if (engine == null) engine = target
            if (registered) return
            registered = true
        }

        appContext = context.applicationContext
        Log.i(TAG, "登记插件与方法通道")

        CompanionNotifPlugin.register(target, context)

        MethodChannel(target.dartExecutor.binaryMessenger, CHANNEL)
            .setMethodCallHandler { call, result ->
                when (call.method) {
                    "start" -> {
                        startService(CompanionForegroundService.ACTION_START, call.argument<String>("text"))
                        result.success(true)
                    }

                    "update" -> {
                        startService(CompanionForegroundService.ACTION_UPDATE, call.argument<String>("text"))
                        result.success(true)
                    }

                    "stop" -> {
                        startService(CompanionForegroundService.ACTION_STOP, null)
                        result.success(true)
                    }

                    "isRunning" -> result.success(CompanionForegroundService.isRunning)

                    /* 分享完成（Dart 侧 `appShare()` 在 share_plus 报 success 之后调用）。
                     * 这一刻我们在后台、目标应用在前台，弹不出对话框 ⇒ 用通知问一句，
                     * 见 ShareReturnNotifier 的注释。 */
                    "shareDone" -> {
                        appContext?.let { ShareReturnNotifier.show(it) }
                        result.success(true)
                    }

                    "setKeepAlive" -> {
                        val on = call.argument<Boolean>("on") ?: true
                        appContext
                            ?.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
                            ?.edit()
                            ?.putBoolean(KEY_KEEP_ALIVE, on)
                            ?.apply()
                        Log.i(TAG, "「保持连接」= $on")
                        result.success(true)
                    }

                    else -> result.notImplemented()
                }
            }
    }

    /**
     * 后台路径：把引擎与 Dart 一起拉起来。
     *
     * 与 Activity 自建引擎等价（先登记插件再执行入口），区别只在于此时没有 FlutterView ——
     * Dart 侧会因此走"无视图"分支：只跑连接与通知转发，等视图出现再挂 UI。
     */
    fun start(context: Context): FlutterEngine {
        val app = context.applicationContext

        synchronized(this) {
            engine?.let { return it }

            val created = FlutterEngine(app)
            // 引擎自建时不带插件，这里补上（embedding 自己建引擎时也是这一步）。
            GeneratedPluginRegistrant.registerWith(created)
            registerOnce(app, created)

            created.dartExecutor.executeDartEntrypoint(
                DartExecutor.DartEntrypoint.createDefault(),
            )

            engine = created
            Log.i(TAG, "进程为后台服务重建：已拉起 FlutterEngine")
            return created
        }
    }

    /**
     * 只有"确实配过码表"才拉起引擎，供**通知监听服务**那条唤醒路径调用。
     *
     * 为什么监听路径也要管：用户从最近任务划掉 App 之后，系统 1 s 内就把监听服务绑回来
     * （`Start proc … for service {CompanionNotificationListener}`），而前台服务那一步
     * MIUI 会拖很久 —— 2026-09-25 实测 100 s 仍未投递。于是进程回来了、Dart 却没起来，
     * 表现就是"自启动了但没连码表"。那条路径同样没有 Activity，所以同样得在这里补引擎。
     *
     * 没配过码表的用户不该为它养一个 Flutter 引擎，所以先查一次已绑定设备。
     */
    fun ensureStartedIfPaired(context: Context) {
        if (engineOrNull() != null) return

        /* 设置页「保持连接」关掉时，连引擎都不该建：建了就是一个只会连着码表的后台进程，
         * 正是用户关那个开关想避免的事。 */
        if (!keepAliveWanted(context)) {
            Log.i(TAG, "「保持连接」已关闭，不为后台拉起引擎")
            return
        }

        if (!hasBondedCompanion(context)) return

        try {
            start(context)
        } catch (e: Exception) {
            Log.e(TAG, "监听服务拉起 FlutterEngine 失败: ${e.message}", e)
        }
    }

    /** 「保持连接」开关（Dart 侧通过 [CHANNEL] 的 `setKeepAlive` 写进来）。缺省开。 */
    private fun keepAliveWanted(context: Context): Boolean =
        context.applicationContext
            .getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .getBoolean(KEY_KEEP_ALIVE, true)

    /** 系统里有没有已配对的码表。名字判据与 Dart 侧 `CompanionProto.matchesAdvName` 对齐。 */
    private fun hasBondedCompanion(context: Context): Boolean {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S &&
            context.checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) !=
            PackageManager.PERMISSION_GRANTED
        ) {
            return false
        }

        return try {
            val adapter =
                (context.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager)?.adapter

            adapter?.bondedDevices?.any { device ->
                val name = device.name?.lowercase() ?: return@any false
                COMPANION_NAME_HINTS.any { name.contains(it) }
            } == true
        } catch (e: Exception) {
            Log.w(TAG, "查已绑定设备失败: ${e.message}")
            false
        }
    }

    /**
     * 发指令给前台服务。
     *
     * `from_dart=true` 是给服务的判据：只有 Dart 侧（活的引擎）发来的指令，它才认里面
     * 那句文案 —— 系统重启服务时会把最后一次的 Intent 连 extras 一起重放，照搬就会把
     * 上一次真连接时的"已连接"当成当前状态显示。
     */
    private fun startService(action: String, text: String?) {
        val context = appContext ?: return

        val intent = Intent(context, CompanionForegroundService::class.java).apply {
            this.action = action
            putExtra(CompanionForegroundService.EXTRA_FROM_DART, true)
            text?.let { putExtra(CompanionForegroundService.EXTRA_TEXT, it) }
        }

        // startForegroundService 要求服务在 5 秒内调用 startForeground()，
        // 停止走普通 startService 即可，避免 Android 12+ 的后台启动限制。
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O &&
            action != CompanionForegroundService.ACTION_STOP
        ) {
            context.startForegroundService(intent)
        } else {
            context.startService(intent)
        }
    }
}

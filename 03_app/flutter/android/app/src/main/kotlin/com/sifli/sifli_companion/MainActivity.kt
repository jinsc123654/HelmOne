package com.sifli.sifli_companion

import android.content.Context
import android.content.Intent
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine

/**
 * 界面壳：只负责把**进程级**引擎接到 UI 上（引擎归 [CompanionEngine] 管，原因见它的注释）。
 *
 * 两个方向都要照顾到：
 * - 进程刚起来（用户点图标）：这里还没有引擎，返回 null 让 FlutterActivity 照旧自建，
 *   行为与改造前完全一致；
 * - 进程是被前台服务拉起来的（用户杀掉 App 之后）：[CompanionEngine] 里已经有一份正在
 *   跑、并且可能已经连上码表的 Dart isolate，必须复用它 —— 再建一个就会有两份 isolate
 *   抢同一条链路。
 */
class MainActivity : FlutterActivity() {

    override fun provideFlutterEngine(context: Context): FlutterEngine? =
        CompanionEngine.engineOrNull()

    /** 引擎是进程级的：Activity 销毁不能把它带走，否则"退到后台还保持连接"无从谈起。 */
    override fun shouldDestroyEngineWithHost(): Boolean = false

    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)
        CompanionEngine.registerOnce(this, flutterEngine)
    }

    /**
     * 分享的**根因修复**：把系统选择器放进**独立任务**。
     *
     * share_plus 是在本 Activity 上调 `startActivityForResult(chooserIntent, …)` 启动选择器的，
     * **不带 `FLAG_ACTIVITY_NEW_TASK`** ⇒ 选择器和用户选中的目标应用（微信）都落进**本任务**；
     * 而微信"返回"是把整个任务压到后台、并不结束自己 ⇒ 按返回一路回到桌面、最近任务卡被顶成
     * "标题 Helm One、顶上却是微信"；最坏的时候我们的 MainActivity 被系统回收、任务里只剩微信
     * —— **点 App 图标打开的会是微信**（2026-09-27 用 dumpsys + logcat 实测到这一步）。
     *
     * 在这里补 flag：只对**选择器**生效，其它 startActivity 一律不动。
     * 为什么不改 share_plus 源码：这里同样有效，还不用 vendor 一个 fork 去跟版本。
     *
     * 补上之后：选择器+微信在独立任务里，我们的任务保持"只有一个 MainActivity" ⇒
     * 微信返回时下面就是我们的任务（**按返回回到码表**）、图标还是我们的、最近任务卡干净。
     */
    private fun withShareTask(intent: Intent): Intent {
        if (Intent.ACTION_CHOOSER == intent.action) {
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        }
        return intent
    }

    override fun startActivity(intent: Intent) {
        super.startActivity(withShareTask(intent))
    }

    /** share_plus 走的是这个重载（`withResult` 在 API 22+ 恒为 true）。 */
    @Suppress("DEPRECATION")
    override fun startActivityForResult(intent: Intent, requestCode: Int) {
        super.startActivityForResult(withShareTask(intent), requestCode)
    }
}

package com.sifli.sifli_companion

import android.app.Notification
import android.os.Bundle
import android.os.Parcelable

/**
 * QQ / 微信等会话通知经常不把正文放在 EXTRA_TITLE / EXTRA_TEXT，
 * 而是 MessagingStyle extras、TEXT_LINES、ticker。统一抽成标题和正文。
 *
 * 只读公开 extras，不调用 [Notification.MessagingStyle] 的静态 API
 * （部分 compileSdk 上该方法不可见）。
 */
internal object NotifParse {

    data class Text(val title: String, val text: String) {
        val isBlank: Boolean get() = title.isBlank() && text.isBlank()
    }

    fun extract(n: Notification): Text {
        val extras = n.extras ?: return Text("", cs(n.tickerText))
        val msg = messagingFromExtras(extras)
        var title = firstNonBlank(
            cs(extras.getCharSequence(Notification.EXTRA_TITLE)),
            cs(extras.getCharSequence(Notification.EXTRA_TITLE_BIG)),
            cs(extras.getCharSequence(Notification.EXTRA_CONVERSATION_TITLE)),
            msg?.first.orEmpty(),
            cs(extras.getCharSequence(Notification.EXTRA_SUB_TEXT)),
        )
        var text = firstNonBlank(
            cs(extras.getCharSequence(Notification.EXTRA_TEXT)),
            cs(extras.getCharSequence(Notification.EXTRA_BIG_TEXT)),
            cs(extras.getCharSequence(Notification.EXTRA_SUMMARY_TEXT)),
            cs(extras.getCharSequence(Notification.EXTRA_INFO_TEXT)),
            textLines(extras),
            msg?.second.orEmpty(),
            cs(n.tickerText),
        )
        if (title.isBlank() && msg != null) title = msg.first
        if (text.isBlank() && msg != null) text = msg.second
        return Text(title, text)
    }

    private fun messagingFromExtras(extras: Bundle): Pair<String, String>? {
        val conv = cs(extras.getCharSequence(Notification.EXTRA_CONVERSATION_TITLE))
        val messages = extrasMessages(extras)
        if (messages.isEmpty()) return null
        val last = messages.last()
        val title = conv.ifBlank { last.sender }
        val text = when {
            conv.isNotBlank() && last.sender.isNotBlank() && last.body.isNotBlank() ->
                "${last.sender}: ${last.body}"
            else -> last.body.ifBlank { last.sender }
        }
        if (title.isBlank() && text.isBlank()) return null
        return title to text
    }

    private data class Msg(val sender: String, val body: String)

    private fun extrasMessages(extras: Bundle): List<Msg> {
        val arr = parcelArray(extras, Notification.EXTRA_MESSAGES) ?: return emptyList()
        return arr.mapNotNull { itemToMsg(it) }
    }

    private fun itemToMsg(item: Any?): Msg? {
        if (item == null) return null
        if (item is Bundle) {
            val body = firstNonBlank(
                cs(item.getCharSequence("text")),
                cs(item.getString("text")),
            )
            val sender = firstNonBlank(
                cs(item.getCharSequence("sender")),
                cs(item.getString("sender")),
            )
            if (body.isBlank() && sender.isBlank()) return null
            return Msg(sender, body)
        }
        val body = invokeCs(item, "getText")
        val sender = firstNonBlank(
            invokeCs(item, "getSender"),
            invokePersonName(item),
        )
        if (body.isBlank() && sender.isBlank()) return null
        return Msg(sender, body)
    }

    @Suppress("DEPRECATION")
    private fun parcelArray(extras: Bundle, key: String): Array<out Parcelable>? {
        return try {
            extras.getParcelableArray(key)
        } catch (_: Exception) {
            null
        }
    }

    private fun invokeCs(obj: Any, method: String): String {
        return try {
            cs(obj.javaClass.getMethod(method).invoke(obj) as? CharSequence)
        } catch (_: Exception) {
            ""
        }
    }

    private fun invokePersonName(obj: Any): String {
        val person = try {
            obj.javaClass.getMethod("getSenderPerson").invoke(obj)
                ?: obj.javaClass.getMethod("getPerson").invoke(obj)
        } catch (_: Exception) {
            null
        } ?: return ""
        return invokeCs(person, "getName")
    }

    private fun textLines(extras: Bundle): String {
        val arr = extras.getCharSequenceArray(Notification.EXTRA_TEXT_LINES) ?: return ""
        return arr.map { cs(it) }.filter { it.isNotBlank() }.joinToString("\n")
    }

    private fun cs(v: CharSequence?): String = v?.toString()?.trim().orEmpty()

    private fun firstNonBlank(vararg values: String): String =
        values.firstOrNull { it.isNotBlank() }.orEmpty()
}

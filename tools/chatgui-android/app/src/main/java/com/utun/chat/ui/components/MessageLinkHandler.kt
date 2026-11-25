package com.utun.chat.ui.components

import android.content.Intent
import android.net.Uri
import android.widget.Toast
import androidx.compose.runtime.Composable
import androidx.compose.ui.platform.LocalContext
import com.utun.chat.R
import com.utun.chat.data.LogManager
import com.utun.chat.data.MessageLinks

/** Один обработчик ссылок для чата, PM и карточек; внутренний адрес получает владелец навигации. */
@Composable
fun messageLinkHandler(onInternal: (String) -> Unit): (String) -> Unit {
    val context = LocalContext.current
    return { link ->
        if (link.startsWith("utun://")) onInternal(link)
        else try {
            require(MessageLinks.isWebUrl(link)) { "Invalid web URL" }
            val uri = Uri.parse(link)
            context.startActivity(Intent(Intent.ACTION_VIEW, uri))
            LogManager.addLog("INFO", "LINK", "External open requested scheme=${uri.scheme} host=${uri.host}")
        } catch (e: Exception) {
            LogManager.addLog("ERROR", "LINK", "External open failed bytes=${link.length} reason=${e.javaClass.simpleName}")
            Toast.makeText(context, R.string.link_open_failed, Toast.LENGTH_LONG).show()
        }
    }
}

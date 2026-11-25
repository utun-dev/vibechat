package com.utun.chat.ui.screens

import android.app.Activity
import android.content.ContextWrapper
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.remember
import androidx.compose.ui.platform.LocalContext
import androidx.core.view.ViewCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import com.utun.chat.data.LogManager

/** Immersive-режим viewer без смены ориентации; возвращает прежнюю видимость обеих панелей и поведение swipe.
 * Вызывающий экран должен быть единственным владельцем fullscreen окна. identity — диагностическое имя без секретов. */
@Composable
internal fun ViewerFullscreenWindow(identity: String) {
    val context = LocalContext.current
    val activity = remember(context) {
        generateSequence(context) { (it as? ContextWrapper)?.baseContext }.filterIsInstance<Activity>().firstOrNull()
    }
    DisposableEffect(activity, identity) {
        val window = activity?.window
        val controller = window?.let { WindowCompat.getInsetsController(it, it.decorView) }
        val insets = window?.let { ViewCompat.getRootWindowInsets(it.decorView) }
        val statusVisible = insets?.isVisible(WindowInsetsCompat.Type.statusBars()) ?: true
        val navigationVisible = insets?.isVisible(WindowInsetsCompat.Type.navigationBars()) ?: true
        val behavior = controller?.systemBarsBehavior
        if (controller != null) try {
            controller.systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            controller.hide(WindowInsetsCompat.Type.systemBars())
            LogManager.addLog("INFO", "Viewer", "Fullscreen entered viewer=$identity")
        } catch (e: Exception) {
            LogManager.addLog("ERROR", "Viewer", "Cannot enter fullscreen viewer=$identity: $e")
        }
        onDispose {
            if (controller != null && behavior != null) {
                val restore: List<Pair<String, () -> Unit>> = listOf(
                    "behavior" to { controller.systemBarsBehavior = behavior },
                    "status" to {
                        if (statusVisible) controller.show(WindowInsetsCompat.Type.statusBars())
                        else controller.hide(WindowInsetsCompat.Type.statusBars())
                    },
                    "navigation" to {
                        if (navigationVisible) controller.show(WindowInsetsCompat.Type.navigationBars())
                        else controller.hide(WindowInsetsCompat.Type.navigationBars())
                    }
                )
                var failures = 0
                for ((part, action) in restore) try { action() } catch (e: Exception) {
                    failures++
                    LogManager.addLog("ERROR", "Viewer", "Window restore failed viewer=$identity part=$part: $e")
                }
                if (failures == 0) LogManager.addLog("INFO", "Viewer", "Window restored viewer=$identity")
            }
        }
    }
}

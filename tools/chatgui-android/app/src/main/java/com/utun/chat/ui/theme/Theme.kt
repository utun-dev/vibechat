package com.utun.chat.ui.theme

import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import com.utun.chat.data.LogManager

// Общая палитра экранов, диалогов и сообщений; подписи не ослабляем прозрачностью.
object ChatColors {
    val Background = Color(0xFF0F1B22)
    val Panel = Color(0xFF1B2733)
    val Raised = Color(0xFF24323D)
    val Outgoing = Color(0xFF214563)
    val IncomingBubble = Color(0xFF161E25)
    val OutgoingBubble = Color(0xFF112332)
    val BubbleShadow = Color.Black.copy(alpha = 0.3f)
    val Text = Color(0xFFE6EDF3)
    val Secondary = Color(0xFFAEBCC8)
    val Accent = Color(0xFF79B8FF)
    val Success = Color(0xFF81C784)
    val Error = Color(0xFFFFB4AB)
}

private val DarkColors = darkColorScheme(
    primary = ChatColors.Accent, onPrimary = ChatColors.Background,
    primaryContainer = ChatColors.Outgoing, onPrimaryContainer = ChatColors.Text,
    secondary = ChatColors.Accent, onSecondary = ChatColors.Background,
    secondaryContainer = ChatColors.Raised, onSecondaryContainer = ChatColors.Text,
    tertiary = ChatColors.Success, onTertiary = ChatColors.Background,
    tertiaryContainer = Color(0xFF203C2C), onTertiaryContainer = ChatColors.Text,
    background = ChatColors.Background, onBackground = ChatColors.Text,
    surface = ChatColors.Panel, onSurface = ChatColors.Text,
    surfaceVariant = ChatColors.Raised, onSurfaceVariant = ChatColors.Secondary,
    surfaceTint = Color.Transparent,
    inverseSurface = ChatColors.Raised, inverseOnSurface = ChatColors.Text, inversePrimary = ChatColors.Accent,
    outline = Color(0xFF8293A1), outlineVariant = Color(0xFF435563),
    error = ChatColors.Error, onError = Color(0xFF4A1518),
    errorContainer = Color(0xFF54252A), onErrorContainer = ChatColors.Text,
    scrim = Color.Black
)

/** Постоянная Material3 dark palette и фоновый Surface на весь контейнер; системная/dynamic тема не выбирается. */
@Composable
fun VibeChatTheme(content: @Composable () -> Unit) {
    LaunchedEffect(Unit) {
        LogManager.addLog("INFO", "UI", "Dark theme active: message bubbles incoming=#161E25 outgoing=#112332 " +
            "shadow=black shadow_alpha=0.3 shadow_offset_dp=2")
    }
    MaterialTheme(colorScheme = DarkColors) {
        Surface(modifier = Modifier.fillMaxSize(), color = ChatColors.Background, content = content)
    }
}

package com.utun.chat.ui.components

import com.utun.chat.R
import androidx.compose.ui.res.stringResource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedCard
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.LifecycleResumeEffect
import com.utun.chat.data.BatteryOptimizationPermission
import com.utun.chat.data.LogManager

/** Исключение из оптимизации батареи: источник истины — PowerManager, повторная проверка выполняется на каждом RESUME.
 * null означает ошибку чтения, а не отказ пользователя: показываем диагностику/«Повторить» без запуска системного запроса.
 * «Разрешить» ещё раз проверяет ОС и открывает запрос только при известном false; pending сохраняется при пересоздании UI.
 * Возврат из системного экрана снимает pending и читает состояние заново, resultCode не используется.
 * enabled блокирует только кнопку; настройка не останавливает ядро и не блокирует завершение первоначальной настройки. */
@Composable
fun BatteryOptimizationSetting(source: String, enabled: Boolean = true) {
    val context = LocalContext.current
    var granted by remember(context) { mutableStateOf(BatteryOptimizationPermission.granted(context)) }
    var lastLoggedState by remember(context) { mutableStateOf<Boolean?>(null) }
    var requestPending by rememberSaveable { mutableStateOf(false) }
    var error by rememberSaveable { mutableStateOf<Int?>(null) }

    // Результат системного диалога определяем повторной проверкой, а не resultCode.
    LifecycleResumeEffect(context, source) {
        granted = BatteryOptimizationPermission.granted(context)
        if (lastLoggedState != granted) {
            LogManager.addLog(if (granted == null) "ERROR" else "INFO", "PERMISSION",
                "Battery optimization exemption: source=$source granted=$granted")
            lastLoggedState = granted
        }
        if (requestPending) {
            val level = when (granted) { true -> "INFO"; false -> "WARN"; null -> "ERROR" }
            LogManager.addLog(level, "PERMISSION", "Battery optimization request completed: source=$source granted=$granted")
            requestPending = false
        }
        if (granted == true) error = null
        onPauseOrDispose { }
    }

    OutlinedCard(modifier = Modifier.fillMaxWidth()) {
        Column(modifier = Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(stringResource(R.string.background_unrestricted), style = MaterialTheme.typography.titleMedium)
            Text(stringResource(R.string.battery_optimization_hint),
                style = MaterialTheme.typography.bodySmall)
            Text(stringResource(when (granted) {
                true -> R.string.allowed; false -> R.string.not_allowed; null -> R.string.battery_state_failed
            }), style = MaterialTheme.typography.bodyMedium, color = when (granted) {
                true -> MaterialTheme.colorScheme.primary
                false -> MaterialTheme.colorScheme.onSurfaceVariant
                null -> MaterialTheme.colorScheme.error
            })
            if (granted != true) {
                Button(enabled = enabled && !requestPending, onClick = {
                    error = null
                    val retryQuery = granted == null
                    granted = BatteryOptimizationPermission.granted(context)
                    if (granted == false && !retryQuery) {
                        requestPending = true
                        if (!BatteryOptimizationPermission.request(context, source)) {
                            requestPending = false
                            error = R.string.battery_settings_failed
                        }
                    }
                }) { Text(stringResource(if (granted == null) R.string.retry else R.string.grant)) }
            }
            error?.let { Text(stringResource(it), color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall) }
        }
    }
}

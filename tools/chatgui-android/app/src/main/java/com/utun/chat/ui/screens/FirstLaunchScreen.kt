package com.utun.chat.ui.screens

import com.utun.chat.R
import androidx.compose.ui.res.stringResource
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.utun.chat.data.LogManager
import com.utun.chat.ui.components.BatteryOptimizationSetting
import com.utun.chat.ui.components.LanguageSetting
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.launch

/** Первичная настройка: непустое имя без управляющих символов передаётся onComplete с обрезанными краями.
 * Callback сохраняет настройки; до его завершения повторная отправка запрещена. Отмена coroutine не считается ошибкой. */
@Composable
fun FirstLaunchScreen(initialName: String, onComplete: suspend (String) -> Unit) {
    var name by rememberSaveable { mutableStateOf(initialName) }
    var saving by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<Int?>(null) }
    val scope = rememberCoroutineScope()
    val validName = name.trim().isNotEmpty() && name.none { it.isISOControl() }

    Surface(modifier = Modifier.fillMaxSize()) {
        Column(
            modifier = Modifier.fillMaxSize().safeDrawingPadding().imePadding()
                .verticalScroll(rememberScrollState()).padding(24.dp),
            verticalArrangement = Arrangement.Center,
            horizontalAlignment = Alignment.CenterHorizontally
        ) {
            Text(stringResource(R.string.welcome), style = MaterialTheme.typography.headlineMedium)
            Spacer(Modifier.height(12.dp))
            Text(stringResource(R.string.device_name_hint))
            Spacer(Modifier.height(24.dp))
            LanguageSetting()
            Spacer(Modifier.height(12.dp))
            OutlinedTextField(
                value = name,
                onValueChange = { name = it; error = null },
                label = { Text(stringResource(R.string.device_nickname)) },
                singleLine = true,
                enabled = !saving,
                modifier = Modifier.fillMaxWidth()
            )
            error?.let {
                Spacer(Modifier.height(8.dp))
                Text(stringResource(it), color = MaterialTheme.colorScheme.error)
            }
            Spacer(Modifier.height(24.dp))
            BatteryOptimizationSetting(source = "setup", enabled = !saving)
            Spacer(Modifier.height(24.dp))
            Button(
                enabled = validName && !saving,
                modifier = Modifier.fillMaxWidth(),
                onClick = {
                    saving = true
                    scope.launch {
                        try {
                            onComplete(name.trim())
                        } catch (e: CancellationException) {
                            throw e
                        } catch (e: Exception) {
                            LogManager.addLog("ERROR", "SETUP", "Failed to save device name: $e")
                            error = R.string.device_name_save_failed
                        } finally {
                            saving = false
                        }
                    }
                }
            ) {
                Text(stringResource(if (saving) R.string.saving else R.string.continue_action))
            }
        }
    }
}

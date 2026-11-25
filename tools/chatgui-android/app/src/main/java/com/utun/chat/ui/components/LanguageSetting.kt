package com.utun.chat.ui.components

import androidx.compose.foundation.layout.*
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ArrowDropDown
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.res.stringResource
import com.utun.chat.R
import com.utun.chat.data.AppLanguage
import com.utun.chat.data.LogManager

/** Выбор одного языка каталога либо «Как в системе»; название текущего запроса учитывает приоритетный список Android.
 * Чтение повторяется при новой Configuration/явной попытке; ошибка не выдаётся за системный выбор.
 * После select перечитывает фактический запрос Android, включая отказ после частичного применения; ошибка показывается отдельно.
 * Успех означает принятие запроса AppCompat; фактические ресурсы/пересоздание Activity применяет Android. */
@Composable
fun LanguageSetting() {
    var expanded by remember { mutableStateOf(false) }
    var retry by remember { mutableIntStateOf(0) }
    var selectionFailed by remember { mutableStateOf(false) }
    val configuration = LocalConfiguration.current
    val state = remember(configuration, retry) {
        try { Result.success(AppLanguage.selectedTag() to AppLanguage.supportedTags)
        } catch (error: Exception) { Result.failure(error) }
    }
    val selection = state.getOrNull()
    val selected = selection?.first
    val systemLabel = stringResource(R.string.language_system)
    val options = listOf("" to systemLabel) + selection?.second.orEmpty().map { it to AppLanguage.nativeName(it) }
    val selectedName = if (selected == null) stringResource(R.string.retry)
        else options.firstOrNull { it.first == selected }?.second ?: AppLanguage.nativeName(selected)
    Column(Modifier.fillMaxWidth()) {
        Box(Modifier.fillMaxWidth()) {
            OutlinedButton(onClick = {
                selectionFailed = false
                if (state.isFailure) {
                    retry++
                    LogManager.addLog("DEBUG", "LANGUAGE", "Language settings read retry requested")
                } else {
                    expanded = true
                    LogManager.addLog("DEBUG", "LANGUAGE", "Language menu opened: selected=$selected options=${options.map { it.first }}")
                }
            }, modifier = Modifier.fillMaxWidth()) {
                Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                    Column(Modifier.weight(1f)) {
                        Text(stringResource(R.string.interface_language), style = MaterialTheme.typography.labelMedium)
                        Text(selectedName, style = MaterialTheme.typography.bodyLarge)
                    }
                    Icon(if (state.isFailure) Icons.Default.Refresh else Icons.Default.ArrowDropDown, contentDescription = null)
                }
            }
            DropdownMenu(expanded = expanded && state.isSuccess, onDismissRequest = { expanded = false }) {
                options.forEach { (tag, name) ->
                    DropdownMenuItem(text = { Text(name) }, onClick = {
                        expanded = false
                        selectionFailed = !AppLanguage.select(tag)
                        retry++
                    })
                }
            }
        }
        val error = when { state.isFailure -> R.string.language_load_failed; selectionFailed -> R.string.language_apply_failed; else -> null }
        error?.let { Text(stringResource(it), color = MaterialTheme.colorScheme.error, style = MaterialTheme.typography.bodySmall) }
    }
}

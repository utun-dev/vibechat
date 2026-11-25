package com.utun.chat.ui.screens

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.utun.chat.R
import com.utun.chat.viewmodel.ChatViewModel

/** Приглашение из строки участника или шапки PM; состоянием и отправкой владеет вызывающий ViewModel.
 * pending блокирует выбор/отправку, но не закрытие; errorRes переводится при отображении. */
@Composable
fun PmInviteDialog(state: ChatViewModel.PmInviteState, onChoose: (String) -> Unit, onSend: () -> Unit, onDismiss: () -> Unit) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(stringResource(R.string.invite_to_group)) },
        text = {
            Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(stringResource(R.string.invite_recipient, state.name))
                Text(stringResource(R.string.invite_lifetime), style = MaterialTheme.typography.bodySmall)
                Text(stringResource(if (state.groups.isEmpty()) R.string.invite_no_groups else R.string.invite_choose_group))
                state.groups.forEach { group ->
                    Row(Modifier.fillMaxWidth().clickable(enabled = !state.pending) { onChoose(group.id) },
                        verticalAlignment = Alignment.CenterVertically) {
                        RadioButton(selected = state.groupId == group.id, onClick = { onChoose(group.id) }, enabled = !state.pending)
                        Text(group.name, modifier = Modifier.weight(1f))
                    }
                }
                if (state.pending) {
                    LinearProgressIndicator(Modifier.fillMaxWidth())
                    Text(stringResource(R.string.invite_sending))
                }
                if (state.errorRes != 0) Text(stringResource(state.errorRes), color = MaterialTheme.colorScheme.error)
            }
        },
        confirmButton = {
            TextButton(onClick = onSend, enabled = !state.pending && state.groupId.isNotEmpty()) { Text(stringResource(R.string.invite)) }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text(stringResource(R.string.cancel)) } }
    )
}

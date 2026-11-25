package com.utun.chat.ui.screens

import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.utun.chat.R
import com.utun.chat.data.DevgroupRepository
import kotlinx.coroutines.delay

private fun deviceInvitationState(state: Int): Int = when (state) {
    1 -> R.string.devgroup_waiting; 2 -> R.string.devgroup_checking; 3 -> R.string.devgroup_transferring
    4 -> R.string.devgroup_joined; 5 -> R.string.devgroup_rejected; 6 -> R.string.devgroup_expired
    8 -> R.string.devgroup_waiting_return; 9 -> R.string.devgroup_checking; 10 -> R.string.devgroup_transferring
    else -> R.string.devgroup_unknown
}

/** Диалог живёт на уровне Activity, но входящее приглашение открывается только из меню self. */
@Composable
fun DevicePairingDialogs() {
    val state by DevgroupRepository.state.collectAsState()
    val invitation = state.invitations.firstOrNull { it.id == state.outgoingId }
    var pin by remember(state.outgoingId, invitation?.input) { mutableStateOf("") }
    LaunchedEffect(state.outgoingVisible, state.invitations.any { it.pending }) {
        if (state.outgoingVisible || state.invitations.any { it.pending }) while (true) { DevgroupRepository.command(); delay(1000) }
    }
    if (state.renameFailed) {
        AlertDialog(onDismissRequest = DevgroupRepository::dismissRenameError,
            title = { Text(stringResource(R.string.devgroup_rename)) },
            text = { Text(stringResource(R.string.devgroup_rename_failed)) },
            confirmButton = { TextButton(onClick = DevgroupRepository::dismissRenameError) { Text(stringResource(android.R.string.ok)) } })
        return
    }
    if (state.leaveFailed) {
        AlertDialog(onDismissRequest = DevgroupRepository::dismissLeaveError,
            title = { Text(stringResource(R.string.devgroup_disconnect)) },
            text = { Text(stringResource(R.string.devgroup_failed)) },
            confirmButton = { TextButton(onClick = DevgroupRepository::dismissLeaveError) { Text(stringResource(android.R.string.ok)) } })
        return
    }
    if (!state.outgoingVisible) return
    val busy = state.accepting.isNotEmpty()
    val input = invitation?.input ?: 0
    val shownPin = invitation?.pin ?: state.pin
    AlertDialog(onDismissRequest = DevgroupRepository::closeOutgoing,
        title = { Text(stringResource(R.string.devgroup_linking)) },
        text = { Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
            if (state.choosing) {
                Text(stringResource(R.string.devgroup_select_device))
                state.invitations.filter { it.pending }.forEach { entry ->
                    TextButton(onClick = { DevgroupRepository.openPairing(entry.id) }) { Text("${entry.peer} / ${entry.id.take(8)}") }
                }
            } else {
                if (invitation != null) Text(invitation.peer)
                if (invitation?.outgoing == false) {
                    Text(stringResource(R.string.devgroup_incoming_explanation))
                    Text(stringResource(R.string.devgroup_unexpected_request))
                }
                if (shownPin.isNotEmpty()) Text(stringResource(
                    if (invitation?.outgoing == false) R.string.devgroup_return_pin else R.string.devgroup_pin, shownPin))
                Text(stringResource(deviceInvitationState(invitation?.state ?: if (state.outgoingId.isEmpty()) 1 else 6)))
            }
            if (input != 0) {
                OutlinedTextField(value = pin, onValueChange = { if (it.length <= 4 && it.all { c -> c in '0'..'9' }) pin = it },
                    label = { Text(stringResource(if (input == 1) R.string.devgroup_pin_hint else R.string.devgroup_return_hint)) },
                    enabled = !busy, singleLine = true,
                    keyboardOptions = androidx.compose.foundation.text.KeyboardOptions(keyboardType = androidx.compose.ui.text.input.KeyboardType.NumberPassword))
                if (invitation?.pinError == true) Text(stringResource(R.string.devgroup_pin_retry), color = MaterialTheme.colorScheme.error)
            }
            if (state.failed) Text(stringResource(R.string.devgroup_failed), color = MaterialTheme.colorScheme.error)
        } }, confirmButton = {
            if (input != 0) TextButton(enabled = !busy && pin.length == 4, onClick = {
                DevgroupRepository.command(2, text = state.outgoingId, pin = pin); pin = ""
            }) { Text(stringResource(R.string.devgroup_check_pin)) }
        }, dismissButton = { TextButton(onClick = DevgroupRepository::closeOutgoing) { Text(stringResource(R.string.close)) } })
}

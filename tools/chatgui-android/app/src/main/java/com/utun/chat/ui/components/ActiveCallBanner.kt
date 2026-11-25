package com.utun.chat.ui.components

import com.utun.chat.R
import androidx.compose.ui.res.stringResource
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.CallEnd
import androidx.compose.material.icons.filled.PhoneInTalk
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.utun.chat.viewmodel.ChatViewModel
import com.utun.chat.data.CallPhase

private fun fmtDuration(sec: Int): String {
    val m = sec / 60
    val s = sec % 60
    return "%02d:%02d".format(m, s)
}

/** Свёрнутый исходящий/активный звонок: OUTGOING/RINGING/WAIT_DECISION/ACTIVE, duration берётся из controller.
 * Caller размещает полосу над интерфейсом; callbacks открывают CallScreen/завершают звонок. Incoming/ENDED скрыты.
 */
@Composable
fun ActiveCallBanner(
    viewModel: ChatViewModel,
    onOpen: () -> Unit,
    onHangup: () -> Unit
) {
    val st = viewModel.callState.collectAsState().value ?: return
    val phase = st.phase
    if (phase != CallPhase.OUTGOING && phase != CallPhase.RINGING && phase != CallPhase.ACTIVE && phase != CallPhase.WAIT_DECISION) return

    val name = st.peerName.ifEmpty { st.peerAddress ?: st.channelId }
    val status = when (phase) {
        CallPhase.ACTIVE -> stringResource(R.string.call_talking_prefix) + fmtDuration(st.durationSeconds)
        CallPhase.RINGING -> stringResource(R.string.call_ringing)
        CallPhase.WAIT_DECISION -> stringResource(R.string.phone_busy_warning)
        else -> stringResource(R.string.call_connecting)
    }

    Row(
        modifier = Modifier
            .fillMaxWidth()
            .statusBarsPadding()
            .background(Color(0xFF1F6FEB))
            .clickable(onClick = onOpen)
            .padding(start = 12.dp, end = 4.dp, top = 6.dp, bottom = 6.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Icon(
            Icons.Filled.PhoneInTalk,
            contentDescription = stringResource(R.string.call_noun),
            tint = Color.White,
            modifier = Modifier.size(22.dp)
        )
        Spacer(Modifier.width(12.dp))
        Column(modifier = Modifier.weight(1f)) {
            Text(
                name,
                color = Color.White,
                fontSize = 14.sp,
                fontWeight = FontWeight.Medium,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis
            )
            Text(
                status,
                color = Color(0xFFD5E4FF),
                fontSize = 12.sp,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis
            )
        }
        Spacer(Modifier.width(8.dp))
        Box(
            modifier = Modifier.size(40.dp).background(Color(0xFFB32632), CircleShape),
            contentAlignment = Alignment.Center
        ) {
            IconButton(onClick = onHangup) {
                Icon(Icons.Filled.CallEnd, stringResource(R.string.hang_up), tint = Color.White, modifier = Modifier.size(22.dp))
            }
        }
    }
}

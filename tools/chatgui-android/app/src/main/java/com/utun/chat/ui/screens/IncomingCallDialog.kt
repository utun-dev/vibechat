package com.utun.chat.ui.screens

import com.utun.chat.R
import androidx.compose.ui.res.stringResource
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Call
import androidx.compose.material.icons.filled.CallEnd
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.utun.chat.viewmodel.ChatViewModel
import com.utun.chat.data.CallController
import com.utun.chat.data.StreamController
import com.utun.chat.data.StreamPlayerEngine

/**
 * Входящий звонок — inline-оверлей внутри окна активити (не отдельное Dialog-окно):
 * так он отрисовывается и поверх keyguard при пробуждении полноэкранным intent'ом.
 * Вызывающий экран показывает его только для INCOMING. preparing запрещает повторное принятие, отклонение доступно всегда.
 */
@Composable
fun IncomingCallDialog(
    viewModel: ChatViewModel
) {
    val st = viewModel.callState.collectAsState().value ?: return
    val name = st.peerName.ifEmpty { st.peerAddress ?: st.channelId }
    val radio = viewModel.radioAudio.activeGroup.collectAsState().value
    val publisher = StreamController.state.collectAsState().value
    val viewing = StreamPlayerEngine.viewing.collectAsState().value

    Box(
        modifier = Modifier.fillMaxSize().background(Color(0xCC000000)),
        contentAlignment = Alignment.Center
    ) {
        Surface(
            shape = MaterialTheme.shapes.large,
            color = Color(0xFF1B2733)
        ) {
            Column(
                modifier = Modifier.padding(32.dp),
                horizontalAlignment = Alignment.CenterHorizontally
            ) {
                Box(
                    modifier = Modifier.size(80.dp).background(Color(0xFF1F6FEB), CircleShape),
                    contentAlignment = Alignment.Center
                ) {
                    Text(String(Character.toChars(name.codePointAt(0))).uppercase(), fontSize = 34.sp,
                        color = Color.White, fontWeight = FontWeight.Bold)
                }
                Spacer(Modifier.height(20.dp))
                Text(name, color = Color.White, fontSize = 22.sp, fontWeight = FontWeight.Medium)
                Spacer(Modifier.height(6.dp))
                Text(stringResource(R.string.incoming_call_waiting), color = Color(0xFFAEBCC8), fontSize = 14.sp)
                Spacer(Modifier.height(6.dp))
                Text(stringResource(R.string.connection_label, st.connectionType.label), color = Color(0xFFAEBCC8), fontSize = 13.sp)
                if (radio != null || publisher?.ongoing == true || viewing) {
                    Spacer(Modifier.height(12.dp))
                    if (publisher?.ongoing == true) Text(stringResource(R.string.phone_call_stop_publish), color = Color(0xFFFFCC80))
                    if (viewing) Text(stringResource(R.string.phone_call_stop_view), color = Color(0xFFFFCC80))
                    if (radio != null) Text(stringResource(R.string.call_incoming_stop_radio), color = Color(0xFFFFCC80))
                }
                if (st.preparing) {
                    Spacer(Modifier.height(12.dp))
                    Text(stringResource(R.string.call_preparing_audio), color = Color.White, fontSize = 14.sp)
                }
                Spacer(Modifier.height(28.dp))

                Row(horizontalArrangement = Arrangement.spacedBy(48.dp)) {
                    Column(horizontalAlignment = Alignment.CenterHorizontally) {
                        Box(
                            modifier = Modifier.size(64.dp).background(Color(0xFFB32632), CircleShape),
                            contentAlignment = Alignment.Center
                        ) {
                            IconButton(onClick = { viewModel.declineCall() }) {
                                Icon(Icons.Filled.CallEnd, stringResource(R.string.decline), tint = Color.White, modifier = Modifier.size(30.dp))
                            }
                        }
                        Text(stringResource(R.string.decline), color = Color(0xFFAEBCC8), fontSize = 12.sp)
                    }
                    Column(horizontalAlignment = Alignment.CenterHorizontally) {
                        Box(
                            modifier = Modifier.size(64.dp).background(Color(0xFF43A047), CircleShape),
                            contentAlignment = Alignment.Center
                        ) {
                            IconButton(onClick = { viewModel.acceptCall() }, enabled = !st.preparing && !st.answering) {
                                Icon(Icons.Filled.Call, stringResource(R.string.accept), tint = Color.White, modifier = Modifier.size(30.dp))
                            }
                        }
                        Text(stringResource(R.string.accept), color = Color(0xFFAEBCC8), fontSize = 12.sp)
                    }
                }
            }
        }
    }
}

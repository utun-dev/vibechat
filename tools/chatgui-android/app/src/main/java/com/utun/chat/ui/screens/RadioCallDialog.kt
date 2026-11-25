package com.utun.chat.ui.screens

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import com.utun.chat.R
import com.utun.chat.data.RadioCallController
import com.utun.chat.data.RadioCallPhase
import com.utun.chat.data.RadioCallState
import com.utun.chat.data.StreamController
import com.utun.chat.data.StreamPlayerEngine
import com.utun.chat.ui.components.PortableRadioIcon

/** Полноэкранная карточка снимка RadioCallState внутри Activity; режим keyguard задаёт сама Activity.
 * INCOMING предлагает accept/decline, WAIT_DECISION — продолжить/завершить. Во время PREPARING кнопок нет:
 * Activity возвращает фактический результат включения рации через activated. ENDED показывает причину
 * до удаления карточки контроллером. Компонент не владеет звуком, сигналингом или deadline вызова. */
@Composable
fun RadioCallDialog(state: RadioCallState) {
    val viewing = StreamPlayerEngine.viewing.collectAsState().value
    val publishing = StreamController.state.collectAsState().value?.ongoing == true
    Box(Modifier.fillMaxSize().background(Color(0xCC000000)), contentAlignment = Alignment.Center) {
        Surface(shape = MaterialTheme.shapes.large) {
            Column(Modifier.padding(28.dp), horizontalAlignment = Alignment.CenterHorizontally) {
                PortableRadioIcon(stringResource(R.string.radio_call))
                Text(stringResource(if (state.incoming) R.string.radio_call_incoming else R.string.radio_call))
                Spacer(Modifier.height(16.dp))
                Text(state.peerName, style = MaterialTheme.typography.titleLarge)
                Text(stringResource(R.string.radio_call_channel, state.channelName))
                Spacer(Modifier.height(12.dp))
                Text(stringResource(when (state.phase) {
                    RadioCallPhase.OUTGOING -> R.string.radio_call_checking
                    RadioCallPhase.INCOMING -> R.string.radio_call_waiting
                    RadioCallPhase.RINGING -> R.string.radio_call_ringing
                    RadioCallPhase.PREPARING, RadioCallPhase.ANSWERING -> R.string.radio_call_preparing
                    RadioCallPhase.WAIT_DECISION -> R.string.phone_busy_warning
                    RadioCallPhase.ENDED -> RadioCallController.reasonText(state.reason)
                }))
                Spacer(Modifier.height(20.dp))
                if (state.incoming && state.phase == RadioCallPhase.INCOMING) {
                    if (viewing) Text(stringResource(R.string.radio_call_stop_view), color = Color(0xFFFFCC80))
                    if (publishing) Text(stringResource(R.string.radio_call_keep_stream))
                }
                if (state.phase != RadioCallPhase.ENDED && state.phase != RadioCallPhase.PREPARING) {
                    Row(horizontalArrangement = Arrangement.spacedBy(16.dp)) {
                        TextButton(onClick = { RadioCallController.end(state.id) }) {
                            Text(stringResource(if (state.phase == RadioCallPhase.INCOMING) R.string.decline
                                else if (state.phase == RadioCallPhase.WAIT_DECISION) R.string.finish_call else android.R.string.cancel))
                        }
                        if (state.phase == RadioCallPhase.INCOMING) {
                            Button(onClick = RadioCallController::accept) { Text(stringResource(R.string.accept)) }
                        } else if (state.phase == RadioCallPhase.WAIT_DECISION) {
                            Button(onClick = { RadioCallController.continueCall(state.id) }) { Text(stringResource(R.string.call_anyway)) }
                        }
                    }
                }
            }
        }
    }
}

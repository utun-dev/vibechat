package com.utun.chat.ui.screens

import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothManager
import android.content.Intent
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalLifecycleOwner
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.Lifecycle
import com.utun.chat.ChatApplication
import com.utun.chat.R
import com.utun.chat.data.BleRadio
import com.utun.chat.data.LogManager
import com.utun.chat.data.NativeLib
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.withContext
import org.json.JSONArray

private data class BluetoothPeer(val id: String, val name: String, val address: String, val rssi: Int, val broadcasts: Long)

/** Снимок DataStore без чтения диска на main; принятые записи принадлежат Application.
 * Разрешения/включение адаптера подтверждаются Android, ошибки запуска показываются в этой вкладке. */
@Composable
fun BluetoothSettingsTab() {
    val app = ChatApplication.instance
    val provider = app.configProvider
    val settings = remember(provider) { provider.changes.observe("Bluetooth settings", provider.bluetoothSettings) }
    val confirmedFlags by settings.collectAsState(initial = null)
    val inviteSettings = remember(provider) { provider.changes.observe("Bluetooth invitations", provider.bluetoothInvitesSettings) }
    val receiveInvites by inviteSettings.collectAsState(initial = null)
    val flags = confirmedFlags ?: 0
    var denied by remember { mutableStateOf(false) }
    var failed by remember { mutableStateOf(false) }
    val settingFailed by provider.changes.failed.collectAsState()
    val state by BleRadio.state.collectAsState()
    val advertisingState by BleRadio.advertisingState.collectAsState()
    val advertisingPower by BleRadio.advertisingPowerDbm.collectAsState()
    val lifecycle by LocalLifecycleOwner.current.lifecycle.currentStateFlow.collectAsState()
    var peers by remember { mutableStateOf(emptyList<BluetoothPeer>()) }
    var peersFailed by remember { mutableStateOf(false) }
    LaunchedEffect(state, lifecycle) {
        if (state != 1) { peers = emptyList(); peersFailed = false; return@LaunchedEffect }
        if (!lifecycle.isAtLeast(Lifecycle.State.STARTED)) return@LaunchedEffect
        while (isActive) {
            try {
                peers = withContext(Dispatchers.IO) {
                    val snapshot = JSONArray(NativeLib.getBluetoothPeers())
                    List(snapshot.length()) { i ->
                        val peer = snapshot.getJSONObject(i)
                        BluetoothPeer(peer.getString("id"), peer.getString("name"), peer.getString("address"),
                            peer.getInt("rssi"), peer.getLong("broadcasts"))
                    }.sortedWith(compareBy<BluetoothPeer> { it.name }.thenBy { it.id })
                }
                peersFailed = false
            } catch (e: CancellationException) { throw e }
            catch (e: Exception) {
                peersFailed = true
                LogManager.addLog("WARN", "discovery", "BLE devices snapshot failed: $e")
            }
            delay(1000)
        }
    }
    fun save(bit: Int, enabled: Boolean) { provider.changes.submit("Bluetooth settings") { provider.setBleFlag(bit, enabled) } }
    val enable = rememberLauncherForActivityResult(ActivityResultContracts.StartActivityForResult()) {
        if (it.resultCode == android.app.Activity.RESULT_OK) { denied = false; save(1, true) } else denied = true
    }
    fun activate() {
        try {
            failed = false
            if (app.getSystemService(BluetoothManager::class.java)?.adapter?.isEnabled == true) { denied = false; save(1, true) }
            else enable.launch(Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE))
        } catch (e: Exception) { failed = true; LogManager.addLog("ERROR", "discovery", "Bluetooth enable request failed: $e") }
    }
    val permissions = rememberLauncherForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) {
        try { if (BleRadio.permitted()) activate() else denied = true }
        catch (e: Exception) { failed = true; LogManager.addLog("ERROR", "discovery", "Bluetooth permission result failed: $e") }
    }
    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
            Text(stringResource(R.string.ble_enabled))
            Switch(enabled = confirmedFlags != null, checked = flags and 1 != 0, onCheckedChange = {
                try {
                    failed = false
                    if (!it) save(1, false)
                    else if (BleRadio.permitted()) activate() else permissions.launch(BleRadio.permissions())
                } catch (e: Exception) { failed = true; LogManager.addLog("ERROR", "discovery", "Bluetooth permission request failed: $e") }
            })
        }
        listOf(2 to R.string.ble_advertise, 4 to R.string.ble_incoming).forEach { (bit, label) ->
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween, verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text(stringResource(label))
                    Text(stringResource(if (bit == 2) R.string.ble_advertise_hint else R.string.ble_incoming_hint),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
                Switch(enabled = confirmedFlags != null, checked = flags and bit != 0,
                    onCheckedChange = { save(bit, it) })
            }
        }
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween, verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text(stringResource(R.string.ble_invites_enabled))
                Text(stringResource(R.string.ble_invites_hint), style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            Switch(enabled = receiveInvites != null, checked = receiveInvites == true, onCheckedChange = { enabled ->
                provider.changes.submit("Bluetooth invitations") {
                    val value = if (enabled) "1" else "0"
                    provider.setValue("chat.bluetooth_invites_enabled", value)
                    if (NativeLib.isReady()) NativeLib.setChatSetting("bluetooth_invites_enabled", value)
                }
            })
        }
        Text(stringResource(R.string.ble_hint), style = MaterialTheme.typography.bodySmall)
        Text(stringResource(when (state) { 0 -> R.string.ble_starting; 1 -> R.string.ble_ready; else -> R.string.ble_stopped }))
        if (denied) Text(stringResource(R.string.ble_permission), color = MaterialTheme.colorScheme.error)
        if (confirmedFlags == null) LinearProgressIndicator(Modifier.fillMaxWidth())
        if (settingFailed) Text(stringResource(R.string.settings_update_failed), color = MaterialTheme.colorScheme.error)
        if (failed || (state == 1 && flags and 2 != 0 && advertisingState == 2)) Text(stringResource(R.string.ble_failed), color = MaterialTheme.colorScheme.error)
        if (state == 1) {
            Text(stringResource(R.string.ble_power_requested, BleRadio.advertisingPowerRequestDbm), style = MaterialTheme.typography.bodySmall)
            Text(if (advertisingPower == null) stringResource(R.string.ble_power_dbm_unavailable)
                else stringResource(R.string.ble_power_selected, advertisingPower!!), style = MaterialTheme.typography.bodySmall)
            Text(stringResource(R.string.ble_power_connection_unavailable), style = MaterialTheme.typography.bodySmall)
            HorizontalDivider()
            Text(stringResource(R.string.ble_devices), style = MaterialTheme.typography.titleMedium)
            if (peersFailed) Text(stringResource(R.string.ble_devices_failed), color = MaterialTheme.colorScheme.error)
            else if (peers.isEmpty()) Text(stringResource(R.string.ble_devices_empty), style = MaterialTheme.typography.bodySmall)
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                peers.forEach { peer ->
                    key(peer.id) {
                        Row(Modifier.fillMaxWidth().padding(vertical = 3.dp),
                            horizontalArrangement = Arrangement.spacedBy(10.dp), verticalAlignment = Alignment.CenterVertically) {
                            Column(Modifier.weight(1f)) {
                                Text(peer.name.ifEmpty { peer.id }, style = MaterialTheme.typography.bodyMedium,
                                    lineHeight = 16.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
                                Text(stringResource(R.string.ble_broadcast_received, peer.broadcasts),
                                    color = MaterialTheme.colorScheme.onSurfaceVariant, style = MaterialTheme.typography.bodySmall,
                                    lineHeight = 16.sp, maxLines = 1, softWrap = false)
                            }
                            SelectionContainer {
                                Column {
                                    Text(peer.id, color = MaterialTheme.colorScheme.primary, fontFamily = FontFamily.Monospace,
                                        style = MaterialTheme.typography.bodySmall, lineHeight = 16.sp, maxLines = 1, softWrap = false)
                                    Text(peer.address, color = MaterialTheme.colorScheme.tertiary, fontFamily = FontFamily.Monospace,
                                        style = MaterialTheme.typography.bodySmall, lineHeight = 16.sp, maxLines = 1, softWrap = false)
                                }
                            }
                            Column(Modifier.widthIn(min = 64.dp), horizontalAlignment = Alignment.End) {
                                Text("${if (peer.rssi == 127) "—" else peer.rssi} dBm", color = MaterialTheme.colorScheme.secondary,
                                    style = MaterialTheme.typography.bodySmall, lineHeight = 16.sp, maxLines = 1, softWrap = false)
                                Text(stringResource(R.string.ble_lqi_unavailable), color = MaterialTheme.colorScheme.onSurfaceVariant,
                                    style = MaterialTheme.typography.bodySmall, lineHeight = 16.sp, maxLines = 1, softWrap = false)
                            }
                        }
                    }
                }
            }
        }
    }
}

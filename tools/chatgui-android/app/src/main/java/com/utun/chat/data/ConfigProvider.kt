package com.utun.chat.data

import android.content.Context
import android.os.Build
import androidx.datastore.preferences.core.booleanPreferencesKey
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.intPreferencesKey
import androidx.datastore.preferences.core.floatPreferencesKey
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.datastore.preferences.core.stringSetPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.Deferred
import java.net.Inet4Address
import java.net.Inet6Address
import java.net.InetAddress
import java.net.NetworkInterface
import java.io.File
import java.io.IOException

private val Context.dataStore by preferencesDataStore("utun_config")

data class VoiceSettings(
    val preset: Int = 1,
    val compressorEnabled: Boolean = true,
    val maxGainDb: Int = 20,
    val lookbackMs: Int = 200,
    val lookaheadMs: Int = 100,
    val riseRate: Int = 6,
    val targetLevelDb: Int = -3,
    val gainOnlyOnSpeech: Boolean = false,
    val vadMaxGainDb: Int = 10
)

data class RadioCompressorSettings(val enabled: Boolean = true, val maxGainDb: Int = 20, val riseRate: Int = 6)

/**
 * Поставщик Application: getString/getInt остаются синхронным API для JNI.
 * Файловые операции возвращают Deferred общей очереди; await не блокирует UI и не передаёт ожидающему владение запросом.
 * При формировании конфига настройки и keys.conf читаются один раз; чтение/запись utun.cfg сериализованы.
 * UI принимает изменения через changes: сначала DataStore commit, затем runtime. Наблюдение через changes
 * сохраняет подтверждённый снимок UI при ошибке чтения; Bluetooth-флаги меняются атомарно по отдельному биту.
 */
class ConfigProvider(private val context: Context) {
    private val streamLowLatencyKey = booleanPreferencesKey("stream_low_latency")
    val streamLowLatency: Flow<Boolean> = context.dataStore.data.map { it[streamLowLatencyKey] ?: false }.distinctUntilChanged()
    suspend fun saveStreamLowLatency(low: Boolean) {
        try { context.dataStore.edit { it[streamLowLatencyKey] = low } }
        catch (e: Exception) { LogManager.addLog("ERROR", "stream", "Playback policy save failed: $e"); throw e }
    }
    val changes = SettingsOperations()
    private val configFiles = ConfigFileStore { level, message -> LogManager.addLog(level, "CFG", message) }
    private val bleFlags = intPreferencesKey("bluetooth_flags")
    val bluetoothSettings: Flow<Int> = context.dataStore.data.map { preferences ->
        val flags = preferences[bleFlags] ?: DEFAULT_BLE_FLAGS
        if (flags !in 0..7) LogManager.addLog("WARN", "discovery", "Invalid stored BLE flags=$flags; disabling Bluetooth")
        flags.takeIf { it in 0..7 } ?: 0
    }.distinctUntilChanged()
    suspend fun setBleFlag(bit: Int, enabled: Boolean) {
        require(bit in setOf(1, 2, 4))
        context.dataStore.edit {
            val flags = (it[bleFlags] ?: DEFAULT_BLE_FLAGS).takeIf { value -> value in 0..7 } ?: 0
            it[bleFlags] = if (enabled) flags or bit else flags and bit.inv()
        }
        LogManager.addLog("INFO", "discovery", "BLE setting bit=$bit enabled=$enabled")
    }
    private val nodePrivateKey = stringPreferencesKey("node_private_key")
    private val nodePublicKey = stringPreferencesKey("node_public_key")
    private val nodeName = stringPreferencesKey("node_name")
    private val dbPath = stringPreferencesKey("db_path")
    private val debugConsoleLevel = stringPreferencesKey("debug_console_level")
    private val debugLogFile = stringPreferencesKey("debug_log_file")
    private val debugCategories = stringPreferencesKey("debug_categories")
    private val logUdpIp = stringPreferencesKey("log_udp_ip")
    private val logUdpPort = intPreferencesKey("log_udp_port")
    private val logUdpEnabled = booleanPreferencesKey("log_udp_enabled")
    private val chatStorageAutoload = intPreferencesKey("chat_storage_autoload")
    private val chatStorageAutoloadMaxsizeMb = intPreferencesKey("chat_storage_autoload_maxsize_mb")
    private val chatStorageMaxsizeGb = intPreferencesKey("chat_storage_maxsize_gb")
    private val chatOpusCodecPreset = intPreferencesKey("chat_opus_codec_preset")
    private val streamSubblocks = intPreferencesKey("stream_subblocks")
    private val streamRsRedundant = intPreferencesKey("stream_rs_redundant")
    private val streamStallTimeout = intPreferencesKey("stream_stall_timeout_ms")
    private val streamMaxLiveLag = intPreferencesKey("stream_max_live_lag_ms")
    private val streamBudgetKeys = StreamFecParameters.budgets.associate { it.key to intPreferencesKey(it.key) }
    private val streamCamera = stringPreferencesKey("stream_capture_camera")
    private val streamWidth = intPreferencesKey("stream_capture_width")
    private val streamHeight = intPreferencesKey("stream_capture_height")
    private val streamFps = intPreferencesKey("stream_capture_fps")
    private val streamCodec = intPreferencesKey("stream_capture_codec")
    private val streamRotation = intPreferencesKey("stream_capture_rotation")
    private val streamBitrate = intPreferencesKey("stream_capture_bitrate")
    private val streamAudio = booleanPreferencesKey("stream_capture_audio")
    private val photoCamera = stringPreferencesKey("photo_capture_camera")
    private val photoWidth = intPreferencesKey("photo_capture_width")
    private val photoHeight = intPreferencesKey("photo_capture_height")
    private val photoJpegQuality = intPreferencesKey("photo_capture_jpeg_quality")
    private val photoRotation = intPreferencesKey("photo_capture_rotation")
    private val videoCamera = stringPreferencesKey("video_capture_camera")
    private val videoHeight = intPreferencesKey("video_capture_height")
    private val videoCodec = intPreferencesKey("video_capture_codec")
    private val videoBitrate = intPreferencesKey("video_capture_bitrate")
    private val videoRotation = intPreferencesKey("video_capture_rotation")
    private val callVideoCamera = stringPreferencesKey("call_video_camera")
    private val callVideoWidth = intPreferencesKey("call_video_width")
    private val callVideoHeight = intPreferencesKey("call_video_height")
    private val callVideoFps = intPreferencesKey("call_video_fps")
    private val callVideoCodec = intPreferencesKey("call_video_codec")
    private val callJitterMaxReserveMs = intPreferencesKey("call_jitter_max_reserve_ms")
    private val callVideoBitrateRange = stringPreferencesKey("call_video_bitrate_range_kbps")
    private val chatCompressorEnabled = intPreferencesKey("chat_compressor_enabled")
    private val noiseSuppressionEnabledKey = intPreferencesKey("chat_noise_suppression_enabled")
    private val noiseSuppressionDbKey = intPreferencesKey("chat_noise_suppression_db")
    private val chatCompressorMaxGainDb = intPreferencesKey("chat_compressor_max_gain_db")
    private val chatCompressorRiseRate = intPreferencesKey("chat_compressor_rise_rate")
    private val chatCompressorLookbackMs = intPreferencesKey("chat_compressor_lookback_ms")
    private val chatCompressorLookaheadMs = intPreferencesKey("chat_compressor_lookahead_ms")
    private val chatCompressorTargetLevelDb = intPreferencesKey("chat_compressor_target_level_db")
    private val compressorGainOnlyOnSpeech = booleanPreferencesKey("compressor_gain_only_on_speech")
    private val chatMediaDownloadMaxPeers = intPreferencesKey("chat_media_download_max_peers")
    private val chatStandbyActiveSec = intPreferencesKey("chat_standby_active_sec")
    private val chatStandbySleepSec = intPreferencesKey("chat_standby_sleep_sec")
    private val chatStandbyMinSleepSec = intPreferencesKey("chat_standby_min_sleep_sec")
    private val chatStandbyKeepaliveMs = intPreferencesKey("chat_standby_keepalive_ms")
    private val chatGroupAutoconnect = intPreferencesKey("chat_group_autoconnect")
    private val chatBluetoothInvitesEnabled = intPreferencesKey("chat_bluetooth_invites_enabled")
    val bluetoothInvitesSettings: Flow<Boolean> = context.dataStore.data.map { (it[chatBluetoothInvitesEnabled] ?: 0) != 0 }.distinctUntilChanged()
    private val chatLocalInvitesEnabled = intPreferencesKey("chat_local_invites_enabled")
    private val chatLocalBroadcastsEnabled = intPreferencesKey("chat_local_broadcasts_enabled")
    private val firstLaunchDone = booleanPreferencesKey("first_launch_done")
    private val connectionModeKey = stringPreferencesKey("connection_mode")
    private val connectionOrderKey = stringPreferencesKey("connection_order")
    private val hideMySocketsKey = booleanPreferencesKey("hide_my_sockets")
    private val autoSocketProtocolsKey = stringPreferencesKey("auto_socket_protocols")
    private val serviceEnabledKey = booleanPreferencesKey("service_enabled")
    private val guiUtunEnabled = intPreferencesKey("gui_utun_enabled")
    private val startOnBootKey = booleanPreferencesKey("start_on_boot")
    private val soundEnabledKey = booleanPreferencesKey("sound_enabled")
    // Порядок совпадает с CALL_POLICY_* ядра: показ, предупреждение, отказ при занятости, запрет звонков, запрет рации.
    private val incomingCallKeys = listOf("phone_status_enabled", "phone_notify_enabled", "phone_reject_enabled",
        "block_incoming_calls", "block_incoming_radio").map(::booleanPreferencesKey)
    val incomingCallOptions: Flow<List<Boolean>> = context.dataStore.data.map { prefs ->
        incomingCallKeys.map { prefs[it] ?: false }
    }.distinctUntilChanged()

    suspend fun setIncomingCallOption(option: Int, enabled: Boolean) {
        require(option in incomingCallKeys.indices)
        context.dataStore.edit { it[incomingCallKeys[option]] = enabled }
        LogManager.addLog("INFO", "PhoneState", "${incomingCallKeys[option].name}=$enabled")
    }
    private val mutedChannelsKey = stringSetPreferencesKey("muted_channels")
    private val callAudioRouteKey = stringPreferencesKey("call_audio_route")
    private val proximityEnabledKey = booleanPreferencesKey("proximity_enabled")
    private val proximityThresholdKey = floatPreferencesKey("proximity_threshold_cm")
    /** Использование датчика и порог в сантиметрах; по умолчанию включён, 5 см. */
    val proximitySettings: Flow<Pair<Boolean, Float>> = context.dataStore.data.map { prefs ->
        val threshold = prefs[proximityThresholdKey] ?: 5f
        val valid = threshold.isFinite() && threshold in 0.1f..10f
        if (!valid) LogManager.addLog("WARN", "Proximity", "Invalid saved threshold=$threshold; using 5 cm")
        (prefs[proximityEnabledKey] ?: true) to if (valid) threshold else 5f
    }.distinctUntilChanged()

    suspend fun setProximityEnabled(enabled: Boolean) {
        context.dataStore.edit { it[proximityEnabledKey] = enabled }
        LogManager.addLog("INFO", "Proximity", "Setting saved enabled=$enabled")
    }

    suspend fun setProximityThreshold(thresholdCm: Float) {
        if (!thresholdCm.isFinite() || thresholdCm !in 0.1f..10f) {
            LogManager.addLog("ERROR", "Proximity", "Invalid threshold=$thresholdCm cm")
            throw IllegalArgumentException("Proximity threshold must be within 0.1..10 cm")
        }
        context.dataStore.edit { it[proximityThresholdKey] = thresholdCm }
        LogManager.addLog("INFO", "Proximity", "Setting saved threshold=$thresholdCm cm")
    }
    private val mediaAudioProfileKey = stringPreferencesKey("media_audio_profile")
    private val radioAutoEnabledKey = intPreferencesKey("radio_auto_enabled")
    private val radioAutoModeKey = intPreferencesKey("radio_auto_mode")
    private val radioLevelOnKey = intPreferencesKey("radio_level_on_dbfs")
    private val radioLevelOffKey = intPreferencesKey("radio_level_off_dbfs")
    private val radioVadThresholdKey = intPreferencesKey("radio_vad_threshold")
    private val radioAutoHangoverMsKey = intPreferencesKey("radio_auto_hangover_ms")
    private val radioVadCompressorMaxGainDbKey = intPreferencesKey("radio_vad_compressor_max_gain_db")
    private val radioVolumePttKey = booleanPreferencesKey("radio_volume_ptt")
    private val radioOverlayKey = booleanPreferencesKey("radio_overlay")
    private val radioKeepMicrophoneReadyKey = booleanPreferencesKey("radio_keep_microphone_ready")
    private val radioMicSourceKey = intPreferencesKey("radio_mic_source")
    private val radioOpusModeKey = intPreferencesKey("radio_opus_mode")
    private val radioOpusBitrateKey = intPreferencesKey("radio_opus_bitrate")
    private val radioBurstFramesKey = intPreferencesKey("radio_burst_frames")
    private val radioCaptureStereoKey = intPreferencesKey("radio_capture_stereo")
    private val radioCompressorEnabledKey = intPreferencesKey("radio_compressor_enabled")
    private val radioAecEnabledKey = intPreferencesKey("radio_aec_enabled")
    private val radioCompressorMaxGainDbKey = intPreferencesKey("radio_compressor_max_gain_db")

    // Трансляция наблюдает те же настройки TX, что и рация; история PCM у каждого владельца своя.
    val radioCompressorSettings: Flow<RadioCompressorSettings> = context.dataStore.data.map { prefs ->
        RadioCompressorSettings((prefs[radioCompressorEnabledKey] ?: 1) != 0,
            prefs[radioCompressorMaxGainDbKey] ?: 20, prefs[chatCompressorRiseRate] ?: 6)
    }.distinctUntilChanged()

    private fun readKeysFromFile(f: File): Pair<String, String>? {
        if (!f.exists()) return null
        try {
            val lines = f.readLines()
            var pub = ""; var priv = ""
            for (l in lines) {
                when {
                    l.startsWith("public_key=") -> pub = l.removePrefix("public_key=").trim()
                    l.startsWith("private_key=") -> priv = l.removePrefix("private_key=").trim()
                }
            }
            if (pub.length == 64 && priv.length == 64 && (pub + priv).all { it in '0'..'9' || it in 'a'..'f' || it in 'A'..'F' })
                return Pair(pub, priv)
            LogManager.addLog("WARN", "CFG", "keys file has malformed key pair path=${f.path}")
        } catch (e: Exception) {
            LogManager.addLog("ERROR", "CFG", "keys file read failed path=${f.path}: ${e.javaClass.simpleName}: ${e.message}")
        }
        return null
    }

    fun getDeviceName(): String = Build.MODEL.ifEmpty { "Android" }

    val allValues: Flow<Map<String, String>> = context.dataStore.data.map { prefs ->
        mapOf(
            "node.private_key" to (prefs[nodePrivateKey] ?: ""),
            "node.public_key" to (prefs[nodePublicKey] ?: ""),
            "node.name" to (prefs[nodeName] ?: getDeviceName()),
            "gui.utun_enabled" to (prefs[guiUtunEnabled]?.toString() ?: "0"),
            "control.port" to MonitoringSettings.read(prefs).port.toString(),
            "db.path" to (prefs[dbPath] ?: context.filesDir.absolutePath),
            "debug.console_level" to (prefs[debugConsoleLevel] ?: "info"),
            "debug.log_file" to (prefs[debugLogFile] ?: "utun.log"),
            "debug.categories" to (prefs[debugCategories] ?: ""),
            "log_udp.enabled" to if (prefs[logUdpEnabled] ?: false) "1" else "0",
            "log_udp.ip" to (prefs[logUdpIp] ?: UdpLogSettings.DEFAULT_IP),
            "log_udp.port" to (prefs[logUdpPort]?.toString() ?: UdpLogSettings.DEFAULT_PORT.toString())
        )
    }

    private fun udpLogSettings(prefs: Preferences): UdpLogSettings = UdpLogSettings(prefs[logUdpEnabled] ?: false,
        prefs[logUdpIp] ?: UdpLogSettings.DEFAULT_IP, prefs[logUdpPort] ?: UdpLogSettings.DEFAULT_PORT)

    suspend fun getUdpLogSettings(): UdpLogSettings = configFiles.submit("load UDP log settings") {
        udpLogSettings(context.dataStore.data.first())
    }.await()

    suspend fun getMonitoringSettings(): MonitoringSettings = configFiles.submit("load monitoring settings") {
        MonitoringSettings.read(context.dataStore.data.first())
    }.await()

    /** Атомарно сохраняет enabled/endpoint и обновляет INI также при остановленном ядре.
     * Runtime применяет общий restart настроек при выходе с экрана. */
    fun saveMonitoringSettings(settings: MonitoringSettings): Deferred<Unit> = configFiles.submit("save monitoring settings") {
        context.dataStore.edit { settings.writeTo(it) }
        val snapshot = configSnapshot()
        val servers = loadServers(File(snapshot.directory, "utun.cfg"))
        saveConfig(snapshot, servers.active, servers.inactive)
        LogManager.addLog("INFO", "CFG", "Monitoring saved enabled=${settings.enabled} " +
            if (settings.enabled) "endpoint=${settings.ip}:${settings.port}" else "endpoint removed")
    }

    fun saveUdpLogSettings(settings: UdpLogSettings): Deferred<Unit> = configFiles.submit("save UDP log settings") {
        context.dataStore.edit { prefs ->
            prefs[logUdpEnabled] = settings.enabled; prefs[logUdpIp] = settings.ip; prefs[logUdpPort] = settings.port
        }
        LogManager.addLog("INFO", "UDPLog", "Settings saved enabled=${settings.enabled} active=${settings.active} " +
            "target=${settings.ip}:${settings.port}")
    }

    fun getCallVideoBitrateRange(): CallVideoBitrateRange = runBlocking {
        configFiles.submit("load call video bitrate") { videoBitrateRange(context.dataStore.data.first()) }.await()
    }

    private fun videoBitrateRange(prefs: Preferences): CallVideoBitrateRange {
        val value = prefs[callVideoBitrateRange] ?: return CallVideoBitrateRange()
        return CallVideoBitrateRange.parse(value) ?: run {
            LogManager.addLog("WARN", "CallSettings", "Invalid video bitrate range '$value'; using 300:4000")
            CallVideoBitrateRange()
        }
    }

    fun saveCallVideoBitrateRange(range: CallVideoBitrateRange): Deferred<Unit> = configFiles.submit("save call video bitrate") {
        context.dataStore.edit { it[callVideoBitrateRange] = range.encode() }
        LogManager.addLog("INFO", "CallSettings", "Video bitrate range saved: ${range.encode()} kbps")
    }

    /** Очередь Application сохраняет выбор после закрытия экрана и завершает записи перед последующим чтением. */
    suspend fun getPhotoCaptureSettings(): PhotoCaptureSettings = configFiles.submit("load photo capture") {
        val prefs = context.dataStore.data.first()
        try { PhotoCaptureSettings(prefs[photoCamera] ?: "", prefs[photoWidth] ?: 0, prefs[photoHeight] ?: 0, prefs[photoJpegQuality] ?: 85,
            requireNotNull(StreamOrientation.entries.firstOrNull { it.degrees == (prefs[photoRotation] ?: StreamOrientation.AUTO.degrees) })) }
        catch (e: IllegalArgumentException) {
            LogManager.addLog("WARN", "Capture", "Invalid photo settings camera=${prefs[photoCamera]} " +
                "size=${prefs[photoWidth]}x${prefs[photoHeight]} jpegQuality=${prefs[photoJpegQuality]} " +
                "rotation=${prefs[photoRotation]}; using defaults")
            PhotoCaptureSettings()
        }
    }.await()

    fun savePhotoCaptureSettings(settings: PhotoCaptureSettings): Deferred<Unit> = configFiles.submit("save photo capture") {
        context.dataStore.edit { prefs ->
            prefs[photoCamera] = settings.camera; prefs[photoWidth] = settings.width; prefs[photoHeight] = settings.height
            prefs[photoJpegQuality] = settings.jpegQuality
            prefs[photoRotation] = settings.orientation.degrees
        }
        LogManager.addLog("INFO", "Capture", "Photo settings saved: $settings")
    }

    suspend fun getVideoCaptureSettings(): VideoCaptureSettings = configFiles.submit("load video capture") {
        val prefs = context.dataStore.data.first()
        try { VideoCaptureSettings(prefs[videoCamera] ?: "", prefs[videoHeight] ?: 720, prefs[videoCodec] ?: 1, prefs[videoBitrate] ?: 2_000_000,
            requireNotNull(StreamOrientation.entries.firstOrNull { it.degrees == (prefs[videoRotation] ?: StreamOrientation.AUTO.degrees) })) }
        catch (e: IllegalArgumentException) {
            LogManager.addLog("WARN", "Capture", "Invalid video settings camera=${prefs[videoCamera]} " +
                "height=${prefs[videoHeight]} codec=${prefs[videoCodec]} bitrate=${prefs[videoBitrate]} " +
                "rotation=${prefs[videoRotation]}; using defaults")
            VideoCaptureSettings()
        }
    }.await()

    fun saveVideoCaptureSettings(settings: VideoCaptureSettings): Deferred<Unit> = configFiles.submit("save video capture") {
        context.dataStore.edit { prefs ->
            prefs[videoCamera] = settings.camera; prefs[videoHeight] = settings.height
            prefs[videoCodec] = settings.codec; prefs[videoBitrate] = settings.bitrate
            prefs[videoRotation] = settings.orientation.degrees
        }
        LogManager.addLog("INFO", "Capture", "Video settings saved: $settings")
    }

    /** Синхронное чтение только в worker подготовки видеозвонка; enabled остаётся состоянием текущего звонка. */
    fun getCallVideoCaptureSettings(): CallVideoSettings = runBlocking {
        configFiles.submit("load call video capture") {
            val prefs = context.dataStore.data.first()
            CallVideoSettings(camera = prefs[callVideoCamera] ?: "", width = prefs[callVideoWidth] ?: 640,
                height = prefs[callVideoHeight] ?: 480, codec = prefs[callVideoCodec] ?: 0, fps = prefs[callVideoFps] ?: 30)
        }.await()
    }

    fun saveCallVideoCaptureSettings(settings: CallVideoSettings): Deferred<Unit> = configFiles.submit("save call video capture") {
        context.dataStore.edit { prefs ->
            prefs[callVideoCamera] = settings.camera; prefs[callVideoWidth] = settings.width; prefs[callVideoHeight] = settings.height
            prefs[callVideoFps] = settings.fps; prefs[callVideoCodec] = settings.codec
        }
        LogManager.addLog("INFO", "CallSettings", "Video capture settings saved: ${settings.copy(enabled = false)}")
    }

    suspend fun getMediaAudioProfile(): MediaAudioProfile {
        val value = context.dataStore.data.first()[mediaAudioProfileKey]
        return MediaAudioProfile.entries.firstOrNull { it.name == value } ?: MediaAudioProfile.AUTO
    }

    suspend fun setMediaAudioProfile(profile: MediaAudioProfile) {
        context.dataStore.edit { it[mediaAudioProfileKey] = profile.name }
        LogManager.addLog("INFO", "media_audio", "common profile saved=$profile")
    }

    suspend fun saveStreamSettings(subblocks: Int, redundant: Int, stallTimeoutMs: Int, maxLiveLagMs: Int,
                                   budgets: Map<String, Int>? = null) {
        context.dataStore.edit { prefs ->
            val limits = budgets ?: StreamFecParameters.budgets.associate {
                it.key to (prefs[streamBudgetKeys.getValue(it.key)] ?: it.defaultValue)
            }
            if (!StreamFecParameters.validBudgets(limits)
                || !StreamFecParameters.valid(subblocks, redundant, limits["stream_regular_limit"] ?: 0)
                || stallTimeoutMs !in 100..60000 || maxLiveLagMs !in 1000..60000) {
                LogManager.addLog("WARN", "STREAM", "Settings rejected n=$subblocks r=$redundant stall_ms=$stallTimeoutMs lag_ms=$maxLiveLagMs budgets=$limits")
                throw IllegalArgumentException("Invalid stream settings: n=$subblocks r=$redundant budgets=$limits")
            }
            prefs[streamSubblocks] = subblocks
            prefs[streamRsRedundant] = redundant
            prefs[streamStallTimeout] = stallTimeoutMs
            prefs[streamMaxLiveLag] = maxLiveLagMs
            limits.forEach { (key, value) -> prefs[streamBudgetKeys.getValue(key)] = value }
        }
        LogManager.addLog("INFO", "STREAM", "Settings saved n=$subblocks r=$redundant stall_ms=$stallTimeoutMs lag_ms=$maxLiveLagMs budgets=$budgets")
    }

    suspend fun getStreamCaptureSettings(): StreamCaptureSettings = configFiles.submit("load stream capture") {
        val prefs = context.dataStore.data.first()
        StreamCaptureSettings(prefs[streamCamera] ?: "", prefs[streamWidth] ?: 1280, prefs[streamHeight] ?: 720,
            prefs[streamFps] ?: 30, prefs[streamCodec] ?: StreamVideoCodec.H264.id, prefs[streamRotation] ?: StreamOrientation.AUTO.degrees,
            prefs[streamBitrate], prefs[streamAudio] ?: true)
    }.await()

    fun saveStreamCaptureSettings(config: StreamCaptureConfig?, audio: Boolean?): Deferred<Unit> = configFiles.submit("save stream capture") {
        context.dataStore.edit { prefs ->
            config?.let {
                prefs[streamCamera] = it.mode.camera
                prefs[streamWidth] = it.mode.width; prefs[streamHeight] = it.mode.height; prefs[streamFps] = it.mode.fps
                prefs[streamCodec] = it.mode.videoCodec.id; prefs[streamRotation] = it.orientation.degrees
                if (it.videoBitrate == null) prefs.remove(streamBitrate) else prefs[streamBitrate] = it.videoBitrate
            }
            audio?.let { prefs[streamAudio] = it }
        }
        LogManager.addLog("INFO", "stream", "capture settings saved config=$config audio=$audio")
    }

    fun getString(key: String): String? = runBlocking {
        val prefs = context.dataStore.data.first()
        val keys = if (key == "node.private_key" || key == "node.public_key")
            readKeysFromFile(File(prefs[dbPath] ?: context.filesDir.absolutePath, "keys.conf")) else null
        stringValue(key, prefs, keys)
    }

    private fun stringValue(key: String, prefs: Preferences, keys: Pair<String, String>?): String? =
        when (key) {
            "node.private_key" -> keys?.second ?: prefs[nodePrivateKey]
            "node.public_key" -> keys?.first ?: prefs[nodePublicKey]
            "node.name" -> prefs[nodeName] ?: getDeviceName()
            "db.path" -> prefs[dbPath] ?: context.filesDir.absolutePath
            "control.bind_ip" -> MonitoringSettings.read(prefs).ip
            "debug.console_level" -> prefs[debugConsoleLevel] ?: "info"
            "debug.log_file" -> prefs[debugLogFile] ?: "utun.log"
            "debug.categories" -> prefs[debugCategories] ?: ""
            "log_udp.ip" -> prefs[logUdpIp] ?: UdpLogSettings.DEFAULT_IP
            else -> null
        }

    fun getInt(key: String): Int = runBlocking {
        intValue(key, context.dataStore.data.first())
    }

    private fun intValue(key: String, prefs: Preferences): Int =
        when (key) {
            "gui.utun_enabled" -> prefs[guiUtunEnabled] ?: 0
            "control.enabled" -> if (MonitoringSettings.read(prefs).enabled) 1 else 0
            "control.port" -> MonitoringSettings.read(prefs).port
            "log_udp.enabled" -> if (prefs[logUdpEnabled] ?: false) 1 else 0
            "log_udp.port" -> prefs[logUdpPort] ?: UdpLogSettings.DEFAULT_PORT
            "chat.storage_autoload" -> prefs[chatStorageAutoload] ?: 1
            "chat.storage_autoload_maxsize_mb" -> prefs[chatStorageAutoloadMaxsizeMb] ?: 100
            "chat.storage_maxsize_gb" -> prefs[chatStorageMaxsizeGb] ?: 1
            "chat.opus_codec_preset" -> prefs[chatOpusCodecPreset] ?: 1
            "chat.stream_subblocks" -> prefs[streamSubblocks] ?: 8
            "chat.stream_rs_redundant" -> prefs[streamRsRedundant] ?: 0
            "chat.stream_stall_timeout_ms" -> prefs[streamStallTimeout] ?: 500
            "chat.stream_max_live_lag_ms" -> prefs[streamMaxLiveLag] ?: 10000
            "chat.stream_regular_limit", "chat.stream_temporary_limit", "chat.stream_background_limit",
            "chat.stream_peer_temporary_limit", "chat.stream_peer_limit", "chat.stream_control_limit", "chat.stream_extra_receive_limit" -> {
                val budget = StreamFecParameters.budgets.first { "chat.${it.key}" == key }
                prefs[streamBudgetKeys.getValue(budget.key)] ?: budget.defaultValue
            }
            "chat.call_jitter_max_reserve_ms" -> prefs[callJitterMaxReserveMs] ?: 1000
            "chat.compressor_enabled" -> prefs[chatCompressorEnabled] ?: 1
            "chat.noise_suppression_enabled" -> if ((prefs[noiseSuppressionEnabledKey] ?: 1) != 0) 1 else 0
            "chat.noise_suppression_db" -> (prefs[noiseSuppressionDbKey] ?: NoiseSuppressionSettings.DEFAULT_DB)
                .coerceIn(NoiseSuppressionSettings.MIN_DB, NoiseSuppressionSettings.MAX_DB)
            "chat.compressor_max_gain_db" -> prefs[chatCompressorMaxGainDb] ?: 20
            "chat.compressor_rise_rate" -> prefs[chatCompressorRiseRate] ?: 6
            "chat.media_download_max_peers" -> prefs[chatMediaDownloadMaxPeers] ?: 3
            "chat.standby_active_sec" -> prefs[chatStandbyActiveSec] ?: 2
            "chat.standby_sleep_sec" -> prefs[chatStandbySleepSec] ?: 60
            "chat.standby_min_sleep_sec" -> prefs[chatStandbyMinSleepSec] ?: 15
            "chat.standby_keepalive_ms" -> prefs[chatStandbyKeepaliveMs] ?: 30000
            "chat.group_autoconnect" -> prefs[chatGroupAutoconnect] ?: 1
            "chat.bluetooth_invites_enabled" -> prefs[chatBluetoothInvitesEnabled] ?: 0
            "chat.local_invites_enabled" -> prefs[chatLocalInvitesEnabled] ?: 1
            "chat.local_broadcasts_enabled" -> prefs[chatLocalBroadcastsEnabled] ?: 1
            "radio.auto_enabled" -> prefs[radioAutoEnabledKey] ?: 0
            "radio.vad_threshold" -> prefs[radioVadThresholdKey] ?: 50
            "radio.auto_hangover_ms" -> prefs[radioAutoHangoverMsKey] ?: 200
            "radio.vad_compressor_max_gain_db" -> prefs[radioVadCompressorMaxGainDbKey] ?: 10
            "radio.opus_mode" -> prefs[radioOpusModeKey] ?: 0
            "radio.opus_bitrate" -> prefs[radioOpusBitrateKey] ?: 32
            "radio.burst_frames" -> (prefs[radioBurstFramesKey] ?: 4).coerceIn(1, 10)
            "radio.capture_stereo" -> prefs[radioCaptureStereoKey] ?: 0
            "radio.compressor_enabled" -> prefs[radioCompressorEnabledKey] ?: 1
            "radio.aec_enabled" -> prefs[radioAecEnabledKey] ?: 1
            "radio.compressor_max_gain_db" -> prefs[radioCompressorMaxGainDbKey] ?: 20
            else -> 0
        }

    suspend fun setValue(key: String, value: String) {
        require(value.none { it == '\r' || it == '\n' || it == '\u0000' }) {
            LogManager.addLog("ERROR", "CFG", "Setting rejected: control characters key=$key")
            "Configuration value must fit one line"
        }
        context.dataStore.edit { prefs ->
            when (key) {
                "gui.utun_enabled" -> prefs[guiUtunEnabled] = if (value == "1" || value == "yes" || value == "true") 1 else 0
                "node.private_key" -> prefs[nodePrivateKey] = value
                "node.public_key" -> prefs[nodePublicKey] = value
                "node.name" -> prefs[nodeName] = value
                "db.path" -> prefs[dbPath] = value
                "debug.console_level" -> prefs[debugConsoleLevel] = value
                "debug.log_file" -> prefs[debugLogFile] = value
                "debug.categories" -> prefs[debugCategories] = value
                "log_udp.ip" -> prefs[logUdpIp] = value
                "log_udp.port" -> prefs[logUdpPort] = value.toIntOrNull() ?: 0
                "log_udp.enabled" -> prefs[logUdpEnabled] = value == "1" || value == "yes" || value == "true"
                "chat.storage_autoload" -> prefs[chatStorageAutoload] = value.toIntOrNull() ?: 1
                "chat.storage_autoload_maxsize_mb" -> prefs[chatStorageAutoloadMaxsizeMb] = value.toIntOrNull() ?: 100
                "chat.storage_maxsize_gb" -> prefs[chatStorageMaxsizeGb] = value.toIntOrNull() ?: 1
                "chat.opus_codec_preset" -> prefs[chatOpusCodecPreset] = value.toIntOrNull() ?: 1
                "chat.call_jitter_max_reserve_ms" -> prefs[callJitterMaxReserveMs] = (value.toIntOrNull() ?: 1000).coerceIn(500, 2000)
                "chat.compressor_enabled" -> prefs[chatCompressorEnabled] = value.toIntOrNull() ?: 1
                "chat.noise_suppression_enabled" -> prefs[noiseSuppressionEnabledKey] = (value.toIntOrNull() ?: 1).coerceIn(0, 1)
                "chat.noise_suppression_db" -> prefs[noiseSuppressionDbKey] = (value.toIntOrNull() ?: NoiseSuppressionSettings.DEFAULT_DB)
                    .coerceIn(NoiseSuppressionSettings.MIN_DB, NoiseSuppressionSettings.MAX_DB)
                "chat.compressor_max_gain_db" -> prefs[chatCompressorMaxGainDb] = value.toIntOrNull() ?: 20
                "chat.compressor_rise_rate" -> prefs[chatCompressorRiseRate] = value.toIntOrNull() ?: 6
                "chat.media_download_max_peers" -> prefs[chatMediaDownloadMaxPeers] = value.toIntOrNull() ?: 3
                "chat.standby_active_sec" -> prefs[chatStandbyActiveSec] = value.toIntOrNull() ?: 2
                "chat.standby_sleep_sec" -> prefs[chatStandbySleepSec] = value.toIntOrNull() ?: 60
                "chat.standby_min_sleep_sec" -> prefs[chatStandbyMinSleepSec] = value.toIntOrNull() ?: 15
                "chat.standby_keepalive_ms" -> prefs[chatStandbyKeepaliveMs] = value.toIntOrNull() ?: 30000
                "chat.group_autoconnect" -> prefs[chatGroupAutoconnect] = value.toIntOrNull() ?: 1
                "chat.bluetooth_invites_enabled" -> prefs[chatBluetoothInvitesEnabled] = value.toIntOrNull() ?: 0
                "chat.local_invites_enabled" -> prefs[chatLocalInvitesEnabled] = value.toIntOrNull() ?: 1
                "chat.local_broadcasts_enabled" -> prefs[chatLocalBroadcastsEnabled] = value.toIntOrNull() ?: 1
                "radio.auto_enabled" -> prefs[radioAutoEnabledKey] = value.toIntOrNull() ?: 0
                "radio.vad_threshold" -> prefs[radioVadThresholdKey] = value.toIntOrNull() ?: 50
                "radio.auto_hangover_ms" -> prefs[radioAutoHangoverMsKey] = value.toIntOrNull() ?: 200
                "radio.vad_compressor_max_gain_db" -> prefs[radioVadCompressorMaxGainDbKey] = (value.toIntOrNull() ?: 10).coerceIn(1, 60)
                "radio.opus_mode" -> prefs[radioOpusModeKey] = value.toIntOrNull() ?: 0
                "radio.opus_bitrate" -> prefs[radioOpusBitrateKey] = value.toIntOrNull() ?: 32
                "radio.burst_frames" -> {
                    val frames = value.toInt()
                    require(frames in 1..10) { "Radio burst size must be 1..10: $frames" }
                    prefs[radioBurstFramesKey] = frames
                }
                "radio.capture_stereo" -> prefs[radioCaptureStereoKey] = value.toIntOrNull() ?: 0
                "radio.compressor_enabled" -> prefs[radioCompressorEnabledKey] = value.toIntOrNull() ?: 1
                "radio.aec_enabled" -> prefs[radioAecEnabledKey] = (value.toIntOrNull() ?: 1).coerceIn(0, 1)
                "radio.compressor_max_gain_db" -> prefs[radioCompressorMaxGainDbKey] = value.toIntOrNull() ?: 20
                else -> {
                    LogManager.addLog("ERROR", "CFG", "Unknown setting rejected key=$key")
                    throw IllegalArgumentException("Unknown configuration key: $key")
                }
            }
        }
        LogManager.addLog("INFO", "CFG", "Setting saved key=$key")
    }

    private fun radioAutoSettings(prefs: Preferences) = RadioAutoSettings(
        enabled = (prefs[radioAutoEnabledKey] ?: 0) != 0,
        mode = prefs[radioAutoModeKey] ?: RadioAutoSettings.VAD,
        levelOn = prefs[radioLevelOnKey] ?: -35, levelOff = prefs[radioLevelOffKey] ?: -45,
        hangoverMs = prefs[radioAutoHangoverMsKey] ?: 200,
        vadThreshold = prefs[radioVadThresholdKey] ?: 50, vadGain = prefs[radioVadCompressorMaxGainDbKey] ?: 10
    )

    fun getRadioAutoSettings(): RadioAutoSettings = runBlocking { radioAutoSettings(context.dataStore.data.first()) }
    val radioAutoSettings: Flow<RadioAutoSettings> = context.dataStore.data.map { radioAutoSettings(it) }

    suspend fun saveRadioAutoSettings(settings: RadioAutoSettings) {
        if (!settings.valid()) {
            LogManager.addLog("ERROR", "radio", "Invalid automatic transmission settings: $settings")
            throw IllegalArgumentException("Invalid automatic transmission settings")
        }
        context.dataStore.edit { prefs ->
            prefs[radioAutoEnabledKey] = if (settings.enabled) 1 else 0
            prefs[radioAutoModeKey] = settings.mode
            prefs[radioLevelOnKey] = settings.levelOn; prefs[radioLevelOffKey] = settings.levelOff
            prefs[radioAutoHangoverMsKey] = settings.hangoverMs
            prefs[radioVadThresholdKey] = settings.vadThreshold; prefs[radioVadCompressorMaxGainDbKey] = settings.vadGain
        }
        LogManager.addLog("INFO", "radio", "Saved automatic transmission settings: $settings")
    }

    // JNI подтверждает применение в uasync; захват возобновляется только после этого callback.
    @Suppress("unused")
    fun onRadioSettingsApplied(requestId: Long, success: Boolean) {
        RadioAudioEngine.instance().onSettingsApplied(requestId, success)
    }

    fun getNoiseSuppressionSettings(): NoiseSuppressionSettings = runBlocking {
        val prefs = context.dataStore.data.first()
        val rawDb = prefs[noiseSuppressionDbKey] ?: NoiseSuppressionSettings.DEFAULT_DB
        val db = intValue("chat.noise_suppression_db", prefs)
        if (db != rawDb) LogManager.addLog("WARN", "AudioSettings", "Invalid noise suppression normalized: $rawDb -> $db")
        NoiseSuppressionSettings(intValue("chat.noise_suppression_enabled", prefs) != 0, db)
    }

    suspend fun saveNoiseSuppressionSettings(settings: NoiseSuppressionSettings) {
        context.dataStore.edit { prefs ->
            prefs[noiseSuppressionEnabledKey] = if (settings.enabled) 1 else 0
            prefs[noiseSuppressionDbKey] = settings.suppressDb
        }
        LogManager.addLog("INFO", "AudioSettings", "Saved noise suppression: $settings")
    }

    fun getVoiceSettings(): VoiceSettings = runBlocking {
        val prefs = context.dataStore.data.first()
        val defaults = VoiceSettings()
        val raw = VoiceSettings(
            preset = prefs[chatOpusCodecPreset] ?: defaults.preset,
            compressorEnabled = (prefs[chatCompressorEnabled] ?: 1) != 0,
            maxGainDb = prefs[chatCompressorMaxGainDb] ?: defaults.maxGainDb,
            lookbackMs = prefs[chatCompressorLookbackMs] ?: defaults.lookbackMs,
            lookaheadMs = prefs[chatCompressorLookaheadMs] ?: defaults.lookaheadMs,
            riseRate = prefs[chatCompressorRiseRate] ?: defaults.riseRate,
            targetLevelDb = prefs[chatCompressorTargetLevelDb] ?: defaults.targetLevelDb,
            gainOnlyOnSpeech = prefs[compressorGainOnlyOnSpeech] ?: false,
            vadMaxGainDb = prefs[radioVadCompressorMaxGainDbKey] ?: 10
        )
        val settings = raw.copy(preset = raw.preset.coerceIn(0, 2), maxGainDb = raw.maxGainDb.coerceIn(0, 30),
            lookbackMs = raw.lookbackMs.coerceIn(50, 500), lookaheadMs = raw.lookaheadMs.coerceIn(20, 500),
            riseRate = raw.riseRate.coerceIn(2, 10), targetLevelDb = raw.targetLevelDb.coerceIn(-40, 0),
            vadMaxGainDb = raw.vadMaxGainDb.coerceIn(1, 60))
        if (raw != settings) LogManager.addLog("WARN", "AudioSettings", "Invalid saved voice settings normalized: $raw -> $settings")
        LogManager.addLog("INFO", "AudioSettings", "Loaded: $settings")
        settings
    }

    suspend fun saveVoiceSettings(settings: VoiceSettings) {
        context.dataStore.edit { prefs ->
            prefs[chatOpusCodecPreset] = settings.preset
            prefs[chatCompressorEnabled] = if (settings.compressorEnabled) 1 else 0
            prefs[chatCompressorMaxGainDb] = settings.maxGainDb
            prefs[chatCompressorLookbackMs] = settings.lookbackMs
            prefs[chatCompressorLookaheadMs] = settings.lookaheadMs
            prefs[chatCompressorRiseRate] = settings.riseRate
            prefs[chatCompressorTargetLevelDb] = settings.targetLevelDb
            prefs[compressorGainOnlyOnSpeech] = settings.gainOnlyOnSpeech
            prefs[radioVadCompressorMaxGainDbKey] = settings.vadMaxGainDb
        }
        LogManager.addLog("INFO", "AudioSettings", "Saved: $settings")
    }

    private companion object {
        const val DEFAULT_BLE_FLAGS = 7 // Использование BLE, реклама и входящие подключения; приглашения задаются отдельно.
        private val ipv6FlagsWarning = java.util.concurrent.atomic.AtomicBoolean(false)
        /** Accept only global unicast (2000::/3) and unique local (fc00::/7) IPv6. */
        fun isUsableV6(addr: Inet6Address): Boolean {
            if (addr.isLoopbackAddress || addr.isLinkLocalAddress || addr.isMulticastAddress) return false
            val b = addr.address  // 16 bytes
            val hi = ((b[0].toInt() and 0xFF) shl 8) or (b[1].toInt() and 0xFF)
            // fc00::/7 → fc00-fdff,  2000::/3 → 2000-3fff
            return hi in 0xFC00..0xFDFF || hi in 0x2000..0x3FFF
        }

        // 0x20 = IFA_F_TEMPORARY в /proc/net/if_inet6
        fun isTemporaryV6(ifName: String, address: Inet6Address): Boolean {
            return try {
                File("/proc/net/if_inet6").useLines { lines ->
                    temporaryIpv6Address(ifName, address.address, lines)
                }
            } catch (e: Exception) {
                if (ipv6FlagsWarning.compareAndSet(false, true)) LogManager.addLog("WARN", "CFG",
                    "IPv6 temporary flags unavailable; keeping usable IPv6 addresses: ${e.javaClass.simpleName}: ${e.message}")
                false
            }
        }
    }

    /* ── First launch ── */

    fun isFirstLaunch(): Boolean = runBlocking {
        !(context.dataStore.data.first()[firstLaunchDone] ?: false)
    }

    suspend fun completeFirstLaunch(name: String) {
        val nick = name.trim()
        require(nick.isNotEmpty() && nick.none { it.isISOControl() }) { "Invalid device name" }
        context.dataStore.edit { prefs ->
            prefs[nodeName] = nick
            prefs[firstLaunchDone] = true
        }
        LogManager.addLog("INFO", "SETUP", "First launch completed: device name=$nick")
    }

    /* ── Connection mode ── */

    fun getConnectionMode(): String = runBlocking {
        connectionMode(context.dataStore.data.first())
    }

    private fun connectionMode(prefs: Preferences): String {
        val mode = prefs[connectionModeKey] ?: return "auto"
        if (mode == "auto" || mode == "manual") return mode
        LogManager.addLog("WARN", "CFG", "Unknown stored connection mode '$mode'; using auto")
        return "auto"
    }

    suspend fun setConnectionMode(mode: String) {
        require(mode == "auto" || mode == "manual") {
            LogManager.addLog("ERROR", "CFG", "Invalid connection mode rejected: $mode")
            "Unknown connection mode"
        }
        context.dataStore.edit { prefs -> prefs[connectionModeKey] = mode }
        LogManager.addLog("INFO", "CFG", "Connection mode saved: $mode")
    }

    fun getConnectionOrder(): String = runBlocking { context.dataStore.data.first()[connectionOrderKey] ?: "RUT2-2" }

    fun getHideMySockets(): Boolean = runBlocking { context.dataStore.data.first()[hideMySocketsKey] ?: true }

    suspend fun setHideMySockets(value: Boolean) {
        context.dataStore.edit { it[hideMySocketsKey] = value }
        LogManager.addLog("INFO", "CFG", "hide_my_sockets=$value saved")
    }

    suspend fun setConnectionOrder(value: String) {
        require(Regex("[TUR]{1,3}[123](-[1-9][0-9]{0,3})?").matches(value))
        val letters = value.takeWhile { it in "TUR" }
        require(letters.toSet().size == letters.length)
        val suffix = value.drop(letters.length)
        require(suffix == "1" || suffix == "3" ||
            (suffix.startsWith("2-") && suffix.drop(2).toIntOrNull() in 1..3600))
        context.dataStore.edit { it[connectionOrderKey] = value }
        LogManager.addLog("INFO", "CFG", "connection_order=$value (applies after restart)")
    }

    fun getAutoSocketProtocols(): AutoSocketProtocols = runBlocking {
        autoSocketProtocols(context.dataStore.data.first())
    }

    private fun autoSocketProtocols(prefs: Preferences): AutoSocketProtocols {
        val value = prefs[autoSocketProtocolsKey] ?: return AutoSocketProtocols.BOTH
        return AutoSocketProtocols.values().firstOrNull { it.configValue == value } ?: run {
            LogManager.addLog("WARN", "CFG", "invalid auto_socket_protocols='$value', using tcp+udp")
            AutoSocketProtocols.BOTH
        }
    }

    suspend fun setAutoSocketProtocols(protocols: AutoSocketProtocols) {
        context.dataStore.edit { prefs -> prefs[autoSocketProtocolsKey] = protocols.configValue }
        LogManager.addLog("INFO", "CFG", "auto_socket_protocols=${protocols.configValue}")
    }

    /* ── Background service enabled/disabled ── */

    val serviceEnabledFlow: Flow<Boolean> = context.dataStore.data.map { it[serviceEnabledKey] ?: true }

    fun getServiceEnabled(): Boolean = runBlocking {
        context.dataStore.data.first()[serviceEnabledKey] ?: true
    }

    suspend fun setServiceEnabled(enabled: Boolean) {
        context.dataStore.edit { prefs -> prefs[serviceEnabledKey] = enabled }
    }

    val startOnBootFlow: Flow<Boolean> = context.dataStore.data.map { it[startOnBootKey] ?: true }

    suspend fun setStartOnBoot(enabled: Boolean) {
        context.dataStore.edit { prefs -> prefs[startOnBootKey] = enabled }
        LogManager.addLog("INFO", "CONFIG", "Start on boot setting saved: enabled=$enabled")
    }

    // Одним снимком проверяем все условия автозапуска.
    suspend fun shouldStartOnBoot(): Boolean {
        val prefs = context.dataStore.data.first()
        val setupDone = prefs[firstLaunchDone] ?: false
        val startOnBoot = prefs[startOnBootKey] ?: true
        val serviceEnabled = prefs[serviceEnabledKey] ?: true
        val reason = when {
            !setupDone -> "setup_incomplete"
            !startOnBoot -> "autostart_disabled"
            !serviceEnabled -> "service_disabled"
            else -> "start"
        }
        LogManager.addLog("INFO", "BOOT",
            "Boot decision: setupDone=$setupDone startOnBoot=$startOnBoot serviceEnabled=$serviceEnabled reason=$reason")
        return setupDone && startOnBoot && serviceEnabled
    }

    /** Ширина колонки хранится только на этом устройстве и не входит в протокол группы. */
    fun kanbanColumnWidth(group: String, column: String): Flow<Int> = changes.observe("kanban width $group $column",
        context.dataStore.data.map { (it[intPreferencesKey("kanban_${group}_${column}_width")] ?: 290).coerceIn(220, 1200) }
            .distinctUntilChanged())

    fun saveKanbanColumnWidth(group: String, column: String, width: Int) = changes.submit("kanban width $group $column") {
        require(width in 220..1200)
        context.dataStore.edit { it[intPreferencesKey("kanban_${group}_${column}_width")] = width }
        LogManager.addLog("DEBUG", "Kanban", "Local column width saved group=$group column=$column width=$width")
    }

    /* ── Notification sound (global toggle + per-group mute) ── */

    /** Один подтверждённый снимок для Application; ошибка чтения не заменяет прежнее mute-состояние defaults. */
    val notificationSoundSettings: Flow<Pair<Boolean, Set<String>>> = context.dataStore.data.map { prefs ->
        (prefs[soundEnabledKey] ?: true) to (prefs[mutedChannelsKey] ?: emptySet())
    }.distinctUntilChanged()

    fun getSoundEnabled(): Boolean = runBlocking {
        context.dataStore.data.first()[soundEnabledKey] ?: true
    }

    suspend fun setSoundEnabled(enabled: Boolean) {
        context.dataStore.edit { prefs -> prefs[soundEnabledKey] = enabled }
    }

    fun getMutedChannels(): Set<String> = runBlocking {
        context.dataStore.data.first()[mutedChannelsKey] ?: emptySet()
    }

    suspend fun setChannelMuted(channelId: String, muted: Boolean) {
        context.dataStore.edit { prefs ->
            val cur = prefs[mutedChannelsKey] ?: emptySet()
            prefs[mutedChannelsKey] = if (muted) cur + channelId else cur - channelId
        }
    }

    /* ── Audio route for calls (запоминание источника аудио) ── */

    fun getCallAudioRoute(): AudioRoute = runBlocking {
        val s = context.dataStore.data.first()[callAudioRouteKey]
        AudioRoute.entries.firstOrNull { it.name == s } ?: AudioRoute.EARPIECE
    }

    suspend fun setCallAudioRoute(r: AudioRoute) {
        context.dataStore.edit { prefs -> prefs[callAudioRouteKey] = r.name }
    }

    /* ── Рация: volume-кнопки PTT и оверлей ── */

    /* Кэш быстрых настроек рации: чтение не блокирует главный поток.
     * Ленивая загрузка один раз, дальше — из памяти; set обновляет кэш после сохранения. */
    @Volatile private var radioVolumePttCached = false
    @Volatile private var radioVolumePttLoaded = false

    fun getRadioVolumePtt(): Boolean {
        if (!radioVolumePttLoaded) {
            radioVolumePttCached = runBlocking { context.dataStore.data.first()[radioVolumePttKey] ?: false }
            radioVolumePttLoaded = true
        }
        return radioVolumePttCached
    }

    suspend fun setRadioVolumePtt(enabled: Boolean) {
        try {
            context.dataStore.edit { prefs -> prefs[radioVolumePttKey] = enabled }
            radioVolumePttCached = enabled
            radioVolumePttLoaded = true
            LogManager.addLog("INFO", "PTT", "Volume PTT enabled=$enabled")
        } catch (e: IOException) {
            LogManager.addLog("ERROR", "PTT", "Cannot save volume PTT enabled=$enabled: $e")
            throw e
        }
    }

    val radioVolumePttEnabled: Flow<Boolean> = context.dataStore.data.map { it[radioVolumePttKey] ?: false }
    val radioOverlayEnabled: Flow<Boolean> = context.dataStore.data.map { it[radioOverlayKey] ?: false }
    val radioKeepMicrophoneReady: Flow<Boolean> = context.dataStore.data.map { it[radioKeepMicrophoneReadyKey] ?: true }

    fun getRadioKeepMicrophoneReady(): Boolean = runBlocking { context.dataStore.data.first()[radioKeepMicrophoneReadyKey] ?: true }

    suspend fun setRadioKeepMicrophoneReady(enabled: Boolean) {
        context.dataStore.edit { it[radioKeepMicrophoneReadyKey] = enabled }
        LogManager.addLog("INFO", "radio", "Keep microphone ready saved enabled=$enabled")
    }
    val radioAutoEnabled: Flow<Boolean> = context.dataStore.data.map { (it[radioAutoEnabledKey] ?: 0) != 0 }

    @Volatile private var radioOverlayCached = false
    @Volatile private var radioOverlayLoaded = false

    fun getRadioOverlay(): Boolean {
        if (!radioOverlayLoaded) {
            radioOverlayCached = runBlocking { context.dataStore.data.first()[radioOverlayKey] ?: false }
            radioOverlayLoaded = true
        }
        return radioOverlayCached
    }

    suspend fun setRadioOverlay(enabled: Boolean) {
        context.dataStore.edit { prefs -> prefs[radioOverlayKey] = enabled }
        radioOverlayCached = enabled
        radioOverlayLoaded = true
        LogManager.addLog("INFO", "radio", "Overlay setting saved enabled=$enabled")
    }

    @Volatile private var radioMicSourceCached = 0
    @Volatile private var radioMicSourceLoaded = false

    fun getRadioMicSource(): Int {
        if (!radioMicSourceLoaded) {
            radioMicSourceCached = runBlocking { context.dataStore.data.first()[radioMicSourceKey] ?: 0 }
            radioMicSourceLoaded = true
        }
        return radioMicSourceCached
    }

    suspend fun setRadioMicSource(source: Int) {
        context.dataStore.edit { prefs -> prefs[radioMicSourceKey] = source }
        radioMicSourceCached = source
        radioMicSourceLoaded = true
        LogManager.addLog("INFO", "radio", "Microphone source saved source=$source")
    }

    @Volatile private var radioCaptureStereoCached = 0
    @Volatile private var radioCaptureStereoLoaded = false

    fun getRadioCaptureStereo(): Boolean {
        if (!radioCaptureStereoLoaded) {
            radioCaptureStereoCached = runBlocking { context.dataStore.data.first()[radioCaptureStereoKey] ?: 0 }
            radioCaptureStereoLoaded = true
        }
        return radioCaptureStereoCached != 0
    }

    suspend fun setRadioCaptureStereo(enabled: Boolean) {
        context.dataStore.edit { prefs -> prefs[radioCaptureStereoKey] = if (enabled) 1 else 0 }
        radioCaptureStereoCached = if (enabled) 1 else 0
        radioCaptureStereoLoaded = true
        LogManager.addLog("INFO", "radio", "Stereo capture setting saved enabled=$enabled")
    }

    /* ── Server entries ── */

    suspend fun detectInterfaces(saved: List<ServerEntry> = emptyList(),
                                 protocols: AutoSocketProtocols = getAutoSocketProtocols()): List<ServerEntry> {
        val servers = mutableListOf<ServerEntry>()
        val seen = mutableSetOf<String>()
        try {
            val nis = NetworkInterface.getNetworkInterfaces()
            while (nis != null && nis.hasMoreElements()) {
                val ni = nis.nextElement()
                if (!ni.isUp || ni.isLoopback) continue
                for (a in ni.interfaceAddresses) {
                    val addr = a.address
                    val ip = when {
                        addr is Inet4Address -> addr.hostAddress
                        addr is Inet6Address && isUsableV6(addr) && !isTemporaryV6(ni.name, addr) -> addr.hostAddress?.substringBefore('%')
                        else -> null
                    } ?: continue
                    val isV6 = ip.contains(":")
                    val key = "${ni.name}/${if (isV6) "v6" else "v4"}"
                    if (!seen.add(key)) continue
                    servers.addAll(protocols.createServers(ni.name, ip, saved))
                }
            }
        } catch (e: Exception) {
            LogManager.addLog("ERROR", "CFG", "detectInterfaces failed: ${e.message}")
            throw e // Частичный снимок не должен заменять работающую конфигурацию.
        }
        return servers
    }

    /* ── Config file as single source of truth for servers ── */

    fun configFilePath(): String {
        val dir = getString("db.path") ?: context.filesDir.absolutePath
        return "$dir/utun.cfg"
    }

    private data class ConfigSnapshot(val preferences: Preferences, val directory: File, val keys: Pair<String, String>?)

    /** Одно чтение DataStore и одна пара keys.conf на весь сохраняемый/передаваемый снимок. */
    private suspend fun configSnapshot(): ConfigSnapshot {
        val prefs = context.dataStore.data.first()
        val directory = File(prefs[dbPath] ?: context.filesDir.absolutePath)
        return ConfigSnapshot(prefs, directory, readKeysFromFile(File(directory, "keys.conf")))
    }

    /**
     * В общей очереди читает активные секции; includeCommented добавляет неактивные. IP — текущий либо wildcard.
     * Отсутствие файла — пустой список; ошибка чтения передаётся через await и логируется.
     */
    fun loadServersFromConfig(includeCommented: Boolean = false): Deferred<List<ServerEntry>> = configFiles.submit("load servers") {
        val prefs = context.dataStore.data.first()
        val saved = loadServers(File(prefs[dbPath] ?: context.filesDir.absolutePath, "utun.cfg"))
        if (includeCommented) saved.active + saved.inactive else saved.active
    }

    private data class SavedServers(val active: List<ServerEntry>, val inactive: List<ServerEntry>)

    private fun loadServers(file: File): SavedServers {
        if (!file.exists()) return SavedServers(emptyList(), emptyList())
        val text = file.readText()
        val all = ServerConfig.parse(text.lineSequence(), true) { warning -> LogManager.addLog("WARN", "CFG", warning) }
        // Все ошибки уже диагностированы полным проходом; второй выбирает только активные секции того же текста.
        val active = ServerConfig.parse(text.lineSequence(), false) {}
        val activeKeys = active.map { it.socketKey }.toSet()
        val inactive = all.filter { it.socketKey !in activeKeys }.distinctBy { it.socketKey }
        val live = if (all.isEmpty()) emptyList() else getAvailableInterfaces()
        return SavedServers(resolveServers(active, live), resolveServers(inactive, live))
    }

    private fun resolveServers(saved: List<ServerEntry>, live: List<Pair<String, String>>): List<ServerEntry> =
        saved.map { server ->
            val isV6 = server.bindIp.contains(':')
            val realIp = live.firstOrNull { (ifn, ip) ->
                ifn == server.ifName && ip.contains(':') == isV6
            }?.second
            server.copy(bindIp = realIp ?: server.bindIp)
        }

    /**
     * Копирует список сразу и принимает сохранение в очередь Application; закрытие Settings не отменяет запись.
     * Успешный await подтверждает замену файла; ошибка сохраняет прежний файл и логируется.
     */
    fun saveServersToConfig(servers: List<ServerEntry>, commented: List<ServerEntry> = emptyList()): Deferred<Unit> {
        val active = servers.toList()
        val inactive = commented.toList()
        return configFiles.submit("save servers") {
            saveConfig(configSnapshot(), active, inactive)
            Unit
        }
    }

    /** Только в configFiles: оба текста из одного снимка; dormant-секции сохраняются, в runtime не передаются. */
    private fun saveConfig(snapshot: ConfigSnapshot, servers: List<ServerEntry>, commented: List<ServerEntry>): String {
        val text = buildConfigFromServers(snapshot, servers, commented)
        val runtime = if (commented.isEmpty()) text else buildConfigFromServers(snapshot, servers)
        configFiles.replace(File(snapshot.directory, "utun.cfg"), text)
        LogManager.addLog("INFO", "CFG", "saveServersToConfig: ${servers.size} active, ${commented.size} commented, " +
            "hide_my_sockets=${snapshot.preferences[hideMySocketsKey] ?: true} " +
            "storage_unit_size=${intValue("chat.storage_autoload_maxsize_mb", snapshot.preferences)}M " +
            "storage_total_size=${intValue("chat.storage_maxsize_gb", snapshot.preferences)}G; " +
            servers.joinToString(", ") { "${it.ifName}=${it.protocol}:${it.port}" })
        return runtime
    }

    /** Формирует INI из готового снимка; не читает DataStore/файлы и не сохраняет результат. */
    private fun buildConfigFromServers(snapshot: ConfigSnapshot, servers: List<ServerEntry>,
                                       commented: List<ServerEntry> = emptyList()): String {
        val prefs = snapshot.preferences
        val pubKey = stringValue("node.public_key", prefs, snapshot.keys) ?: ""
        val privKey = stringValue("node.private_key", prefs, snapshot.keys) ?: ""
        val nodeName = stringValue("node.name", prefs, snapshot.keys) ?: getDeviceName()
        val dbDir = snapshot.directory.path
        return buildString {
            appendLine("[global]")
            if (nodeName.isNotBlank()) appendLine("my_node_name=$nodeName")
            if (pubKey.length == 64) appendLine("my_public_key=$pubKey")
            if (privKey.length == 64) appendLine("my_private_key=$privKey")
            appendLine("db_path=$dbDir")
            appendLine("debug_level=${stringValue("debug.console_level", prefs, snapshot.keys) ?: "info"}")
            appendLine("db_sync_enabled=1")
            appendLine("chatserver_enabled=1")
            appendLine("auto_sockets=android")
            appendLine("auto_socket_protocols=${autoSocketProtocols(prefs).configValue}")
            appendLine("connection_order=${prefs[connectionOrderKey] ?: "RUT2-2"}")
            appendLine("hide_my_sockets=${if (prefs[hideMySocketsKey] ?: true) "yes" else "no"}")
            appendLine()
            appendLine("[gui]")
            appendLine("utun_enabled=${intValue("gui.utun_enabled", prefs)}")
            appendLine()
            for (srv in servers) appendServerSection(this, srv, false)
            for (srv in commented) appendServerSection(this, srv, true)
            MonitoringSettings.read(prefs).appendConfig(this)
            appendLine("[allowed_keys]")
            appendLine("allow_all=yes")
            appendLine()
            appendLine("[ntp]")
            appendLine("enabled=no")
            appendLine()
            appendLine("[chat]")
            val incoming = incomingCallKeys.map { prefs[it] ?: false }
            val policy = incoming.withIndex().fold(0) { mask, (index, value) -> mask or if (value) (1 shl index) else 0 }
            appendLine("incoming_call_policy=$policy")
            val chatAutoload = intValue("chat.storage_autoload", prefs)
            val chatAutoloadMaxMb = intValue("chat.storage_autoload_maxsize_mb", prefs)
            val chatMaxGb = intValue("chat.storage_maxsize_gb", prefs)
            val chatOpusPreset = intValue("chat.opus_codec_preset", prefs)
            val radioOpusMode = intValue("radio.opus_mode", prefs)
            val radioOpusBitrate = intValue("radio.opus_bitrate", prefs)
            val radioCaptureStereo = intValue("radio.capture_stereo", prefs)
            val radioCompEnabled = intValue("radio.compressor_enabled", prefs)
            val radioCompMaxGain = intValue("radio.compressor_max_gain_db", prefs)
            val chatCompEnabled = intValue("chat.compressor_enabled", prefs)
            val chatCompMaxGain = intValue("chat.compressor_max_gain_db", prefs)
            val chatCompRiseRate = intValue("chat.compressor_rise_rate", prefs)
            val chatDlMaxPeers = intValue("chat.media_download_max_peers", prefs)
            appendLine("storage_autoload=$chatAutoload")
            appendLine("storage_unit_size=${chatAutoloadMaxMb}M")
            appendLine("storage_total_size=${chatMaxGb}G")
            appendLine("opus_codec_preset=$chatOpusPreset")
            val radioAuto = radioAutoSettings(prefs)
            appendLine("radio_auto_enabled=${if (radioAuto.enabled) 1 else 0}")
            appendLine("radio_auto_mode=${radioAuto.mode}")
            appendLine("radio_level_on_dbfs=${radioAuto.levelOn}")
            appendLine("radio_level_off_dbfs=${radioAuto.levelOff}")
            appendLine("radio_auto_hangover_ms=${radioAuto.hangoverMs}")
            appendLine("radio_vad_threshold=${radioAuto.vadThreshold}")
            appendLine("radio_vad_compressor_max_gain_db=${radioAuto.vadGain}")
            appendLine("radio_opus_mode=$radioOpusMode")
            appendLine("radio_opus_bitrate=$radioOpusBitrate")
            appendLine("radio_burst_frames=${intValue("radio.burst_frames", prefs)}")
            appendLine("radio_capture_stereo=$radioCaptureStereo")
            appendLine("radio_compressor_enabled=$radioCompEnabled")
            appendLine("radio_aec_enabled=${intValue("radio.aec_enabled", prefs)}")
            appendLine("radio_compressor_max_gain_db=$radioCompMaxGain")
            appendLine("stream_subblocks=${intValue("chat.stream_subblocks", prefs)}")
            appendLine("stream_rs_redundant=${intValue("chat.stream_rs_redundant", prefs)}")
            appendLine("stream_stall_timeout_ms=${intValue("chat.stream_stall_timeout_ms", prefs)}")
            appendLine("stream_max_live_lag_ms=${intValue("chat.stream_max_live_lag_ms", prefs)}")
            StreamFecParameters.budgets.forEach { appendLine("${it.key}=${intValue("chat.${it.key}", prefs)}") }
            appendLine("call_jitter_max_reserve_ms=${intValue("chat.call_jitter_max_reserve_ms", prefs)}")
            appendLine("call_video_bitrate_range_kbps=${videoBitrateRange(prefs).encode()}")
            appendLine("compressor_enabled=$chatCompEnabled")
            appendLine("noise_suppression_enabled=${intValue("chat.noise_suppression_enabled", prefs)}")
            appendLine("noise_suppression_db=${intValue("chat.noise_suppression_db", prefs)}")
            appendLine("compressor_max_gain_db=$chatCompMaxGain")
            appendLine("compressor_rise_rate=$chatCompRiseRate")
            appendLine("media_download_max_peers=$chatDlMaxPeers")
            appendLine("standby_active_sec=${intValue("chat.standby_active_sec", prefs)}")
            appendLine("standby_sleep_sec=${intValue("chat.standby_sleep_sec", prefs)}")
            appendLine("standby_min_sleep_sec=${intValue("chat.standby_min_sleep_sec", prefs)}")
            appendLine("standby_keepalive_ms=${intValue("chat.standby_keepalive_ms", prefs)}")
            appendLine("group_autoconnect=${intValue("chat.group_autoconnect", prefs)}")
            appendLine("bluetooth_invites_enabled=${intValue("chat.bluetooth_invites_enabled", prefs)}")
            appendLine("local_invites_enabled=${intValue("chat.local_invites_enabled", prefs)}")
            appendLine("local_broadcasts_enabled=${intValue("chat.local_broadcasts_enabled", prefs)}")
            appendLine()
            appendLine("[debug]")
            val dbgCat = stringValue("debug.categories", prefs, snapshot.keys) ?: ""
            if (dbgCat.isNotEmpty()) {
                for (entry in dbgCat.split(",")) {
                    val trimmed = entry.trim()
                    if (trimmed.isNotEmpty()) appendLine(trimmed)
                }
            }
            val udpLog = udpLogSettings(prefs)
            udpLog.appendConfig(this)
            LogManager.addLog(if (udpLog.enabled && !udpLog.active) "WARN" else "INFO", "UDPLog",
                "Startup network logging enabled=${udpLog.enabled} active=${udpLog.active} target=${udpLog.ip}:${udpLog.port}")
        }
    }

    /** Append one [server:...] section; comment=true prefixes each line with '#'. */
    private fun appendServerSection(sb: StringBuilder, srv: ServerEntry, comment: Boolean) {
        val family = if (srv.bindIp.contains(":")) "v6" else "v4"
        val p = if (comment) "#" else ""
        val addr = if (family == "v6") "[::]" else srv.bindIp.ifBlank { "0.0.0.0" }
        sb.appendLine("${p}[server:${srv.sectionName}]")
        sb.appendLine("${p}addr=$addr:${srv.port}")
        sb.appendLine("${p}type=public")
        sb.appendLine("${p}transport=${srv.protocol}")
        if (family == "v6") sb.appendLine("${p}netif=${srv.ifName}")
        sb.appendLine()
    }

    /** Синхронный список (интерфейс, IP) для UI/конфига; ошибка и fallback на loopback видны в CFG-логе. */
    fun getAvailableInterfaces(): List<Pair<String, String>> = runBlocking {
        val list = mutableListOf<Pair<String, String>>()
        try {
            val nis = NetworkInterface.getNetworkInterfaces()
            while (nis != null && nis.hasMoreElements()) {
                val ni = nis.nextElement()
                if (!ni.isUp || ni.isLoopback) continue
                for (a in ni.interfaceAddresses) {
                    val addr = a.address
                    val ip = when {
                        addr is Inet4Address -> addr.hostAddress
                        addr is Inet6Address && isUsableV6(addr) && !isTemporaryV6(ni.name, addr) -> addr.hostAddress?.substringBefore('%')
                        else -> null
                    } ?: continue
                    list.add(ni.name to ip)
                }
            }
        } catch (e: Exception) {
            LogManager.addLog("ERROR", "CFG", "getAvailableInterfaces failed: ${e.javaClass.simpleName}: ${e.message}")
        }
        if (list.isEmpty()) {
            LogManager.addLog("WARN", "CFG", "getAvailableInterfaces: no usable addresses, using lo=127.0.0.1")
            list.add("lo" to "127.0.0.1")
        }
        list
    }

    /** В очереди сформировать/сохранить единый снимок и вернуть runtime-текст для start/restart/updateSockets. */
    fun buildConfigText(): Deferred<String> = configFiles.submit("build config") {
        val snapshot = configSnapshot()
        val protocols = autoSocketProtocols(snapshot.preferences)
        val cfgFile = File(snapshot.directory, "utun.cfg")
        val stored = loadServers(cfgFile)
        val saved = stored.active + stored.inactive
        val mode = connectionMode(snapshot.preferences)
        val active = if (mode == "auto") detectInterfaces(saved, protocols) else stored.active
        val activeKeys = active.map { it.socketKey }.toSet()
        val inactive = saved.filter { it.socketKey !in activeKeys }.distinctBy { it.socketKey }
        LogManager.addLog("INFO", "CFG", "buildConfigText: mode=$mode protocols=${protocols.configValue} " +
            "active=${active.size} inactive=${inactive.size}; " + active.joinToString(", ") { "${it.ifName}=${it.protocol}:${it.port}" })
        saveConfig(snapshot, active, inactive)
    }
}

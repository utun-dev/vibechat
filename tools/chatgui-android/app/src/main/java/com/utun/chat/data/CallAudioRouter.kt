package com.utun.chat.data

import android.Manifest
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothHeadset
import android.bluetooth.BluetoothProfile
import android.content.Context
import android.content.pm.PackageManager
import android.media.AudioDeviceCallback
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import com.utun.chat.ChatApplication
import java.util.concurrent.ConcurrentHashMap

/** Аудио-выход звонка: динамик (earpiece) ↔ громкая связь (speaker) ↔ гарнитура. */
enum class AudioRoute { EARPIECE, SPEAKER, HEADSET }

/**
 * Маршрутизация звонка и общего media-аудио; мониторинг проводных/USB и Bluetooth устройств.
 *
 * - EARPIECE: разговорный динамик (isSpeakerphoneOn=false).
 * - SPEAKER:  громкая связь (isSpeakerphoneOn=true).
 * - HEADSET: проводная/USB, Bluetooth SCO/LE; без глобального маршрута также A2DP.
 *
 * Гарнитура детектится через AudioDeviceCallback (проводная, USB, SCO) и
 * BluetoothHeadset profile (классический BT до установки SCO). Без BLUETOOTH_CONNECT
 * нельзя читать этот профиль; опубликованные AudioManager устройства остаются доступны.
 *
 * Владельцы — CallAudioEngine и MediaAudioController. Все изменения сериализованы монитором router:
 * media start может приходить из I/O, callbacks — с main. Observer вызывается синхронно под монитором,
 * должен завершаться быстро и не ждать другой поток, которому нужен router.
 * start откатывает частичную инициализацию и бросает ошибку владельцу. stop независимо очищает ресурсы;
 * отказ сохраняет владельца и бросает ошибку. Новый start сначала подтверждает прежнюю очистку.
 * AudioManager: route — обработанная категория, current* — цели, null при ожидании BT. Отказ не публикует новую цель.
 * Физический маршрут подтверждает AudioTrack/AudioRecord; при Telecom route меняется только по его подтверждению.
 * Владение profile proxy сохраняется при disconnect сервиса и заканчивается успешным close.
 * Неосвобождённые ресурсы удерживаются process-ссылкой; main повторяет cleanup через 250 мс..30 с.
 * Запоздалые/заменённые BT proxy закрываются отдельно, без остановки нового активного маршрута.
 */
class CallAudioRouter {
    companion object {
        private val retained = ConcurrentHashMap.newKeySet<CallAudioRouter>()
    }

    @Volatile var route = AudioRoute.SPEAKER
        private set
    @Volatile var preferredRoute = AudioRoute.SPEAKER
        private set
    @Volatile var headsetAvailable = false
        private set
    @Volatile var hasEarpiece = true
        private set

    /* Доступность входных устройств захвата (для строки аудиоисточников рации). */
    @Volatile var wiredInputAvailable = false
        private set
    @Volatile var btInputAvailable = false
        private set

    /** Куда откатываться при потере гарнитуры (звонок — EARPIECE, рация — SPEAKER). */
    @Volatile var fallbackRoute = AudioRoute.EARPIECE

    /** Следовать за подключением/отключением гарнитуры. Ручной выбор действует до следующего изменения доступности. */
    @Volatile var autoHeadset = false
        private set

    /** Уведомление об изменении маршрута или доступности гарнитуры. */
    @Volatile var onChanged: ((AudioRoute, Boolean) -> Unit)? = null

    /** Начало перестройки тракта: кратковременные underrun не характеризуют размер буфера. */
    @Volatile var onRouteRequested: (() -> Unit)? = null

    /** Уведомление об изменении доступности входных устройств (проводная, BT). */
    @Volatile var onInputDevicesChanged: ((Boolean, Boolean) -> Unit)? = null

    /** Целевое устройство вывода (для прямого роутирования AudioTrack через setPreferredDevice). */
    @Volatile var onOutputDevice: ((AudioDeviceInfo?) -> Unit)? = null

    @Volatile var currentOutputDevice: AudioDeviceInfo? = null
        private set

    /** Целевое устройство захвата (для прямого роутирования AudioRecord через setPreferredDevice).
     *  non-null только для гарнитуры (проводная/USB/BT-SCO); для динамика/громкой — null (дефолт). */
    @Volatile var onInputDevice: ((AudioDeviceInfo?) -> Unit)? = null

    @Volatile var currentInputDevice: AudioDeviceInfo? = null
        private set

    private val am: AudioManager =
        ChatApplication.instance.getSystemService(Context.AUDIO_SERVICE) as AudioManager

    private var preferWiredHeadset = false
    @Volatile private var started = false
    private var globalRouting = true
    private var communicationOwned = false
    private var scoOwned = false
    private var scoFlagOwned = false
    private var desiredRoute = AudioRoute.SPEAKER
    private var routeApplied = false
    private var telecomGeneration = 0L
    @Volatile var telecom: UtunConnection? = null
        private set

    /** Для звонка Telecom — единственный владелец маршрута; рация продолжает использовать AudioManager. */
    @Synchronized fun bindTelecom(connection: UtunConnection?) {
        if (telecom === connection) return
        if (connection != null) releaseGlobalRoute()
        val generation = ++telecomGeneration
        telecom?.onRouteChanged = null
        telecom?.onEndpointsChanged = null
        telecom = connection
        routeApplied = false
        currentOutputDevice = null; currentInputDevice = null
        connection?.onRouteChanged = { actual ->
            synchronized(this) {
                if (started && telecom === connection && generation == telecomGeneration) {
                    route = actual
                    onChanged?.invoke(route, headsetAvailable)
                }
            }
        }
        connection?.onEndpointsChanged = {
            synchronized(this) {
                if (started && telecom === connection && generation == telecomGeneration) onDevicesChanged()
            }
        }
        connection?.confirmedRoute?.let { route = it }
        LogManager.addLog("INFO", "CallAudioRouter", "routing owner=${if (connection != null) "telecom" else "audio-manager"}")
        if (started) {
            onOutputDevice?.invoke(null); onInputDevice?.invoke(null)
            onDevicesChanged(force = true)
            onChanged?.invoke(route, headsetAvailable)
        }
    }
    private var deviceCallback: AudioDeviceCallback? = null
    @Volatile private var btHeadset: BluetoothHeadset? = null // Текущий proxy, не признак подключения сервиса.
    private val btProxies = linkedMapOf<BluetoothProfile, Pair<BluetoothAdapter, Int>>() // Все полученные владельцы до close.
    private var btProfileGeneration = 0L // Поколение запроса, инвалидируется до закрытия; под монитором router.
    private val cleanupHandler = Handler(Looper.getMainLooper())
    private var cleanupDelay = 250L
    private val cleanupRetry = Runnable {
        synchronized(this) {
            if (started) {
                releaseBtProxies()
                scheduleCleanup()
            } else {
                try { stop() } catch (e: Exception) {
                    LogManager.addLog("WARN", "CallAudioRouter", "cleanup retry incomplete generation=$btProfileGeneration: $e")
                }
            }
        }
    }
    /** Остатки прежней остановки либо неосвобождённые старые proxy активного маршрута. */
    val cleanupPending: Boolean @Synchronized get() = (!started &&
        (deviceCallback != null || communicationOwned || scoOwned || scoFlagOwned)) || btProxies.keys.any { it !== btHeadset }
    /* Неизменяемый снимок после старта/переоценки и событий устройств; capture читает уже найденные устройства. */
    @Volatile private var devices: Array<AudioDeviceInfo>? = null

    @Synchronized fun start() {
        if (started) return
        if (cleanupPending) {
            val connection = telecom
            stop()
            bindTelecom(connection)
        }
        started = true
        val startedAt = SystemClock.elapsedRealtime()

        val cb = object : AudioDeviceCallback() {
            private fun changed() {
                synchronized(this@CallAudioRouter) {
                    if (started && deviceCallback === this) onDevicesChanged()
                }
            }
            override fun onAudioDevicesAdded(addedDevices: Array<out AudioDeviceInfo>?) = changed()
            override fun onAudioDevicesRemoved(removedDevices: Array<out AudioDeviceInfo>?) = changed()
        }
        try {
            var stageAt = SystemClock.elapsedRealtime()
            deviceCallback = cb
            am.registerAudioDeviceCallback(cb, Handler(Looper.getMainLooper()))
            val callbackMs = SystemClock.elapsedRealtime() - stageAt

            stageAt = SystemClock.elapsedRealtime()
            bindBtHeadsetProfile()
            val btProfileMs = SystemClock.elapsedRealtime() - stageAt

            val devicesAt = SystemClock.elapsedRealtime()
            devices = am.getDevices(AudioManager.GET_DEVICES_ALL)
            val devicesMs = SystemClock.elapsedRealtime() - devicesAt
            hasEarpiece = findBuiltinEarpiece() != null
            desiredRoute = if (autoHeadset) (if (isHeadsetConnected()) AudioRoute.HEADSET else fallbackRoute) else preferredRoute
            stageAt = SystemClock.elapsedRealtime()
            check(refresh(forceAuto = true)) { "Initial route request rejected: $desiredRoute" }
            val refreshMs = SystemClock.elapsedRealtime() - stageAt
            LogManager.addLog("INFO", "CallAudioRouter",
                "started totalMs=${SystemClock.elapsedRealtime() - startedAt} callbackMs=$callbackMs btProfileMs=$btProfileMs " +
                    "devicesMs=$devicesMs refreshMs=$refreshMs " +
                    "devices=${devices?.size} hasEarpiece=$hasEarpiece autoHeadset=$autoHeadset fallback=$fallbackRoute")
        } catch (e: Exception) {
            LogManager.addLog("ERROR", "CallAudioRouter", "start failed, releasing partial lifecycle: $e")
            try { stop() } catch (cleanup: Exception) {
                LogManager.addLog("ERROR", "CallAudioRouter", "startup rollback incomplete: $cleanup")
                e.addSuppressed(cleanup)
            }
            throw e
        }
    }

    @Synchronized fun stop() {
        if (!started && !cleanupPending) { bindTelecom(null); return }
        if (started) ++btProfileGeneration
        started = false
        cleanupHandler.removeCallbacks(cleanupRetry)
        try {
            deviceCallback?.let { am.unregisterAudioDeviceCallback(it) }
            deviceCallback = null
        } catch (e: Exception) {
            LogManager.addLog("ERROR", "CallAudioRouter", "device callback unregister failed: $e")
        }
        btHeadset = null
        releaseBtProxies()
        try { releaseGlobalRoute() } catch (e: Exception) {
            LogManager.addLog("ERROR", "CallAudioRouter", "stop cleanup failed: $e")
        }
        devices = null
        routeApplied = false
        currentInputDevice = null; currentOutputDevice = null
        headsetAvailable = false; wiredInputAvailable = false; btInputAvailable = false
        bindTelecom(null)
        scheduleCleanup()
        check(!cleanupPending) {
            val detail = "cleanup pending generation=$btProfileGeneration callback=${deviceCallback != null} " +
                "proxies=${btProxies.size} communication=$communicationOwned sco=$scoOwned/$scoFlagOwned"
            LogManager.addLog("ERROR", "CallAudioRouter", detail)
            detail
        }
        LogManager.addLog("INFO", "CallAudioRouter", "stopped btGeneration=$btProfileGeneration")
    }

    @Synchronized fun setRoute(r: AudioRoute, preferWired: Boolean = false, automatic: Boolean = false) {
        if (r == AudioRoute.HEADSET && !headsetAvailable) {
            LogManager.addLog("WARN", "CallAudioRouter", "setRoute HEADSET rejected: no headset available")
            return
        }
        val preferenceChanged = preferWiredHeadset != preferWired
        preferWiredHeadset = preferWired
        if (desiredRoute == r && route == r && !preferenceChanged && routeApplied) return
        desiredRoute = r
        preferredRoute = r
        if (!automatic && r != AudioRoute.HEADSET) fallbackRoute = r
        if (!started || !applyRoute(r)) return
        LogManager.addLog("INFO", "CallAudioRouter", "requested route=$r actual=$route headsetAvailable=$headsetAvailable automatic=$automatic")
        if (telecom == null) onChanged?.invoke(route, headsetAvailable)
    }

    /** Инжекция сохранённого источника аудио (из DataStore) перед стартом звонка. */
    @Synchronized fun setPreferredRoute(r: AudioRoute) {
        preferredRoute = r
        LogManager.addLog("INFO", "CallAudioRouter", "preferredRoute -> $r")
    }

    /** Включить/выключить автоследование; reapply немедленно применяет авторежим к текущим устройствам. */
    @Synchronized fun setAutoHeadset(enabled: Boolean) {
        val resetPreference = enabled && preferWiredHeadset
        autoHeadset = enabled
        if (enabled) preferWiredHeadset = false
        if (resetPreference && started && route == AudioRoute.HEADSET) applyRoute(route)
        LogManager.addLog("INFO", "CallAudioRouter", "autoHeadset -> $enabled")
    }

    /** Немедленно переоценить маршрут (например, после включения автоследования). */
    @Synchronized fun reapply() {
        if (!started) return
        devices = am.getDevices(AudioManager.GET_DEVICES_ALL)
        refresh(forceAuto = true)
    }

    /* ── применение маршрута ── */
    /** Выбрать глобальный разговорный тракт либо только preferredDevice (media/CAMCORDER).
     * Вызовы звонка сохраняют глобальный режим; общий media-контроллер выбирает режим по захвату. */
    @Synchronized fun setGlobalRouting(enabled: Boolean) {
        if (globalRouting == enabled) return
        if (!enabled && started && telecom == null) {
            releaseGlobalRoute()
        }
        globalRouting = enabled
        LogManager.addLog("INFO", "CallAudioRouter", "global routing=$enabled")
        if (started) applyRoute(desiredRoute)
    }

    @Synchronized private fun applyRoute(r: AudioRoute): Boolean {
        routeApplied = false
        val startedAt = SystemClock.elapsedRealtime()
        try {
            onRouteRequested?.invoke()
            telecom?.let { connection ->
                connection.routeTo(r)
                routeApplied = true
                LogManager.addLog("INFO", "CallAudioRouter", "route requested=$r confirmed=${connection.confirmedRoute} owner=telecom")
                return true
            }
            val resolved = if (r == AudioRoute.EARPIECE && !hasEarpiece) AudioRoute.SPEAKER else r
            if (!globalRouting) {
                val wired = findWiredDevice()
                val wiredInput = wiredInputDevice()
                val headset = if (preferWiredHeadset && (wired != null || wiredInput != null)) wired ?: findBuiltinSpeaker()
                    else findBtMediaDevice() ?: findBtScoDevice() ?: wired ?: findBuiltinSpeaker()?.takeIf { wiredInput != null }
                val output = when (resolved) {
                    AudioRoute.SPEAKER -> findBuiltinSpeaker()
                    AudioRoute.EARPIECE -> findBuiltinEarpiece() ?: findBuiltinSpeaker()
                    AudioRoute.HEADSET -> headset
                }
                val input = if (resolved != AudioRoute.HEADSET) null
                    else if (headset != null && isBtType(headset.type)) btInputDevice() else wiredInputDevice()
                currentOutputDevice = output; currentInputDevice = input
                route = resolved; routeApplied = true
                onOutputDevice?.invoke(output); onInputDevice?.invoke(input)
                LogManager.addLog("INFO", "CallAudioRouter", "per-track requested=$r route=$route output=${output?.id} input=${input?.id}")
                return true
            }
            var dev: AudioDeviceInfo? = null
            var inDev: AudioDeviceInfo? = null
            when (resolved) {
                AudioRoute.EARPIECE -> {
                    stopBluetoothSco()
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                        val target = findBuiltinEarpiece() ?: findBuiltinSpeaker() ?: error("No built-in output for $resolved")
                        communicationOwned = true
                        check(am.setCommunicationDevice(target)) { "Communication device rejected id=${target.id}" }
                        dev = target
                    } else {
                        am.isSpeakerphoneOn = false
                    }
                }
                AudioRoute.SPEAKER -> {
                    stopBluetoothSco()
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                        val target = findBuiltinSpeaker() ?: error("No built-in output for $resolved")
                        communicationOwned = true
                        check(am.setCommunicationDevice(target)) { "Communication device rejected id=${target.id}" }
                        dev = target
                    } else {
                        am.isSpeakerphoneOn = true
                    }
                }
                AudioRoute.HEADSET -> {
                    if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) am.isSpeakerphoneOn = false
                    val wired = findWiredDevice()
                    val useWired = preferWiredHeadset && wired != null
                    val bt = if (useWired) null else findBtScoDevice()
                    if (bt != null) {
                        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                            communicationOwned = true
                            check(am.setCommunicationDevice(bt)) { "Communication device rejected id=${bt.id}" }
                        } else {
                            startBluetoothSco()
                        }
                        dev = bt
                        inDev = btInputDevice()
                    } else if (!useWired && isBtHeadsetProfileConnected()) {
                        /* классический BT: SCO ещё не поднят — запускаем и ждём
                         * AudioDeviceCallback, который пере-применит маршрут */
                        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) startBluetoothSco()
                        else LogManager.addLog("INFO", "CallAudioRouter", "BT route waiting for communication endpoint")
                    } else {
                        stopBluetoothSco()
                        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S && wired != null) {
                            communicationOwned = true
                            check(am.setCommunicationDevice(wired)) { "Communication device rejected id=${wired.id}" }
                        } else {
                            /* API<31: проводная гарнитура роутится автоматически при speakerphone=off */
                            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) am.isSpeakerphoneOn = false
                            else error("Headset output unavailable")
                        }
                        dev = wired
                        inDev = wiredInputDevice()
                    }
                }
            }
            currentOutputDevice = dev
            currentInputDevice = inDev
            route = resolved; routeApplied = true
            onOutputDevice?.invoke(dev)
            onInputDevice?.invoke(inDev)
            LogManager.addLog("INFO", "CallAudioRouter",
                "requested=$r route=$route dev=${dev?.type}/${dev?.productName ?: "-"} inDev=${inDev?.type ?: "-"} " +
                    "preferWired=$preferWiredHeadset mode=${am.mode} speaker=${am.isSpeakerphoneOn} " +
                    "totalMs=${SystemClock.elapsedRealtime() - startedAt}")
            return true
        } catch (e: Exception) {
            routeApplied = false
            LogManager.addLog("ERROR", "CallAudioRouter", "applyRoute requested=$r retained=$route failed: $e")
            return false
        }
    }

    private fun findBuiltinSpeaker(): AudioDeviceInfo? =
        (devices ?: am.getDevices(AudioManager.GET_DEVICES_ALL)).firstOrNull {
            it.isSink && it.type == AudioDeviceInfo.TYPE_BUILTIN_SPEAKER
        }

    private fun findBuiltinEarpiece(): AudioDeviceInfo? =
        (devices ?: am.getDevices(AudioManager.GET_DEVICES_ALL)).firstOrNull {
            it.isSink && it.type == AudioDeviceInfo.TYPE_BUILTIN_EARPIECE
        }

    /* ── входные устройства захвата (для строки аудиоисточников рации) ── */
    fun builtinMicDevice(): AudioDeviceInfo? =
        (devices ?: am.getDevices(AudioManager.GET_DEVICES_ALL)).firstOrNull {
            it.isSource && it.type == AudioDeviceInfo.TYPE_BUILTIN_MIC
        }

    fun wiredInputDevice(): AudioDeviceInfo? =
        (devices ?: am.getDevices(AudioManager.GET_DEVICES_ALL)).firstOrNull { it.isSource && isWiredType(it.type) }

    fun btInputDevice(): AudioDeviceInfo? =
        (devices ?: am.getDevices(AudioManager.GET_DEVICES_ALL)).firstOrNull {
            it.isSource && isBtType(it.type)
        }

    private fun refreshInputDevices() {
        val wired = wiredInputDevice() != null
        val bt = btInputDevice() != null || isBtHeadsetProfileConnected()
        val changed = wired != wiredInputAvailable || bt != btInputAvailable
        wiredInputAvailable = wired
        btInputAvailable = bt
        if (changed) {
            LogManager.addLog("INFO", "CallAudioRouter", "inputDevices wired=$wired bt=$bt")
            onInputDevicesChanged?.invoke(wired, bt)
        }
    }

    /* ── мониторинг гарнитуры ── */
    @Synchronized private fun onDevicesChanged(force: Boolean = false) {
        if (!started) return
        try {
            val previous = devices?.map { "${it.id}/${it.type}/${it.isSink}/${it.isSource}" }?.sorted()
            devices = am.getDevices(AudioManager.GET_DEVICES_ALL)
            val current = devices?.map { "${it.id}/${it.type}/${it.isSink}/${it.isSource}" }?.sorted()
            if (previous != current) LogManager.addLog("INFO", "CallAudioRouter",
                "devices changed id/type/output/input=$current telecomHeadset=${telecom?.headsetAvailable} route=$route")
            hasEarpiece = findBuiltinEarpiece() != null
            // Замена descriptor требует нового запроса даже при прежней категории маршрута.
            refresh(forceAuto = force || previous != current || route == AudioRoute.HEADSET)
        } catch (e: Exception) {
            LogManager.addLog("WARN", "CallAudioRouter", "device refresh failed: $e")
        }
    }

    private fun refresh(forceAuto: Boolean = false): Boolean {
        val now = isHeadsetConnected()
        val prev = headsetAvailable
        headsetAvailable = now
        if (now != prev) {
            LogManager.addLog("INFO", "CallAudioRouter", "headsetAvailable -> $now")
        }
        val previousDesired = desiredRoute
        /* авторежим: маршрут следует за гарнитурой */
        if (autoHeadset && (forceAuto || now != prev)) {
            val want = if (now) AudioRoute.HEADSET else fallbackRoute
            if (desiredRoute != want) {
                desiredRoute = want
                LogManager.addLog("INFO", "CallAudioRouter", "auto route -> $want")
            }
        } else if (!autoHeadset) {
            /* гарнитуру отключили — откатываемся на fallback-маршрут */
            if (!now && desiredRoute == AudioRoute.HEADSET) {
                desiredRoute = fallbackRoute
                LogManager.addLog("INFO", "CallAudioRouter", "headset lost, fallback route -> $fallbackRoute")
            }
            /* гарнитура появилась, а пользователь ранее выбрал её — восстанавливаем */
            if (now && !prev && preferredRoute == AudioRoute.HEADSET && desiredRoute != AudioRoute.HEADSET) {
                desiredRoute = AudioRoute.HEADSET
                LogManager.addLog("INFO", "CallAudioRouter", "headset available, restore preferred route -> HEADSET")
            }
        }
        val applied = if (forceAuto || desiredRoute != previousDesired || !routeApplied) applyRoute(desiredRoute) else true
        if (applied && (now != prev || route == AudioRoute.HEADSET)) {
            onChanged?.invoke(route, now)
        }
        refreshInputDevices()
        return applied
    }

    private fun isHeadsetConnected(): Boolean = telecom?.headsetAvailable
        ?: (findWiredDevice() != null || (!globalRouting && findBtMediaDevice() != null) ||
            (!globalRouting && wiredInputDevice() != null) ||
            findBtScoDevice() != null || isBtHeadsetProfileConnected())

    private fun findWiredDevice(): AudioDeviceInfo? =
        (devices ?: am.getDevices(AudioManager.GET_DEVICES_ALL)).firstOrNull { it.isSink && isWiredType(it.type) }

    private fun isBtType(type: Int): Boolean = type == AudioDeviceInfo.TYPE_BLUETOOTH_SCO ||
        type == AudioDeviceInfo.TYPE_BLUETOOTH_A2DP || (Build.VERSION.SDK_INT >= 31 && type == AudioDeviceInfo.TYPE_BLE_HEADSET)

    private fun findBtMediaDevice(): AudioDeviceInfo? =
        (devices ?: am.getDevices(AudioManager.GET_DEVICES_ALL)).firstOrNull {
            it.isSink && (it.type == AudioDeviceInfo.TYPE_BLUETOOTH_A2DP ||
                (Build.VERSION.SDK_INT >= 31 && it.type == AudioDeviceInfo.TYPE_BLE_HEADSET))
        }

    private fun findBtScoDevice(): AudioDeviceInfo? {
        val available = if (Build.VERSION.SDK_INT >= 31) am.availableCommunicationDevices
            else (devices ?: am.getDevices(AudioManager.GET_DEVICES_ALL)).toList()
        return available.firstOrNull { it.isSink && isBtType(it.type) && it.type != AudioDeviceInfo.TYPE_BLUETOOTH_A2DP }
    }

    private fun isWiredType(type: Int): Boolean = when (type) {
        AudioDeviceInfo.TYPE_WIRED_HEADSET,
        AudioDeviceInfo.TYPE_WIRED_HEADPHONES,
        AudioDeviceInfo.TYPE_USB_HEADSET,
        AudioDeviceInfo.TYPE_USB_DEVICE,
        AudioDeviceInfo.TYPE_USB_ACCESSORY -> true
        else -> false
    }

    /* ── Bluetooth headset profile ── */
    /** Под монитором start; listener относится только к поколению этого запроса. */
    private fun bindBtHeadsetProfile() {
        val generation = ++btProfileGeneration
        try {
            val adapter = BluetoothAdapter.getDefaultAdapter() ?: return
            val received = mutableSetOf<BluetoothProfile>() // Дубликат callback не возобновляет уже освобождённый proxy.
            val accepted = adapter.getProfileProxy(ChatApplication.instance, object : BluetoothProfile.ServiceListener {
                override fun onServiceConnected(profile: Int, proxy: BluetoothProfile) {
                    synchronized(this@CallAudioRouter) {
                        if (proxy in received && proxy !== btHeadset) {
                            LogManager.addLog("WARN", "CallAudioRouter", "retired BT proxy callback ignored proxy=$proxy generation=$generation")
                            releaseBtProxies()
                            scheduleCleanup()
                            return
                        }
                        received.add(proxy)
                        btProxies[proxy] = adapter to profile
                        if (!started || generation != btProfileGeneration || profile != BluetoothProfile.HEADSET || proxy !is BluetoothHeadset) {
                            LogManager.addLog("WARN", "CallAudioRouter", "retired BT proxy received proxy=$proxy profile=$profile " +
                                "generation=$generation current=$btProfileGeneration started=$started")
                            releaseBtProxies()
                            scheduleCleanup()
                            return
                        }
                        btHeadset = proxy
                        releaseBtProxies()
                        scheduleCleanup()
                        LogManager.addLog("INFO", "CallAudioRouter", "bt headset profile connected proxy=$proxy generation=$generation")
                        onDevicesChanged()
                    }
                }

                override fun onServiceDisconnected(profile: Int) {
                    if (profile != BluetoothProfile.HEADSET) return
                    synchronized(this@CallAudioRouter) {
                        if (!started || generation != btProfileGeneration) {
                            LogManager.addLog("DEBUG", "CallAudioRouter", "stale BT disconnect ignored " +
                                "generation=$generation current=$btProfileGeneration")
                            return
                        }
                        // Сервис отключён; proxy остаётся нашим до closeProfileProxy.
                        LogManager.addLog("INFO", "CallAudioRouter", "bt headset profile disconnected, proxy retained " +
                            "proxy=$btHeadset generation=$generation")
                        onDevicesChanged()
                    }
                }
            }, BluetoothProfile.HEADSET)
            LogManager.addLog(if (accepted) "INFO" else "WARN", "CallAudioRouter", "bt profile request generation=$generation accepted=$accepted")
        } catch (e: Exception) {
            LogManager.addLog("WARN", "CallAudioRouter", "bt profile request failed generation=$generation: ${e.message}")
        }
    }

    /** Под монитором router: закрыть все proxy, кроме используемого; ошибки сохраняют adapter/profile/proxy. */
    private fun releaseBtProxies() {
        for ((proxy, owner) in btProxies.toMap()) {
            if (proxy === btHeadset) continue
            try {
                owner.first.closeProfileProxy(owner.second, proxy)
                btProxies.remove(proxy)
                LogManager.addLog("INFO", "CallAudioRouter", "bt owned proxy closed proxy=$proxy generation=$btProfileGeneration")
            } catch (e: Exception) {
                LogManager.addLog("ERROR", "CallAudioRouter", "bt owned proxy close failed proxy=$proxy " +
                    "generation=$btProfileGeneration; retained for retry: $e")
            }
        }
    }

    /** Сильный process-владелец остаётся и при отказе Handler; явный stop/start также повторяет cleanup. */
    private fun scheduleCleanup() {
        cleanupHandler.removeCallbacks(cleanupRetry)
        if (!cleanupPending) { retained.remove(this); cleanupDelay = 250; return }
        retained.add(this)
        val delay = cleanupDelay
        cleanupDelay = (cleanupDelay * 2).coerceAtMost(30_000)
        if (!cleanupHandler.postDelayed(cleanupRetry, delay)) {
            LogManager.addLog("ERROR", "CallAudioRouter", "cleanup retry rejected; owner retained generation=$btProfileGeneration delayMs=$delay")
        } else {
            LogManager.addLog("DEBUG", "CallAudioRouter", "cleanup retry scheduled generation=$btProfileGeneration delayMs=$delay")
        }
    }

    private fun hasBtConnectPermission(): Boolean =
        Build.VERSION.SDK_INT < Build.VERSION_CODES.S ||
        ChatApplication.instance.checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) ==
        PackageManager.PERMISSION_GRANTED

    private fun isBtHeadsetProfileConnected(): Boolean {
        if (!hasBtConnectPermission()) return false
        return try {
            btHeadset?.connectedDevices?.isNotEmpty() == true
        } catch (e: SecurityException) {
            LogManager.addLog("WARN", "CallAudioRouter", "bt connectedDevices denied: ${e.message}")
            false
        }
    }

    /* ── helpers ── */
    /** Снять только собственные глобальные заявки, независимо подтверждая каждую операцию. */
    private fun releaseGlobalRoute() {
        var failure: Exception? = null
        try { stopBluetoothSco() } catch (e: Exception) { failure = e }
        if (communicationOwned) try {
            am.clearCommunicationDevice()
            communicationOwned = false
        } catch (e: Exception) {
            LogManager.addLog("ERROR", "CallAudioRouter", "communication device release failed; retained for retry: $e")
            if (failure == null) failure = e
        }
        failure?.let { throw it }
    }

    private fun startBluetoothSco() {
        scoOwned = true
        scoFlagOwned = true
        am.startBluetoothSco()
        am.isBluetoothScoOn = true
    }

    private fun stopBluetoothSco() {
        var failure: Exception? = null
        if (scoOwned) try { am.stopBluetoothSco(); scoOwned = false } catch (e: Exception) {
            LogManager.addLog("ERROR", "CallAudioRouter", "stopBluetoothSco failed; retained for retry: $e")
            failure = e
        }
        if (scoFlagOwned) try { am.isBluetoothScoOn = false; scoFlagOwned = false } catch (e: Exception) {
            LogManager.addLog("ERROR", "CallAudioRouter", "clear Bluetooth SCO flag failed; retained for retry: $e")
            if (failure == null) failure = e
        }
        failure?.let { throw it }
    }
}

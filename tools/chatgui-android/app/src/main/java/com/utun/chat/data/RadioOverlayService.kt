package com.utun.chat.data

import com.utun.chat.R
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.graphics.PixelFormat
import android.graphics.Point
import android.graphics.drawable.GradientDrawable
import android.os.Build
import android.os.IBinder
import android.os.Looper
import android.provider.Settings
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.ViewConfiguration
import android.view.WindowManager
import android.widget.TextView
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat
import androidx.core.view.doOnLayout
import com.utun.chat.ChatApplication
import kotlin.math.abs
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineExceptionHandler
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow

/**
 * Оверлей PTT поверх других приложений: плавающая круглая кнопка, drag-and-drop.
 * Касание сразу запрашивает ручной PTT (RadioAudioEngine.talkBegin/talkEnd),
 * отпускание — завершает с хвостом 250мс. Двойной тап фиксирует передачу (latch) без удержания,
 * следующее нажатие или drag снимают latch. Работает пока включена опция
 * «PTT поверх приложений» и разрешено «поверх других приложений» (SYSTEM_ALERT_WINDOW).
 * ACTION_CANCEL, отказ окна и stop сразу снимают только OVERLAY, сохраняя другие источники PTT.
 * При включённой автоматике сервис сохраняет доступ к микрофону без плавающей кнопки.
 * Сервис обеспечивает foreground-режим; PCM-захватом и окном автоматики управляет RadioAudioEngine.
 * Управление — на main. start/stop сериализуются через starting/pendingStop; новый запуск ждёт onDestroy.
 * Окно хранится до успешного remove/подтверждённого отсутствия. При отказе уничтоженный владелец остаётся
 * в currentService для повторной очистки через sync; старые touch/layout callbacks не управляют новым окном.
 */
class RadioOverlayService : Service() {

    companion object {
        const val CHANNEL_ID = "radio_overlay"
        const val NOTIF_ID = 1005 // Отдельно от CallService (1002), входящего и результатов вызовов.
        const val COLOR_IDLE = 0xFF81C784.toInt()
        const val COLOR_TALKING = 0xFFE53935.toInt()
        const val COLOR_WAITING = 0xFFFFD54F.toInt()
        const val DRAG_LOCK_MS = 300L

        private var defaultCenter: Point? = null
        private var currentService: RadioOverlayService? = null

        fun refreshLanguage() {
            checkMain()
            currentService?.let { service ->
                if (service.destroyed || pendingStop || !running) return
                try {
                    service.createChannel()
                    requireNotNull(service.getSystemService(NotificationManager::class.java)) { "notification manager unavailable" }
                        .notify(NOTIF_ID, service.buildNotification())
                    LogManager.addLog("DEBUG", "RadioOverlay", "notification language refreshed id=$NOTIF_ID")
                } catch (e: Exception) {
                    LogManager.addLog("ERROR", "RadioOverlay", "notification refresh failed id=$NOTIF_ID: $e")
                }
            }
        }

        // Координаты центра обычной PTT в пикселях экрана; обновляются из layout чата.
        fun setDefaultCenter(x: Int, y: Int) {
            checkMain()
            if (defaultCenter?.x == x && defaultCenter?.y == y) return
            defaultCenter = Point(x, y)
            currentService?.placeAtDefaultCenter()
        }

        private val _isShown = MutableStateFlow(false)
        val isShown: StateFlow<Boolean> = _isShown // Окно добавлено либо эффект add/remove ещё не подтверждён.

        /* Гонка start/stop: stop() до вызова startForeground() роняет FGS.
         * Пока сервис стартует (starting) — взводим pendingStop, а onCreate
         * после startForeground() сам вызовет stopSelf(). */
        @Volatile private var running = false
        @Volatile private var starting = false
        @Volatile private var pendingStop = false
        private var restartAfterStop = false

        fun canDrawOverlays(ctx: Context): Boolean = try {
            Build.VERSION.SDK_INT < Build.VERSION_CODES.M || Settings.canDrawOverlays(ctx)
        } catch (e: Exception) {
            LogManager.addLog("WARN", "RadioOverlay", "Overlay permission query failed: $e")
            false
        }

        private var activityVisible = false
        private var waitingOverlayPermission = false
        var overlayPermissionRequester: (() -> Unit)? = null
        private val _status = MutableStateFlow<Int?>(null)
        val status: StateFlow<Int?> = _status

        private fun setStatus(message: Int?) {
            if (_status.value == message) return
            _status.value = message
            LogManager.addLog("INFO", "RadioOverlay", "status=$message shown=${_isShown.value} running=$running starting=$starting")
        }

        /** Сохранить намерение пользователя; разрешения запрашиваются только после явного включения. */
        suspend fun setEnabled(enabled: Boolean) {
            checkMain()
            try { ChatApplication.instance.configProvider.setRadioOverlay(enabled) } catch (e: Exception) {
                if (e is CancellationException) throw e
                LogManager.addLog("ERROR", "RadioOverlay", "Overlay settings write failed enabled=$enabled: $e")
                throw e
            }
            LogManager.addLog("INFO", "RadioOverlay", "User requested overlay enabled=$enabled")
            if (enabled) requestPermissions() else {
                waitingOverlayPermission = false
                sync(ChatApplication.instance)
            }
        }

        fun setActivityVisible(visible: Boolean) {
            checkMain()
            activityVisible = visible
            if (visible) sync(ChatApplication.instance)
        }

        // Запросы разрешений выполняются только после явной команды пользователя.
        fun requestPermissions() {
            checkMain()
            val ctx = ChatApplication.instance
            val provider = ctx.configProvider
            if (provider.getRadioOverlay() && !canDrawOverlays(ctx)) {
                setStatus(R.string.overlay_permission_required)
                if (waitingOverlayPermission) return
                val request = overlayPermissionRequester
                if (request == null) {
                    LogManager.addLog("WARN", "RadioOverlay", "Overlay permission request deferred: Activity unavailable")
                    return
                }
                waitingOverlayPermission = true
                LogManager.addLog("INFO", "RadioOverlay", "Requesting overlay permission")
                try { request() } catch (e: Exception) {
                    waitingOverlayPermission = false
                    LogManager.addLog("ERROR", "RadioOverlay", "Overlay permission request failed; can retry: $e")
                }
                return
            }
            if ((provider.getRadioOverlay() || provider.getInt("radio.auto_enabled") != 0) && !MicrophonePermission.granted()) {
                setStatus(R.string.microphone_access_required)
                MicrophonePermission.request("Floating PTT / VAD") { sync(ctx) }
                return
            }
            sync(ctx)
        }

        fun overlayPermissionResult() {
            checkMain()
            val wasWaiting = waitingOverlayPermission
            waitingOverlayPermission = false
            val ctx = ChatApplication.instance
            val granted = canDrawOverlays(ctx)
            LogManager.addLog(if (granted) "INFO" else "WARN", "RadioOverlay", "Overlay permission result=$granted pending=$wasWaiting")
            val provider = ctx.configProvider
            if (wasWaiting && granted && provider.getRadioOverlay()) requestPermissions()
            else sync(ctx)
        }

        /** Согласовать настройки/разрешения/активность; очистить прежнее окно до замены владельца. */
        fun sync(ctx: Context) {
            checkMain()
            currentService?.takeIf { it.destroyed }?.let { retired ->
                if (!retired.removeOverlay()) {
                    LogManager.addLog("ERROR", "RadioOverlay", "Service start blocked: old window cleanup pending")
                    return
                }
                currentService = null
            }
            val provider = ChatApplication.instance.configProvider
            val radio = RadioAudioEngine.instance()
            val enabled = provider.getRadioOverlay()
            val vad = provider.getInt("radio.auto_enabled") != 0
            val capture = radio.captureWanted.value
            val overlayAllowed = canDrawOverlays(ctx)
            val micAllowed = MicrophonePermission.granted()
            when {
                !enabled -> setStatus(null)
                !overlayAllowed -> setStatus(R.string.overlay_permission_required)
                !micAllowed -> setStatus(R.string.microphone_access_required)
                !radio.isActive() -> setStatus(R.string.overlay_when_radio_on)
            }
            if (!radio.isActive() || (!capture && !vad && (!enabled || !overlayAllowed)) || !micAllowed) {
                if (!micAllowed && running) {
                    LogManager.addLog("WARN", "RadioOverlay", "Microphone permission revoked; stopping capture and service")
                    radio.cancelTalk()
                    radio.reconfigure()
                }
                stop(ctx)
                return
            }
            if (running && pendingStop) {
                restartAfterStop = true
                return
            }
            if (running) {
                currentService?.refresh()
                return
            }
            if (starting) { pendingStop = false; return }
            if (!activityVisible) {
                if (enabled && overlayAllowed) setStatus(R.string.overlay_open_app)
                LogManager.addLog("DEBUG", "RadioOverlay", "Service start deferred until Activity resume")
                return
            }
            if (enabled && overlayAllowed) setStatus(R.string.overlay_starting)
            pendingStop = false
            starting = true
            try {
                check(ctx.startForegroundService(Intent(ctx, RadioOverlayService::class.java)) != null) { "Overlay service component unavailable" }
                LogManager.addLog("INFO", "RadioOverlay", "Microphone service requested from visible Activity overlay=$enabled vad=$vad capture=$capture")
            } catch (e: Exception) {
                starting = false
                if (enabled) setStatus(R.string.overlay_start_failed)
                LogManager.addLog("ERROR", "RadioOverlay", "Service start failed: $e")
            }
        }

        /** Снять свой PTT/окно сразу; запрос остановки до onCreate ждёт foreground-handshake. Отказ допускает повтор. */
        fun stop(ctx: Context) {
            checkMain()
            restartAfterStop = false
            currentService?.removeOverlay()
            LogManager.addLog("DEBUG", "RadioOverlay", "Service stop requested shown=${_isShown.value} running=$running starting=$starting")
            if (starting) {
                pendingStop = true
                return
            }
            if (running && !pendingStop) {
                pendingStop = true
                try {
                    if (!ctx.stopService(Intent(ctx, RadioOverlayService::class.java))) {
                        pendingStop = false
                        LogManager.addLog("WARN", "RadioOverlay", "Service stop not accepted; can retry")
                    }
                } catch (e: Exception) {
                    pendingStop = false
                    LogManager.addLog("ERROR", "RadioOverlay", "Service stop failed; can retry: $e")
                }
            }
        }

        private fun checkMain() = check(Looper.myLooper() == Looper.getMainLooper()) {
            val message = "RadioOverlayService requires main thread: caller=${Thread.currentThread().name}"
            LogManager.addLog("ERROR", "RadioOverlay", message)
            message
        }
    }

    private var windowManager: WindowManager? = null
    private var overlayView: View? = null
    private var params: WindowManager.LayoutParams? = null
    private var talking = false
    private var positionDragged = false
    private var positioningPending = false
    private var overlayReady = false // addView подтверждён; false также у окна с неясным результатом IPC.
    private var destroyed = false
    private val serviceScope = CoroutineScope(SupervisorJob() + Dispatchers.Main + CoroutineExceptionHandler { _, error ->
        LogManager.addLog("ERROR", "RadioOverlay", "Service observer failed: $error")
        if (currentService === this && !destroyed) {
            removeOverlay()
            setStatus(R.string.overlay_show_failed)
            pendingStop = true
            stopSelf()
        }
    })

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        checkMain()
        super.onCreate()
        var operation = "channel"
        try {
            createChannel()
            operation = "foreground"
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                ServiceCompat.startForeground(this, NOTIF_ID, buildNotification(),
                    ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE)
            } else {
                startForeground(NOTIF_ID, buildNotification())
            }
            operation = "previous owner"
            currentService?.takeIf { it !== this }?.let { previous ->
                check(previous.destroyed && previous.removeOverlay()) { "previous overlay owner still retained" }
            }
            currentService = this
            running = true
            starting = false
            if (pendingStop) {
                LogManager.addLog("INFO", "RadioOverlay", "Cancelled service start completed foreground handshake")
                stopSelf(); return
            }
            operation = "window service"
            windowManager = getSystemService(WINDOW_SERVICE) as? WindowManager ?: error("window manager unavailable")
            serviceScope.launch { RadioAudioEngine.instance().talkingFlow.collect {
                if (!it) talking = false
                updateButtonColor()
            } }
            serviceScope.launch { RadioAudioEngine.instance().captureReady.collect { updateButtonColor() } }
            RadioAudioEngine.instance().refreshMicrophonePolicy()
            serviceScope.launch {
                while (isActive) {
                    delay(1000)
                    if (!MicrophonePermission.granted() ||
                        (overlayView != null && !canDrawOverlays(this@RadioOverlayService))) sync(this@RadioOverlayService)
                }
            }
            LogManager.addLog("INFO", "RadioOverlay", "Foreground microphone service ready notification=$NOTIF_ID")
        } catch (e: Exception) {
            LogManager.addLog("ERROR", "RadioOverlay", "service setup failed operation=$operation: $e")
            if (currentService == null) currentService = this
            if (currentService === this) { starting = false; running = false; pendingStop = true }
            setStatus(R.string.microphone_service_failed)
            stopSelf()
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        checkMain()
        refresh()
        return START_NOT_STICKY
    }

    private fun refresh() {
        if (destroyed || currentService !== this || !running || pendingStop ||
            !RadioAudioEngine.instance().isActive() || !MicrophonePermission.granted()) {
            removeOverlay()
            if (currentService === this) pendingStop = true
            stopSelf()
            return
        }
        val provider = ChatApplication.instance.configProvider
        val wantOverlay = provider.getRadioOverlay() && canDrawOverlays(this)
        if (overlayView != null && !overlayReady && !removeOverlay()) return
        if (wantOverlay && overlayView == null) {
            try { addOverlay() } catch (e: Exception) {
                removeOverlay()
                setStatus(R.string.overlay_show_failed)
                LogManager.addLog("ERROR", "RadioOverlay", "Overlay creation failed: $e")
                if (provider.getInt("radio.auto_enabled") == 0 && !RadioAudioEngine.instance().captureWanted.value) {
                    pendingStop = true; stopSelf()
                }
                return
            }
        }
        if (!wantOverlay) removeOverlay()
        if (overlayReady) setStatus(R.string.overlay_active)
        if (!wantOverlay && provider.getInt("radio.auto_enabled") == 0 && !RadioAudioEngine.instance().captureWanted.value) {
            pendingStop = true; stopSelf()
        }
    }

    /** Снять только свой PTT. Ссылка на окно сохраняется до успешного remove либо подтверждённого отсутствия. */
    private fun removeOverlay(): Boolean {
        talking = false
        overlayReady = false
        positioningPending = false
        if (currentService === this) RadioAudioEngine.instance().cancelTalk(RadioPttSource.OVERLAY)
        val view = overlayView ?: return true
        try { requireNotNull(windowManager) { "owned window without manager" }.removeView(view) } catch (e: IllegalArgumentException) {
            LogManager.addLog("WARN", "RadioOverlay", "Overlay already absent view=${System.identityHashCode(view)}: $e")
        } catch (e: Exception) {
            if (currentService === this) setStatus(R.string.overlay_show_failed)
            LogManager.addLog("ERROR", "RadioOverlay", "Overlay removal failed view=${System.identityHashCode(view)}; retained for retry: $e")
            return false
        }
        overlayView = null
        params = null
        if (currentService === this) _isShown.value = false
        LogManager.addLog("INFO", "RadioOverlay", "Overlay removed view=${System.identityHashCode(view)}; own transmission cancelled")
        return true
    }

    private fun createChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val ch = NotificationChannel(CHANNEL_ID, AppLanguage.text(R.string.radio_ptt), NotificationManager.IMPORTANCE_LOW).apply {
                description = AppLanguage.text(R.string.radio_overlay_description)
            }
            requireNotNull(getSystemService(NotificationManager::class.java)) { "notification manager unavailable" }.createNotificationChannel(ch)
        }
    }

    private fun buildNotification() = NotificationCompat.Builder(AppLanguage.context(), CHANNEL_ID)
        .setSmallIcon(android.R.drawable.ic_btn_speak_now)
        .setContentTitle(AppLanguage.text(R.string.radio_active))
        .setContentText(AppLanguage.text(R.string.radio_notification))
        .setOngoing(true)
        .setOnlyAlertOnce(true)
        .build()

    private fun addOverlay() {
        val wm = requireNotNull(windowManager) { "window manager unavailable" }
        val v = TextView(this).apply {
            text = "\uD83C\uDF99"
            textSize = 21f
            gravity = android.view.Gravity.CENTER
            background = GradientDrawable().apply {
                shape = GradientDrawable.OVAL
                setColor(COLOR_IDLE)
            }
        }
        val touchSlop = (ViewConfiguration.get(this).scaledTouchSlop * 2.5f).toInt()
        val doubleTapTimeout = ViewConfiguration.getDoubleTapTimeout().toLong().coerceAtMost(RadioAudioEngine.RELEASE_HOLD_MS)
        val lp = WindowManager.LayoutParams(
            WindowManager.LayoutParams.WRAP_CONTENT,
            WindowManager.LayoutParams.WRAP_CONTENT,
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O)
                WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY
            else
                @Suppress("DEPRECATION") WindowManager.LayoutParams.TYPE_PHONE,
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE,
            PixelFormat.TRANSLUCENT
        ).apply {
            width = dp(67); height = dp(67)
            gravity = Gravity.TOP or Gravity.LEFT
            x = defaultCenter?.let { it.x - width / 2 } ?: ((resources.displayMetrics.widthPixels - width) / 2)
            y = defaultCenter?.let { it.y - height / 2 } ?: dp(200)
        }
        v.setOnTouchListener(object : View.OnTouchListener {
            private var startX = 0; private var startY = 0
            private var touchX = 0f; private var touchY = 0f
            private var dragging = false
            private var dragAllowed = true
            private var latched = false
            private var lastTapUpTime = 0L
            private var wasLatched = false
            private var secondTap = false

            override fun onTouch(view: View, event: MotionEvent): Boolean {
                if (destroyed || currentService !== this@RadioOverlayService || overlayView !== v || !overlayReady || pendingStop) return false
                if (event.action == MotionEvent.ACTION_DOWN && !RadioAudioEngine.instance().isTalking()) {
                    latched = false; lastTapUpTime = 0; talking = false
                }
                try {
                    if (!MicrophonePermission.granted() || !ChatApplication.instance.configProvider.getRadioOverlay() ||
                        !canDrawOverlays(this@RadioOverlayService)) {
                        removeOverlay()
                        sync(this@RadioOverlayService)
                        return false
                    }
                    when (event.action) {
                        MotionEvent.ACTION_DOWN -> {
                            startX = params?.x ?: lp.x
                            startY = params?.y ?: lp.y
                            touchX = event.rawX
                            touchY = event.rawY
                            dragging = false
                            dragAllowed = true
                            wasLatched = latched
                            secondTap = lastTapUpTime != 0L && event.eventTime - lastTapUpTime <= doubleTapTimeout
                            if (wasLatched) {
                                latched = false
                                lastTapUpTime = 0L
                                endTalk()
                            } else beginTalk()
                            return true
                        }
                        MotionEvent.ACTION_MOVE -> {
                            if (dragging) {
                                lp.x = startX + (event.rawX - touchX).toInt()
                                lp.y = startY + (event.rawY - touchY).toInt()
                                wm.updateViewLayout(v, lp)
                            } else if (dragAllowed) {
                                if (event.eventTime - event.downTime > DRAG_LOCK_MS) {
                                    dragAllowed = false
                                } else if (abs(event.rawX - touchX) > touchSlop || abs(event.rawY - touchY) > touchSlop) {
                                    dragging = true
                                    positionDragged = true
                                    latched = false
                                    lastTapUpTime = 0L
                                    endTalk()
                                    lp.x = startX + (event.rawX - touchX).toInt()
                                    lp.y = startY + (event.rawY - touchY).toInt()
                                    wm.updateViewLayout(v, lp)
                                }
                            }
                            return true
                        }
                        MotionEvent.ACTION_UP -> {
                            val tapped = talking && !dragging && event.eventTime - event.downTime <= RadioAudioEngine.RELEASE_HOLD_MS
                            if (!wasLatched && tapped && secondTap) {
                                latched = true
                                lastTapUpTime = 0L
                            } else {
                                lastTapUpTime = if (!wasLatched && tapped) event.eventTime else 0L
                                endTalk()
                            }
                            return true
                        }
                        MotionEvent.ACTION_CANCEL -> {
                            latched = false
                            lastTapUpTime = 0L
                            endTalk(immediate = true)
                            return true
                        }
                    }
                    return false
                } catch (e: Exception) {
                    latched = false; lastTapUpTime = 0L
                    removeOverlay()
                    setStatus(R.string.overlay_show_failed)
                    LogManager.addLog("ERROR", "RadioOverlay", "Overlay touch failed action=${event.action} x=${event.rawX} " +
                        "y=${event.rawY}; own PTT cancelled: $e")
                    if (ChatApplication.instance.configProvider.getInt("radio.auto_enabled") == 0) { pendingStop = true; stopSelf() }
                    return true
                }
            }
        })
        overlayView = v
        params = lp
        overlayReady = false
        _isShown.value = true // addView может бросить после добавления окна: до rollback считаем окно возможным.
        wm.addView(v, lp)
        overlayReady = true
        updateButtonColor()
        positionDragged = false
        placeAtDefaultCenter()
        LogManager.addLog("INFO", "RadioOverlay", "overlay shown view=${System.identityHashCode(v)} x=${lp.x} y=${lp.y}")
    }

    /* Поправка по фактическим координатам окна учитывает системные отступы оверлея. */
    private fun placeAtDefaultCenter() {
        if (destroyed || currentService !== this || !overlayReady) return
        val v = overlayView ?: return
        if (positionDragged) return
        if (!v.isLaidOut) {
            v.doOnLayout { if (overlayView === v && !destroyed) placeAtDefaultCenter() }
            return
        }
        if (positioningPending) return
        positioningPending = true
        v.postOnAnimation {
            if (destroyed || currentService !== this || overlayView !== v || !overlayReady) return@postOnAnimation
            positioningPending = false
            val target = defaultCenter ?: return@postOnAnimation
            val lp = params ?: return@postOnAnimation
            if (overlayView !== v || positionDragged) return@postOnAnimation
            try {
                val actual = IntArray(2)
                v.getLocationOnScreen(actual)
                val dx = target.x - actual[0] - v.width / 2
                val dy = target.y - actual[1] - v.height / 2
                if (dx == 0 && dy == 0) return@postOnAnimation
                lp.x += dx
                lp.y += dy
                requireNotNull(windowManager) { "window manager unavailable" }.updateViewLayout(v, lp)
                LogManager.addLog("DEBUG", "RadioOverlay", "default position center=${target.x},${target.y} x=${lp.x} y=${lp.y}")
            } catch (e: Exception) {
                LogManager.addLog("ERROR", "RadioOverlay", "default position failed view=${System.identityHashCode(v)} " +
                    "target=${target.x},${target.y} params=${lp.x},${lp.y}: $e")
            }
        }
    }

    private fun beginTalk() {
        if (talking) return
        if (!RadioAudioEngine.instance().talkBegin(RadioPttSource.OVERLAY)) return
        talking = true
        updateButtonColor()
    }

    private fun endTalk(immediate: Boolean = false) {
        if (!talking && !immediate) return
        talking = false
        if (immediate) RadioAudioEngine.instance().cancelTalk(RadioPttSource.OVERLAY)
        else RadioAudioEngine.instance().talkEnd(RadioPttSource.OVERLAY)
        updateButtonColor()
    }

    private fun updateButtonColor() {
        val radio = RadioAudioEngine.instance()
        (overlayView?.background as? GradientDrawable)?.apply {
            setColor(when {
                !radio.talkingFlow.value -> COLOR_IDLE
                !radio.captureReady.value -> COLOR_WAITING
                else -> COLOR_TALKING
            })
            setStroke(if (radio.captureReady.value) 0 else dp(3), COLOR_WAITING)
        }
    }

    private fun dp(v: Int): Int = (v * resources.displayMetrics.density).toInt()

    override fun onDestroy() {
        checkMain()
        if (destroyed) return
        destroyed = true
        val owner = currentService === this
        serviceScope.cancel()
        removeOverlay()
        var restart = false
        if (owner) {
            starting = false; running = false; pendingStop = false
            if (overlayView == null) { currentService = null; windowManager = null }
            if (_status.value == R.string.overlay_active || _status.value == R.string.overlay_starting) {
                setStatus(if (ChatApplication.instance.configProvider.getRadioOverlay()) R.string.overlay_stopped else null)
            }
            try { stopForeground(STOP_FOREGROUND_REMOVE) } catch (e: Exception) {
                LogManager.addLog("ERROR", "RadioOverlay", "Foreground notification removal failed id=$NOTIF_ID: $e")
            }
            restart = restartAfterStop
            restartAfterStop = false
        }
        LogManager.addLog("INFO", "RadioOverlay", "overlay destroyed owner=$owner windowRetained=${overlayView != null} restart=$restart")
        super.onDestroy()
        if (restart) sync(ChatApplication.instance)
    }
}

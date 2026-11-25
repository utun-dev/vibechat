/* app_process + APK: весь протокол снимка настроек рации, без AudioTrack/микрофона и профиля приложения.
 * Аргумент — новый /data/local/tmp/utun-radio-config-audit-<id>/profile; внешняя сеть/обнаружение отключены.
 * Init/stop/restart вызываются напрямую: frontend media-владельцев в этом процессе нет. */
import android.os.Handler
import android.os.Looper
import com.utun.chat.data.ChatEventCallback
import com.utun.chat.data.NativeLib
import com.utun.chat.data.RadioAutoSettings
import java.io.File
import java.util.concurrent.atomic.AtomicInteger
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeout
import kotlin.system.exitProcess

open class RadioConfigReadOnly {
    fun getString(key: String): String? = null
    fun getInt(key: String): Int = 0
}

class RadioConfigProbe : RadioConfigReadOnly() {
    val acks = AtomicInteger()
    val results = Channel<Pair<Long, Boolean>>(Channel.UNLIMITED)
    val snapshots = Channel<String>(Channel.UNLIMITED)
    fun onRadioSettingsApplied(request: Long, success: Boolean) {
        acks.incrementAndGet()
        check(results.trySend(request to success).isSuccess)
    }
    fun onNativeLog(level: Int, category: String, message: String) {
        if (level == 2 && category == "radio" && message.contains("Android radio config request="))
            check(snapshots.trySend(message).isSuccess)
    }
}

private fun nativeInit(directory: File, callback: Any?, logger: RadioConfigProbe) {
    val method = NativeLib::class.java.getDeclaredMethod("nativeInit", String::class.java, Int::class.javaPrimitiveType,
        Any::class.java, Any::class.java)
    method.isAccessible = true
    check(method.invoke(NativeLib, directory.path, 0, callback, logger) == true)
}

private fun native(name: String, config: String? = null): Boolean {
    val method = if (config == null) NativeLib::class.java.getDeclaredMethod(name)
        else NativeLib::class.java.getDeclaredMethod(name, String::class.java)
    method.isAccessible = true
    return (if (config == null) method.invoke(NativeLib) else method.invoke(NativeLib, config)) == true
}

private fun rawConfigure(id: Long, values: IntArray?): Boolean {
    val method = NativeLib::class.java.getDeclaredMethod("nativeRadioConfigure", Long::class.javaPrimitiveType, IntArray::class.java)
    method.isAccessible = true
    return method.invoke(NativeLib, id, values) == true
}

fun main(args: Array<String>) {
    check(args.size == 1)
    val directory = File(args.single()).canonicalFile
    check(directory.path.startsWith("/data/local/tmp/utun-radio-config-audit-") && !directory.exists())
    check(directory.mkdirs())
    if (Looper.getMainLooper() == null) Looper.prepareMainLooper()
    val main = Handler(Looper.getMainLooper())
    val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    val ready = Channel<Unit>(Channel.UNLIMITED)
    val probe = RadioConfigProbe()
    NativeLib.setEventCallback(object : ChatEventCallback {
        override fun onEvent(type: Int, data: ByteArray?) { if (type == 12) main.post { check(ready.trySend(Unit).isSuccess) } }
    })
    var nextRequest = 0x1234567800000000L
    suspend fun apply(values: IntArray, success: Boolean, settings: RadioAutoSettings? = null) {
        val request = ++nextRequest
        check(if (settings == null) rawConfigure(request, values) else NativeLib.radioConfigure(request, settings))
        withTimeout(5_000) {
            check(probe.results.receive() == (request to success))
            val log = probe.snapshots.receive()
            val tokens = listOf("request=$request ", "success=${if (success) 1 else 0} ", "enabled=${values[0]} ",
                "mode=${values[1]} ", "on=${values[2]} ", "off=${values[3]} ", "hangover=${values[4]}ms ",
                "threshold=${values[5]}% ", "vad_gain=${values[6]}dB")
            check(tokens.all(log::contains)) { "radio snapshot mismatch: $log" }
        }
    }
    suspend fun drained() = withTimeout(5_000) {
        NativeLib.ping()
        while (!NativeLib.isResponsive()) delay(1)
        check(probe.results.tryReceive().isFailure && probe.snapshots.tryReceive().isFailure)
    }
    scope.launch {
        try {
            val config = "[global]\ndb_path=${directory.path}\nauto_sockets=no\ndebug_level=error\n" +
                "[ntp]\nenabled=no\n[stun]\nenabled=no\n[chatserver]\nstorage_autoload=0\n" +
                "local_invites_enabled=0\nlocal_broadcasts_enabled=0\n[debug]\nradio=debug\n"
            val selected = RadioAutoSettings(true, RadioAutoSettings.LEVEL, -31, -67, 345, 73, 18)
            nativeInit(directory, null, probe)
            check(NativeLib.start(config)); withTimeout(10_000) { ready.receive() }
            check(!NativeLib.radioConfigure(1, selected)) // Принять запрос без получателя ACK нельзя.
            drained()
            check(withContext(Dispatchers.IO) { native("nativeStop") })
            nativeInit(directory, RadioConfigReadOnly(), probe)
            check(NativeLib.start(config)); withTimeout(10_000) { ready.receive() }
            check(!NativeLib.radioConfigure(1, selected)) // ConfigCallback без метода ACK тоже не принимает запрос.
            drained()
            check(withContext(Dispatchers.IO) { native("nativeStop") })
            nativeInit(directory, probe, probe)
            check(!NativeLib.radioConfigure(2, selected))
            check(NativeLib.start(config)); withTimeout(10_000) { ready.receive() }
            for (settings in listOf(selected, selected.copy(mode = RadioAutoSettings.VAD), selected.copy(mode = RadioAutoSettings.VAD_LEVEL),
                selected.copy(enabled = false, levelOn = 0, levelOff = -90, hangoverMs = 2000, vadThreshold = 95, vadGain = 60)))
                apply(settings.nativeValues(), true, settings)
            check(!rawConfigure(3, null) && !rawConfigure(4, IntArray(6)) && !rawConfigure(5, IntArray(8)))
            val limits = listOf(-1 to 2, 0 to 4, -91 to 1, -91 to 1, 49 to 2001, 9 to 96, 0 to 61)
            for (field in limits.indices) for (value in listOf(limits[field].first, limits[field].second)) {
                val invalid = selected.nativeValues(); invalid[field] = value
                apply(invalid, false)
            }
            apply(selected.copy(levelOff = selected.levelOn).nativeValues(), false)
            apply(selected.copy(levelOff = selected.levelOn + 1).nativeValues(), false)
            drained()
            check(native("nativeRestart", config)); withTimeout(10_000) { ready.receive() }
            apply(selected.nativeValues(), true, selected)
            check(withContext(Dispatchers.IO) { native("nativeStop") })
            check(!NativeLib.radioConfigure(6, selected))
            nativeInit(directory, probe, probe)
            check(NativeLib.start(config)); withTimeout(10_000) { ready.receive() }
            nextRequest = Long.MIN_VALUE
            apply(selected.nativeValues(), true, selected)
            drained()
            check(probe.acks.get() == 22)
        } catch (error: Throwable) {
            error.printStackTrace(System.err)
            throw error
        } finally { check(withContext(Dispatchers.IO) { native("nativeDestroy") }) }
        println("PASS: Android radio config copy, complete snapshot, atomic validation ACK, callback admission and restart correlation")
        exitProcess(0)
    }
    Looper.loop()
}

/* app_process + APK: настоящие PhoneStateMonitor.publish/NativeLib/JNI без Activity и профиля приложения.
 * Новый /data/local/tmp/utun-phone-audit-<id>/profile; сеть/обнаружение отключены, состояние SIM задаёт тест.
 * Приватный publisher вызывается отдельно от Android Telephony; frontend media-владельцы не создаются. */
import android.os.Handler
import android.os.Looper
import com.utun.chat.data.ChatEventCallback
import com.utun.chat.data.LogManager
import com.utun.chat.data.NativeLib
import com.utun.chat.data.PhoneStateMonitor
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

class PhoneStateProbeLog {
    val commands = AtomicInteger()
    val applied = Channel<Pair<Int, Int>>(Channel.UNLIMITED)
    fun onNativeLog(level: Int, category: String, message: String) {
        val prefix = "Android phone state applied state="
        if (level != 1 || category != "call" || !message.contains(prefix)) return
        val state = message.substringAfter(prefix).substringBefore(' ').toInt()
        val policy = message.substringAfter("policy=").trim().toInt()
        commands.incrementAndGet()
        check(applied.trySend(state to policy).isSuccess)
    }
}

private fun nativeInit(directory: File, logger: PhoneStateProbeLog) {
    val method = NativeLib::class.java.getDeclaredMethod("nativeInit", String::class.java, Int::class.javaPrimitiveType,
        Any::class.java, Any::class.java)
    method.isAccessible = true
    check(method.invoke(NativeLib, directory.path, 0, null, logger) == true)
}

private fun native(name: String, config: String? = null): Boolean {
    val method = if (config == null) NativeLib::class.java.getDeclaredMethod(name)
        else NativeLib::class.java.getDeclaredMethod(name, String::class.java)
    method.isAccessible = true
    return (if (config == null) method.invoke(NativeLib) else method.invoke(NativeLib, config)) == true
}

private fun publish(state: Int, policy: Int) {
    val field = PhoneStateMonitor::class.java.getDeclaredField("policy")
    field.isAccessible = true; field.setInt(null, policy)
    val method = PhoneStateMonitor::class.java.getDeclaredMethod("publish", Int::class.javaPrimitiveType)
    method.isAccessible = true; method.invoke(PhoneStateMonitor, state)
}

fun main(args: Array<String>) {
    check(args.size == 1)
    val directory = File(args.single()).canonicalFile
    check(directory.path.startsWith("/data/local/tmp/utun-phone-audit-") && !directory.exists())
    check(directory.mkdirs())
    if (Looper.getMainLooper() == null) Looper.prepareMainLooper()
    val handler = Handler(Looper.getMainLooper())
    val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    val ready = Channel<Unit>(Channel.UNLIMITED)
    val logger = PhoneStateProbeLog()
    NativeLib.setEventCallback(object : ChatEventCallback {
        override fun onEvent(type: Int, data: ByteArray?) { if (type == 12) handler.post { check(ready.trySend(Unit).isSuccess) } }
    })
    suspend fun applied(state: Int, policy: Int) = withTimeout(5_000) { check(logger.applied.receive() == state to policy) }
    suspend fun drained() = withTimeout(5_000) {
        NativeLib.ping()
        while (!NativeLib.isResponsive()) delay(1)
        check(logger.applied.tryReceive().isFailure) { "unexpected phone state command" }
    }
    scope.launch {
        try {
            val config = "[global]\ndb_path=${directory.path}\nauto_sockets=no\ndebug_level=error\n" +
                "[ntp]\nenabled=no\n[stun]\nenabled=no\n[chatserver]\nstorage_autoload=0\n" +
                "local_invites_enabled=0\nlocal_broadcasts_enabled=0\n[debug]\ncall=debug\n"
            check(!NativeLib.setPhoneState(2, 1))
            publish(2, 1) // Отложенный publish не должен подавлять тот же state после готовности ядра.
            nativeInit(directory, logger)
            check(NativeLib.start(config))
            withTimeout(10_000) { ready.receive() }
            publish(2, 1); applied(2, 1)
            publish(2, 32) // Инъекция отказа без OOM: неверная маска отклоняется native-владельцем.
            check(LogManager.getRecent().any { it.category == "PhoneState" && it.level == "WARN" && it.message.contains("not queued") })
            drained()
            publish(2, 1); applied(2, 1)
            for (state in 0..2) for (policy in listOf(0, 1, 2, 4, 8, 16, 31)) {
                check(NativeLib.setPhoneState(state, policy)); applied(state, policy)
            }
            check(!NativeLib.setPhoneState(-1, 0) && !NativeLib.setPhoneState(3, 0))
            check(!NativeLib.setPhoneState(0, -1) && !NativeLib.setPhoneState(0, 32))
            drained()
            check(native("nativeRestart", config))
            withTimeout(10_000) { ready.receive() }
            publish(2, 1); applied(2, 1)
            check(withContext(Dispatchers.IO) { native("nativeStop") })
            check(!NativeLib.isReady() && !NativeLib.setPhoneState(2, 1))
            publish(2, 1)
            nativeInit(directory, logger)
            check(NativeLib.start(config))
            withTimeout(10_000) { ready.receive() }
            publish(2, 1); applied(2, 1)
            drained()
            check(logger.commands.get() == 25)
        } catch (error: Throwable) {
            error.printStackTrace(System.err)
            throw error
        } finally { check(withContext(Dispatchers.IO) { native("nativeDestroy") }) }
        println("PASS: Android phone state JNI acceptance, state/policy values, publisher rejection/retry and restart resync")
        exitProcess(0)
    }
    Looper.loop()
}

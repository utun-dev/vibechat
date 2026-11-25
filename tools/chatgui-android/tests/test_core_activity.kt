/* app_process + APK: настоящие CoreActivity/JNI/standby на Android main, без Activity и профиля приложения.
 * Аргумент — новый /data/local/tmp/utun-activity-audit-<id>/profile; внешние подключения/обнаружение отключены.
 * Native init/stop вызываются напрямую: frontend-владельцы камеры/аудио в этом процессе не создаются. */
import android.os.Handler
import android.os.Looper
import com.utun.chat.data.ChatEventCallback
import com.utun.chat.data.CoreActivity
import com.utun.chat.data.NativeLib
import java.io.File
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeout
import kotlin.system.exitProcess

class ActivityProbeLog {
    val commands = AtomicInteger()
    val applied = Channel<Boolean>(Channel.UNLIMITED)
    fun onNativeLog(level: Int, category: String, message: String) {
        val prefix = "Android activity applied requested="
        if (level != 1 || category != "sys" || !message.contains(prefix)) return
        val value = message.substringAfter(prefix).take(1).toInt()
        check(value in 0..1)
        val effective = message.substringAfter("effective=").trim().toInt()
        check(effective == value) { "activity mode mismatch requested=$value effective=$effective" }
        commands.incrementAndGet()
        check(applied.trySend(value != 0).isSuccess)
    }
}

private fun nativeInit(directory: File, logger: ActivityProbeLog) {
    val method = NativeLib::class.java.getDeclaredMethod("nativeInit", String::class.java, Int::class.javaPrimitiveType,
        Any::class.java, Any::class.java)
    method.isAccessible = true
    check(method.invoke(NativeLib, directory.path, 0, null, logger) == true)
}

private fun nativeStop(name: String = "nativeStop"): Boolean {
    val method = NativeLib::class.java.getDeclaredMethod(name)
    method.isAccessible = true
    return method.invoke(NativeLib) == true
}

fun main(args: Array<String>) {
    check(args.size == 1)
    val directory = File(args.single()).canonicalFile
    check(directory.path.startsWith("/data/local/tmp/utun-activity-audit-") && !directory.exists())
    check(directory.mkdirs())
    if (Looper.getMainLooper() == null) Looper.prepareMainLooper()
    val handler = Handler(Looper.getMainLooper())
    val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    val ready = Channel<Unit>(Channel.UNLIMITED)
    val logger = ActivityProbeLog()
    NativeLib.setEventCallback(object : ChatEventCallback {
        override fun onEvent(type: Int, data: ByteArray?) {
            if (type == 12) handler.post {
                CoreActivity.onCoreStarted()
                check(ready.trySend(Unit).isSuccess)
            }
        }
    })
    suspend fun applied(expected: Boolean) = withTimeout(5_000) { check(logger.applied.receive() == expected) }
    suspend fun drained() = withTimeout(5_000) {
        NativeLib.ping()
        while (!NativeLib.isResponsive()) delay(1)
        check(logger.applied.tryReceive().isFailure) { "duplicate activity command" }
    }
    scope.launch {
        try {
            val config = "[global]\ndb_path=${directory.path}\nauto_sockets=no\n" +
                "[ntp]\nenabled=no\n[stun]\nenabled=no\n[chatserver]\nstorage_autoload=0\n" +
                "local_invites_enabled=0\nlocal_broadcasts_enabled=0\n[debug]\nsys=debug\n"
            nativeInit(directory, logger)
            check(NativeLib.start(config))
            withTimeout(10_000) { ready.receive() }
            applied(false)
            CoreActivity.visible(true); applied(true)
            CoreActivity.transfer(true)
            CoreActivity.call(true)
            CoreActivity.visible(false)
            CoreActivity.call(false)
            CoreActivity.transfer(false); applied(false)
            repeat(3) { CoreActivity.sync() }
            drained()
            check(logger.commands.get() == 3)

            CoreActivity.transfer(true); applied(true)
            check(withContext(Dispatchers.IO) { nativeStop() })
            check(!NativeLib.isReady() && !NativeLib.setActive(true))
            CoreActivity.sync()
            nativeInit(directory, logger)
            check(NativeLib.start(config))
            withTimeout(10_000) { ready.receive() }
            applied(true)
            CoreActivity.transfer(false); applied(false)

            val rejected = AtomicReference<Throwable?>()
            val background = Thread({
                try { CoreActivity.call(true) } catch (error: Throwable) { rejected.set(error) }
            }, "activity-invalid-caller")
            background.start(); background.join(2_000)
            check(!background.isAlive && rejected.get() is IllegalStateException)
            CoreActivity.sync()
            drained()
            check(logger.commands.get() == 6)
            println("PASS: Android activity owners, accepted JNI queue, native ACTIVE/STANDBY, restart resync and thread guard")
        } catch (error: Throwable) {
            error.printStackTrace(System.err)
            throw error
        } finally { check(withContext(Dispatchers.IO) { nativeStop("nativeDestroy") }) }
        exitProcess(0)
    }
    Looper.loop()
}

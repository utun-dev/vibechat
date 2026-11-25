/* app_process + APK: настоящий JNI/worker, удержанный в диагностическом callback до/после create instance.
 * Аргумент — новый /data/local/tmp/utun-shutdown-audit-<id>/profile; компилировать как friend module.
 * CoreShutdown работает на Android main; JNI stop ждёт на IO. Frontend media/Activity здесь не создаются. */
import android.os.Handler
import android.os.Looper
import com.utun.chat.data.ChatEventCallback
import com.utun.chat.data.NativeLib
import com.utun.chat.headless.CoreShutdown
import java.io.File
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.async
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeout
import kotlin.system.exitProcess

class ShutdownProbeLog(private val gate: String?) {
    val entered = CompletableDeferred<Unit>()
    val stopping = CompletableDeferred<Unit>()
    val release = CountDownLatch(1)
    private val armed = AtomicBoolean(gate != null)

    fun onNativeLog(level: Int, category: String, message: String) {
        if (message.contains("Android worker stop requested")) stopping.complete(Unit)
        if (gate != null && message.contains(gate) && armed.compareAndSet(true, false)) {
            println("worker held at $gate")
            entered.complete(Unit)
            check(release.await(10, TimeUnit.SECONDS)) { "test did not release worker at $gate" }
        }
    }
}

private fun nativeStop(name: String = "nativeStop"): Boolean {
    val method = NativeLib::class.java.getDeclaredMethod(name)
    method.isAccessible = true
    return method.invoke(NativeLib) == true
}

private fun nativeInit(directory: File, logger: ShutdownProbeLog) {
    val database = File(directory, "chats.db")
    val existed = database.exists()
    val method = NativeLib::class.java.getDeclaredMethod("nativeInit", String::class.java, Int::class.javaPrimitiveType,
        Any::class.java, Any::class.java)
    method.isAccessible = true
    check(method.invoke(NativeLib, directory.path, 0, null, logger) == true)
    check(database.exists() == existed) { "callback initialization created a database before worker startup" }
}

fun main(args: Array<String>) {
    check(args.size == 1)
    val directory = File(args.single()).canonicalFile
    check(directory.path.startsWith("/data/local/tmp/utun-shutdown-audit-") && !directory.exists())
    check(directory.mkdirs())
    if (Looper.getMainLooper() == null) Looper.prepareMainLooper()
    val handler = Handler(Looper.getMainLooper())
    val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    val started = AtomicInteger()
    val readiness = Channel<Unit>(Channel.UNLIMITED)
    val gates = mutableListOf<ShutdownProbeLog>()
    NativeLib.setEventCallback(object : ChatEventCallback {
        override fun onEvent(type: Int, data: ByteArray?) {
            if (type == 12) { started.incrementAndGet(); check(readiness.trySend(Unit).isSuccess) }
        }
    })
    scope.launch {
        try {
            val config = "[global]\ndb_path=${directory.path}\nauto_sockets=no\n" +
                "[ntp]\nenabled=no\n[stun]\nenabled=no\n[chatserver]\nstorage_autoload=0\n" +
                "local_invites_enabled=0\nlocal_broadcasts_enabled=0\n"
            for (phase in listOf("Android identity saved", "core started")) {
                val logger = ShutdownProbeLog(phase).also { gates.add(it) }
                nativeInit(directory, logger)
                val previousStarts = started.get()
                check(NativeLib.start(config))
                withTimeout(10_000) { logger.entered.await() }
                check(!NativeLib.isReady()) { "worker unexpectedly ready while held at $phase" }
                val shutdown = CoreShutdown(scope, stop = {
                    check(withContext(Dispatchers.IO) { nativeStop() }) { "native stop rejected" }
                }, log = { level, message -> println("$level $message") })
                val cleanup = shutdown.request()
                val retired = async(start = CoroutineStart.UNDISPATCHED) { shutdown.await() }
                retired.cancel()
                var replacementInitialized = false
                val replacement = async(start = CoroutineStart.UNDISPATCHED) {
                    shutdown.await()
                    check(started.get() == previousStarts && !NativeLib.isReady()) { "cancelled worker announced ready" }
                    nativeInit(directory, ShutdownProbeLog(null))
                    replacementInitialized = true
                    check(NativeLib.start(config))
                }
                withTimeout(10_000) { logger.stopping.await() }
                check(!cleanup.isCompleted && !replacementInitialized)
                val heartbeat = CompletableDeferred<Unit>()
                handler.post { heartbeat.complete(Unit) }
                withTimeout(1_000) { heartbeat.await() }
                check(!cleanup.isCompleted) { "stop returned while worker was held at $phase" }
                logger.release.countDown()
                withTimeout(10_000) { cleanup.await(); replacement.await(); readiness.receive() }
                check(replacementInitialized && NativeLib.isReady() && started.get() == previousStarts + 1)
                withTimeout(10_000) { shutdown.request().await() }
                check(!NativeLib.isReady())
                println("PASS: cancellation at $phase, responsive main, retained JNI callbacks, replacement starts after join")
            }
        } catch (error: Throwable) {
            error.printStackTrace(System.err)
            throw error
        } finally {
            gates.forEach { it.release.countDown() }
            check(withContext(Dispatchers.IO) { nativeStop("nativeDestroy") })
        }
        exitProcess(0)
    }
    Looper.loop()
}

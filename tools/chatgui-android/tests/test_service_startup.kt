/* app_process + APK: CoreStartup на настоящем Android main looper и JNI worker, без Activity/профиля приложения.
 * Аргумент — новый /data/local/tmp/utun-startup-audit-<id>/profile. Внешние подключения отключены.
 * Компилировать как friend module приложения; nativeStop вызывается напрямую, frontend audio/video здесь не создаются. */
import android.os.Handler
import android.os.Looper
import com.utun.chat.data.ChatEventCallback
import com.utun.chat.data.NativeLib
import com.utun.chat.headless.CoreStartup
import java.io.File
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.async
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeout
import kotlin.system.exitProcess

private fun nativeCommand(name: String, config: String? = null): Any? {
    val method = if (config == null) NativeLib::class.java.getDeclaredMethod(name)
        else NativeLib::class.java.getDeclaredMethod(name, String::class.java)
    method.isAccessible = true
    return if (config == null) method.invoke(NativeLib) else method.invoke(NativeLib, config)
}

fun main(args: Array<String>) {
    check(args.size == 1)
    val directory = File(args.single()).canonicalFile
    check(directory.path.startsWith("/data/local/tmp/utun-startup-audit-") && !directory.exists())
    check(directory.mkdirs())
    if (Looper.getMainLooper() == null) Looper.prepareMainLooper()
    val handler = Handler(Looper.getMainLooper())
    val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    val confirmations = Channel<Unit>(Channel.UNLIMITED)
    val failure = CompletableDeferred<Unit>()
    var readyCalls = 0
    var failures = 0
    val startup = CoreStartup(scope, NativeLib::isReady, onReady = {
        readyCalls++
        check(confirmations.trySend(Unit).isSuccess)
    }, onFailure = { failures++; failure.complete(Unit) }, log = { level, message -> println("$level $message") })
    var activeStartup = startup
    NativeLib.setEventCallback(object : ChatEventCallback {
        override fun onEvent(type: Int, data: ByteArray?) {
            if (type == 12) handler.post { activeStartup.coreStarted() }
        }
    })
    scope.launch {
        try {
            val config = "[global]\ndb_path=${directory.path}\nauto_sockets=no\n" +
                "[ntp]\nenabled=no\n[stun]\nenabled=no\n[chatserver]\nstorage_autoload=0\n"
            val prepared = CompletableDeferred<String>()
            val submitted = CompletableDeferred<Unit>()
            var submissions = 0
            startup.start({ prepared.await() }) {
                submissions++
                val accepted = NativeLib.start(it)
                submitted.complete(Unit)
                accepted
            }
            val settings = async(start = CoroutineStart.UNDISPATCHED) { startup.awaitReady() }
            check(submissions == 0 && !settings.isCompleted)
            prepared.complete(config)
            withTimeout(10_000) { submitted.await() }
            // nativeStart принят, но main ещё не доставил SERVICE_STARTED.
            check(submissions == 1 && !settings.isCompleted)
            withTimeout(10_000) { settings.await(); confirmations.receive() }
            check(readyCalls == 1 && failures == 0 && NativeLib.isReady())
            check(nativeCommand("nativeRestart", config) == true)
            withTimeout(10_000) { confirmations.receive() }
            check(readyCalls == 2 && failures == 0 && NativeLib.isReady())
            startup.stop()
            check(runCatching { startup.awaitReady() }.isFailure)
            nativeCommand("nativeStop")
            check(!NativeLib.isReady())

            val timed = CoreStartup(scope, NativeLib::isReady, onReady = { error("invalid config started") },
                onFailure = { failures++; failure.complete(Unit) }, log = { level, message -> println("$level $message") },
                timeoutMillis = 500)
            activeStartup = timed
            timed.start({ "[global]\nmy_private_key=invalid\n" }, NativeLib::start)
            val error = withTimeout(5_000) { runCatching { timed.awaitReady() }.exceptionOrNull() }
            withTimeout(5_000) { failure.await() }
            check(error is IllegalStateException && error.message!!.contains("readiness timeout"))
            check(failures == 1 && !NativeLib.isReady())
            timed.stop()
            println("PASS: Android main-thread readiness, delayed config, native restart, owner stop and asynchronous startup failure")
        } catch (error: Throwable) {
            error.printStackTrace(System.err)
            throw error
        } finally { nativeCommand("nativeStop") }
        exitProcess(0)
    }
    Looper.loop()
}

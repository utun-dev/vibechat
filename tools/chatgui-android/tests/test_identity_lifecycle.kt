/* app_process + APK: JNI и настоящий worker без Activity, профиля приложения и внешних соединений.
 * Аргумент — новый каталог внутри /data/local/tmp; libutun.so/зависимости должны быть в java.library.path.
 * JNI restart вызывается напрямую: frontend-владельцы камеры/аудио в этом процессе не создаются. */
import com.utun.chat.data.ChatEventCallback
import com.utun.chat.data.NativeLib
import java.io.File
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
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
    check(directory.path.startsWith("/data/local/tmp/utun-identity-audit-") && !directory.exists())
    check(directory.mkdirs())
    val events = LinkedBlockingQueue<Int>()
    NativeLib.setEventCallback(object : ChatEventCallback {
        override fun onEvent(type: Int, data: ByteArray?) {
            if (type == 12 || type == 13) events.offer(type)
        }
    })
    fun awaitStarted(expectStop: Boolean) {
        if (expectStop) check(events.poll(10, TimeUnit.SECONDS) == 13) { "old core did not stop" }
        check(events.poll(10, TimeUnit.SECONDS) == 12 && NativeLib.isReady()) { "new core did not start" }
    }
    fun identity(): Triple<String, String, String> {
        val lines = File(directory, "keys.conf").readLines()
        check(lines.size == 2 && lines[0].startsWith("public_key=") && lines[1].startsWith("private_key="))
        val keys = lines[0].removePrefix("public_key=") to lines[1].removePrefix("private_key=")
        check(keys.first.length == 64 && keys.second.length == 64)
        val address = NativeLib.getMySigningAddress()
        check(address.length == 64) { "core identity unavailable" }
        return Triple(keys.first, keys.second, address)
    }
    val config = "[global]\ndb_path=${directory.path}\nauto_sockets=no\n[ntp]\nenabled=no\n" +
        "[stun]\nenabled=no\n[chatserver]\nstorage_autoload=0\n"
    try {
        check(NativeLib.start(config))
        awaitStarted(expectStop = false)
        val original = identity()
        val staleConfig = "[global]\nmy_public_key=${original.first}\nmy_private_key=${original.second}\n$config"
        check(nativeCommand("nativeRegenerateKeys", staleConfig) == true)
        awaitStarted(expectStop = true)
        val generated = identity()
        check(generated.first != original.first && generated.second != original.second && generated.third != original.third) { "key regeneration retained old pair" }
        check(nativeCommand("nativeRestart", staleConfig) == true)
        awaitStarted(expectStop = true)
        check(identity() == generated) { "stale config reverted persisted identity" }
    } finally {
        nativeCommand("nativeStop")
    }
    check(!NativeLib.isReady())
    println("PASS: Android JNI start, full identity regeneration, persisted keys, stale-config restart and stop")
    exitProcess(0)
}

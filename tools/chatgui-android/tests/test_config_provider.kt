import android.content.Context
import android.content.ContextWrapper
import com.utun.chat.data.AutoSocketProtocols
import com.utun.chat.data.ConfigProvider
import com.utun.chat.data.ServerEntry
import com.utun.chat.data.NoiseSuppressionSettings
import com.utun.chat.data.StreamFecParameters
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Deferred
import kotlinx.coroutines.launch
import kotlinx.coroutines.cancelAndJoin
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import java.io.File
import kotlin.system.exitProcess

/* Запуск через app_process с собранным APK в CLASSPATH: настоящие ConfigProvider/DataStore,
 * отдельный каталог без доступа к профилю приложения. Полученные .cfg проверяет test_node_config --validate. */
fun main(args: Array<String>): Unit = runBlocking {
    val directory = File(args.single())
    check(directory.mkdirs()) { "Use a new output directory: $directory" }
    val context = object : ContextWrapper(null) {
        override fun getApplicationContext(): Context = this
        override fun getFilesDir(): File = directory
    }
    val provider = ConfigProvider(context)
    provider.setValue("node.name", "Android config probe")
    provider.setValue("node.private_key", "704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f")
    provider.setValue("node.public_key", "7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f")
    provider.saveServersToConfig(emptyList()).await()
    val defaults = File(provider.configFilePath()).readText()
    check(provider.getHideMySockets() && "hide_my_sockets=yes\n" in defaults)
    provider.setHideMySockets(false)
    check(!provider.getHideMySockets() && "hide_my_sockets=no\n" in provider.buildConfigText().await())
    provider.setHideMySockets(true)
    check(provider.getHideMySockets() && "hide_my_sockets=yes\n" in provider.buildConfigText().await())
    provider.setHideMySockets(false)
    check(provider.getNoiseSuppressionSettings() == NoiseSuppressionSettings())
    check("noise_suppression_enabled=1\n" in defaults && "noise_suppression_db=-20\n" in defaults)
    provider.saveNoiseSuppressionSettings(NoiseSuppressionSettings(false, -35))
    check(provider.getNoiseSuppressionSettings() == NoiseSuppressionSettings(false, -35))
    check(provider.getInt("chat.noise_suppression_enabled") == 0 && provider.getInt("chat.noise_suppression_db") == -35)
    val noiseOff = provider.buildConfigText().await()
    check("noise_suppression_enabled=0\n" in noiseOff && "noise_suppression_db=-35\n" in noiseOff)
    provider.saveNoiseSuppressionSettings(NoiseSuppressionSettings())
    check("storage_unit_size=100M\n" in defaults && "storage_total_size=1G\n" in defaults)
    File(directory, "default.cfg").writeText(defaults)
    provider.setValue("chat.storage_autoload_maxsize_mb", "23")
    provider.setValue("chat.storage_maxsize_gb", "7")
    provider.setValue("debug.categories", "config=debug,chat_sync=info,stream=debug")
    provider.setConnectionMode("manual")
    for (protocols in AutoSocketProtocols.values()) {
        provider.setAutoSocketProtocols(protocols)
        val servers = protocols.createServers("probe", "127.0.0.1", emptyList()) +
            protocols.createServers("probe", "::1", emptyList())
        val inactive = listOf(ServerEntry("inactive", "offline", "192.0.2.1", "tcp", 4567))
        for (utun in 0..1) {
            provider.setValue("gui.utun_enabled", "$utun")
            provider.saveServersToConfig(servers, inactive).await()
            val saved = File(provider.configFilePath()).readText()
            check("#[server:offline_v4_tcp]" in saved)
            val text = provider.buildConfigText().await()
            check("storage_unit_size=23M\n" in text && "storage_total_size=7G\n" in text)
            check("storage_autoload_maxsize_mb=" !in text && "storage_maxsize_gb=" !in text)
            check("auto_socket_protocols=${protocols.configValue}\n" in text && "utun_enabled=$utun\n" in text)
            check("radio_vad_enabled=" !in text && "radio_auto_enabled=" in text)
            check(!text.contains("[server:offline_v4_tcp]"))
            val name = "${protocols.configValue.replace('+', '-')}-utun$utun"
            File(directory, "$name-saved.cfg").writeText(saved)
            File(directory, "$name-start.cfg").writeText(text)
        }
    }
    fun values(text: String): Map<String, String> = text.lineSequence().map { it.trim() }
        .filter { !it.startsWith("#") && it.contains('=') }.associate { it.substringBefore('=') to it.substringAfter('=') }
    val copied = mutableListOf(ServerEntry("copy", "probe", "127.0.0.1", "tcp", 3344))
    val save = provider.saveServersToConfig(copied)
    copied.clear()
    save.await()
    check(provider.loadServersFromConfig().await().single().port == 3344)

    lateinit var intent: Deferred<Unit>
    val screen = launch(start = CoroutineStart.UNDISPATCHED) {
        intent = provider.saveServersToConfig(listOf(ServerEntry("intent", "probe", "127.0.0.1", "tcp", 4455)))
        intent.await()
    }
    screen.cancelAndJoin()
    intent.await()
    check(provider.loadServersFromConfig().await().single().port == 4455)

    provider.setValue("gui.utun_enabled", "0")
    val budgets = StreamFecParameters.budgets.associate { it.key to it.defaultValue } + ("stream_regular_limit" to 256)
    provider.saveStreamSettings(255, 0, 500, 10000, budgets)
    val budgetText = provider.buildConfigText().await()
    budgets.forEach { (key, value) -> check("$key=$value\n" in budgetText && provider.getInt("chat.$key") == value) }
    check(runCatching { provider.saveStreamSettings(255, 0, 500, 10000, budgets + ("stream_regular_limit" to 192)) }.isFailure)
    check(provider.getInt("chat.stream_regular_limit") == 256 && provider.getInt("chat.stream_subblocks") == 255)
    provider.saveStreamSettings(8, 0, 500, 10000)
    val stop = AtomicBoolean()
    val mutations = AtomicInteger()
    val editor = launch(Dispatchers.IO) {
        while (!stop.get() && mutations.get() < 512) {
            val index = mutations.getAndIncrement()
            if (index % 2 == 0) provider.saveStreamSettings(8, 0, 500, 10000) else provider.saveStreamSettings(16, 2, 750, 12000)
        }
    }
    try {
        repeat(24) { index ->
            val text = provider.buildConfigText().await()
            val runtime = values(text)
            val stream = listOf(runtime["stream_subblocks"], runtime["stream_rs_redundant"],
                runtime["stream_stall_timeout_ms"], runtime["stream_max_live_lag_ms"])
            check(stream in listOf(listOf("8", "0", "500", "10000"), listOf("16", "2", "750", "12000"))) { "Mixed preferences snapshot: $stream" }
            val saved = values(File(provider.configFilePath()).readText())
            check(runtime == saved) { "Saved and runtime config differ at snapshot $index" }
            File(directory, "concurrent-$index.cfg").writeText(text)
        }
    } finally { stop.set(true); editor.cancelAndJoin() }
    check(mutations.get() > 0)

    val cfg = File(provider.configFilePath())
    val previous = File(directory, "previous.cfg")
    check(cfg.renameTo(previous))
    check(cfg.mkdir())
    val sentinel = File(cfg, "old")
    sentinel.writeText("preserve")
    try {
        check(runCatching { provider.buildConfigText().await() }.isFailure)
        check(runCatching { provider.saveServersToConfig(emptyList()).await() }.isFailure)
        check(sentinel.readText() == "preserve")
        check(directory.listFiles()!!.none { it.name.startsWith(".utun.cfg.") })
    } finally {
        check(sentinel.delete())
        check(cfg.delete())
        check(previous.renameTo(cfg))
    }
    provider.buildConfigText().await()
    println("PASS: real ConfigProvider/DataStore defaults, protocols, IPv4/IPv6, inactive sockets, UTUN, " +
        "input ownership, cancelled waiter, 24 coherent snapshots, ${mutations.get()} edits and IO failure recovery")
    exitProcess(0)
}

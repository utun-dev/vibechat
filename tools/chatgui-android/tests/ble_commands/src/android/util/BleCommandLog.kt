package android.util

/** JVM-приёмник Android Log: LogManager продолжает сохранять настоящую историю и уровни. */
class Log {
    companion object {
        @JvmStatic fun println(priority: Int, tag: String, message: String): Int {
            System.out.println("$priority $tag: $message"); return message.length
        }
    }
}

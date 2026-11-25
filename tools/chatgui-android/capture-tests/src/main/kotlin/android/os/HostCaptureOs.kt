package android.os

/** SDK/priority seam отдельного hardware-reader JVM процесса. */
object Build { object VERSION { var SDK_INT = 36 } }
object Process {
    const val THREAD_PRIORITY_AUDIO = -16
    var failPriority = false
    fun setThreadPriority(priority: Int) {
        check(priority == THREAD_PRIORITY_AUDIO)
        if (failPriority) throw SecurityException("audio priority denied")
    }
}

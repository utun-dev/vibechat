package androidx.core.content

import android.content.Context
import android.os.Handler
import android.os.Looper
import java.util.concurrent.Executor

object ContextCompat {
    fun getMainExecutor(context: Context) = Executor { check(Handler(Looper.getMainLooper()).post(it)) { "Main executor rejected callback" } }
}

package androidx.camera.lifecycle

import android.content.Context
import androidx.camera.core.CameraSelector
import androidx.camera.core.UseCase
import androidx.lifecycle.LifecycleOwner
import java.util.concurrent.Executor

class HostCameraFuture {
    var ready = false
    private var callback: Runnable? = null
    private var executor: Executor? = null
    fun get(): ProcessCameraProvider = ProcessCameraProvider.shared
    fun addListener(listener: Runnable, delivery: Executor) {
        callback = listener; executor = delivery
        if (ready) delivery.execute(listener)
    }
    fun complete() { ready = true; callback?.let { executor!!.execute(it) } }
}
class ProcessCameraProvider {
    companion object {
        var shared = ProcessCameraProvider()
        var future = HostCameraFuture()
        var failLookup = false
        fun getInstance(context: Context): HostCameraFuture { check(!failLookup); return future }
    }
    var failures = emptySet<String>()
    val bound = mutableSetOf<UseCase>()
    var binds = 0
    var unbinds = 0
    fun bindToLifecycle(owner: LifecycleOwner, selector: CameraSelector, vararg cases: UseCase) {
        binds++; bound.addAll(cases); check("bind" !in failures) { "Camera bind failed after effect" }
    }
    fun unbind(vararg cases: UseCase) {
        unbinds++; check("unbind" !in failures) { "Camera unbind failed" }; bound.removeAll(cases.toSet())
        check("unbind after" !in failures) { "Camera unbind failed after effect" }
    }
}

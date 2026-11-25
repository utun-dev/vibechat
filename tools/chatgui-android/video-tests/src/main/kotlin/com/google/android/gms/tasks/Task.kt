package com.google.android.gms.tasks

import java.util.concurrent.Executor

/** Управляемая асинхронная граница ML Kit; включает отказ регистрации listener до/после побочного эффекта. */
class Task<T> {
    private val listeners = mutableListOf<Pair<Executor, (Task<T>) -> Unit>>()
    var failures = emptySet<String>()
    var isComplete = false
        private set
    var isSuccessful = false
        private set
    var isCanceled = false
        private set
    var exception: Exception? = null
        private set
    private var value: T? = null
    val result: T get() { check(isSuccessful); return checkNotNull(value) }
    fun addOnCompleteListener(executor: Executor, listener: (Task<T>) -> Unit): Task<T> {
        check("listener" !in failures) { "Task listener registration failed" }
        listeners.add(executor to listener)
        if (isComplete) executor.execute { listener(this) }
        check("listener after" !in failures) { "Task listener registration failed after effect" }
        return this
    }
    fun finish(result: T? = null, failure: Exception? = null, canceled: Boolean = false) {
        check(!isComplete)
        value = result; exception = failure; isCanceled = canceled; isSuccessful = failure == null && !canceled; isComplete = true
        listeners.toList().forEach { (executor, callback) -> executor.execute { callback(this) } }
    }
}

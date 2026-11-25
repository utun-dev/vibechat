package android.os

/** Host main принадлежит потоку JUnit; фоновые mutators проверяются отдельным Thread. */
class Looper private constructor() {
    companion object {
        private val main = Looper()
        private val owner = Thread.currentThread()
        fun getMainLooper() = main
        fun myLooper(): Looper? = if (Thread.currentThread() === owner) main else null
    }
}

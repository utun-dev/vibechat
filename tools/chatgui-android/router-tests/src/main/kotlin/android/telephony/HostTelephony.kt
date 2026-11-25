package android.telephony

import android.os.Handler
import android.os.Looper
import java.util.concurrent.Executor

/** Только host-тест: SIM и Binder-события управляются тестом; номера/реальные устройства не читаются. */
open class TelephonyCallback { interface CallStateListener { fun onCallStateChanged(state: Int) } }
open class PhoneStateListener {
    companion object { const val LISTEN_NONE = 0; const val LISTEN_CALL_STATE = 32 }
    open fun onCallStateChanged(state: Int, phoneNumber: String?) {}
}

class TelephonyManager(val subscriptionId: Int = -1) {
    companion object { const val CALL_STATE_IDLE = 0; const val CALL_STATE_RINGING = 1; const val CALL_STATE_OFFHOOK = 2 }
    data class Registration(val callback: Any, val executor: Executor)
    val subscriptions = mutableMapOf<Int, TelephonyManager>()
    val history = mutableListOf<Registration>()
    val active = mutableSetOf<Any>()
    var initialState = CALL_STATE_IDLE
    var automaticInitial = true
    var failRegister = false
    var failRemove = false
    var failQuery = false
    var registerAttempts = 0
    var removeAttempts = 0
    var queries = 0
    fun createForSubscriptionId(id: Int): TelephonyManager = subscriptions.getOrPut(id) { TelephonyManager(id) }
    val callStateForSubscription: Int get() = snapshot()
    val callState: Int get() = snapshot()
    private fun snapshot(): Int { queries++; check(!failQuery) { "phone snapshot unavailable" }; return initialState }
    private fun add(callback: Any, executor: Executor) {
        registerAttempts++
        history.add(Registration(callback, executor))
        check(!failRegister) { "phone registration unavailable" }
        active.add(callback)
        if (automaticInitial) deliver(history.lastIndex, initialState)
    }
    private fun remove(callback: Any) {
        removeAttempts++
        check(!failRemove) { "phone observer removal unavailable" }
        active.remove(callback)
    }
    fun registerTelephonyCallback(executor: Executor, callback: TelephonyCallback) = add(callback, executor)
    fun unregisterTelephonyCallback(callback: TelephonyCallback) = remove(callback)
    fun listen(callback: PhoneStateListener, events: Int) {
        if (events == PhoneStateListener.LISTEN_NONE) remove(callback)
        else { check(events == PhoneStateListener.LISTEN_CALL_STATE); add(callback, Executor { Handler(Looper.getMainLooper()).post(it) }) }
    }

    /** Уже переданное ОС событие можно доставить и после удаления слушателя. */
    fun deliver(index: Int, state: Int) {
        val registration = history[index]
        registration.executor.execute {
            when (val callback = registration.callback) {
                is TelephonyCallback.CallStateListener -> callback.onCallStateChanged(state)
                is PhoneStateListener -> callback.onCallStateChanged(state, null)
                else -> error("Unsupported phone callback")
            }
        }
    }
}

data class SubscriptionInfo(val subscriptionId: Int)
class SubscriptionManager {
    open class OnSubscriptionsChangedListener { open fun onSubscriptionsChanged() {} }
    data class Registration(val listener: OnSubscriptionsChangedListener, val executor: Executor)
    val history = mutableListOf<Registration>()
    val active = mutableSetOf<OnSubscriptionsChangedListener>()
    var ids = listOf(10)
    var failAdd = false
    var failRemove = false
    var failQuery = false
    var addAttempts = 0
    var removeAttempts = 0
    val activeSubscriptionInfoList: List<SubscriptionInfo> get() {
        check(!failQuery) { "SIM list unavailable" }
        return ids.map(::SubscriptionInfo)
    }
    fun addOnSubscriptionsChangedListener(listener: OnSubscriptionsChangedListener) =
        addOnSubscriptionsChangedListener(Executor { Handler(Looper.getMainLooper()).post(it) }, listener)
    fun addOnSubscriptionsChangedListener(executor: Executor, listener: OnSubscriptionsChangedListener) {
        addAttempts++
        history.add(Registration(listener, executor))
        check(!failAdd) { "SIM listener registration unavailable" }
        active.add(listener)
        deliver(history.lastIndex)
    }
    fun removeOnSubscriptionsChangedListener(listener: OnSubscriptionsChangedListener) {
        removeAttempts++
        check(!failRemove) { "SIM listener removal unavailable" }
        active.remove(listener)
    }
    fun deliver(index: Int = history.lastIndex) {
        val registration = history[index]
        registration.executor.execute { registration.listener.onSubscriptionsChanged() }
    }
}

package android.net

data class Uri private constructor(private val value: String) {
    companion object { fun parse(value: String) = Uri(value) }
    override fun toString() = value
}

class Network
class LinkProperties

/** Только host-тест: listener принадлежит конкретному manager; старые callbacks тест может доставить после удаления. */
class ConnectivityManager {
    open class NetworkCallback {
        open fun onAvailable(network: Network) {}
        open fun onLost(network: Network) {}
        open fun onLinkPropertiesChanged(network: Network, linkProperties: LinkProperties) {}
    }
    fun interface OnNetworkActiveListener { fun onNetworkActive() }
    val listeners = mutableListOf<OnNetworkActiveListener>()
    val history = mutableListOf<OnNetworkActiveListener>()
    val callbacks = mutableListOf<NetworkCallback>()
    val callbackHistory = mutableListOf<NetworkCallback>()
    var addAttempts = 0
    var removeAttempts = 0
    var failAdd = false
    var failRemove = false
    var failQuery = false
    var active = false

    fun addDefaultNetworkActiveListener(listener: OnNetworkActiveListener) {
        addAttempts++
        if (failAdd) throw SecurityException("network listener registration denied")
        check(listener !in listeners)
        listeners.add(listener)
        history.add(listener)
    }

    fun removeDefaultNetworkActiveListener(listener: OnNetworkActiveListener) {
        removeAttempts++
        if (failRemove) throw IllegalStateException("network listener removal failed")
        check(listeners.remove(listener)) { "listener belongs to another manager or was already removed" }
    }

    val isDefaultNetworkActive: Boolean get() {
        if (failQuery) throw IllegalStateException("network activity query failed")
        return active
    }

    fun deliver(index: Int) { history[index].onNetworkActive() }

    fun registerDefaultNetworkCallback(callback: NetworkCallback) {
        if (failAdd) throw SecurityException("network callback registration denied")
        check(callback !in callbacks)
        callbacks.add(callback)
        callbackHistory.add(callback)
    }

    fun unregisterNetworkCallback(callback: NetworkCallback) {
        if (failRemove) throw IllegalStateException("network callback removal failed")
        check(callbacks.remove(callback)) { "callback belongs to another manager or was already removed" }
    }
}

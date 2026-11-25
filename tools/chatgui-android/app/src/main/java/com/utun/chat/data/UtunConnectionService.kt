package com.utun.chat.data

import android.telecom.Connection
import android.telecom.ConnectionRequest
import android.telecom.ConnectionService
import android.telecom.PhoneAccountHandle
import android.telecom.DisconnectCause

/**
 * Self-managed ConnectionService для P2P-звонков uTun.
 *
 * Telecom вызывает onCreateOutgoingConnection (placeCall) / onCreateIncomingConnection
 * (addNewIncomingCall); здесь создаётся UtunConnection, который регистрируется в
 * TelecomCallManager по callId из extras. Само аудио и сигналинг ведёт приложение.
 * Входящий адрес — Ed25519. Исходящий может использовать локальный адрес DM до выбора устройства. Плохие extras/отклонённая регистрация возвращают canceled Connection;
 * SDK-ошибка создания возвращает failed Connection и сообщает controller. Отклонённый живой объект Telecom не получает.
 */
class UtunConnectionService : ConnectionService() {

    override fun onCreateIncomingConnectionFailed(account: PhoneAccountHandle?, request: ConnectionRequest?) {
        val id = requestId(request)
        LogManager.addLog("WARN", "UtunConnSvc", "Telecom rejected incoming call id=$id")
        TelecomCallManager.notifyConnectionFailed(id)
    }

    override fun onCreateOutgoingConnectionFailed(account: PhoneAccountHandle?, request: ConnectionRequest?) {
        val id = requestId(request)
        LogManager.addLog("WARN", "UtunConnSvc", "Telecom rejected outgoing call id=$id")
        TelecomCallManager.notifyConnectionFailed(id)
    }

    override fun onCreateIncomingConnection(
        connectionManagerPhoneAccount: PhoneAccountHandle?,
        request: ConnectionRequest?
    ): Connection? {
        val (callId, peer, name) = parseRequest(request, incoming = true) ?: run {
            LogManager.addLog("WARN", "UtunConnSvc", "onCreateIncomingConnection: bad request extras")
            TelecomCallManager.notifyConnectionFailed(requestId(request))
            return Connection.createCanceledConnection()
        }
        LogManager.addLog("INFO", "UtunConnSvc",
            "onCreateIncomingConnection id=0x%016x peer=%s".format(callId, peer))
        return createConnection(callId, peer, name)
    }

    override fun onCreateOutgoingConnection(
        connectionManagerPhoneAccount: PhoneAccountHandle?,
        request: ConnectionRequest?
    ): Connection? {
        val (callId, peer, name) = parseRequest(request, incoming = false) ?: run {
            LogManager.addLog("WARN", "UtunConnSvc", "onCreateOutgoingConnection: bad request extras")
            TelecomCallManager.notifyConnectionFailed(requestId(request))
            return Connection.createCanceledConnection()
        }
        LogManager.addLog("INFO", "UtunConnSvc",
            "onCreateOutgoingConnection id=0x%016x peer=%s".format(callId, peer))
        return createConnection(callId, peer, name)
    }

    private fun createConnection(callId: Long, peer: String, displayName: String): Connection {
        return try {
            val conn = UtunConnection(callId, peer, displayName)
            if (TelecomCallManager.registerConnection(callId, conn)) conn else Connection.createCanceledConnection()
        } catch (e: Exception) {
            LogManager.addLog("ERROR", "UtunConnSvc", "connection creation failed id=$callId peer=$peer: $e")
            TelecomCallManager.notifyConnectionFailed(callId)
            Connection.createFailedConnection(DisconnectCause(DisconnectCause.ERROR))
        }
    }

    private fun requestId(request: ConnectionRequest?): Long = try {
        request?.extras?.getLong(TelecomCallManager.EXTRA_CALL_ID, 0) ?: 0
    } catch (e: Exception) {
        LogManager.addLog("ERROR", "UtunConnSvc", "connection request ID decode failed: $e"); 0
    }

    private fun parseRequest(request: ConnectionRequest?, incoming: Boolean): Triple<Long, String, String>? = try {
        val extras = request?.extras
        if (extras == null) {
            LogManager.addLog("WARN", "UtunConnSvc", "connection request has no extras"); null
        } else {
            val callId = extras.getLong(TelecomCallManager.EXTRA_CALL_ID, 0L)
            val peer = extras.getString(TelecomCallManager.EXTRA_PEER_ADDRESS).orEmpty()
            val name = extras.getString(TelecomCallManager.EXTRA_DISPLAY_NAME) ?: ""
            val direct = !incoming && peer.startsWith("dm:") && peer.substring(3).toLongOrNull()?.let { it > 0 } == true
            if (callId == 0L || (!direct && !MemberFilesProtocol.validAddress(peer))) {
                LogManager.addLog("WARN", "UtunConnSvc", "connection request missing identity id=$callId peer=$peer; keys=${extras.keySet()}")
                null
            } else Triple(callId, peer, name)
        }
    } catch (e: Exception) {
        LogManager.addLog("ERROR", "UtunConnSvc", "connection request extras decode failed: $e"); null
    }
}

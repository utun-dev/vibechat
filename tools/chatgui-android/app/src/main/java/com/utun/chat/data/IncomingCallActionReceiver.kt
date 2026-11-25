package com.utun.chat.data

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent

/** На main отклонить текущий ещё звонящий P2P/радиовызов; доставленные после stop или замены действия игнорируются. */
class IncomingCallActionReceiver : BroadcastReceiver() {

    companion object {
        const val ACTION_DECLINE = "com.utun.chat.action.DECLINE_CALL"
    }

    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != ACTION_DECLINE) {
            LogManager.addLog("WARN", "InCall", "ignored notification action=${intent.action}")
            return
        }
        val callId = intent.getLongExtra(IncomingCallManager.EXTRA_CALL_ID, 0L)
        if (callId == 0L) {
            LogManager.addLog("ERROR", "InCall", "DECLINE action: bad call_id")
            return
        }
        val radio = intent.getBooleanExtra(IncomingCallManager.EXTRA_RADIO_CALL, false)
        if (!IncomingCallManager.isRinging(callId, radio)) {
            LogManager.addLog("WARN", "InCall", "stale DECLINE ignored callId=$callId radio=$radio")
            return
        }
        LogManager.addLog("INFO", "InCall", "DECLINE action callId=$callId radio=$radio")
        if (radio) {
            RadioCallController.end(callId)
            return
        }
        CallController.end(callId, 2)
    }
}

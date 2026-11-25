package com.utun.chat

import android.app.Activity
import android.app.Instrumentation
import android.content.Intent
import android.os.Bundle
import android.os.SystemClock
import com.utun.chat.data.LocalDiscovery
import com.utun.chat.data.BleRadio
import com.utun.chat.data.NativeLib
import com.utun.chat.data.LogManager
import com.utun.chat.headless.HeadlessService

/** Только ручной тест на физическом устройстве: временный raw/ETCP echo общего C API. */
class BlePeerProbe : Instrumentation() {
    private lateinit var args: Bundle
    override fun onCreate(arguments: Bundle) { super.onCreate(arguments); args = arguments; start() }
    override fun onStart() {
        val result = Bundle()
        var code = Activity.RESULT_OK
        var restoreInvites: String? = null
        try {
            startActivitySync(Intent(targetContext, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
            targetContext.startForegroundService(Intent(targetContext, HeadlessService::class.java))
            val end = SystemClock.elapsedRealtime() + 30_000
            while (!NativeLib.isReady()) { check(SystemClock.elapsedRealtime() < end) { "core startup timeout" }; Thread.sleep(100) }
            Thread.sleep(1000) // Дождаться обычной политики SERVICE_STARTED перед тестовой.
            check(BleRadio.permitted()) { "Bluetooth permissions missing" }
            val flags = (if (args.getString("etcp") == "true") 39 else 15) or
                (if (args.getString("initiator") == "true") 16 else 0)
            if (args.getString("invites") == "true") {
                restoreInvites = ChatApplication.instance.configProvider.getInt("chat.bluetooth_invites_enabled").toString()
                NativeLib.setChatSetting("bluetooth_invites_enabled", "1")
            }
            check(BleRadio.nativeConfigure(flags)) { "configure refused" }
            if (args.getString("pauseScan") == "true") {
                Thread.sleep(2000)
                check(BleRadio.pauseScanForProbe()) { "pause scan command refused" }
            }
            val seconds = (args.getString("seconds") ?: "100").toLong()
            require(seconds in 1..600) { "seconds must be in 1..600" }
            if (args.getString("invites") == "true") {
                val deadline = SystemClock.elapsedRealtime() + seconds * 1000
                var replied = false
                while (SystemClock.elapsedRealtime() < deadline) {
                    runOnMainSync {
                        val received = LocalDiscovery.invitations.value.firstOrNull()
                        if (!replied && received?.device?.name == "ETCP-desktop") {
                            LocalDiscovery.send(received.device, received.link)
                            LocalDiscovery.dismissInvitation(); replied = true
                        }
                    }
                    if (replied && LocalDiscovery.delivery.value?.received == true) break
                    Thread.sleep(100)
                }
                check(replied && LocalDiscovery.delivery.value?.received == true) { "invitation roundtrip timeout" }
                result.putString("invites", "received and acknowledged in both directions")
                val remaining = deadline - SystemClock.elapsedRealtime()
                if (remaining > 0) Thread.sleep(remaining) // Не закрывать GATT до конца окна ETCP-проверки.
            } else Thread.sleep(seconds * 1000)
            result.putString("stream", "BLE echo session completed; verify desktop RESULT\n")
        } catch (e: Exception) {
            code = Activity.RESULT_CANCELED; result.putString("stream", "BLE FAIL: $e\n")
            LogManager.addLog("ERROR", "discovery", "BLE probe failed: $e")
        } finally {
            try { restoreInvites?.let { NativeLib.setChatSetting("bluetooth_invites_enabled", it) }; BleRadio.applySettings() }
            catch (e: Exception) {
                code = Activity.RESULT_CANCELED
                result.putString("stream", result.getString("stream", "") + "BLE settings restore failed: $e\n")
                LogManager.addLog("ERROR", "discovery", "BLE probe settings restore failed: $e")
            }
        }
        finish(code, result)
    }
}

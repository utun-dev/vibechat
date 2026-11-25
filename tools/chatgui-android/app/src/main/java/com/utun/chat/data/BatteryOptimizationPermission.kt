package com.utun.chat.data

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.PowerManager
import android.provider.Settings

/** Адаптер Android power allowlist, используемый первоначальной настройкой и экраном настроек.
 * Выбор хранит ОС; отдельного флага приложения/оптимистического grant нет. Ошибка чтения — неизвестное состояние.
 * Запуск системного экрана не означает выдачу исключения: владелец повторяет granted() после RESUME. */
object BatteryOptimizationPermission {
    /** Актуальное состояние пакета context: true/false — ответ ОС, null — недоступный сервис или ошибка запроса с диагностикой.
     * Результат не кэшируется; это исключение из оптимизации, а не гарантия снятия всех фоновых ограничений. */
    fun granted(context: Context): Boolean? = try {
        val power = context.getSystemService(PowerManager::class.java)
        if (power == null) {
            LogManager.addLog("ERROR", "PERMISSION", "Power service unavailable: package=${context.packageName}")
            null
        } else power.isIgnoringBatteryOptimizations(context.packageName)
    } catch (error: Exception) {
        LogManager.addLog("ERROR", "PERMISSION", "Battery optimization query failed: package=${context.packageName} error=$error")
        null
    }

    /** Из UI открыть запрос для package context, при отказе — общий список исключений.
     * context должен поддерживать startActivity без NEW_TASK (Activity); source нужен только для диагностики.
     * true — один экран запущен, false — оба запуска не удались; каждое исключение и окончательный отказ логируются. */
    fun request(context: Context, source: String): Boolean {
        val actions = listOf(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS, Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS)
        for (action in actions) {
            val intent = Intent(action)
            if (action == Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS) intent.data = Uri.parse("package:${context.packageName}")
            try {
                context.startActivity(intent)
                LogManager.addLog("INFO", "PERMISSION", "Battery optimization request: source=$source action=$action package=${context.packageName}")
                return true
            } catch (e: Exception) {
                LogManager.addLog("WARN", "PERMISSION", "Cannot open battery optimization screen: source=$source action=$action error=$e")
            }
        }
        LogManager.addLog("ERROR", "PERMISSION", "Battery optimization request failed: source=$source package=${context.packageName}")
        return false
    }
}

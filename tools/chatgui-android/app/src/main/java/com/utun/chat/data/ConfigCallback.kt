package com.utun.chat.data

/** Вспомогательное описание синхронных getters конфигурации: String может отсутствовать, Int задаёт provider.
 * JNI связывает ConfigProvider по сигнатурам методов; актуальный радио-ACK — onRadioSettingsApplied(Long, Boolean).
 * Этот интерфейс не задаёт полный контракт callback объекта nativeInit. */
interface ConfigCallback {
    fun getString(key: String): String?
    fun getInt(key: String): Int
}

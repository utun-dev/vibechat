package com.utun.chat.data

import android.media.AudioDeviceInfo

/** Hardware-only JVM seam: MediaAudioSession, общие подписки и временная шкала компилируются из приложения. */
internal class AndroidMicrophoneCapture private constructor(
    val input: AudioDeviceInfo, val voice: Boolean, val stereo: Boolean, val micSource: Int, val generation: Long,
    val accept: (MicrophoneCapture.Frame) -> Boolean, val failed: (Exception) -> Unit, val confirmed: () -> Unit
) : AutoCloseable {
    companion object {
        val opened = mutableListOf<AndroidMicrophoneCapture>()
        var failOpen = false
        var failClose = false
        var autoConfirm = true
        var attempts = 0
        fun reset() { opened.clear(); failOpen = false; failClose = false; autoConfirm = true; attempts = 0 }
        fun open(input: AudioDeviceInfo, voice: Boolean, stereo: Boolean, micSource: Int, generation: Long,
                 accept: (MicrophoneCapture.Frame) -> Boolean, failed: (Exception) -> Unit,
                 confirmed: () -> Unit): AndroidMicrophoneCapture {
            attempts++
            check(!failOpen) { "hardware capture open failed" }
            check(opened.none { !it.closed }) { "overlapping hardware readers" }
            return AndroidMicrophoneCapture(input, voice, stereo, micSource, generation, accept, failed, confirmed).also {
                opened.add(it)
                if (autoConfirm) confirmed()
            }
        }
    }
    var closed = false
        private set
    var closeAttempts = 0
        private set
    override fun close() {
        closeAttempts++
        check(!failClose) { "hardware capture close failed" }
        closed = true
    }
}

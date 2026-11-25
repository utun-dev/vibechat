package android.media

import android.content.Context
import android.net.Uri
import android.os.Handler
import android.os.Looper

/** Host API для alert-тестов: атрибуты фиксируются при prepare внутри create, как в Android. */
class AudioAttributes private constructor(val usage: Int, val contentType: Int) {
    companion object {
        const val USAGE_MEDIA = 1; const val USAGE_VOICE_COMMUNICATION = 2
        const val USAGE_NOTIFICATION = 5; const val USAGE_NOTIFICATION_RINGTONE = 6
        const val CONTENT_TYPE_SPEECH = 1; const val CONTENT_TYPE_MUSIC = 2; const val CONTENT_TYPE_SONIFICATION = 4
    }
    class Builder {
        private var usage = USAGE_MEDIA
        private var contentType = CONTENT_TYPE_MUSIC
        fun setUsage(value: Int) = apply { usage = value }
        fun setContentType(value: Int) = apply { contentType = value }
        fun setLegacyStreamType(value: Int) = apply { check(value == AudioManager.STREAM_RING); usage = USAGE_NOTIFICATION_RINGTONE }
        fun build() = AudioAttributes(usage, contentType)
    }
}

class AudioFocusRequest private constructor(val attributes: AudioAttributes?, val listener: ((Int) -> Unit)?, val handler: Handler?) {
    class Builder(gain: Int) {
        init { check(gain == AudioManager.AUDIOFOCUS_GAIN_TRANSIENT) }
        private var attributes: AudioAttributes? = null
        private var listener: ((Int) -> Unit)? = null
        private var handler: Handler? = null
        fun setAudioAttributes(value: AudioAttributes) = apply { attributes = value }
        fun setOnAudioFocusChangeListener(value: (Int) -> Unit, delivery: Handler) = apply { listener = value; handler = delivery }
        fun build() = AudioFocusRequest(attributes, listener, handler)
    }
    fun deliver(change: Int) { handler?.post { listener?.invoke(change) } }
}

/** Управляемые lifecycle failures; callbacks намеренно сохраняются после release для теста запоздалой доставки. */
class MediaPlayer private constructor(val resourceId: Int, val effectiveAttributes: AudioAttributes, var failures: Set<String>) {
    companion object {
        val created = mutableListOf<MediaPlayer>()
        var nextFailures = emptySet<String>()
        fun reset() { created.clear(); nextFailures = emptySet() }
        fun create(context: Context, resourceId: Int): MediaPlayer? =
            create(context, resourceId, AudioAttributes.Builder().build(), AudioManager.AUDIO_SESSION_ID_GENERATE)
        fun create(context: Context, resourceId: Int, attributes: AudioAttributes, sessionId: Int): MediaPlayer? {
            check(Looper.myLooper() === Looper.getMainLooper())
            check(sessionId == AudioManager.AUDIO_SESSION_ID_GENERATE)
            check("create" !in nextFailures) { "create failed" }
            if ("null" in nextFailures) return null
            return MediaPlayer(resourceId, attributes, nextFailures).also { created.add(it) }
        }
    }
    var released = false
        private set
    var playing = false
        private set
    var startCalls = 0
        private set
    var releaseCalls = 0
        private set
    var isLooping = false
        set(value) { fail("loop"); field = value }
    var completion: ((MediaPlayer) -> Unit)? = null
        private set
    var error: ((MediaPlayer, Int, Int) -> Boolean)? = null
        private set
    private fun fail(operation: String) {
        check(!released) { "operation $operation after release" }
        check(operation !in failures) { "$operation failed" }
    }
    // Позднее изменение не меняет effectiveAttributes подготовленного плеера.
    fun setAudioAttributes(attributes: AudioAttributes) { fail("attributes") }
    fun setOnCompletionListener(listener: (MediaPlayer) -> Unit) { fail("completion listener"); completion = listener }
    fun setOnErrorListener(listener: (MediaPlayer, Int, Int) -> Boolean) { fail("error listener"); error = listener }
    fun start() { startCalls++; fail("start"); playing = true; fail("start after") }
    fun stop() { fail("stop") }
    fun release() {
        releaseCalls++
        check("release" !in failures) { "release failed" }
        released = true
        playing = false
        check("release after" !in failures) { "release failed after effect" }
    }
    fun complete() { completion?.invoke(this) }
    fun playbackError(what: Int = 100, extra: Int = -1004): Boolean {
        val handled = error?.invoke(this, what, extra) ?: false
        if (!handled) complete()
        return handled
    }
}

class Ringtone internal constructor(var failures: Set<String>) {
    var audioAttributes: AudioAttributes? = null
        set(value) { check("attributes" !in failures) { "ringtone attributes failed" }; field = value }
    var isLooping = false
        set(value) { check("loop" !in failures) { "ringtone loop failed" }; field = value }
    var playCalls = 0
    var stopCalls = 0
    var playing = false
        private set
    fun play() {
        playCalls++; check("play" !in failures) { "ringtone play failed" }
        playing = true; check("play after" !in failures) { "ringtone play failed after effect" }
    }
    fun stop() {
        stopCalls++; check("stop" !in failures) { "ringtone stop failed" }
        playing = false; check("stop after" !in failures) { "ringtone stop failed after effect" }
    }
}

object RingtoneManager {
    var nextFailures = emptySet<String>()
    val created = mutableListOf<Ringtone>()
    val uris = mutableListOf<Uri>()
    fun reset() { nextFailures = emptySet(); created.clear(); uris.clear() }
    fun getRingtone(context: Context, uri: Uri): Ringtone? {
        uris.add(uri)
        check("get" !in nextFailures) { "getRingtone failed" }
        if ("null" in nextFailures) return null
        return Ringtone(nextFailures).also { created.add(it) }
    }
}

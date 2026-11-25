package com.utun.chat.data

import org.json.JSONObject
import java.util.Locale

/** Узел входящей цепочки одного видео: durationUs=null — неизвестно, ноль — известная пустая очередь.
 * stale/down не стирают последнюю длительность; shortName ограничен восемью Unicode code points. */
data class StreamBufferNode(val name: String, val durationUs: Long?, val frames: Long = 0,
                            val stale: Boolean = false, val down: Boolean = false) {
    val shortName: String get() = if (name.codePointCount(0, name.length) <= 8) name
        else name.substring(0, name.offsetByCodePoints(0, 8)) + "…"
    val time: String get() = durationUs?.let { String.format(Locale.ROOT, "%.1f", it / 1_000_000.0).removeSuffix(".0") + "S" } ?: "?"
}

/** Native JSON содержит до 32 nodes с flags known/stale/down (биты 1/2/4), us/frames/name и self.
 * lag_ms/lag_valid/waiting_live описывают подтверждённое отставание и ожидание свежего KEY без остановки показа.
 * fromNative дописывает локальный playback reserveUs (null — неизвестно); tempoMilli=1000 — 1x.
 * Некорректный JSON/отрицательные счётчики бросают ошибку caller; тот сохраняет прежнюю телеметрию и логирует её. */
data class StreamPlaybackTelemetry(val nodes: List<StreamBufferNode> = emptyList(), val tempoMilli: Int = 1000,
                                   val liveLagMs: Long? = null, val waitingLive: Boolean = false) {
    val speed: String? get() = if (tempoMilli <= 1000) null
        else String.format(Locale.ROOT, "%.2f", tempoMilli / 1000.0).removeSuffix("0") + "x"

    companion object {
        fun fromNative(data: ByteArray, reserveUs: Long?, tempoMilli: Int): StreamPlaybackTelemetry {
            val json = JSONObject(data.toString(Charsets.UTF_8))
            val lag = json.getLong("lag_ms")
            val lagValid = json.getBoolean("lag_valid")
            val waiting = json.getBoolean("waiting_live")
            require(lag >= 0 && (!waiting || lagValid))
            val array = json.getJSONArray("nodes")
            require(array.length() <= 32)
            val nodes = List(array.length()) { index ->
                val row = array.getJSONObject(index)
                val flags = row.getInt("flags")
                val duration = row.getLong("us")
                val frames = row.getLong("frames")
                require(flags in 0..7 && duration >= 0 && frames >= 0)
                StreamBufferNode(row.getString("name"), duration.takeIf { flags and 1 != 0 }, frames,
                    stale = flags and 2 != 0, down = flags and 4 != 0)
            } + StreamBufferNode(json.getString("self"), reserveUs)
            return StreamPlaybackTelemetry(nodes, tempoMilli, lag.takeIf { lagValid }, waiting)
        }
    }
}

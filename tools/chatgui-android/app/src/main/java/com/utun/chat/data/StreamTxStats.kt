package com.utun.chat.data

data class StreamBufferPeer(val peer: String, val worst: Long, val best: Long, val subflows: Int)
data class StreamBufferPositions(val first: Long = 0, val head: Long = 0, val total: Int = 0,
                                 val peers: List<StreamBufferPeer> = emptyList()) {
    fun fraction(next: Long): Float = if (head <= first) 1f
        else ((next.coerceIn(first, head) - first).toDouble() / (head - first)).toFloat()
}

/** TX snapshot: sampleUs — MONOTONIC мкс, счётчики данных — байты, connections — число соединений.
 * acked1s — десять завершённых интервалов по 100 мс для всех соединений ядра.
 * buffer.first/head и peer worst/best — seq пачек, total — все получатели, peers — до 16 отставших.
 * fromNative читает 15 + 7*peerCount полей JNI; первые четыре слова каждой строки хранят Ed25519, ошибки формата бросаются caller. */
data class StreamTxStats(val id: Long, val sampleUs: Long, val videoBytes: Long, val audioBytes: Long,
                         val streamBytes: Long, val transportBytes: Long, val inflightBytes: Long,
                         val acked1s: Long, val totalBytes: Long, val connections: Long, val buffer: StreamBufferPositions,
                         val sourceOverloaded: Boolean, val videoBitrate: Int) {
    companion object {
        fun fromNative(id: Long, data: LongArray): StreamTxStats {
            require(data.size >= 15 && data.take(15).all { it >= 0 } && data[12] <= 16 && data[13] <= 1 && data[14] <= Int.MAX_VALUE)
            require(data.size == 15 + data[12].toInt() * 7 && data[9] <= data[10] && data[12] <= data[11] && data[11] <= Int.MAX_VALUE)
            val peers = List(data[12].toInt()) { i ->
                val offset = 15 + i * 7
                val peer = (0..3).joinToString("") { data[offset + it].toULong().toString(16).padStart(16, '0') }
                require((0..3).any { data[offset + it] != 0L } && data[offset + 4] >= 0 &&
                    data[offset + 5] >= data[offset + 4] && data[offset + 6] in 1..255)
                StreamBufferPeer(peer, data[offset + 4], data[offset + 5], data[offset + 6].toInt())
            }
            return StreamTxStats(id, data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7], data[8],
                StreamBufferPositions(data[9], data[10], data[11].toInt(), peers), data[13] != 0L, data[14].toInt())
        }
    }
}

package com.utun.chat.data

import java.nio.ByteBuffer
import java.nio.ByteOrder

/** Ближайшие записи viewer: порядок каждого трека независим, EOF не ждёт чужой трек.
 * GAP/CLOSED отменяют прежние данные. add принимает до 16 BE записей (header 28 bytes + payload)
 * и проверяет всю пачку до изменения очереди; при ошибке очередь сохраняется.
 * take разрешает EOF обойти другой трек, сохраняя порядок собственного. Только player worker;
 * ограничение общей очереди/backpressure задаёт caller, он же логирует исключения. */
internal class StreamReadQueue {
    private val records = mutableListOf<ByteArray>()
    val size: Int get() = records.size
    val videos: Int get() = records.count { it[0].toInt() == 1 }
    val audios: Int get() = records.count { it[0].toInt() == 2 }
    fun clear() = records.clear()

    fun add(batch: ByteArray): Int {
        val input = ByteBuffer.wrap(batch).order(ByteOrder.BIG_ENDIAN)
        val parsed = mutableListOf<ByteArray>()
        while (input.hasRemaining()) {
            check(input.remaining() >= 28) { "Truncated stream record header" }
            val start = input.position()
            val kind = batch[start].toInt()
            val length = input.getInt(start + 16)
            check(kind in 1..8 && length >= 0 && length <= input.remaining() - 28) { "Invalid stream record kind=$kind length=$length" }
            check(parsed.size < 16) { "Stream batch exceeds record budget" }
            parsed.add(batch.copyOfRange(start, start + 28 + length))
            input.position(start + 28 + length)
        }
        for (record in parsed) {
            val kind = record[0].toInt()
            if (kind == 3 || kind == 6) records.clear()
            records.add(record)
        }
        return parsed.size
    }

    fun take(video: Boolean, audio: Boolean): ByteArray? {
        var videoBefore = false
        var audioBefore = false
        for (i in records.indices) {
            val eligible = when (records[i][0].toInt()) {
                1 -> video.also { videoBefore = true }
                2 -> audio.also { audioBefore = true }
                7 -> !videoBefore
                8 -> !audioBefore
                else -> i == 0
            }
            if (eligible) return records.removeAt(i)
        }
        return null
    }
}

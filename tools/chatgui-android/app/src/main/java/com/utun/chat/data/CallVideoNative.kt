package com.utun.chat.data

/** JNI общего C video port: один TX mailbox, ограниченный RX, call ID в операциях порта.
 * CAPS — ByteArray(64), формат — LongArray(9), TX meta — 12 полей, RX meta — 13;
 * первые девять полей задаёт CallVideoFormat, далее frameId/ptsUs/flags/receivedUs.
 * JNI копирует данные и не хранит заимствованные массивы. null/false означают отказ/неготовность API,
 * а исключения JVM передаются caller. Аппаратная проба и порядок остановки остаются у Kotlin-владельцев. */
internal object CallVideoNative {
    /** Ограничить CAPS для обеих ориентаций после аппаратных проб; null — некорректные/несовместимые CAPS. */
    external fun rotatableCaps(caps: ByteArray): ByteArray?
    init { System.loadLibrary("utun") }
    /** Открыть порт; повтор своего id обновляет CAPS, не сбрасывая RX. false — отказ. */
    external fun open(id: Long, caps: ByteArray): Boolean
    /** Идемпотентно закрыть порт своего id после остановки producers/RX; raw playout handle закрывается отдельно. */
    external fun close(id: Long)
    /** CAPS пира либо null, пока нет согласования/порта. */
    external fun peer(id: Long): ByteArray?
    /** 12 полей: keySerial, rateGeneration/request/bps, configuredGeneration, error, min/maxBps,
     * decodedGeneration/accepted, canEncode, txIdle; null — порт недоступен. */
    external fun status(id: Long): LongArray?
    /** Поворот YUV plane: direct sliced buffers с первым пикселем в offset 0, strides в байтах;
     * width/height — исходные пиксели, rotation=0/90/180/270. false — некорректная геометрия/буферы. */
    external fun rotatePlane(source: java.nio.ByteBuffer, sourceRow: Int, sourcePixel: Int,
        destination: java.nio.ByteBuffer, destinationRow: Int, destinationPixel: Int, width: Int, height: Int, rotation: Int): Boolean
    /** RGBA_8888 → одна плоскость Y/U/V flexible YUV420, direct buffers с учётом обоих strides. */
    external fun rgbaPlane(source: java.nio.ByteBuffer, sourceRow: Int, sourcePixel: Int,
        destination: java.nio.ByteBuffer, destinationRow: Int, destinationPixel: Int, width: Int, height: Int, plane: Int): Boolean
    /** Выбрать совместимый формат из CAPS, сохранив generation/размер/частоту desired; null — отказ. */
    external fun choose(local: ByteArray, peer: ByteArray, desired: LongArray): LongArray?
    /** Опубликовать фактический формат и bitrate в бит/с; false — отказ порта/формата. */
    external fun format(id: Long, format: LongArray, bps: Int): Boolean
    /** Ответить на rate request после IDR; bps=0 означает отказ/отмену. */
    external fun applied(id: Long, generation: Long, request: Long, bps: Int)
    /** 1 — frame принят, 0 — mailbox занят (повторить тот же frame), <0 — ошибка. */
    external fun send(id: Long, meta: LongArray, bytes: ByteArray): Int
    /** Извлечь encoded frame и заполнить meta[13]; null — кадра нет/отказ, meta тогда не использовать. */
    external fun pull(id: Long, meta: LongArray): ByteArray?
    /** Создать независимый playout handle; 0 — ошибка. RX сериализует все обращения, destroy после join допустим у caller. */
    external fun playoutCreate(id: Long): Long
    /** Освободить handle ровно один раз; после возврата ни одна операция с ним не допустима. */
    external fun playoutDestroy(handle: Long)
    /** Сбросить таймер/якорь при смене Surface; handle остаётся тем же. */
    external fun playoutReset(handle: Long)
    /** CLOCK_MONOTONIC в мкс; pts<0 — нет готового output, localQueued/reserve — число кадров.
     * >0: ждать микросекунды (100..5000); 0: показать; <0: пропустить только показ. */
    external fun playoutStep(handle: Long, pts: Long, received: Long, now: Long, intervalUs: Int,
        localQueued: Int, reserve: Int): Long
    /** Подтвердить фактический показ pts в Surface; now — MONOTONIC мкс. */
    external fun playoutPresented(handle: Long, pts: Long, now: Long)
    external fun requestKey(id: Long)
    /** Установить границы bitrate в бит/с; false — отказ. */
    external fun range(id: Long, minimum: Int, maximum: Int): Boolean
    /** Проверить uAV2 KEY: width/height/level/profile/dpb либо null. */
    external fun inspect(codec: Int, bytes: ByteArray): LongArray?
    /** Синтетический uAV2 KEY для аппаратной пробы decoder; null — неподдерживаемые параметры. */
    external fun probeKey(codec: Int, profile: Int, width: Int, height: Int, fps: Int = 30): ByteArray?
    external fun supported(local: ByteArray, peer: ByteArray, format: LongArray): Boolean
    /** Сообщить принятие/отказ поколения decoder после фактического результата. */
    external fun decoded(id: Long, generation: Long, accepted: Boolean)
}

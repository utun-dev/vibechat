package com.utun.chat.data

import kotlin.math.min

internal data class StreamGridShape(val columns: Int, val rows: Int) {
    /** Неполный последний ряд делит всю ширину между оставшимися источниками. */
    fun columnsInRow(row: Int, count: Int): Int = minOf(columns, count - row * columns)
}

/** Выбрать сетку по реальным width/height источников: максимальная суммарная площадь целых кадров.
 * При равной площади выбирает меньше пустых ячеек; размеры/aspect — конечные, aspect > 0.
 * Пустой список/неположительные размеры дают 1x1. Неверный aspect заменяется 16:9.
 * В сетке видео не поворачиваются; единственная плитка выбирает поворот отдельно при вписывании. */
internal fun streamGridShape(aspects: List<Double>, width: Double, height: Double): StreamGridShape {
    val count = aspects.size
    if (count <= 0 || width <= 0 || height <= 0) return StreamGridShape(1, 1)
    var best = StreamGridShape(1, count)
    var bestArea = -1.0
    var bestEmpty = count
    for (columns in 1..count) {
        val rows = (count + columns - 1) / columns
        val cellHeight = height / rows
        val shape = StreamGridShape(columns, rows)
        val area = aspects.mapIndexed { index, value ->
            val aspect = value.takeIf { it.isFinite() && it > 0 } ?: (16.0 / 9)
            val cellWidth = width / shape.columnsInRow(index / columns, count)
            val videoWidth = min(cellWidth, cellHeight * aspect)
            videoWidth * videoWidth / aspect
        }.sum()
        val empty = rows * columns - count
        if (area > bestArea + 0.001 || (kotlin.math.abs(area - bestArea) <= 0.001 && empty < bestEmpty)) {
            best = StreamGridShape(columns, rows); bestArea = area; bestEmpty = empty
        }
    }
    return best
}

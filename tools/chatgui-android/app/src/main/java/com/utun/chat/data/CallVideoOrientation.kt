package com.utun.chat.data

/** Угол OrientationEventListener: 90° означает левое ребро сверху, независимо от экрана.
 * Первый достоверный отсчёт принимается сразу; далее 60° гистерезис и 250 мс устойчивости.
 * observe/confirm вызываются одним worker с монотонными nowMs; неизвестный angle сбрасывает только кандидата.
 * rotation(sensor, front) задаёт физический поворот YUV, не поворот UI/прямого preview.
 * resetPending не забывает подтверждённый угол между включениями камеры. */
internal class CallVideoOrientation {
    var degrees = 0
        private set
    private var known = false
    private var candidate = -1
    private var candidateAt = 0L

    fun observe(angle: Int, nowMs: Long): Boolean {
        if (angle !in 0..359) { candidate = -1; return false }
        val next = ((angle + 45) / 90 * 90) % 360
        if (!known) { known = true; degrees = next; return true }
        val difference = kotlin.math.abs(angle - degrees).let { minOf(it, 360 - it) }
        if (next == degrees || difference < 60) candidate = -1
        else if (candidate != next) { candidate = next; candidateAt = nowMs }
        return false
    }

    fun confirm(nowMs: Long): Boolean {
        if (candidate < 0 || nowMs - candidateAt < 250) return false
        degrees = candidate; candidate = -1; return true
    }

    fun resetPending() { candidate = -1 }
    fun rotation(sensor: Int, front: Boolean): Int = (sensor + (if (front) -degrees else degrees) + 360) % 360
}

import com.utun.chat.data.RadioVadWindow

fun main() {
    val minute = 60_000L
    val duration = 15 * minute
    val window = RadioVadWindow(duration)
    window.setForeground(true, 0)
    window.setEnabled(true, 0)
    check(window.isAllowed(120 * minute)) { "Foreground VAD expired" }
    check(window.timeLeftMs(120 * minute) == duration) { "Foreground consumed background time" }

    window.setForeground(false, 120 * minute)
    check(window.deadlineMs == 135 * minute)
    window.setForeground(true, 125 * minute)
    check(window.timeLeftMs(300 * minute) == duration) { "Returning to the app did not reset the timer" }
    check(window.isAllowed(300 * minute))
    window.setForeground(false, 300 * minute)
    window.setEnabled(true, 301 * minute) // Перенастройка не продлевает окно.
    window.setForeground(false, 302 * minute)
    check(window.deadlineMs == 315 * minute) { "New background period did not get the full timeout" }
    check(window.isAllowed(315 * minute - 1))
    check(!window.isAllowed(315 * minute)) { "Background timer did not expire" }

    window.setForeground(true, 316 * minute)
    check(window.isAllowed(500 * minute)) { "Returning to the app did not resume VAD" }
    check(window.timeLeftMs(500 * minute) == duration)
    window.setForeground(false, 500 * minute)
    check(window.deadlineMs == 515 * minute)
    window.close() // Погасший экран закрывает окно сразу.
    check(!window.isAllowed(501 * minute))
    window.restart(502 * minute)
    check(window.deadlineMs == 517 * minute)

    window.setEnabled(false, 503 * minute)
    window.setForeground(true, 504 * minute)
    check(!window.isAllowed(504 * minute)) { "Disabled VAD resumed automatically" }
    window.setEnabled(true, 505 * minute)
    check(window.isAllowed(1000 * minute))

    val backgroundStart = RadioVadWindow(duration)
    backgroundStart.setEnabled(true, minute)
    check(backgroundStart.deadlineMs == 16 * minute)
    check(!backgroundStart.isAllowed(16 * minute))
    backgroundStart.restart(17 * minute) // Явное повторное включение после истечения.
    check(backgroundStart.deadlineMs == 32 * minute)
    println("PASS: foreground unlimited and resets timer; full background timeout; expiry; screen; enable/reconfigure")
}

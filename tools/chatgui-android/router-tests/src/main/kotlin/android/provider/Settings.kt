package android.provider

/** Имена настоящих Android actions; host-тест проверяет target/порядок и не открывает системный UI. */
object Settings {
    var overlayAllowed = true
    var failOverlayQuery = false
    fun canDrawOverlays(context: android.content.Context): Boolean {
        check(!failOverlayQuery) { "overlay permission query unavailable" }
        return overlayAllowed
    }
    const val ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS = "android.settings.REQUEST_IGNORE_BATTERY_OPTIMIZATIONS"
    const val ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS = "android.settings.IGNORE_BATTERY_OPTIMIZATION_SETTINGS"
}

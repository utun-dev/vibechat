package com.utun.chat.data

import android.hardware.camera2.CameraManager

/** Границы сохранённых настроек и списка объективов; реальные Camera2 callbacks остаются у engine. */
class VideoHostConfig {
    var failures = emptySet<String>()
    fun getCallVideoCaptureSettings(): CallVideoSettings { check("settings load" !in failures); return CallVideoSettings() }
    fun getCallVideoBitrateRange(): CallVideoBitrateRange { check("range load" !in failures); return CallVideoBitrateRange() }
    fun saveCallVideoCaptureSettings(settings: CallVideoSettings) { check("settings save" !in failures) }
    fun saveCallVideoBitrateRange(range: CallVideoBitrateRange) { check("range save" !in failures) }
}
class VideoHostLens(val cameraId: String = "0", val label: String = "host", val mm: String = "26mm", val eqFocal: Float = 26f)
object CameraLensManager { fun enumerate(manager: CameraManager) = listOf(VideoHostLens()) }

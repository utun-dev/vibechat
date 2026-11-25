package com.utun.chat.data

import com.utun.chat.R
import android.graphics.ImageFormat
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraMetadata
import android.hardware.camera2.CameraManager
import android.util.Size
import androidx.camera.camera2.interop.Camera2CameraInfo
import androidx.camera.core.CameraSelector
import androidx.camera.lifecycle.ProcessCameraProvider
import kotlin.math.abs
import kotlin.math.round
import kotlin.math.sqrt

data class PhotoResolution(val width: Int, val height: Int) {
    val label: String get() = "${width}×${height}"
    val area: Long get() = width.toLong() * height.toLong()
}

enum class LensType { SELFIE, MAIN, ULTRA_WIDE, MACRO, TELE }

data class LensOption(
    val type: LensType,
    val cameraId: String,
    val eqFocal: Float
) {
    val label: String get() = AppLanguage.text(when (type) {
        LensType.SELFIE -> R.string.lens_selfie
        LensType.MAIN -> R.string.lens_main
        LensType.ULTRA_WIDE -> R.string.lens_wide
        LensType.MACRO -> R.string.lens_macro
        LensType.TELE -> R.string.lens_tele
    })
    val mm: String get() = if (eqFocal > 0f) AppLanguage.text(R.string.focal_mm, round(eqFocal).toInt()) else ""

    fun selector(): CameraSelector = CameraSelector.Builder()
        .addCameraFilter { infos -> infos.filter { Camera2CameraInfo.from(it).cameraId == cameraId } }
        .build()
}

/** Метаданные объективов CameraX/Camera2 без открытия камеры. MAIN — AF-камера, ближайшая к 26 мм,
 * остальные задние классифицируются по отношению фокусных расстояний и fixed focus; front — SELFIE.
 * Эквивалент 35 мм вычисляется по диагонали сенсора, отсутствие размеров оставляет доступное focal значение.
 * Camera2 пропускает с логом ошибки отдельных камер; CameraX/получение списка может бросить caller. */
object CameraLensManager {

    private const val MAIN_REF_FOCAL = 26f

    private data class BackLens(
        val cameraId: String,
        val eqFocal: Float,
        val fixedFocus: Boolean
    )

    private data class LensInfo(val id: String, val facing: Int, val eq: Float, val minFocus: Float)

    fun enumerate(provider: ProcessCameraProvider): List<LensOption> = enumerate(provider.availableCameraInfos.map { info ->
        val c2 = Camera2CameraInfo.from(info)
        LensInfo(c2.cameraId, c2.getCameraCharacteristic(CameraCharacteristics.LENS_FACING) ?: CameraMetadata.LENS_FACING_BACK,
            eqFocal35(c2), c2.getCameraCharacteristic(CameraCharacteristics.LENS_INFO_MINIMUM_FOCUS_DISTANCE) ?: 0f)
    })

    fun enumerate(manager: CameraManager): List<LensOption> = enumerate(manager.cameraIdList.mapNotNull { id ->
        try {
            val c = manager.getCameraCharacteristics(id)
            val focal = c.get(CameraCharacteristics.LENS_INFO_AVAILABLE_FOCAL_LENGTHS)?.firstOrNull() ?: 0f
            val sensor = c.get(CameraCharacteristics.SENSOR_INFO_PHYSICAL_SIZE)
            val diagonal = sensor?.let { sqrt(it.width * it.width + it.height * it.height) } ?: 0f
            LensInfo(id, c.get(CameraCharacteristics.LENS_FACING) ?: CameraMetadata.LENS_FACING_BACK,
                if (diagonal > 0f) focal * 43.27f / diagonal else focal,
                c.get(CameraCharacteristics.LENS_INFO_MINIMUM_FOCUS_DISTANCE) ?: 0f)
        } catch (e: Exception) { LogManager.addLog("ERROR", "Lens", "camera enumeration id=$id: ${e.message}"); null }
    })

    private fun enumerate(cameras: List<LensInfo>): List<LensOption> {
        val result = mutableListOf<LensOption>()
        val backs = mutableListOf<BackLens>()
        for (info in cameras) {
            if (info.facing == CameraMetadata.LENS_FACING_FRONT) {
                result.add(LensOption(LensType.SELFIE, info.id, info.eq)); continue
            }
            if (info.facing != CameraMetadata.LENS_FACING_BACK) continue
            backs.add(BackLens(info.id, info.eq, info.minFocus == 0f))
            LogManager.addLog("INFO", "Lens", "back cam id=${info.id} eq=${info.eq} fixedFocus=${info.minFocus == 0f}")
        }

        if (backs.isEmpty()) {
            LogManager.addLog("WARN", "Lens", "no back cameras found")
            return result
        }

        /* main = камера с автофокусом, ближайшая к 26мм; fallback — ближайшая к 26мм */
        val afBacks = backs.filter { !it.fixedFocus }
        val main = (if (afBacks.isNotEmpty()) afBacks else backs)
            .minByOrNull { abs(it.eqFocal - MAIN_REF_FOCAL) } ?: backs[0]

        fun typeOf(l: BackLens): LensType = when {
            l === main -> LensType.MAIN
            main.eqFocal <= 0f -> LensType.MAIN
            l.eqFocal < main.eqFocal * 0.7f -> LensType.ULTRA_WIDE
            l.eqFocal > main.eqFocal * 1.4f -> LensType.TELE
            l.fixedFocus -> LensType.MACRO
            else -> LensType.MAIN
        }

        val backOptions = backs.sortedBy { it.eqFocal }.map { l ->
            val type = typeOf(l)
            LensOption(type, l.cameraId, l.eqFocal)
        }
        result.addAll(backOptions)

        LogManager.addLog("INFO", "Lens", "enumerated: ${result.joinToString { "${it.label} ${it.mm}".trim() }}")
        return result
    }

    private fun eqFocal35(c2: Camera2CameraInfo): Float {
        val focal = (c2.getCameraCharacteristic(CameraCharacteristics.LENS_INFO_AVAILABLE_FOCAL_LENGTHS)
            ?: floatArrayOf()).firstOrNull() ?: 0f
        val sensor = c2.getCameraCharacteristic(CameraCharacteristics.SENSOR_INFO_PHYSICAL_SIZE)
        if (focal <= 0f || sensor == null || sensor.width <= 0f) return focal
        val diag = sqrt(sensor.width * sensor.width + sensor.height * sensor.height)
        if (diag <= 0f) return focal
        return focal * 43.27f / diag
    }

    /** Целевые разрешения CameraX: 640x480/720p/1080p/native в пределах максимальной JPEG площади.
     * CameraX подбирает фактический поддержанный размер; отсутствие камеры/JPEG размеров даёт пустой список с WARN. */
    fun photoResolutions(provider: ProcessCameraProvider, cameraId: String): List<PhotoResolution> {
        val info = provider.availableCameraInfos.firstOrNull {
            Camera2CameraInfo.from(it).cameraId == cameraId
        }
        if (info == null) {
            LogManager.addLog("WARN", "Lens", "photoResolutions: camera $cameraId not found")
            return emptyList()
        }
        val c2 = Camera2CameraInfo.from(info)
        val map = c2.getCameraCharacteristic(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
        val jpegSizes = map?.getOutputSizes(ImageFormat.JPEG) ?: emptyArray<Size>()
        if (jpegSizes.isEmpty()) {
            LogManager.addLog("WARN", "Lens", "photoResolutions: no JPEG sizes for $cameraId")
            return emptyList()
        }

        val native = jpegSizes.maxByOrNull { it.width.toLong() * it.height.toLong() } ?: jpegSizes[0]
        val candidates = listOf(
            PhotoResolution(640, 480),
            PhotoResolution(1280, 720),
            PhotoResolution(1920, 1080),
            PhotoResolution(native.width, native.height)
        )
        val result = candidates
            .filter { it.area <= native.width.toLong() * native.height.toLong() }
            .distinctBy { it.label }
        LogManager.addLog("INFO", "Lens", "photoResolutions $cameraId: ${result.joinToString { it.label }}")
        return result
    }
}

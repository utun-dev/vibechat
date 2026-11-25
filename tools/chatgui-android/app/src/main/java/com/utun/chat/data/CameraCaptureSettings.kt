package com.utun.chat.data

/** Независимый выбор фото: camera ищется по ID, затем MAIN/первая; width/height=0 выбирают максимальную площадь.
 * Размеры заданы парой 0/0 либо 1..16384, JPEG quality — один из QUALITIES.
 * resolve* возвращают null для пустого списка, неподдержанный размер заменяется максимальным. */
data class PhotoCaptureSettings(val camera: String = "", val width: Int = 0, val height: Int = 0, val jpegQuality: Int = 85,
                                val orientation: StreamOrientation = StreamOrientation.AUTO) {
    init {
        require((width == 0 && height == 0) || (width in 1..16384 && height in 1..16384))
        require(jpegQuality in QUALITIES)
    }

    fun resolveCamera(lenses: List<LensOption>): LensOption? = lenses.firstOrNull { it.cameraId == camera }
        ?: lenses.firstOrNull { it.type == LensType.MAIN } ?: lenses.firstOrNull()

    fun resolveResolution(sizes: List<PhotoResolution>): PhotoResolution? =
        sizes.firstOrNull { it.width == width && it.height == height } ?: sizes.maxByOrNull { it.area }

    companion object { val QUALITIES = listOf(50, 75, 85, 95, 100) }
}

/** Отдельный preset записи: camera по ID, затем SELFIE/первая; height — метка 480p/720p/1080p.
 * codec=1 AVC/2 HEVC, bitrate в бит/с из BITRATES; неверные значения отвергает конструктор.
 * Пустой список камер даёт null; разрешения/capabilities проверяет владелец CameraX. */
data class VideoCaptureSettings(val camera: String = "", val height: Int = 720, val codec: Int = 1, val bitrate: Int = 2_000_000,
                                val orientation: StreamOrientation = StreamOrientation.AUTO) {
    init { require(height in HEIGHTS); require(codec in 1..2); require(bitrate in BITRATES) }

    fun resolveCamera(lenses: List<LensOption>): LensOption? = lenses.firstOrNull { it.cameraId == camera }
        ?: lenses.firstOrNull { it.type == LensType.SELFIE } ?: lenses.firstOrNull()

    companion object {
        val HEIGHTS = listOf(480, 720, 1080)
        val BITRATES = listOf(256_000, 512_000, 1_000_000, 2_000_000, 4_500_000, 8_000_000)
    }
}

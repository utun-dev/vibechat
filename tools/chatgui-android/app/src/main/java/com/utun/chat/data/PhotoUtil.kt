package com.utun.chat.data

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Matrix
import android.media.ExifInterface

/**
 * Размеры и декодирование локального изображения с учётом всех восьми EXIF-ориентаций.
 * Вызывать на IO worker: методы читают файл синхронно, не проверяют MIME, размер файла или лимит пикселей.
 * Отсутствующий/нечитаемый EXIF трактуется как обычная ориентация; допуск источника проверяет вызывающий код.
 */
object PhotoUtil {
    data class Metadata(val width: Int, val height: Int, val mime: String)

    /** Заголовок изображения, размеры для отображения и фактический формат; пиксели не выделяются. */
    fun metadata(path: String): Metadata {
        val opts = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        BitmapFactory.decodeFile(path, opts)
        require(opts.outWidth in 1..16384 && opts.outHeight in 1..16384) { "Invalid image dimensions ${opts.outWidth}x${opts.outHeight}" }
        val mime = opts.outMimeType.orEmpty()
        require(mime in setOf("image/jpeg", "image/png", "image/gif", "image/bmp", "image/webp")) { "Unsupported image format: $mime" }
        return if (isRotated90(orientation(path))) Metadata(opts.outHeight, opts.outWidth, mime)
            else Metadata(opts.outWidth, opts.outHeight, mime)
    }

    /** Ограниченное декодирование для превью/просмотрщика, с EXIF и сохранением пропорций. */
    fun decodeFitted(path: String, maxDimension: Int): Bitmap? {
        require(maxDimension > 0)
        val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        BitmapFactory.decodeFile(path, bounds)
        if (bounds.outWidth <= 0 || bounds.outHeight <= 0) {
            LogManager.addLog("ERROR", "Image", "Cannot read image dimensions path=$path"); return null
        }
        var sample = 1
        while (maxOf(bounds.outWidth, bounds.outHeight) / sample > maxDimension) sample *= 2
        return decodeOriented(path, sample)
    }

    private fun orientation(path: String): Int = try {
        ExifInterface(path).getAttributeInt(ExifInterface.TAG_ORIENTATION, ExifInterface.ORIENTATION_NORMAL)
    } catch (_: Exception) {
        ExifInterface.ORIENTATION_NORMAL
    }

    private fun isRotated90(orientation: Int): Boolean = when (orientation) {
        ExifInterface.ORIENTATION_ROTATE_90, ExifInterface.ORIENTATION_ROTATE_270,
        ExifInterface.ORIENTATION_TRANSPOSE, ExifInterface.ORIENTATION_TRANSVERSE -> true
        else -> false
    }

    /** Размер полного кадра после EXIF без декодирования пикселей; (0, 0), если bounds не прочитаны.
     * Для поворота 90/270 и transpose/transverse ширина и высота меняются местами. */
    fun orientedDimensions(path: String): Pair<Int, Int> {
        val opts = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        BitmapFactory.decodeFile(path, opts)
        val w = opts.outWidth
        val h = opts.outHeight
        if (w <= 0 || h <= 0) return Pair(0, 0)
        return if (isRotated90(orientation(path))) Pair(h, w) else Pair(w, h)
    }

    /** Декодировать с inSampleSize для BitmapFactory и применить EXIF; Bitmap принадлежит получателю.
     * null — декодирование не удалось. При исключении трансформации возвращается исходный декодированный
     * Bitmap без исправления ориентации; при успешной замене промежуточный raw освобождается. */
    fun decodeOriented(path: String, inSampleSize: Int): Bitmap? {
        val o = orientation(path)
        val opts = BitmapFactory.Options().apply { this.inSampleSize = inSampleSize }
        val raw = try { BitmapFactory.decodeFile(path, opts) } catch (_: Exception) { null } ?: return null
        val matrix = Matrix()
        when (o) {
            ExifInterface.ORIENTATION_FLIP_HORIZONTAL -> matrix.setScale(-1f, 1f)
            ExifInterface.ORIENTATION_ROTATE_180 -> matrix.setRotate(180f)
            ExifInterface.ORIENTATION_FLIP_VERTICAL -> { matrix.setRotate(180f); matrix.postScale(-1f, 1f) }
            ExifInterface.ORIENTATION_TRANSPOSE -> { matrix.setRotate(90f); matrix.postScale(-1f, 1f) }
            ExifInterface.ORIENTATION_ROTATE_90 -> matrix.setRotate(90f)
            ExifInterface.ORIENTATION_TRANSVERSE -> { matrix.setRotate(-90f); matrix.postScale(-1f, 1f) }
            ExifInterface.ORIENTATION_ROTATE_270 -> matrix.setRotate(-90f)
            else -> return raw
        }
        val rotated = try {
            Bitmap.createBitmap(raw, 0, 0, raw.width, raw.height, matrix, true)
        } catch (_: Exception) { raw }
        if (rotated !== raw) raw.recycle()
        return rotated
    }
}

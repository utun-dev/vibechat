package com.utun.chat.data

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.net.Uri
import android.os.Build
import org.json.JSONObject
import java.io.File
import java.io.IOException
import java.security.MessageDigest
import java.util.UUID
import kotlin.math.roundToInt

/** Квадрат в нормализованных координатах полного оригинала после EXIF-ориентации.
 * x/y — левый верхний угол, w/h — доли ширины/высоты; квадратность проверяется в пикселях оригинала. */
data class AvatarCrop(val x: Double, val y: Double, val w: Double, val h: Double)
/** Манифест v1: имена объектов с SHA256, точные размеры файлов в байтах, ориентированные размеры и выбранный квадрат. */
data class AvatarRecord(val original: String, val preview: String, val originalBytes: Long, val previewBytes: Long,
                        val width: Int, val height: Int, val crop: AvatarCrop)

/**
 * Локальные объекты пользовательского аватара; I/O, декодирование и подготовку выполнять на IO worker.
 * Набор состоит из неизменённого JPEG/PNG/WebP-оригинала, квадратного WebP 256×256 и JSON-манифеста v1.
 * Имя каждого объекта — SHA256 его точных байтов плюс расширение; root задаёт каталог текущего аккаунта.
 * read/record/image проверяют данные, store публикует через временный файл и rename в том же каталоге.
 * Нарушение формата/лимита/хеша бросает исключение: вызывающий код логирует ошибку и выбирает повтор/отказ.
 */
object AvatarAssets {
    fun digest(bytes: ByteArray) = AvatarProtocol.hash(MessageDigest.getInstance("SHA-256").digest(bytes))
    /** Прочитать file с проверкой лимита байтов и SHA256 из name; имя самого file может быть временным. */
    fun read(file: File, name: String, limit: Int): ByteArray {
        require(AvatarProtocol.validObject(name) && file.length() in 1..limit) { "invalid object file=$name size=${file.length()}" }
        val bytes = file.inputStream().use { it.readBytes() }
        require(bytes.size <= limit && digest(bytes) == name.take(64)) { "avatar object hash mismatch file=$name" }
        return bytes
    }
    /** Проверить имя/хеш и сохранить точные bytes; ограничения размера/формата проверяет вызывающий код. */
    fun store(root: File, name: String, bytes: ByteArray) {
        require(AvatarProtocol.validObject(name) && digest(bytes) == name.take(64)) { "invalid cache object name=$name" }
        if (!root.isDirectory && !root.mkdirs()) throw IOException("cannot create avatar cache")
        val temp = File(root, ".part-${UUID.randomUUID()}")
        try { temp.writeBytes(bytes); if (!temp.renameTo(File(root, name))) throw IOException("cannot publish object=$name") }
        finally { if (temp.exists() && !temp.delete()) LogManager.addLog("WARN", "Avatar", "Cannot remove cache temp=$temp") }
    }
    /** Скопировать URI в cacheDir, максимум 32 МиБ; успешный результат обязан удалить получатель.
     * Изображение здесь не декодируется и не проверяется; при отказе временный файл удаляется. */
    fun copySource(context: Context, uri: Uri): File {
        val file = File.createTempFile("avatar-editor-", ".image", context.cacheDir)
        try {
            context.contentResolver.openInputStream(uri)?.use { input -> file.outputStream().use { output ->
                val chunk = ByteArray(8192); var size = 0
                while (true) {
                    val count = input.read(chunk); if (count < 0) break
                    size += count; require(size <= AvatarProtocol.MAX_ORIGINAL) { "original exceeds 32 MiB" }
                    output.write(chunk, 0, count)
                }
            } } ?: throw IOException("cannot open selected image")
            return file
        } catch (e: Exception) {
            if (!file.delete()) LogManager.addLog("WARN", "Avatar", "Cannot remove rejected source=$file")
            throw e
        }
    }
    /** Прочитать манифест из root с лимитом 4096 байт, проверкой SHA256 и схемы parse. */
    fun record(root: File, name: String): AvatarRecord = parse(read(File(root, name), name, AvatarProtocol.MAX_MANIFEST))
    /** Проверить JSON v1: имена/лимиты, размеры до 16×1024×1024 пикселей и квадрат crop с допуском 2 пикселя.
     * Сами объекты и их соответствие манифесту здесь не читаются. */
    fun parse(bytes: ByteArray): AvatarRecord {
        val o = JSONObject(String(bytes, Charsets.UTF_8))
        val r = AvatarRecord(o.getString("original"), o.getString("preview"), o.getLong("original_bytes"), o.getLong("preview_bytes"),
            o.getInt("width"), o.getInt("height"), AvatarCrop(o.getDouble("x"), o.getDouble("y"), o.getDouble("w"), o.getDouble("h")))
        val c = r.crop
        require(o.getInt("version") == 1 && AvatarProtocol.validObject(r.original) && !r.original.endsWith(".json") &&
            AvatarProtocol.validObject(r.preview) && r.preview.endsWith(".webp") && r.originalBytes in 1..AvatarProtocol.MAX_ORIGINAL &&
            r.previewBytes in 1..AvatarProtocol.MAX_USER && r.width > 0 && r.height > 0 && r.width.toLong()*r.height <= 16L*1024*1024 &&
            listOf(c.x,c.y,c.w,c.h).all { it.isFinite() } && c.x >= 0 && c.y >= 0 && c.w > 0 && c.h > 0 &&
            c.x+c.w <= 1.00000001 && c.y+c.h <= 1.00000001 && kotlin.math.abs(c.w*r.width-c.h*r.height) <= 2) { "invalid avatar manifest" }
        return r
    }
    /** Декодировать оригинал либо превью: проверить SHA256, заявленный размер байтов, MIME и предел пикселей.
     * Превью обязано быть WebP 256×256. Оригинал уменьшается при декодировании и сохраняет полный кадр без crop.
     * Применяется EXIF через PhotoUtil; возвращённый Bitmap принадлежит вызывающему коду. */
    fun image(root: File, record: AvatarRecord, original: Boolean): Bitmap {
        val name = if (original) record.original else record.preview
        val bytes = read(File(root, name), name, if (original) AvatarProtocol.MAX_ORIGINAL else AvatarProtocol.MAX_USER)
        require(bytes.size.toLong() == if (original) record.originalBytes else record.previewBytes) { "manifest object size mismatch" }
        val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        BitmapFactory.decodeByteArray(bytes, 0, bytes.size, bounds)
        require(bounds.outWidth > 0 && bounds.outHeight > 0 && bounds.outWidth.toLong()*bounds.outHeight <= 16L*1024*1024 &&
            bounds.outMimeType in listOf("image/jpeg", "image/png", "image/webp")) { "invalid avatar image" }
        if (!original) require(bounds.outWidth == 256 && bounds.outHeight == 256 && bounds.outMimeType == "image/webp") { "invalid WebP preview" }
        var sample = 1; while (maxOf(bounds.outWidth,bounds.outHeight)/sample > 2048) sample *= 2
        return PhotoUtil.decodeOriented(File(root,name).path,sample) ?: throw IOException("avatar decode failed")
    }
    /** Подготовить три объекта из source до 32 МиБ/16×1024×1024 пикселей и квадратного selection после EXIF.
     * Сохраняет исходные байты, WebP 256×256 quality=85 и нормализованный манифест; возвращает имя JSON и Bitmap превью.
     * Исходный source не удаляется, возвращённый Bitmap не recycle-ится; оба ресурса остаются у вызывающего кода. */
    fun prepare(source: File, root: File, selection: AvatarCrop): Pair<String, Bitmap> {
        require(source.length() in 1..AvatarProtocol.MAX_ORIGINAL) { "invalid original size" }
        val original = source.readBytes()
        val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        BitmapFactory.decodeByteArray(original,0,original.size,bounds)
        val suffix = when (bounds.outMimeType) { "image/jpeg" -> ".jpg"; "image/png" -> ".png"; "image/webp" -> ".webp"
            else -> throw IOException("unsupported avatar image") }
        val (width,height) = PhotoUtil.orientedDimensions(source.path)
        require(width > 0 && height > 0 && width.toLong()*height <= 16L*1024*1024) { "original exceeds 16 megapixels" }
        val decoded = PhotoUtil.decodeOriented(source.path,1) ?: throw IOException("cannot decode source")
        val x = (selection.x*width).roundToInt(); val y = (selection.y*height).roundToInt()
        val side = (selection.w*width).roundToInt()
        require(x >= 0 && y >= 0 && side > 0 && x+side <= width && y+side <= height &&
            kotlin.math.abs(selection.h*height-side) <= 2) { "invalid selected square" }
        val crop = Bitmap.createBitmap(decoded,x,y,side,side)
        val scaled = Bitmap.createScaledBitmap(crop,256,256,true)
        try {
            val output = java.io.ByteArrayOutputStream()
            val format = if (Build.VERSION.SDK_INT >= 30) Bitmap.CompressFormat.WEBP_LOSSY else Bitmap.CompressFormat.WEBP
            require(scaled.compress(format,85,output)) { "WebP encoder failed" }
            val preview = output.toByteArray(); require(preview.size <= AvatarProtocol.MAX_USER) { "preview exceeds limit" }
            val originalName = digest(original)+suffix; val previewName = digest(preview)+".webp"
            val manifest = JSONObject().put("version",1).put("original",originalName).put("preview",previewName)
                .put("original_bytes",original.size).put("preview_bytes",preview.size).put("width",width).put("height",height)
                .put("x",x.toDouble()/width).put("y",y.toDouble()/height).put("w",side.toDouble()/width).put("h",side.toDouble()/height)
                .toString().toByteArray(Charsets.UTF_8)
            val name = digest(manifest)+".json"
            store(root,originalName,original); store(root,previewName,preview); store(root,name,manifest)
            LogManager.addLog("INFO", "Avatar", "Prepared file=$name original=${original.size} preview=${preview.size} crop=$x,$y,$side")
            return name to scaled.copy(Bitmap.Config.ARGB_8888,false)
        } finally {
            if (scaled !== crop && scaled !== decoded) scaled.recycle()
            if (crop !== decoded) crop.recycle()
            decoded.recycle()
        }
    }
}

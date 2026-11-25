package android.graphics

/** Только размеры и владение промежуточным кадром, без пикселей/драйвера. */
class Bitmap(val width: Int, val height: Int) {
    var recycled = false
    fun recycle() { recycled = true }
    companion object {
        fun createScaledBitmap(source: Bitmap, width: Int, height: Int, filter: Boolean) = Bitmap(width, height)
    }
}

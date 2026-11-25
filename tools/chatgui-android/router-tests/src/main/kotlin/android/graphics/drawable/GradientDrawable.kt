package android.graphics.drawable

class GradientDrawable {
    companion object { const val OVAL = 1 }
    var shape = 0
    var color = 0
        private set
    var strokeWidth = 0
        private set
    var strokeColor = 0
        private set
    fun setColor(value: Int) { color = value }
    fun setStroke(width: Int, color: Int) { strokeWidth = width; strokeColor = color }
}

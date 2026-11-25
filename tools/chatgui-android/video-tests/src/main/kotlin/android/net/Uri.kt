package android.net

data class Uri(private val text: String) {
    override fun toString() = text
    companion object { fun fromParts(scheme: String, part: String, fragment: String?) = Uri("$scheme:$part") }
}

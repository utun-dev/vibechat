package android.content

import android.net.Uri
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.InputStream
import java.io.OutputStream

/** Управляемые URI без Android provider; ошибки чтения/записи задаёт конкретный тест. */
class ContentResolver {
    var displayName = "document.bin"
    var input: (Uri) -> InputStream? = { ByteArrayInputStream(byteArrayOf(1, 2, 3)) }
    var output: (Uri) -> OutputStream? = { ByteArrayOutputStream() }
    var delete: (Uri) -> Boolean = { true }
    fun query(uri: Uri, projection: Array<String>, selection: String?, args: Array<String>?, order: String?) = Cursor(displayName)
    fun openInputStream(uri: Uri) = input(uri)
    fun openOutputStream(uri: Uri, mode: String) = output(uri)
}

class Cursor(private val name: String) : AutoCloseable {
    fun moveToFirst() = true
    fun getString(column: Int) = name
    override fun close() {}
}

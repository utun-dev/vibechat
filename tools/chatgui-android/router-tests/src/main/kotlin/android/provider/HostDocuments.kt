package android.provider

import android.content.ContentResolver
import android.net.Uri

object OpenableColumns { const val DISPLAY_NAME = "_display_name" }
object DocumentsContract { fun deleteDocument(resolver: ContentResolver, uri: Uri) = resolver.delete(uri) }

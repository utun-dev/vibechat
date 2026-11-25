package androidx.core.content

import com.utun.chat.RecorderHostContext

object ContextCompat { fun checkSelfPermission(context: RecorderHostContext, name: String) = if (context.permissionGranted) 0 else -1 }

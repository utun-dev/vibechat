package com.utun.chat.data

import java.io.File
import org.junit.Assert.*
import org.junit.Test

/** Сопоставляет настоящий JNI lookup с JVM-дескриптором скомпилированного Kotlin callback. */
class NativeLogContractTest {
    @Test fun nativeLookupMatchesTheCompiledKotlinByteArrayCallback() {
        val method = LogManager::class.java.declaredMethods.single { it.name == "onNativeLog" }
        val descriptor = "(" + method.parameterTypes.joinToString("") {
            if (it == Int::class.javaPrimitiveType) "I" else it.name.replace('.', '/')
        } + ")" + if (method.returnType == Void.TYPE) "V" else method.returnType.name
        val source = File(requireNotNull(System.getProperty("utun.androidBridgeSource"))).readText()
        val native = Regex("""GetMethodID\(env, cls, "onNativeLog", "([^"]+)"\)""").find(source)
        assertNotNull("JNI onNativeLog lookup missing", native)
        assertEquals("(I[B[B)V", descriptor)
        assertEquals(descriptor, native!!.groupValues[1])
    }
}

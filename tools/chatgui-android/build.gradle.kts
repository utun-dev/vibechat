// Общие версии плагинов; app и отдельный audio-probe включают Android plugin в своих модулях, JVM harness — kotlin.jvm.
plugins {
    id("com.android.application") version "8.13.2" apply false
    id("org.jetbrains.kotlin.android") version "2.2.20" apply false
    id("org.jetbrains.kotlin.jvm") version "2.2.20" apply false
    id("org.jetbrains.kotlin.plugin.compose") version "2.2.20" apply false
}

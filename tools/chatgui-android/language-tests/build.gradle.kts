plugins { id("org.jetbrains.kotlin.jvm") }

kotlin {
    jvmToolchain(17)
    sourceSets.named("main") {
        // Настоящий AppLanguage; Android/AppCompat и владельцы уведомлений моделируются только в этом JVM-модуле.
        kotlin.srcDir("../app/src/main/java")
        kotlin.include("android/**", "androidx/**", "org/xmlpull/**", "com/utun/chat/LanguageHostApp.kt",
            "com/utun/chat/data/LanguageHostEnvironment.kt", "com/utun/chat/headless/LanguageHostHeadless.kt",
            "com/utun/chat/data/AppLanguage.kt")
    }
}

dependencies { testImplementation("junit:junit:4.13.2") }
tasks.test {
    val config = rootProject.file("app/src/main/res/xml/locales_config.xml")
    inputs.file(config)
    systemProperty("utun.localesConfig", config.absolutePath)
    testLogging { showStandardStreams = true }
}

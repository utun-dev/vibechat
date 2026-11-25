plugins { id("org.jetbrains.kotlin.jvm") }

kotlin {
    jvmToolchain(17)
    sourceSets.named("test") { kotlin.srcDir("src") }
}

// Исполняем скомпилированный production BleRadio; SDK-модели существуют только в classpath этого JVM-модуля.
val appTests = provider {
    project(":app").tasks.named<org.gradle.api.tasks.testing.Test>("testDebugUnitTest").get()
}
dependencies { testImplementation(files(provider { appTests.get().classpath })) }
tasks.named("compileTestKotlin") { dependsOn(":app:compileDebugUnitTestKotlin") }
tasks.test {
    dependsOn(":app:buildBleCommandTestJni")
    systemProperty("utun.bleCommandTestJni", project(":app").layout.buildDirectory
        .file("ble-command-tests/libble_commands.so").get().asFile.absolutePath)
    testLogging { showStandardStreams = true }
}

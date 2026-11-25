pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "chatgui-android"
include(":app")
include(":audio-probe")
include(":router-tests")
include(":capture-tests")
include(":language-tests")
include(":call-service-tests")
include(":video-tests")
include(":ble-command-tests")
project(":ble-command-tests").projectDir = file("tests/ble_commands")

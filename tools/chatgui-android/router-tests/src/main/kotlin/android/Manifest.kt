package android

/** Только host-тест: имена разрешений без Android runtime. */
object Manifest {
    object permission {
        const val BLUETOOTH_CONNECT = "android.permission.BLUETOOTH_CONNECT"
        const val READ_PHONE_STATE = "android.permission.READ_PHONE_STATE"
        const val RECORD_AUDIO = "android.permission.RECORD_AUDIO"
    }
}

object R { object drawable {
    const val ic_dialog_info = 1; const val ic_btn_speak_now = 2; const val ic_menu_call = 3
    const val ic_menu_camera = 4; const val ic_media_pause = 5
} }

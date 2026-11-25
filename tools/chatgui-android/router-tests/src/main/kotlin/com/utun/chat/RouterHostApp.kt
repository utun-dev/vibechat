package com.utun.chat

import android.content.Context

/** Только host-тест; экземпляр заменяется до создания router. */
object ChatApplication { var instance = Context() }
class MainActivity

/** Только идентификаторы звуков, Android resources в host JVM не загружаются. */
object R {
    object raw {
        const val ringtone = 1; const val call_connecting = 2; const val call_ringing = 3; const val msg_incoming = 4
        const val radio_incoming = 5
    }
    object string {
        const val incoming_calls = 101; const val incoming_call_notification = 102; const val radio_call_incoming = 103
        const val incoming_call = 104; const val decline = 105; const val hang_up = 106; const val accept = 107
        const val media_audio_fallback = 108; const val media_audio_failed = 109
        const val microphone_ptt_retry = 110
        const val radio_call = 111; const val radio_call_declined = 112; const val radio_call_busy = 113
        const val radio_call_cancelled = 114; const val radio_call_no_answer = 115; const val radio_call_audio_error = 116
        const val phone_busy_warning = 117; const val radio_call_already_active = 118; const val radio_call_unavailable = 119
        const val overlay_permission_required = 120; const val microphone_access_required = 121; const val overlay_when_radio_on = 122
        const val overlay_open_app = 123; const val overlay_starting = 124; const val overlay_start_failed = 125
        const val microphone_service_failed = 126; const val overlay_show_failed = 127; const val overlay_active = 128
        const val radio_ptt = 129; const val radio_overlay_description = 130; const val radio_active = 131
        const val radio_notification = 132; const val overlay_stopped = 133
        const val ongoing_call = 134; const val ongoing_call_description = 135; const val call_talking = 136
        const val call_ringing = 137; const val call_setup = 138; const val call_media_stop_failed = 139
        const val connection_none = 140; const val connection_direct = 141; const val connection_reverse = 142
        const val connection_indirect = 143; const val call_incoming_stop_media = 144; const val call_outgoing_stop_media = 145
        const val call_incoming_stop_radio = 146; const val call_outgoing_stop_radio = 147
        const val call_incoming_stop_stream = 148; const val call_outgoing_stop_stream = 149
        const val call_route_save_failed = 150
        const val files_preparing = 201; const val files_transferring = 202; const val files_completed = 203
        const val files_cancelled = 204; const val files_failed = 205; const val files_replace_title = 206; const val files_saving = 207
        const val stream_resources_busy = 301; const val stream_failed = 302; const val stream_mode_restored = 303
        const val stream_title = 304; const val stream_live = 305; const val stream_stop = 306
        const val radio_call_stop_view = 307; const val radio_call_keep_stream = 308
        const val stream_capacity_exceeded = 309
    }
}

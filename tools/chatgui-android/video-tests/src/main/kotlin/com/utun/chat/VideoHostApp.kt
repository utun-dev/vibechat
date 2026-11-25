package com.utun.chat

import android.content.Context
import com.utun.chat.data.VideoHostConfig

object ChatApplication { val instance = VideoHostApplication() }
class VideoHostApplication : Context() {
    val configProvider = VideoHostConfig()
}
object R { object string {
    const val call_video_unavailable = 1; const val call_video_mode_restored = 2
    const val stream_ended = 3; const val stream_connecting = 4; const val stream_live = 5; const val stream_waiting_key = 6
    const val stream_waiting_decoder = 7; const val stream_group_closed = 8; const val chat_service_stopped = 9
    const val stream_source_changed = 10; const val stream_member_removed = 11; const val stream_failed = 12
    const val stream_decoder_unsupported = 13
    const val video_open_failed = 14; const val video_playback_failed = 15
    const val stream_source_offline = 16
    const val stream_capacity_exceeded = 17
} }

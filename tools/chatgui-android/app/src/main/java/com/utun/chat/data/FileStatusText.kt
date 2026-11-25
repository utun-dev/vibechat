package com.utun.chat.data

import com.utun.chat.R

/** ID строки для MF-ошибки 1..14; IO_ERROR=9 и неизвестное значение дают files_error_io.
 * Успех 0 не предназначен для этого API. Перевод выполняется UI при отображении, а не сохраняется в состоянии. */
fun fileStatusResource(status: Int): Int = when (status) {
    1 -> R.string.files_error_access
    2 -> R.string.files_error_missing
    3 -> R.string.files_error_path
    4 -> R.string.files_error_range
    5 -> R.string.files_error_exists
    6 -> R.string.files_error_busy
    7 -> R.string.files_error_stale
    8 -> R.string.files_error_space
    10 -> R.string.files_error_packet
    11 -> R.string.files_error_hash
    12 -> R.string.files_cancelled
    13 -> R.string.files_error_timeout
    14 -> R.string.files_error_nonempty
    else -> R.string.files_error_io
}

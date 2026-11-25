package com.utun.chat.data

import android.os.Handler
import android.os.Looper
import android.view.OrientationEventListener
import androidx.camera.core.UseCase
import androidx.camera.lifecycle.ProcessCameraProvider
import java.io.File
import java.util.concurrent.ConcurrentHashMap

/** Main: файлы захвата живут до OnImageSaved/onError или VideoRecordEvent.Finalize.
 * discard/close запрещают передачу sender, но не удаляют файл под работающим CameraX writer.
 * Владение Recording сохраняется до успешного close либо Finalize; binding снимает только свои use cases.
 * Неудачная очистка остаётся в process registry и повторяется без UI/native core. */
internal class CameraCaptureResources : AutoCloseable {
    companion object { private val retained = ConcurrentHashMap.newKeySet<CameraCaptureResources>() }
    private class Capture(val file: File) {
        var pending = true
        var discarded = false
        var recording: AutoCloseable? = null
    }
    private data class Binding(val provider: ProcessCameraProvider, val cases: Array<out UseCase>)
    private val files = mutableMapOf<File, Capture>()
    private val orientations = mutableMapOf<OrientationEventListener, Boolean>()
    private val completedWriters = mutableSetOf<File>()
    private var binding: Binding? = null
    private var bindingRetiring = false
    private var closed = false
    private val main = Handler(Looper.getMainLooper())
    private var scheduled = false
    private var delayMs = 250L
    private val retry = Runnable { scheduled = false; cleanup() }

    fun own(file: File) {
        check(!closed && file !in files) { "Capture owner closed or file already owned" }
        files[file] = Capture(file)
        LogManager.addLog("DEBUG", "Capture", "Writer owns cache path=$file")
    }

    fun recording(file: File, recording: AutoCloseable) {
        if (file in completedWriters) return
        val capture = checkNotNull(files[file]) { "Recording has no file owner" }
        if (!capture.pending) return // Синхронный Finalize уже подтвердил закрытый Recording.
        capture.recording = recording
        if (capture.discarded || closed) cleanup()
    }

    /** Подтверждение writer; false означает discard/close и запрещает показывать либо отправлять результат. */
    fun completed(file: File): Boolean {
        val capture = files[file] ?: return false
        completedWriters.add(file)
        capture.pending = false; capture.recording = null
        val usable = !closed && !capture.discarded
        LogManager.addLog("INFO", "Capture", "Writer finished path=$file usable=$usable")
        cleanup()
        return usable
    }

    fun discard(file: File?) {
        val capture = files[file] ?: return
        capture.discarded = true
        LogManager.addLog("INFO", "Capture", "Discard requested path=$file writerPending=${capture.pending}")
        cleanup()
    }

    /** Вызывать только после успешного приёма sender: после этого владелец больше не удаляет файл. */
    fun transferred(file: File) {
        val capture = checkNotNull(files[file]) { "Transferred file has no owner" }
        check(!capture.pending && !capture.discarded && !closed) { "Capture cannot be transferred" }
        files.remove(file)
        LogManager.addLog("INFO", "Capture", "Cache transferred to sender path=$file")
    }

    fun ownOrientation(listener: OrientationEventListener) {
        check(!closed) { "Camera owner closed" }
        orientations[listener] = false
    }

    fun releaseOrientation(listener: OrientationEventListener) {
        if (listener in orientations) orientations[listener] = true
        cleanup()
    }

    /** Сохранить use cases до bindToLifecycle: исключение после эффекта получает независимый rollback. */
    fun binding(provider: ProcessCameraProvider, vararg cases: UseCase) {
        check(!closed && binding == null) { "Previous camera binding still owned" }
        binding = Binding(provider, cases)
    }

    fun unbind(): Boolean {
        val current = binding ?: return true
        bindingRetiring = true
        try {
            current.provider.unbind(*current.cases); binding = null; bindingRetiring = false
            LogManager.addLog("INFO", "Capture", "Own camera use cases unbound count=${current.cases.size}")
        } catch (e: Exception) {
            LogManager.addLog("ERROR", "Capture", "Camera unbind failed; owner retained: $e")
            schedule()
        }
        return binding == null
    }

    override fun close() {
        closed = true
        files.values.forEach { it.discarded = true }
        orientations.keys.toList().forEach { orientations[it] = true }
        cleanup()
    }

    private fun cleanup() {
        if (closed || bindingRetiring) unbind()
        for ((listener, retiring) in orientations.toMap()) {
            if (!retiring) continue
            try { listener.disable(); orientations.remove(listener) }
            catch (e: Exception) { LogManager.addLog("ERROR", "Capture", "Orientation disable failed; owner retained: $e") }
        }
        for (capture in files.values.toList()) {
            if (!capture.discarded) continue
            capture.recording?.let { record ->
                try { record.close(); capture.recording = null }
                catch (e: Exception) { LogManager.addLog("ERROR", "Capture", "Recording close failed path=${capture.file}; retained: $e") }
            }
            if (!capture.pending) try {
                check(capture.file.delete() || !capture.file.exists()) { "File deletion rejected" }
                files.remove(capture.file)
                LogManager.addLog("DEBUG", "Capture", "Discarded cache removed path=${capture.file}")
            } catch (e: Exception) { LogManager.addLog("ERROR", "Capture", "Cache cleanup failed path=${capture.file}; retained: $e") }
        }
        schedule()
    }

    private fun schedule() {
        val pending = (bindingRetiring && binding != null) || orientations.values.any { it } || files.values.any { it.discarded }
        if (!pending) {
            main.removeCallbacks(retry); scheduled = false; delayMs = 250; retained.remove(this)
            return
        }
        retained.add(this)
        // Ожидание callback не опрашиваем: owner удерживается до подтверждения writer.
        val retryable = bindingRetiring || orientations.values.any { it } || files.values.any { it.discarded && (!it.pending || it.recording != null) }
        if (!scheduled && retryable) {
            scheduled = main.postDelayed(retry, delayMs)
            if (scheduled) delayMs = (delayMs * 2).coerceAtMost(30_000)
            else LogManager.addLog("ERROR", "Capture", "Cleanup timer rejected; owner retained")
        }
    }
}

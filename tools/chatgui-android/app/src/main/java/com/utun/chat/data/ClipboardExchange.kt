package com.utun.chat.data

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.widget.Toast
import androidx.core.content.FileProvider
import com.utun.chat.ChatApplication
import com.utun.chat.R
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import java.io.ByteArrayOutputStream
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.UUID

/** Только main: явный Send читает clipboard. Codec работает на IO; полный снимок применяется до E2E ACK.
 * Android принимает текст/PNG; file promises для системного проводника реализованы desktop backend ОС. */
object ClipboardExchange {
    private const val EVENT_HEADER_SIZE = 82
    private const val EVENT_PEER_OFFSET = 48
    private const val EVENT_TYPE_OFFSET = 80
    private const val EVENT_SCOPE_OFFSET = 81
    data class MemberOffer(val group: String, val peer: String, val kind: Int, val clip: ClipData)
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private val busy = MutableStateFlow(false)
    val sending = busy.asStateFlow()
    private val receivedState = MutableStateFlow(0 to 0L)
    val received = receivedState.asStateFlow()
    private val memberState = MutableStateFlow<Map<String, MemberOffer>>(emptyMap())
    val memberOffers = memberState.asStateFlow()
    private val memberTickets = mutableMapOf<String, Long>()
    private var memberSequence = 0L
    fun dismissMember(offer: MemberOffer) {
        if (memberState.value[offer.group] === offer) {
            memberState.value = memberState.value - offer.group
            LogManager.addLog("INFO", "Clipboard", "Member notification dismissed group=${offer.group}")
        }
    }
    fun copyMember(offer: MemberOffer) {
        try { clipboard.setPrimaryClip(offer.clip); notify(R.string.clipboard_member_copied) }
        catch (e: Exception) { LogManager.addLog("WARN", "Clipboard", "Cannot copy member clipboard: $e"); notify(R.string.clipboard_failed) }
    }
    fun sender(offer: MemberOffer): String = app.members.list(offer.group).firstOrNull { it.ed25519 == offer.peer }?.name ?: offer.peer
    private var receivedClip: ClipData? = null
    fun copyReceived() {
        try { receivedClip?.let { clipboard.setPrimaryClip(it) } }
        catch (e: Exception) { LogManager.addLog("WARN", "Clipboard", "Cannot restore received clipboard: $e"); notify(R.string.clipboard_failed) }
    }
    private var request = 0L
    private var generation = 0L
    private var coreEpoch = 0L
    private val app get() = ChatApplication.instance
    private val clipboard get() = app.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
    private fun notify(id: Int) { Toast.makeText(app, AppLanguage.text(id), Toast.LENGTH_LONG).show() }
    private fun image(bytes: ByteArray): Bitmap {
        val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        BitmapFactory.decodeByteArray(bytes, 0, bytes.size, bounds)
        check(bounds.outWidth > 0 && bounds.outHeight > 0 && bounds.outWidth.toLong() * bounds.outHeight <= 8L * 1024 * 1024)
        return checkNotNull(BitmapFactory.decodeByteArray(bytes, 0, bytes.size))
    }
    fun send() = sendTo(null, null)
    fun sendTo(group: String?, peer: String?) {
        if (busy.value) return
        try {
            val address = peer?.hexToByteArray()?.also { check(it.size == 32 && it.any { byte -> byte != 0.toByte() }) }
            val groupKey = group?.let { check(MemberFilesProtocol.validAddress(it)); it.hexToByteArray() }
            check((groupKey != null) == (address != null))
            val clip = clipboard.primaryClip
            if (clip == null || clip.itemCount != 1) { notify(R.string.clipboard_unsupported); return }
            val item = clip.getItemAt(0)
            val uri = item.uri
            val text = item.text?.toString()
            if (uri != null && !clip.description.hasMimeType("image/*")) { notify(R.string.clipboard_unsupported); return }
            busy.value = true
            val id = ++request
            val epoch = coreEpoch
            scope.launch {
                try {
                    val type = if (uri != null) 2 else 1
                    val bytes = withContext(Dispatchers.IO) {
                        if (uri == null) checkNotNull(text).toByteArray(Charsets.UTF_8).also { check(it.isNotEmpty() && it.size <= 1024 * 1024) }
                        else {
                            val input = checkNotNull(app.contentResolver.openInputStream(uri)).use { stream ->
                                ByteArrayOutputStream().use { out ->
                                    val block = ByteArray(65536)
                                    while (true) {
                                        val n = stream.read(block); if (n < 0) break
                                        check(out.size() + n <= 32 * 1024 * 1024); out.write(block, 0, n)
                                    }
                                    out.toByteArray()
                                }
                            }
                            val bitmap = image(input)
                            try { ByteArrayOutputStream().use { out ->
                                check(bitmap.compress(Bitmap.CompressFormat.PNG, 100, out))
                                out.toByteArray().also { check(it.size <= 32 * 1024 * 1024) }
                            } } finally { bitmap.recycle() }
                        }
                    }
                    check(epoch == coreEpoch && NativeLib.nativeClipboardSend(id, type, bytes, groupKey, address))
                    LogManager.addLog("INFO", "Clipboard", "Send queued request=$id type=$type bytes=${bytes.size}")
                } catch (e: OutOfMemoryError) {
                    busy.value = false; LogManager.addLog("ERROR", "Clipboard", "Send image memory limit: $e"); notify(R.string.clipboard_failed)
                } catch (e: Exception) { busy.value = false; LogManager.addLog("WARN", "Clipboard", "Send failed: $e"); notify(R.string.clipboard_failed) }
            }
        } catch (e: Exception) { busy.value = false; LogManager.addLog("WARN", "Clipboard", "Cannot read system clipboard: $e"); notify(R.string.clipboard_failed) }
    }
    fun onEvent(type: Int, data: ByteArray?) {
        if (type == 13) { generation++; coreEpoch++; busy.value = false; memberTickets.clear(); memberState.value = emptyMap(); return }
        if (type == 78) {
            if (data == null || data.size != 25) { LogManager.addLog("ERROR", "Clipboard", "Invalid result"); return }
            val buffer = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN)
            if (buffer.long != request) return
            val state = buffer.get().toInt()
            if (state == 0) return
            busy.value = false
            val total = buffer.int; val delivered = buffer.int; val unavailable = buffer.int; val failed = buffer.int
            LogManager.addLog("INFO", "Clipboard", "Result state=$state total=$total delivered=$delivered unavailable=$unavailable failed=$failed")
            if (state == 2) notify(R.string.clipboard_failed)
            else Toast.makeText(app, AppLanguage.text(R.string.clipboard_result, delivered, unavailable, failed), Toast.LENGTH_LONG).show()
            return
        }
        if (type != 77 || data == null || data.size <= EVENT_HEADER_SIZE || data.size > EVENT_HEADER_SIZE + 32 * 1024 * 1024) {
            LogManager.addLog("ERROR", "Clipboard", "Invalid received snapshot"); return
        }
        val header = data.copyOfRange(0, EVENT_HEADER_SIZE); val kind = data[EVENT_TYPE_OFFSET].toInt(); val member = data[EVENT_SCOPE_OFFSET].toInt() == 1
        val groupKey = data.copyOfRange(16, EVENT_PEER_OFFSET)
        val group = groupKey.toHexString()
        val peerBytes = data.copyOfRange(EVENT_PEER_OFFSET, EVENT_TYPE_OFFSET)
        val peer = peerBytes.toHexString()
        if (groupKey.all { it == 0.toByte() } || peerBytes.all { it == 0.toByte() } || data[EVENT_SCOPE_OFFSET].toInt() !in 0..1 || (member && group !in memberState.value && memberState.value.size >= 8)) {
            LogManager.addLog("WARN", "Clipboard", "Invalid scope or pending limit"); NativeLib.nativeClipboardApplied(header, false); return
        }
        val ticket = if (member) (++memberSequence).also { memberTickets[group] = it } else ++generation
        scope.launch {
            var success = false
            try {
                val content = withContext(Dispatchers.IO) {
                    val payload = data.copyOfRange(EVENT_HEADER_SIZE, data.size)
                    when (kind) {
                        1 -> String(payload, Charsets.UTF_8).also { check(payload.size <= 1024 * 1024 && it.toByteArray(Charsets.UTF_8).contentEquals(payload)) }
                        2 -> {
                            check(payload.take(8).toByteArray().contentEquals(byteArrayOf(-119, 80, 78, 71, 13, 10, 26, 10)))
                            image(payload).recycle()
                            val directory = File(app.cacheDir, "clipboard"); check(directory.isDirectory || directory.mkdirs())
                            File(directory, "${UUID.randomUUID()}.png").also { it.writeBytes(payload) }
                        }
                        else -> error("Desktop file promises cannot be applied to Android clipboard")
                    }
                }
                check(ticket == if (member) memberTickets[group] else generation)
                val clip = if (content is File) ClipData.newUri(app.contentResolver, "image", FileProvider.getUriForFile(app,
                    "${app.packageName}.memberfiles", content)) else ClipData.newPlainText("text", content as String)
                if (member) {
                    check(group in memberState.value || memberState.value.size < 8)
                    memberState.value = memberState.value + (group to MemberOffer(group, peer, kind, clip)); app.members.ensure(group)
                } else { clipboard.setPrimaryClip(clip); receivedClip = clip; receivedState.value = kind to ticket }
                success = true
                LogManager.addLog("INFO", "Clipboard", "Received type=$kind bytes=${data.size - EVENT_HEADER_SIZE} member=$member applied=${!member}")
            } catch (e: OutOfMemoryError) { LogManager.addLog("ERROR", "Clipboard", "Receive image memory limit: $e") }
            catch (e: Exception) { LogManager.addLog("WARN", "Clipboard", "Receive failed: $e") }
            if (member && memberTickets[group] == ticket) memberTickets.remove(group)
            if (!NativeLib.nativeClipboardApplied(header, success)) LogManager.addLog("WARN", "Clipboard", "Cannot post applied ACK")
        }
    }
}

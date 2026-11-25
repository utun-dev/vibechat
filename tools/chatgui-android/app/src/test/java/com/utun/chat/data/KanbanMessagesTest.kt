package com.utun.chat.data

import java.io.File
import java.nio.file.Files
import java.util.Base64
import org.json.JSONObject
import org.junit.After
import org.junit.Assert.*
import org.junit.Test

class KanbanMessagesTest {
    private val root = Files.createTempDirectory("kanban-message-").toFile()
    @After fun cleanup() { assertTrue("temporary files remain: $root", root.deleteRecursively()) }
    private val author = "ab".repeat(32)
    private val objectId = "0123456789abcdef".repeat(2)
    private fun row(type: String = "image/png", description: String = "Фото 😀.png", extra: String = "|w=32|h=24"): JSONObject {
        val name = Base64.getUrlEncoder().encodeToString(description.toByteArray())
        val manifest = "*$name|4|4|1|$objectId|${"a".repeat(64)}|${"b".repeat(32)},${"c".repeat(128)}$extra"
        return JSONObject().put("id", "1".repeat(64)).put("author", author).put("created_at", 1000000L)
            .put("text", "Подпись").put("ct", type).put("d", manifest).put("file_path", "media/cache.png")
    }
    private fun parse(row: JSONObject) = parseKanbanMessage(row, root, author, "Удалено")

    @Test fun missingImageKeepsIdentityNameDimensionsCaptionAndDownloadControl() {
        val parsed = parse(row()); val message = parsed.message
        assertEquals(objectId, parsed.objectId); assertEquals("1".repeat(64), message.messageId)
        assertEquals(author, message.author); assertTrue(message.isOutgoing)
        assertEquals("Фото 😀.png", message.text); assertEquals("Подпись", message.caption)
        assertEquals(32, message.imageWidth); assertEquals(24, message.imageHeight)
        assertEquals(4L, message.fileSize); assertEquals("", message.filePath); assertEquals("", message.downloadState)
        assertEquals(1000L, message.ts); assertEquals(0L, message.editedAt)
        assertEquals(2000L, parse(row().put("updated_at", 2000000L)).message.editedAt)
    }
    @Test fun cachedFileMustExistAndMatchSizeBeforePreviewIsShown() {
        val file = File(root, "media/cache.png"); file.parentFile!!.mkdirs(); file.writeBytes(byteArrayOf(1, 2, 3, 4))
        assertEquals(file.canonicalPath, parse(row()).message.filePath)
        assertEquals("downloaded", parse(row()).message.downloadState)
        file.writeBytes(byteArrayOf(1)); assertEquals("", parse(row()).message.filePath)
        file.delete(); assertEquals("", parse(row()).message.filePath)
    }
    @Test fun matroskaRecordingUsesSharedVideoPlayerMetadata() {
        val message = parse(row("video/x-matroska", "recording.mkv", "|dur=1234|w=1280|h=720")).message
        assertEquals("video/x-matroska", message.contentType); assertEquals(1234, message.videoDurationMs)
        assertEquals(1280, message.videoWidth); assertEquals(720, message.videoHeight)
    }
    @Test fun voiceWaveformAndDurationUseDesktopDescriptor() {
        val message = parse(row("audio/opus", "wf=A/;dur=1500;", "")).message
        assertEquals(listOf(0f, 1f), message.waveform); assertEquals(1500, message.voiceDurationMs)
    }
    @Test fun deletedAttachmentCannotRemainDownloadable() {
        val parsed = parse(row().put("deleted", true).put("d", "broken"))
        assertEquals("", parsed.objectId); assertEquals("", parsed.message.filePath)
        assertEquals("text/plain", parsed.message.contentType); assertEquals("Удалено", parsed.message.text)
    }
    @Test fun malformedIdentityAndEscapingIndexPathsAreRejected() {
        for (broken in listOf(row().put("d", "broken"), row().put("file_path", "../outside"), row().put("file_path", "/outside"))) {
            assertThrows(Exception::class.java) { parse(broken) }
        }
    }
}

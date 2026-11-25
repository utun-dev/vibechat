package com.utun.chat.data

import com.utun.chat.R
import org.json.JSONArray
import org.json.JSONObject
import java.nio.ByteBuffer
import java.nio.ByteOrder

const val EVT_NODEINFO_UPDATED = 16
const val EVT_LINK_STATUS_CHANGED = 17
const val EVT_LOCAL_SOCKETS = 19
const val EVT_CONN_LIST = 20
const val EVT_CONN_METRICS = 21

data class Channel(val id: String, val name: String, val lastMsgAt: Long = 0, val lastMsg: String = "",
                   val peersOnline: Int = 0, val lastMsgEncrypted: Boolean = false, val groupType: String = "chat", val groupEd: String = "")

data class DmConversation(val convId: String, val peerName: String = "",
                          val lastTs: Long = 0, val online: Boolean = false, val generation: Long = 1,
                          val mode: Int = 0, val closed: Boolean = false, val peerEd25519: String = "") {
    val displayName: String get() = peerName.ifBlank { peerEd25519 }
}
data class Message(val id: Long, val author: String, val text: String, val ts: Long,
                   val isOutgoing: Boolean = false, val contentType: String = "text/plain",
                   val filePath: String = "", val waveform: List<Float> = emptyList(),
                   val voiceDurationMs: Int = 0, val fileSize: Long = 0,
                   val imageWidth: Int = 0, val imageHeight: Int = 0,
                   val videoWidth: Int = 0, val videoHeight: Int = 0, val videoDurationMs: Int = 0,
                   val downloadState: String = "", /* "", "downloading", "downloaded", "error" */
                   val deliveryStatusRes: Int = 0, val inviteGroupName: String? = null,
                   val messageId: String = "", val feedback: FeedbackState? = null, val replyTo: String = "",
                   val encryptionState: String = "", val version: Long = 1, val editedAt: Long = 0,
                   val caption: String = "", val replyQuote: String = "", val authorAddress: String = "") { /* Подпись вложения отдельно от отображаемого имени файла. */
    val isEncrypted: Boolean get() = encryptionState.isNotEmpty()
    val isLocked: Boolean get() = isEncrypted && encryptionState != "ready"
}

data class ChatMember(val name: String, val online: Boolean = false,
                      val isAdmin: Boolean = false, val isSuper: Boolean = false,
                      val isModer: Boolean = false, val isStorage: Boolean = false,
                      val radioActive: Boolean = false,
                      val radioMuted: Boolean = false,
                      val phoneStatusKnown: Boolean = false, val phoneBusy: Boolean = false,
                      val isVerified: Boolean = false, val isDeleted: Boolean = false,
                      val nodeType: Int = 0,
                      val rtt: Int = 0, val isSelf: Boolean = false,
                      val directUp: Boolean = false, val indirectUp: Boolean = false,
                      val directPresence: Boolean = false, val indirectPresence: Boolean = false,
                      val bgpPresence: Boolean = false, val bgpUp: Boolean = false, val bgpRtt: Int = 0,
                      val bgpNodes: Int = 0, val connecting: Boolean = false,
                      val avatarServer: String = "", val avatarFile: String = "", val storageRole: Boolean = false,
                      val devgroup: String = "", val devgroupName: String = "",
                      val protocolState: Int = 0, val protocolLocal: Int = 0, val protocolRemote: Int = 0, val ed25519: String = "") {
    val canLinkAsMyDevice: Boolean get() = ed25519.length == 64 && !isSelf && !isDeleted && online && bgpUp && devgroup.isEmpty()
    val flags: Int get() = (if (isSuper) 1 else 0) or (if (isAdmin) 2 else 0) or (if (isModer) 4 else 0)
}

data class InviteCandidate(val address: String, val name: String = "", val isAuto: Boolean = false, val isSelf: Boolean = false)

data class MemberDetail(val name: String = "",
                        val online: Boolean = false, val lastSeen: Long = 0,
                        val created: Long = 0, val addresses: List<MemberAddr> = emptyList(),
                        val bestRtt: Int = 0,
                        val directPresence: Boolean = false, val directUp: Boolean = false,
                        val indirectPresence: Boolean = false, val indirectUp: Boolean = false,
                        val bgpPresence: Boolean = false, val bgpUp: Boolean = false, val bgpRtt: Int = 0,
                        val bgpNodes: Int = 0, val channelId: String = "",
                        val inviterId: String? = null, val inviterName: String = "", val connectionOrder: String = "", val ed25519: String = "")

/** Карточка дополняет live-снимок адресами/инвайтером из БД. */
fun MemberDetail.withMember(member: ChatMember?): MemberDetail = copy(
    name = member?.name ?: name, online = member?.online == true, bestRtt = member?.rtt ?: 0,
    directPresence = member?.directPresence == true, directUp = member?.directUp == true,
    indirectPresence = member?.indirectPresence == true, indirectUp = member?.indirectUp == true,
    bgpPresence = member?.bgpPresence == true, bgpUp = member?.bgpUp == true,
    bgpRtt = member?.bgpRtt ?: 0, bgpNodes = member?.bgpNodes ?: 0
)

data class MemberAddr(val family: Int = 0, val protocol: String = "", val address: String = "",
                      val port: Int = 0, val socketId: Int = 0, val addrType: Int = 0, val rtt: Int = 0,
                      val natMapping: Int = 0, val natInbound: Int = 0, val natSource: Int = 0) {
    val natLabelRes: Int get() = natProfileLabelRes(natMapping, natInbound, natSource)
}

// Происхождение адреса не определяет NAT: результат берём из измеренного профиля сокета.
private fun natProfileLabelRes(mapping: Int, inbound: Int, source: Int): Int = when {
    source == 0 -> R.string.socket_nat_unmeasured
    mapping == 1 && inbound == 1 -> R.string.socket_no_nat_open
    mapping == 1 && inbound == 2 -> R.string.socket_no_nat_filtered
    mapping == 1 -> R.string.socket_no_nat
    mapping == 2 && inbound == 1 -> R.string.socket_eim_open
    mapping == 2 && inbound == 2 -> R.string.socket_eim_filtered
    mapping == 2 -> R.string.address_nat_eim_unverified
    mapping == 3 -> R.string.address_nat_dependent
    else -> R.string.address_nat_unknown
}

enum class LinkDirection { UNKNOWN, OUTGOING, INCOMING }

data class LinkStatus(val linkId: Int = 0, val localFamily: Int = 0, val localAddr: String = "", val localPort: Int = 0,
                      val remoteFamily: Int = 0, val remoteAddr: String = "", val remotePort: Int = 0,
                      val linkState: Int = 0, val linkStatus: Int = 0,
                      val direction: LinkDirection = LinkDirection.UNKNOWN, val hasIp: Boolean = false,
                      val socketName: String = "", val protocol: String = "") {
    val stateLabel: String get() = AppLanguage.text(when (linkState) {
        3 -> if (linkStatus == 1) R.string.connection_up else R.string.connection_down
        1, 2 -> R.string.connection_connecting
        else -> R.string.connection_down
    })
}

data class MemberLinks(val connected: Boolean = false, val initialized: Boolean = false,
                       val linksUp: Int = 0, val links: List<LinkStatus> = emptyList())

data class LocalSocketState(
    val sockId: Int = 0, val ifName: String = "", val bindIp: String = "",
    val port: Int = 0, val protocol: String = "", val state: Int = 0,
    val linksUp: Int = 0, val linksDown: Int = 0,
    val natMapping: Int = 0, val natInbound: Int = 0, val natSource: Int = 0,
    val externalIp: String = "", val externalPort: Int = 0
) {
    val natLabelRes: Int get() = natProfileLabelRes(natMapping, natInbound, natSource)
    val stateLabel: String get() = AppLanguage.text(when (state) {
        2 -> R.string.connection_up; 1 -> R.string.connection_connecting; else -> R.string.connection_down
    })
}

data class ConnInfo(
    val nodeId: Long, val name: String, val linksUp: Boolean = false,
    val initialized: Boolean = false, val proxyClient: Boolean = false,
    val proxyServer: Boolean = false, val linkCount: Int = 0, val rtt: Int = 0
) {
    val status: String get() = if (linksUp) "UP" else if (initialized) "init" else "DOWN"
    val rttDisplay: String get() = if (rtt > 0) String.format("%.1f ms", rtt / 10f) else "-"
    val proxyRole: String get() = when { proxyClient && proxyServer -> "CS"; proxyClient -> "C"; proxyServer -> "S"; else -> "" }
}

/* Format: [count:2][entry:46B]* */
fun parseConnList(data: ByteArray): List<ConnInfo> {
    if (data.size < 2) {
        LogManager.addLog("ERROR", "Repository", "Connection snapshot header truncated bytes=${data.size}"); return emptyList()
    }
    val buf = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN)
    val count = buf.getShort().toInt() and 0xFFFF
    if (data.size != 2 + count * 46) {
        LogManager.addLog("ERROR", "Repository", "Connection snapshot truncated count=$count bytes=${data.size}"); return emptyList()
    }
    val list = mutableListOf<ConnInfo>()
    for (i in 0 until count) {
        val nodeId = buf.getLong()
        val flags = buf.get().toInt() and 0xFF
        val linkCount = buf.get().toInt() and 0xFF
        val rtt = buf.getShort().toInt() and 0xFFFF
        buf.getShort()
        val nameBytes = ByteArray(32); buf.get(nameBytes)
        val name = String(nameBytes, Charsets.UTF_8).substringBefore('\u0000').trimEnd()
        if (nodeId == 0L) continue
        list.add(ConnInfo(
            nodeId = nodeId, name = name.ifEmpty { "%016X".format(nodeId) },
            linksUp = (flags and 1) != 0, initialized = (flags and 2) != 0,
            proxyClient = (flags and 4) != 0, proxyServer = (flags and 8) != 0,
            linkCount = linkCount, rtt = rtt
        ))
    }
    return list
}

/* Размер общего снимка из chat_member.h; одинаковый для desktop и Android. */
const val MEMBER_WIRE_SIZE = 313
data class MemberWire(
    val online: Int, val isSelf: Boolean,
    val flags: Int, val rtt: Int, val natType: Int,
    val storage: Boolean, val name: String,
    val avatarServer: String = "", val avatarFile: String = "", val storageRole: Boolean = false,
    val verified: Boolean = false, val nodeType: Int = 0,
    val presence: Int = 0, val up: Int = 0, val intermediaries: Int = 0,
    val devgroup: String = "", val devgroupName: String = "",
    val protocolState: Int = 0, val protocolLocal: Int = 0, val protocolRemote: Int = 0, val ed25519: String = ""
)

fun parseMemberWire(data: ByteArray, offset: Int = 0): MemberWire? {
    if (offset < 0 || offset > data.size - MEMBER_WIRE_SIZE) return null
    val buf = ByteBuffer.wrap(data, offset, MEMBER_WIRE_SIZE).order(ByteOrder.LITTLE_ENDIAN)
    val online = buf.get().toInt() and 0xFF
    val isSelf = buf.get().toInt() != 0
    val flags = buf.get().toInt() and 0xFF
    val rtt = buf.getShort().toInt() and 0xFFFF
    val natType = buf.get().toInt() and 0xFF
    val storage = buf.get().toInt() != 0
    val nameBytes = ByteArray(64); buf.get(nameBytes)
    val name = String(nameBytes, Charsets.UTF_8).substringBefore('\u0000')
    val avatarServer = AvatarProtocol.hash(ByteArray(32).also { buf.get(it) }).takeIf(MemberFilesProtocol::validAddress).orEmpty()
    val avatarBytes = ByteArray(70); buf.get(avatarBytes)
    val avatarFile = String(avatarBytes, Charsets.UTF_8).substringBefore('\u0000')
    val storageRole = buf.get().toInt() != 0
    val verified = buf.get().toInt() != 0
    val nodeType = buf.get().toInt() and 0xff
    val presence = buf.get().toInt() and 0xff
    val up = buf.get().toInt() and 0xff
    val intermediaries = buf.short.toInt() and 0xffff
    val groupBytes = ByteArray(32); buf.get(groupBytes)
    val devgroup = if (groupBytes.all { it == 0.toByte() }) "" else groupBytes.joinToString("") { "%02x".format(it.toInt() and 255) }
    val groupName = ByteArray(64); buf.get(groupName)
    val devgroupName = String(groupName, Charsets.UTF_8).substringBefore('\u0000')
    val protocolState = buf.get().toInt() and 0xff
    val protocolLocal = buf.short.toInt() and 0xffff
    val protocolRemote = buf.short.toInt() and 0xffff
    val address = ByteArray(32).also { buf.get(it) }
    if (address.all { it == 0.toByte() }) return null
    val ed25519 = address.toHexString()
    return MemberWire(online, isSelf, flags, rtt, natType, storage, name, avatarServer, avatarFile,
        storageRole, verified, nodeType, presence, up, intermediaries, devgroup, devgroupName, protocolState, protocolLocal, protocolRemote, ed25519)
}

/* Полный снимок заменяет все свойства; connecting — локальное состояние запроса GUI. */
fun MemberWire.toChatMember(previous: ChatMember = ChatMember(name)): ChatMember = previous.copy(
    protocolState = protocolState, protocolLocal = protocolLocal, protocolRemote = protocolRemote, ed25519 = ed25519,
    name = name, online = online != 0, devgroup = devgroup, devgroupName = devgroupName,
    isAdmin = (flags and 2) != 0, isSuper = (flags and 1) != 0,
    isModer = (flags and 4) != 0, isStorage = storage,
    isDeleted = (flags and 8) != 0, radioActive = (flags and 0x10) != 0, radioMuted = (flags and 0x20) != 0,
    phoneStatusKnown = online != 0 && (flags and 0x40) != 0,
    phoneBusy = online != 0 && (flags and 0xC0) == 0xC0,
    rtt = if (rtt == 0xFFFF) 0 else rtt, isSelf = isSelf,
    avatarServer = avatarServer, avatarFile = avatarFile, storageRole = storageRole,
    isVerified = verified, nodeType = nodeType,
    directPresence = presence and 1 != 0, directUp = up and 1 != 0,
    indirectPresence = presence and 2 != 0, indirectUp = up and 2 != 0,
    bgpPresence = presence and 4 != 0, bgpUp = up and 4 != 0,
    bgpRtt = if (rtt == 0xffff) 0 else rtt, bgpNodes = intermediaries
)

fun cleanSocketName(raw: String): String =
    raw.removePrefix("as_").replace(Regex("_v[46]_(tcp|udp)$"), " $1")

/** Stateless JNI/JSON adapter. Снимки применяются целиком: null означает отказ, пустой список — корректный пустой снимок. */
class ChatRepository(private val reportFailure: (String) -> Unit = { LogManager.addLog("ERROR", "Repository", it) }) {
    fun getChannels(): List<Channel>? {
        val list = mutableListOf<Channel>()
        try {
            val arr = JSONArray(NativeLib.getChannels())
            for (i in 0 until arr.length()) {
                val obj = arr.getJSONObject(i)
                list.add(Channel(
                    id = obj.getString("id"),
                    name = obj.optString("name", obj.getString("id")),
                    lastMsgAt = obj.optLong("lastMsgAt", 0),
                    lastMsg = obj.optString("lastMsg", ""),
                    peersOnline = obj.optInt("peersOnline", 0),
                    lastMsgEncrypted = obj.optBoolean("lastMsgEncrypted"), groupType = obj.optString("groupType", ""), groupEd = obj.optString("groupEd")
                ))
            }
        } catch (e: Exception) {
            reportFailure("Channel snapshot failed: $e"); return null
        }
        return list
    }

    fun getPeersOnline(channelId: String): Int {
        try {
            val arr = JSONArray(NativeLib.getChannels())
            for (i in 0 until arr.length()) {
                val obj = arr.getJSONObject(i)
                if (obj.getString("id") == channelId) return obj.optInt("peersOnline", 0)
            }
        } catch (e: Exception) { reportFailure("Peer count failed channel=$channelId: $e") }
        return 0
    }

    fun getMessages(channelId: String, limit: Int = 50, anchor: String = "", exact: Boolean = false): List<Message>? {
        val list = mutableListOf<Message>()
        try {
            val arr = JSONArray(NativeLib.getMessages(channelId, limit, anchor, exact))
            for (i in 0 until arr.length()) {
                val obj = arr.getJSONObject(i)
                val text = obj.optString("text", "")
                val contentType = obj.optString("contentType", "text/plain")
                val filePath = obj.optString("filePath", "")
                val encryptionState = obj.optString("encryptionState")
                val encrypted = encryptionState.isNotEmpty()

                var displayText = text
                var fileSize = obj.optLong("fileSize")

                var downloadState = ""
                val localAttrs = obj.optString("localAttrs", "")
                if (filePath.isNotEmpty()) downloadState = "downloaded"
                else if (!encrypted && localAttrs.contains("\"st\":\"fl\"")) downloadState = "downloaded"
                else if (localAttrs.contains("\"st\":\"er\"")) downloadState = "error"

                var waveform = obj.optString("waveform").map {
                    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/".indexOf(it).coerceAtLeast(0) / 63f
                }
                var voiceDurationMs = obj.optInt("durationMs")
                if (!encrypted && contentType == "audio/opus" && text.startsWith("wf=")) {
                    try {
                        val wfEnd = text.indexOf(";dur=")
                        if (wfEnd > 0) {
                            val wfRaw = text.substring(3, wfEnd)
                            waveform = if (wfRaw.length >= 50) {
                                wfRaw.map { c ->
                                    val idx = when (c) {
                                        in 'A'..'Z' -> c - 'A'
                                        in 'a'..'z' -> c - 'a' + 26
                                        in '0'..'9' -> c - '0' + 52
                                        '+' -> 62; '/' -> 63; else -> 0
                                    }
                                    (idx.toFloat() / 63f).coerceIn(0f, 1f)
                                }
                            } else {
                                val parts = wfRaw.split(",")
                                if (parts.size >= 2) {
                                    parts.map { s ->
                                        val v = s.toFloatOrNull() ?: 0f
                                        val db = if (v > 0.00001f) 20 * kotlin.math.log10(v.toDouble()).toFloat() else -100f
                                        ((db + 60f) / 60f).coerceIn(0f, 1f)
                                    }
                                } else emptyList()
                            }
                            val durPart = text.substring(wfEnd + 5).takeWhile { it != ';' }
                            voiceDurationMs = durPart.toIntOrNull() ?: 0
                        }
                    } catch (e: Exception) { LogManager.addLog("WARN", "Repository", "Voice metadata failed channel=$channelId: $e") }
                }

                if (!encrypted && (contentType == "application/octet-stream" || contentType.startsWith("video/") || contentType.startsWith("image/"))) {
                    try {
                        val pipeIdx = text.indexOf('|')
                        if (pipeIdx > 0) {
                            displayText = text.substring(0, pipeIdx)
                            val metaParts = text.substring(pipeIdx + 1).split("|")
                            if (metaParts.isNotEmpty())
                                fileSize = metaParts[0].toLongOrNull() ?: 0L
                        }
                    } catch (e: Exception) { LogManager.addLog("WARN", "Repository", "File metadata failed channel=$channelId: $e") }
                }

                var imageWidth = obj.optInt("width")
                var imageHeight = obj.optInt("height")
                if (!encrypted && contentType.startsWith("image/")) {
                    try {
                        for (part in text.substringAfter('|').split("|")) {
                            when {
                                part.startsWith("w=") -> imageWidth = part.removePrefix("w=").toIntOrNull() ?: 0
                                part.startsWith("h=") -> imageHeight = part.removePrefix("h=").toIntOrNull() ?: 0
                            }
                        }
                    } catch (e: Exception) { LogManager.addLog("WARN", "Repository", "Image metadata failed channel=$channelId: $e") }
                }

                var videoWidth = obj.optInt("width")
                var videoHeight = obj.optInt("height")
                var videoDurationMs = obj.optInt("durationMs")
                if (!encrypted && contentType.startsWith("video/")) {
                    try {
                        for (part in text.substringAfter('|').split("|")) {
                            when {
                                part.startsWith("w=") -> videoWidth = part.removePrefix("w=").toIntOrNull() ?: 0
                                part.startsWith("h=") -> videoHeight = part.removePrefix("h=").toIntOrNull() ?: 0
                                part.startsWith("dur=") -> videoDurationMs = part.removePrefix("dur=").toIntOrNull() ?: 0
                            }
                        }
                    } catch (e: Exception) { LogManager.addLog("WARN", "Repository", "Video metadata failed channel=$channelId: $e") }
                }

                list.add(Message(
                    id = obj.getLong("id"),
                    messageId = obj.optString("messageId"),
                    version = obj.optLong("ver", 1), editedAt = obj.optLong("editedAt"),
                    replyTo = obj.optString("replyTo").takeIf(::validMessageId).orEmpty(),
                    replyQuote = (obj.opt("replyQuote") as? String).orEmpty().takeIf {
                        validMessageId(obj.optString("replyTo"))
                    }.orEmpty(),
                    author = obj.getString("author"),
                    authorAddress = obj.optString("authorAddress"),
                    text = displayText,
                    ts = obj.getLong("ts"),
                    isOutgoing = obj.optBoolean("isOutgoing", false),
                    contentType = contentType,
                    filePath = filePath,
                    waveform = waveform,
                    voiceDurationMs = voiceDurationMs,
                    fileSize = fileSize,
                    imageWidth = imageWidth,
                    imageHeight = imageHeight,
                    videoWidth = videoWidth,
                    videoHeight = videoHeight,
                    videoDurationMs = videoDurationMs,
                    downloadState = downloadState,
                    encryptionState = encryptionState,
                    deliveryStatusRes = if (encryptionState == "ready") R.string.attachment_encrypted else 0
                ))
            }
        } catch (e: Exception) {
            reportFailure("Message snapshot failed channel=$channelId anchor=$anchor: $e"); return null
        }
        return list
    }

    fun sendMessage(channelId: String, text: String) = NativeLib.sendMessage(channelId, text)
    fun sendAttachment(channelId: String, filePath: String) = NativeLib.attachmentSend(channelId, filePath)
    fun sendVideo(channelId: String, filePath: String, durationMs: Int, width: Int, height: Int) =
        NativeLib.videoSend(channelId, filePath, durationMs, width, height)
    fun createChannel(name: String, channelId: String, groupType: String = "chat") = NativeLib.createChannel(name, channelId, groupType)
    fun connectNode(address: String, port: Int, pubkeyHex: String) = NativeLib.connectNode(address, port, pubkeyHex)
    fun joinChannel(link: String, request: Long): Int = NativeLib.joinChannel(link, request)
    fun connectChannel(channelId: String) = NativeLib.connectChannel(channelId)
    fun connectMemberNode(channelId: String, ed25519: String) = NativeLib.connectMemberNode(channelId, ed25519)
    fun isReady() = NativeLib.isReady()

    fun getMembers(channelId: String): List<ChatMember> {
        val list = mutableListOf<ChatMember>()
        try {
            val arr = JSONArray(NativeLib.getChannelMembers(channelId))
            for (i in 0 until arr.length()) {
                val obj = arr.getJSONObject(i)
                val userinfo = try { JSONObject(obj.optString("ui").ifEmpty { "{}" }) }
                catch (e: Exception) {
                    LogManager.addLog("WARN", "Avatar", "Invalid userinfo group=$channelId owner=${obj.optString("ed")}: ${e.message}")
                    JSONObject()
                }
                val avatarFile = userinfo.optString("avatar_file")
                val avatarServer = userinfo.optString("avatar_server").takeIf(MemberFilesProtocol::validAddress).orEmpty()
                val validAvatar = avatarServer.isNotEmpty() && AvatarProtocol.validFile(avatarFile)
                list.add(ChatMember(
                    name = obj.optString("n", ""),
                    online = obj.optInt("on", 0) != 0,
                    isAdmin = obj.optInt("ad", 0) != 0,
                    isSuper = obj.optInt("sp", 0) != 0,
                    isModer = obj.optInt("md", 0) != 0,
                    isStorage = obj.optInt("st", 0) != 0,
                    radioMuted = obj.optInt("rm", 0) != 0,
                    isVerified = obj.optInt("vd", 0) != 0,
                    isDeleted = obj.optInt("dl", 0) != 0,
                    nodeType = obj.optInt("tp", 0),
                    rtt = obj.optInt("rt", 0),
                    isSelf = obj.optInt("is", 0) != 0, ed25519 = obj.optString("ed"),
                    avatarServer = if (validAvatar) avatarServer else "",
                    avatarFile = if (validAvatar) avatarFile else "",
                    storageRole = obj.optInt("sr", 0) != 0
                ))
            }
        } catch (e: Exception) {
            reportFailure("Member snapshot failed channel=$channelId: $e"); return emptyList()
        }
        return list
    }

    fun getMemberDetail(channelId: String, peer: String): MemberDetail? {
        try {
            val obj = JSONObject(NativeLib.getMemberDetail(channelId, peer))
            if (obj.length() == 0) return null
            val addrs = mutableListOf<MemberAddr>()
            val arr = obj.optJSONArray("adr")
            if (arr != null) {
                for (i in 0 until arr.length()) {
                    val a = arr.getJSONObject(i)
                    addrs.add(MemberAddr(
                        family = a.optInt("fm", 0),
                        protocol = a.optString("pt", ""),
                        address = a.optString("ad", ""),
                        port = a.optInt("po", 0),
                        socketId = a.optInt("si", 0),
                        addrType = a.optInt("at", 0),
                        rtt = a.optInt("rt", 0),
                        natMapping = a.getInt("nmapping"),
                        natInbound = a.getInt("ninbound"),
                        natSource = a.getInt("nsource")
                    ))
                }
            }
            return MemberDetail(
                name = obj.optString("n", ""),
                online = obj.optInt("on", 0) != 0,
                lastSeen = obj.optLong("ls", 0),
                created = obj.optLong("cr", 0),
                addresses = addrs, channelId = channelId, connectionOrder = obj.optString("connection_order", ""),
                inviterId = if (obj.isNull("inviter_id")) null else obj.getString("inviter_id"),
                inviterName = obj.optString("inviter_name", ""), ed25519 = obj.optString("ed")
            )
        } catch (e: Exception) { reportFailure("Member detail failed channel=$channelId peer=$peer: $e") }
        return null
    }

    fun setMemberNick(channelId: String, ed: String, nick: String) =
        NativeLib.setMemberNick(channelId, ed, nick)

    fun isChannelOwner(channelId: String): Boolean = NativeLib.isChannelOwner(channelId)

    fun setMemberFlags(channelId: String, ed: String, admin: Boolean, supernode: Boolean, moder: Boolean, storage: Boolean,
                       verified: Boolean, deleted: Boolean) =
        NativeLib.setMemberFlags(channelId, ed, admin, supernode, moder, storage, verified, deleted)

    fun transferAdmin(channelId: String, ed25519: String) = NativeLib.transferAdmin(channelId, ed25519)

    fun deleteChannel(channelId: String) = NativeLib.deleteChannel(channelId)

    fun getMemberLinks(peer: String): MemberLinks? {
        return try { parseMemberLinks(NativeLib.getMemberLinks(peer)) }
        catch (e: Exception) { reportFailure("Member links unavailable peer=$peer: $e"); null }
    }

    /** null — ошибка снимка, {cn:false} либо {} — подтверждённое отсутствие соединения. */
    internal fun parseMemberLinks(json: String): MemberLinks? {
        try {
            val obj = JSONObject(json)
            if (!obj.optBoolean("cn", false)) return MemberLinks()
            val links = mutableListOf<LinkStatus>()
            val arr = obj.optJSONArray("lk")
            if (arr != null) {
                for (i in 0 until arr.length()) {
                    val a = arr.getJSONObject(i)
                    val direction = a.getInt("di")
                    require(direction in LinkDirection.entries.indices) { "Invalid link direction=$direction" }
                    links.add(LinkStatus(
                        linkId = a.optInt("id", 0),
                        localFamily = a.optInt("lf", 0),
                        localAddr = a.optString("la", ""),
                        localPort = a.optInt("lp", 0),
                        remoteFamily = a.optInt("rf", 0),
                        remoteAddr = a.optString("ra", ""),
                        remotePort = a.optInt("rp", 0),
                        linkState = a.optInt("st", 0),
                        linkStatus = a.optInt("ls", 0),
                        direction = LinkDirection.entries[direction],
                        hasIp = a.getBoolean("ip"),
                        socketName = a.optString("sn", ""),
                        protocol = a.optString("pt", "")
                    ))
                }
            }
            return MemberLinks(
                connected = obj.optBoolean("cn", false),
                initialized = obj.optBoolean("in", false),
                linksUp = obj.optInt("lu", 0),
                links = links
            )
        } catch (e: Exception) { reportFailure("Member links snapshot failed: $e") }
        return null
    }

    fun parseLocalSockets(json: String): List<LocalSocketState>? {
        try {
            val arr = JSONArray(json)
            val list = mutableListOf<LocalSocketState>()
            for (i in 0 until arr.length()) {
                val obj = arr.getJSONObject(i)
                val socketName = obj.optString("nm", "")
                val cleanName = socketName.removePrefix("as_").replace(Regex("_v[46]_(tcp|udp)$"), "")
                list.add(LocalSocketState(
                    sockId = obj.optInt("si", 0),
                    ifName = cleanName,
                    bindIp = obj.optString("ad", ""),
                    port = obj.optInt("po", 0),
                    protocol = obj.optString("pt", ""),
                    state = obj.optInt("st", 0),
                    linksUp = obj.optInt("lu", 0),
                    linksDown = obj.optInt("ld", 0),
                    natMapping = obj.getInt("nmapping"),
                    natInbound = obj.getInt("ninbound"),
                    natSource = obj.getInt("nsource"),
                    externalIp = obj.getString("external"),
                    externalPort = obj.getInt("externalPort")
                ))
            }
            list.sortWith(compareBy<LocalSocketState> { it.ifName }.thenBy { it.bindIp }.thenBy { it.protocol }.thenBy { it.port })
            return list
        } catch (e: Exception) { reportFailure("Socket snapshot parse failed: $e") }
        return null
    }

    fun close() {}

    /* ── DM (личные сообщения) ── */

    /** ID зафиксированной ядром беседы, null — отказ. Режим не выводится из node ID. */
    fun dmStart(peer: String, sourceChId: String, peerName: String, groups: Boolean = false): String? =
        NativeLib.dmStart(peer, sourceChId, peerName, groups)

    fun dmSend(convId: String, text: String, generation: Long, requestId: Long): Boolean =
        NativeLib.dmSend(convId, text, generation, requestId)

    fun parseDmConversations(json: String): List<DmConversation>? {
        val list = mutableListOf<DmConversation>()
        try {
            val arr = JSONArray(json)
            for (i in 0 until arr.length()) {
                val obj = arr.getJSONObject(i)
                list.add(DmConversation(
                    convId = obj.getString("conv_id"),
                    peerName = obj.optString("peer_name", ""),
                    lastTs = obj.optLong("last_ts", 0), generation = obj.getLong("generation"),
                    mode = obj.optInt("mode"), closed = obj.optBoolean("closed"),
                    peerEd25519 = obj.optString("peer_ed25519")
                ))
            }
        } catch (e: Exception) {
            reportFailure("PM conversation snapshot failed: $e"); return null
        }
        return list
    }

    fun parseDmMessages(json: String, peerName: String = ""): List<Message>? {
        val list = mutableListOf<Message>()
        try {
            val arr = JSONArray(json)
            for (i in 0 until arr.length()) {
                val obj = arr.getJSONObject(i)
                val dir = obj.optInt("dir", 0)
                val outgoing = dir == 1
                val file = obj.optString("ct") == "file"
                val status = obj.optString("media_status")
                val path = obj.optString("media_path")
                val delivery = if (file) when {
                    status == "delivered" -> R.string.delivery_delivered
                    status == "expired" -> R.string.delivery_expired
                    path.isNotEmpty() -> R.string.delivery_received
                    status == "downloading" -> R.string.delivery_loading
                    status == "unavailable" -> R.string.dm_file_unavailable
                    else -> R.string.dm_download
                } else if (obj.optBoolean("source")) {
                    if (obj.optBoolean("pending")) R.string.delivery_pending else R.string.delivery_delivered
                } else 0
                val address = obj.optString("author_ed25519")
                val author = peerName.ifBlank { address }
                list.add(Message(
                    id = obj.optLong("seq", 0),
                    messageId = "$address:${obj.optLong("seq")}",
                    authorAddress = address,
                    author = author,
                    text = obj.optString("data", ""),
                    ts = obj.optLong("ts", 0),
                    isOutgoing = outgoing,
                    contentType = obj.optString("content_type", "text/plain"),
                    inviteGroupName = if (obj.optString("ct") == "invite") obj.getJSONObject("tags").getString("group_name") else null,
                    filePath = path,
                    fileSize = obj.optLong("media_size"),
                    waveform = obj.optString("waveform").map { "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/".indexOf(it).coerceAtLeast(0) / 63f },
                    voiceDurationMs = obj.optInt("duration_ms"),
                    videoDurationMs = obj.optInt("duration_ms"),
                    videoWidth = obj.optInt("width"),
                    videoHeight = obj.optInt("height"),
                    imageWidth = obj.optInt("width"),
                    imageHeight = obj.optInt("height"),
                    downloadState = if (!file) "" else if (path.isNotEmpty()) "downloaded" else when (status) {
                        "downloading" -> "downloading"
                        "expired", "unavailable" -> "error"
                        else -> ""
                    },
                    deliveryStatusRes = delivery
                ))
            }
        } catch (e: Exception) {
            reportFailure("PM message snapshot failed: $e"); return null
        }
        return list
    }
}

#include "db_manager.h"
#include "../src/debug_ui.h"
#include "../../lib/debug_config.h"
#include <cstring>
#include <exception>
#include <new>
#include <QSet>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCoreApplication>
#include <QPointer>
#include "chat/chat_poll.h"
#include "chat/group_avatar.h"
#include "dm/dm_identity.h"
#include "routing_layer/devgroup.h"
#include "db_async.h"

/* ── helpers ── */

static QByteArray parseJsonField(const QByteArray& json, const char* key) {
    return QJsonDocument::fromJson(json).object().value(QString::fromLatin1(key)).toString().toUtf8();
}

QString DbReadView::sanitizeTableName(const QString& chId) {
    QString s;
    s.reserve(chId.size());
    for (int i = 0; i < chId.size(); i++) {
        QChar c = chId[i];
        if (c.isLetterOrNumber() || c == '_') s += c;
        else s += '_';
    }
    return s;
}

QString DbReadView::msgTableName(const QString& chId) const {
    return QStringLiteral("msg_") + sanitizeTableName(chId);
}
QString DbReadView::peersTableName(const QString& chId) const {
    return QStringLiteral("peers_") + sanitizeTableName(chId);
}

/* ── lifecycle ── */

DbManager::DbManager(QObject* parent)
    : QObject(parent)
{
}

DbManager::~DbManager() {
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "DbManager: destroyed");
}

sqlite3_stmt* DbReadView::prepareOrNull(const char* sql) const {
    if (!m_db) { recordError(SQLITE_MISUSE); GUI_WARN("read skipped: database unavailable sql=%s", sql); return nullptr; }
    sqlite3_stmt* stmt = nullptr;
    const int rc = sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        recordError(rc);
        const char* err = sqlite3_errmsg(m_db);
        if (strstr(err, "no such table")) GUI_DEBUG("optional table unavailable: %s — %s", sql, err);
        else GUI_ERROR("prepare failed: %s — %s", sql, err);
        sqlite3_finalize(stmt);
        return nullptr;
    }
    if (!sqlite3_stmt_readonly(stmt)) {
        recordError(SQLITE_READONLY); GUI_ERROR("GUI write rejected: %s", sql); sqlite3_finalize(stmt); return nullptr;
    }
    return stmt;
}

/* Ошибки bind/step/finalize не превращаются в частичный успешный снимок. */
bool DbReadView::finishRead(sqlite3_stmt* statement, int result) const {
    sqlite3* db = sqlite3_db_handle(statement);
    const QByteArray sql = sqlite3_sql(statement);
    const bool ok = result == SQLITE_ROW || result == SQLITE_DONE;
    if (!ok) { recordError(result); GUI_ERROR("read failed: rc=%d sql=%s error=%s", result, sql.constData(), sqlite3_errmsg(db)); }
    const int final = sqlite3_finalize(statement);
    if (final != SQLITE_OK) {
        recordError(final); GUI_ERROR("read finalize failed: rc=%d sql=%s error=%s", final, sql.constData(), sqlite3_errmsg(db));
    }
    return ok && final == SQLITE_OK;
}

void DbReadView::recordError(int code) const {
    if (m_sqliteCode == SQLITE_OK && code != SQLITE_OK) m_sqliteCode = code;
}

QString DbReadView::colText(sqlite3_stmt* stmt, int col) {
    return QString::fromUtf8(
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, col)));
}

QByteArray DbReadView::colBlob(sqlite3_stmt* stmt, int col) {
    int len = sqlite3_column_bytes(stmt, col);
    if (len <= 0) return {};
    return QByteArray(static_cast<const char*>(sqlite3_column_blob(stmt, col)), len);
}

/* ── channels ── */

QString DbReadView::groupType(const QString& channelId) const {
    sqlite3_stmt* st = prepareOrNull("SELECT group_type FROM channels WHERE channel_id=?");
    if (!st) return {};
    const QByteArray channel = channelId.toUtf8(); QString result;
    int rc = sqlite3_bind_text(st, 1, channel.constData(), channel.size(), SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) result = QString::fromUtf8(reinterpret_cast<const char*>(sqlite3_column_text(st, 0)));
    return finishRead(st, rc) ? result : QString();
}

/* Все вложенные SELECT выполняются в READ-транзакции вызывающего worker. */
NavigationSnapshot DbReadView::getNavigation() const {
    NavigationSnapshot snapshot;
    snapshot.channels = getChannels(getUiStateInt("channel_sort", 0) == 1 ? ChannelSortMode::Recent : ChannelSortMode::Alphabetical);
    for (const auto& channel : snapshot.channels) {
        if (sqlError() != SQLITE_OK) return {};
        snapshot.previews.insert(channel.channelId, getChannelLastMessage(channel.channelId));
        if (channel.avatarVersion) snapshot.avatars.insert(channel.channelId, getChannelAvatar(channel.channelId));
    }
    if (sqlError() == SQLITE_OK) snapshot.conversations = getDmConversations();
    return sqlError() == SQLITE_OK ? snapshot : NavigationSnapshot();
}

QList<ChannelRow> DbReadView::getChannels(ChannelSortMode sort) const {
    QList<ChannelRow> list;
    const char* orderBy = (sort == ChannelSortMode::Recent)
        ? " ORDER BY last_msg_at DESC, name COLLATE NOCASE ASC"
        : " ORDER BY name COLLATE NOCASE ASC";
    char sql[512]; snprintf(sql, sizeof(sql),
        "SELECT channel_id, name, owner_node_id,"
        " x25519_pubkey, x25519_privkey, ed25519_pubkey, ed25519_privkey, signature,"
        " last_msg_at, created_at,avatar_version,avatar_hash"
        " FROM channels%s", orderBy);
    sqlite3_stmt* stmt = prepareOrNull(sql);
    if (!stmt) return list;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        ChannelRow r;
        r.channelId       = colText(stmt, 0);
        r.name            = colText(stmt, 1);
        r.ownerNodeId     = colBlob(stmt, 2);
        r.x25519_pubkey   = colBlob(stmt, 3);
        r.x25519_privkey  = colBlob(stmt, 4);
        r.ed25519_pubkey  = colBlob(stmt, 5);
        r.ed25519_privkey = colBlob(stmt, 6);
        r.signature       = colBlob(stmt, 7);
        r.lastMsgAt       = sqlite3_column_int64(stmt, 8);
        r.createdAt       = sqlite3_column_int64(stmt, 9);
        r.avatarVersion   = (quint64)sqlite3_column_int64(stmt, 10);
        r.avatarHash      = colBlob(stmt, 11);
        list.append(r);
    }
    if (!finishRead(stmt, rc)) return {};
    for (auto& ch : list) {
        ch.groupType = groupType(ch.channelId);
        ch.lastReadMsgId = getUiStateInt64(
            QStringLiteral("ch_%1_last_read").arg(ch.channelId), 0);
        ch.lastPosMsgId = getUiStateInt64(
            QStringLiteral("ch_%1_last_pos").arg(ch.channelId), 0);
        ch.unreadCount = getUnreadCount(ch.channelId, ch.lastReadMsgId);
    }
    return list;
}

QByteArray DbReadView::getChannelAvatar(const QString& channelId, bool* ok) const {
    const auto snapshot = getChannelAvatarSnapshot(channelId);
    if (ok) *ok = snapshot.found;
    return snapshot.bytes;
}

/* Версия/hash и JPEG принадлежат одной строке; SQLite-указатели закрываются до возврата. */
ChannelAvatarSnapshot DbReadView::getChannelAvatarSnapshot(const QString& channelId) const {
    ChannelAvatarSnapshot snapshot;
    if (!m_db) { recordError(SQLITE_MISUSE); GUI_WARN("group avatar: database unavailable channel=%s", qPrintable(channelId)); return {}; }
    sqlite3_stmt* st = nullptr;
    const char* stage = "prepare";
    int rc = sqlite3_prepare_v2(m_db, "SELECT avatar_data,avatar_version,avatar_hash FROM channels WHERE channel_id=?", -1, &st, nullptr);
    const QByteArray id = channelId.toLatin1();
    if (rc == SQLITE_OK) { stage = "bind"; rc = sqlite3_bind_text(st, 1, id.constData(), id.size(), SQLITE_TRANSIENT); }
    if (rc == SQLITE_OK) { stage = "step"; rc = sqlite3_step(st); }
    const char* detail = nullptr;
    int type = -1, length = 0;
    if (rc == SQLITE_ROW) {
        stage = "blob"; type = sqlite3_column_type(st, 0); length = sqlite3_column_bytes(st, 0);
        snapshot.version = quint64(sqlite3_column_int64(st, 1)); snapshot.hash = colBlob(st, 2);
        if (sqlite3_column_type(st, 1) != SQLITE_INTEGER || sqlite3_column_int64(st, 1) < 0 ||
            (snapshot.version ? snapshot.hash.size() != 32 : !snapshot.hash.isEmpty())) {
            rc = SQLITE_CORRUPT; detail = "invalid avatar version/hash";
        } else if (type == SQLITE_NULL && !snapshot.version) snapshot.bytes.clear(); // Аватар ещё не публиковался
        else if (type != SQLITE_BLOB) { rc = SQLITE_CORRUPT; detail = "expected a BLOB or NULL for an unpublished avatar"; }
        else if (length > GROUP_AVATAR_MAX) { rc = SQLITE_TOOBIG; detail = "avatar exceeds the protocol size limit"; }
        else {
            const char* blob = static_cast<const char*>(sqlite3_column_blob(st, 0));
            if (length && !blob) { rc = SQLITE_NOMEM; detail = "SQLite returned no BLOB data"; }
            else snapshot.bytes = QByteArray(blob, length);
        }
    }
    const int final = sqlite3_finalize(st);
    if ((rc != SQLITE_ROW && rc != SQLITE_DONE) || final != SQLITE_OK) {
        recordError(rc == SQLITE_ROW || rc == SQLITE_DONE ? final : rc);
        GUI_ERROR("group avatar: read channel=%s stage=%s rc=%d finalize=%d type=%d bytes=%d limit=%d: %s",
                  qPrintable(channelId), stage, rc, final, type, length, GROUP_AVATAR_MAX, detail ? detail : sqlite3_errmsg(m_db)); return {};
    }
    if (rc == SQLITE_DONE) {
        GUI_DEBUG("group avatar: channel disappeared during read channel=%s", qPrintable(channelId)); return {};
    }
    snapshot.found = true;
    return snapshot;
}

bool DbReadView::clipboardAllowed(const QString& channel) const {
    sqlite3_stmt* st = prepareOrNull("SELECT json_type(description_json,'$.clipboard_members')='true' FROM channels WHERE channel_id=?");
    if (!st) return false;
    const auto id = channel.toLatin1(); int rc = sqlite3_bind_text(st, 1, id.constData(), id.size(), SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    const bool allowed = rc == SQLITE_ROW && sqlite3_column_int(st, 0) == 1;
    return finishRead(st, rc) && allowed;
}

bool DbReadView::isLocalChannelMember(const QString& chId) const {
    const QByteArray sql = QStringLiteral("SELECT 1 FROM \"%1\" WHERE record_state=0 AND ed25519_pubkey="
                                          "(SELECT ed25519_pubkey FROM local_identity WHERE id=1)").arg(peersTableName(chId)).toUtf8();
    sqlite3_stmt* statement = prepareOrNull(sql.constData());
    if (!statement) return false;
    const int result = sqlite3_step(statement);
    return finishRead(statement, result) && result == SQLITE_ROW;
}

/* Формирует человекочитаемое тело превью последнего сообщения по content_type и "d"-полю. */
static QString mediaPreviewBody(const QString& ct, const QByteArray& d) {
    bool isText = ct.isEmpty() || ct == QStringLiteral("text/plain") ||
                  ct == QStringLiteral("voice_transcription");
    if (isText) {
        QString t = QString::fromUtf8(d);
        if (t.size() > 60) { t = t.left(60); t += QStringLiteral("..."); }
        return t;
    }
    if (ct == QStringLiteral("audio/opus")) {
        int durMs = 0;
        if (d.startsWith('*')) {
            int pipe = d.indexOf('|');
            if (pipe > 1) {
                QByteArray b64 = d.mid(1, pipe - 1);
                b64.replace('-', '+');
                b64.replace('_', '/');
                QString raw = QString::fromUtf8(QByteArray::fromBase64(b64));
                int durPos = raw.indexOf(QStringLiteral(";dur="));
                if (durPos >= 0) {
                    int end = raw.indexOf(';', durPos + 5);
                    QString ds = (end > 0) ? raw.mid(durPos + 5, end - durPos - 5)
                                           : raw.mid(durPos + 5);
                    durMs = ds.toInt();
                }
            }
        }
        if (durMs > 0) {
            int s = durMs / 1000;
            return QStringLiteral("Voice message (%1:%2)")
                .arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0'));
        }
        return QStringLiteral("Voice message");
    }
    if (ct.startsWith(QStringLiteral("video/"))) return QStringLiteral("Video");
    if (ct.startsWith(QStringLiteral("image/"))) return QStringLiteral("Photo");
    /* файл и прочие нетекстовые: base64-префикс — имя файла */
    if (d.startsWith('*')) {
        int pipe = d.indexOf('|');
        QByteArray b64 = d.mid(1, (pipe > 1) ? pipe - 1 : d.size() - 1);
        b64.replace('-', '+');
        b64.replace('_', '/');
        QString fname = QString::fromUtf8(QByteArray::fromBase64(b64));
        if (!fname.isEmpty()) return fname;
    }
    return QStringLiteral("File");
}

QString DbReadView::getChannelLastMessage(const QString& channelId) const {
    if (groupType(channelId) != QStringLiteral("chat")) return {};
    QString sql = QStringLiteral(
        "SELECT accepted_key, data FROM \"%1\" ORDER BY id DESC LIMIT 1")
        .arg(msgTableName(channelId));
    sqlite3_stmt* stmt = prepareOrNull(sql.toUtf8().constData());
    if (!stmt) return {};
    QString result;
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        const QByteArray author = colBlob(stmt, 0);
        QByteArray jdata = colBlob(stmt, 1);
        QString ct = QString::fromUtf8(parseJsonField(jdata, "ct"));
        QByteArray d = parseJsonField(jdata, "d");
        QString nick = getDisplayName(author, channelId);
        result = nick + QStringLiteral(": ") + mediaPreviewBody(ct, d);
    }
    return finishRead(stmt, rc) ? result : QString();
}

/* ── messages (UI) ── */

/* Единое чтение подписанного сообщения для истории, переходов и разрешения цитат. */
static MessageRow readMessage(sqlite3_stmt* stmt, const QByteArray& self) {
    MessageRow m;
    m.id = sqlite3_column_int64(stmt, 0);
    const auto json = QJsonDocument::fromJson(DbReadView::colBlob(stmt, 1)).object();
    m.contentType = json.value("ct").toString("text/plain"); m.data = json.value("d").toString().toUtf8();
    if (m.contentType.isEmpty()) m.contentType = "text/plain";
    const QByteArray reply = json.value("reply_to").toString().toLatin1();
    if (!reply.isEmpty()) {
        m.replyTo = QByteArray::fromHex(reply);
        if (reply.size() != 64 || m.replyTo.size() != 32 || m.replyTo.toHex() != reply) {
            GUI_WARN("reply: malformed reference message_row=%lld", (long long)m.id); m.replyTo.clear();
        }
    }
    if (!m.replyTo.isEmpty()) m.replyQuote = json.value("reply_quote").toString();
    m.timestamp = sqlite3_column_int64(stmt, 2); m.signature = DbReadView::colBlob(stmt, 3);
    m.localAttrs = DbReadView::colBlob(stmt, 4); m.messageId = DbReadView::colBlob(stmt, 5);
    m.version = quint64(sqlite3_column_int64(stmt, 6)); m.editedAt = sqlite3_column_int64(stmt, 7);
    m.authorEd = DbReadView::colBlob(stmt, 8);
    m.isOutgoing = self.size() == 32 && m.authorEd == self;
    return m;
}

/* Окно вокруг оригинала выбирается одним SQL snapshot, включая одинаковые timestamp. */
QList<MessageRow> DbReadView::getMessages(const QString& chId, int limit, const QByteArray& around) const {
    QList<MessageRow> list;
    if (limit <= 0 || (!around.isEmpty() && around.size() != 32)) {
        recordError(SQLITE_MISUSE); GUI_ERROR("history: invalid anchor/window bytes=%d limit=%d", int(around.size()), limit); return list;
    }
    QString sql = QStringLiteral(
        "SELECT id, data, timestamp, author_signature, local_attrs, message_id, ver, edited_at, accepted_key"
        " FROM \"%1\" ORDER BY timestamp DESC, message_id DESC LIMIT ?1").arg(msgTableName(chId));
    if (!around.isEmpty()) sql += QStringLiteral(
        " OFFSET min(max(0,(SELECT count(*) FROM \"%1\" n JOIN \"%1\" a ON a.message_id=?2 "
        "WHERE (n.timestamp,n.message_id)>(a.timestamp,a.message_id)) - ?1/2),"
        "max(0,(SELECT count(*) FROM \"%1\")-?1))").arg(msgTableName(chId));
    sqlite3_stmt* stmt = prepareOrNull(sql.toUtf8().constData());
    if (!stmt) return list;
    int rc = sqlite3_bind_int(stmt, 1, limit);
    if (rc == SQLITE_OK && !around.isEmpty()) rc = sqlite3_bind_blob(stmt, 2, around.constData(), around.size(), SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) { finishRead(stmt, rc); return {}; }
    const QByteArray self = localEd25519();
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        list.prepend(readMessage(stmt, self));
    }
    if (!finishRead(stmt, rc)) return {};
    if (!around.isEmpty()) GUI_DEBUG("history: window loaded ch=%s anchor=%s rows=%d limit=%d",
                                    qPrintable(chId), around.toHex().constData(), int(list.size()), limit);
    return list;
}

QList<MessageRow> DbReadView::getMessagesSince(const QString& chId, qint64 sinceId) const {
    QList<MessageRow> list;
    QString sql = QStringLiteral(
        "SELECT id, data, timestamp, author_signature, local_attrs, message_id, ver, edited_at, accepted_key"
        " FROM \"%1\" WHERE id > ? ORDER BY id ASC").arg(msgTableName(chId));
    sqlite3_stmt* stmt = prepareOrNull(sql.toUtf8().constData());
    if (!stmt) return list;
    int rc = sqlite3_bind_int64(stmt, 1, sinceId);
    if (rc != SQLITE_OK) { finishRead(stmt, rc); return {}; }
    const QByteArray self = localEd25519();
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        list.append(readMessage(stmt, self));
    }
    return finishRead(stmt, rc) ? list : QList<MessageRow>();
}

MessageRow DbReadView::getMessageById(const QString& chId, qint64 msgId) const {
    MessageRow m;
    QString sql = QStringLiteral(
        "SELECT id, data, timestamp, author_signature, local_attrs, message_id, ver, edited_at, accepted_key"
        " FROM \"%1\" WHERE id = ?").arg(msgTableName(chId));
    sqlite3_stmt* stmt = prepareOrNull(sql.toUtf8().constData());
    if (!stmt) return m;
    int rc = sqlite3_bind_int64(stmt, 1, msgId);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        m = readMessage(stmt, localEd25519());
    }
    return finishRead(stmt, rc) ? m : MessageRow();
}

/* Метаданные и состояние ключа читаются одним SELECT, курсор закрыт до передачи данных UI. */
AttachmentRow DbReadView::getAttachment(const QString& channel, const QByteArray& messageId) const {
    AttachmentRow row;
    if (messageId.size() != 32) { recordError(SQLITE_MISUSE); GUI_ERROR("attachment lookup: invalid key bytes=%d", int(messageId.size())); return row; }
    sqlite3_stmt* st = prepareOrNull("SELECT a.ready,a.downloaded,a.decrypting,a.error,a.plain_path,a.info,a.content_type,"
        "COALESCE(r.state,-1),length(a.key)=32 FROM attachment_keys a LEFT JOIN attachment_key_requests r"
        " ON r.channel=a.channel AND r.message_id=a.message_id AND r.incoming=0 WHERE a.channel=? AND a.message_id=?");
    if (!st) return row;
    const QByteArray bytes = channel.toUtf8();
    int rc = sqlite3_bind_text(st, 1, bytes.constData(), bytes.size(), SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 2, messageId.constData(), messageId.size(), SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        row.found = true; row.ready = sqlite3_column_int(st, 0); row.downloaded = sqlite3_column_int(st, 1);
        row.decrypting = sqlite3_column_int(st, 2); row.error = sqlite3_column_int(st, 3);
        row.path = colText(st, 4); row.info = colBlob(st, 5); row.contentType = colText(st, 6);
        row.requestState = sqlite3_column_int(st, 7); row.hasKey = sqlite3_column_int(st, 8);
    }
    return finishRead(st, rc) ? row : AttachmentRow();
}

/* ── DM-беседы (UI, read-only) ── */

/* Поиск ограничен таблицей выбранного канала; отсутствие оригинала допустимо при неполной синхронизации. */
MessageRow DbReadView::getMessageByKey(const QString& chId, const QByteArray& key) const {
    MessageRow message;
    if (key.size() != 32) { recordError(SQLITE_MISUSE); GUI_ERROR("reply: invalid lookup key bytes=%d", int(key.size())); return message; }
    const auto sql = QStringLiteral("SELECT id,data,timestamp,author_signature,local_attrs,message_id,ver,edited_at,accepted_key "
                                   "FROM \"%1\" WHERE message_id=?").arg(msgTableName(chId)).toUtf8();
    sqlite3_stmt* stmt = prepareOrNull(sql.constData());
    if (!stmt) return message;
    int rc = sqlite3_bind_blob(stmt, 1, key.constData(), key.size(), SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) message = readMessage(stmt, localEd25519());
    return finishRead(stmt, rc) ? message : MessageRow();
}

/* Короткий обычный текст без рекурсивных цитат и служебного описания медиа. */
QString DbReadView::messagePreview(const MessageRow& message) {
    const auto& ct = message.contentType;
    QString text;
    if (ct == "poll") {
        struct chat_poll poll;
        const auto wire = QByteArray::fromBase64(message.data.mid(4), QByteArray::Base64UrlEncoding);
        if (chat_poll_decode(reinterpret_cast<const uint8_t*>(wire.constData()), wire.size(), &poll) == 0)
            text = QString::fromUtf8(poll.question);
        else text = QCoreApplication::translate("ChatGui", "Invalid poll");
    } else if (ct == "audio/opus") text = QCoreApplication::translate("ChatGui", "Voice message");
    else if (ct.startsWith("image/")) text = QCoreApplication::translate("ChatGui", "Photo");
    else if (ct.startsWith("video/")) text = QCoreApplication::translate("ChatGui", "Video");
    else text = mediaPreviewBody(ct, message.data);
    return text.simplified().left(160);
}

QList<DmConversation> DbReadView::getDmConversations() const {
    QList<DmConversation> list;
    sqlite3_stmt* stmt = prepareOrNull(
        "SELECT c.conv_id, c.peer_ed25519, COALESCE(peer_name,''), last_ts, last_out_seq, last_in_seq, COALESCE(h.generation,1),"
        " c.mode,c.closed,c.peer_party"
        " FROM dm_conversations c LEFT JOIN dm_history h USING(conv_id) WHERE COALESCE(h.hidden,0)=0 ORDER BY last_ts DESC");
    if (!stmt) return list;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        DmConversation c;
        c.convId     = colText(stmt, 0);
        c.peerName   = colText(stmt, 2);
        if (sqlite3_column_bytes(stmt, 1) == 32)
            c.peerEd25519 = QByteArray(static_cast<const char*>(sqlite3_column_blob(stmt, 1)), 32);
        c.lastTs     = sqlite3_column_int64(stmt, 3);
        c.lastOutSeq = sqlite3_column_int64(stmt, 4);
        c.lastInSeq  = sqlite3_column_int64(stmt, 5);
        c.generation = (quint64)sqlite3_column_int64(stmt, 6);
        c.mode = sqlite3_column_int(stmt, 7); c.closed = sqlite3_column_int(stmt, 8) != 0;
        struct dm_party party;
        if (sqlite3_column_bytes(stmt, 9) == DM_PARTY_WIRE_SIZE &&
            !dm_party_decode(static_cast<const uint8_t*>(sqlite3_column_blob(stmt, 9)), &party)) {
            c.peerGroup = party.kind == DM_PARTY_GROUP;
            struct devgroup_record record;
            struct devgroup_profile profile;
            if (c.peerGroup && devgroup_latest(m_db, party.ed, &record, &profile) == 1) c.peerName = QString::fromUtf8(profile.name);
        }
        list.append(c);
    }
    return finishRead(stmt, rc) ? list : QList<DmConversation>();
}

/* ── peers (UI) ── */

QMap<QByteArray, QString> DbReadView::getLocalNicknames(const QString& chId) const {
    QMap<QByteArray, QString> m;
    QString sql = QStringLiteral(
        "SELECT ed25519_pubkey, local_nick FROM \"%1\" WHERE record_state=0 AND local_nick IS NOT NULL AND local_nick != ''"
    ).arg(peersTableName(chId));
    sqlite3_stmt* stmt = prepareOrNull(sql.toUtf8().constData());
    if (!stmt) return m;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
        m.insert(colBlob(stmt, 0), colText(stmt, 1));
    return finishRead(stmt, rc) ? m : QMap<QByteArray, QString>();
}

QString DbReadView::getDisplayName(const QByteArray& ed25519, const QString& channel) const {
    if (ed25519.size() != 32) {
        GUI_WARN("display name: invalid signing key length=%d", int(ed25519.size())); return {};
    }
    if (!channel.isEmpty()) {
        const auto sql = QStringLiteral("SELECT local_nick FROM \"%1\" WHERE ed25519_pubkey=? AND record_state=0")
            .arg(peersTableName(channel)).toUtf8();
        if (sqlite3_stmt* st = prepareOrNull(sql.constData())) {
            int rc = sqlite3_bind_blob(st, 1, ed25519.constData(), 32, SQLITE_STATIC);
            if (rc == SQLITE_OK) rc = sqlite3_step(st);
            const QString name = rc == SQLITE_ROW ? colText(st, 0) : QString();
            if (finishRead(st, rc) && !name.isEmpty()) return name;
        }
    }
    sqlite3_stmt* st = prepareOrNull(
        "SELECT COALESCE(NULLIF(a.display_name,''),n.name) FROM nodes n LEFT JOIN accounts a ON a.node_x=n.x25519_pubkey"
        " WHERE n.ed25519_pubkey=? ORDER BY n.node_id LIMIT 1");
    if (st) {
        int rc = sqlite3_bind_blob(st, 1, ed25519.constData(), 32, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_step(st);
        const QString name = rc == SQLITE_ROW ? colText(st, 0) : QString();
        if (finishRead(st, rc) && !name.isEmpty()) return name;
    }
    return QString::fromLatin1(ed25519.toHex());
}

QByteArray DbReadView::localEd25519() const {
    sqlite3_stmt* stmt = prepareOrNull("SELECT ed25519_pubkey FROM local_identity WHERE id=1");
    if (!stmt) return {};
    const int rc = sqlite3_step(stmt);
    const QByteArray key = rc == SQLITE_ROW ? colBlob(stmt, 0) : QByteArray();
    if (!finishRead(stmt, rc)) return {};
    if (!key.isEmpty() && key.size() != 32) { recordError(SQLITE_CORRUPT); GUI_ERROR("local Ed25519 length=%d", int(key.size())); return {}; }
    return key;
}

/* ── accounts ── */

QList<AccountRow> DbReadView::getAccounts(bool contactsOnly) const {
    QList<AccountRow> list;
    const char* sql = contactsOnly
        ? "SELECT node_x, display_name, avatar_color, avatar_letter, is_contact"
          " FROM accounts WHERE is_contact=1 ORDER BY display_name"
        : "SELECT node_x, display_name, avatar_color, avatar_letter, is_contact"
          " FROM accounts ORDER BY display_name";
    sqlite3_stmt* stmt = prepareOrNull(sql);
    if (!stmt) return list;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        AccountRow a;
        a.nodeKey      = colBlob(stmt, 0);
        a.displayName  = colText(stmt, 1);
        a.avatarColor  = colText(stmt, 2);
        a.avatarLetter = colText(stmt, 3);
        a.isContact    = sqlite3_column_int(stmt, 4) != 0;
        list.append(a);
    }
    return finishRead(stmt, rc) ? list : QList<AccountRow>();
}

AccountRow DbReadView::getAccount(const QByteArray& ed) const {
    AccountRow a;
    if (ed.size() != 32) return a;
    sqlite3_stmt* stmt = prepareOrNull(
        "SELECT node_x, display_name, avatar_color, avatar_letter, is_contact"
        " FROM accounts WHERE node_x=(SELECT x25519_pubkey FROM nodes WHERE ed25519_pubkey=?)");
    if (!stmt) return a;
    int rc = sqlite3_bind_blob(stmt, 1, ed.constData(), 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        a.nodeKey      = colBlob(stmt, 0);
        a.displayName  = colText(stmt, 1);
        a.avatarColor  = colText(stmt, 2);
        a.avatarLetter = colText(stmt, 3);
        a.isContact    = sqlite3_column_int(stmt, 4) != 0;
    }
    return finishRead(stmt, rc) ? a : AccountRow();
}

/* ── ui_state ── */

QString DbReadView::getUiState(const QString& key) const {
    sqlite3_stmt* stmt = prepareOrNull("SELECT value FROM ui_state WHERE key=?");
    if (!stmt) return {};
    int rc = sqlite3_bind_text(stmt, 1, key.toUtf8().constData(), -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    QString val;
    if (rc == SQLITE_ROW) val = colText(stmt, 0);
    return finishRead(stmt, rc) ? val : QString();
}

int DbReadView::getUiStateInt(const QString& key, int defaultVal) const {
    QString val = getUiState(key);
    if (val.isEmpty()) return defaultVal;
    bool ok = false;
    int v = val.toInt(&ok);
    return ok ? v : defaultVal;
}

qint64 DbReadView::getUiStateInt64(const QString& key, qint64 defaultVal) const {
    QString val = getUiState(key);
    if (val.isEmpty()) return defaultVal;
    bool ok = false;
    qint64 v = val.toLongLong(&ok);
    return ok ? v : defaultVal;
}

int DbReadView::getUnreadCount(const QString& channelId, qint64 lastReadMsgId) const {
    if (groupType(channelId) != QStringLiteral("chat")) return 0;
    QString sql = QStringLiteral(
        "SELECT COUNT(*) FROM \"%1\" WHERE id > ?").arg(msgTableName(channelId));
    sqlite3_stmt* stmt = prepareOrNull(sql.toUtf8().constData());
    if (!stmt) return 0;
    int rc = sqlite3_bind_int64(stmt, 1, lastReadMsgId);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    int count = 0;
    if (rc == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
    return finishRead(stmt, rc) ? count : 0;
}

/* ── integrity checks ── */

enum class PragmaMode { IntegrityCheck, Command };

/* SQLite сообщает нарушения целостности строками даже при SQLITE_OK; сохраняем первое и дополнительные сообщения. */
static int integrityCallback(void* data, int nCols, char** values, char** /*names*/) {
    auto* result = (IntegrityResult*)data;
    try {
        for (int i = 0; i < nCols; i++) {
            if (values[i]) {
                QString line = QString::fromUtf8(values[i]);
                if (result->message.isEmpty()) result->message = line;
                else if (line != "ok") result->errors.append(line);
            }
        }
    } catch (const std::bad_alloc&) {
        result->sqliteCode = SQLITE_NOMEM; DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "DB integrity callback allocation failed"); return 1;
    } catch (const std::exception& error) {
        result->sqliteCode = SQLITE_ABORT; DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "DB integrity callback exception: %s", error.what()); return 1;
    } catch (...) {
        result->sqliteCode = SQLITE_ABORT; DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "DB integrity callback unknown exception"); return 1;
    }
    return 0;
}

/* Проверки требуют ответа ok; у команды обслуживания успех определяется завершением SQL, без обязательных строк. */
static IntegrityResult runPragma(sqlite3* db, const char* pragma, PragmaMode mode) {
    IntegrityResult result;
    DEBUG_DEBUG(DEBUG_CATEGORY_CONFIG, "DB maintenance begin operation=%s db=%p integrity=%d",
                pragma, static_cast<void*>(db), mode == PragmaMode::IntegrityCheck);
    if (!db) {
        result.sqliteCode = SQLITE_MISUSE;
        result.message = "Database not open";
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "DB maintenance failed operation=%s: database not open", pragma);
        return result;
    }
    QElapsedTimer timer;
    timer.start();
    char* errMsg = nullptr;
    const int rc = sqlite3_exec(db, pragma, mode == PragmaMode::IntegrityCheck ? integrityCallback : nullptr, &result, &errMsg);
    if (rc != SQLITE_ABORT || result.sqliteCode == SQLITE_OK) result.sqliteCode = rc;
    result.elapsedMs = (int)timer.elapsed();
    if (rc != SQLITE_OK) {
        result.message = QString::fromUtf8(errMsg ? errMsg : sqlite3_errmsg(db));
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "DB maintenance failed operation=%s rc=%d elapsed=%dms: %s",
                    pragma, rc, result.elapsedMs, qPrintable(result.message));
        sqlite3_free(errMsg);
        return result;
    }
    sqlite3_free(errMsg);
    if (mode == PragmaMode::Command) {
        result.ok = true;
        result.message = "ok";
    } else {
        result.ok = result.message == "ok" && result.errors.isEmpty();
        if (result.message.isEmpty()) result.message = "Integrity check returned no result";
    }
    if (result.ok) {
        DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "DB maintenance complete operation=%s rc=%d elapsed=%dms result=ok", pragma, rc, result.elapsedMs);
    } else {
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "DB maintenance failed operation=%s rc=%d elapsed=%dms extra_errors=%d: %s",
                    pragma, rc, result.elapsedMs, int(result.errors.size()), qPrintable(result.message));
        for (const auto& error : result.errors)
            DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "DB maintenance detail operation=%s: %s", pragma, qPrintable(error));
    }
    return result;
}

IntegrityResult DbReadView::integrityCheckQuick() const {
    return runPragma(m_db, "PRAGMA quick_check", PragmaMode::IntegrityCheck);
}

IntegrityResult DbReadView::integrityCheckFull() const {
    return runPragma(m_db, "PRAGMA integrity_check", PragmaMode::IntegrityCheck);
}

bool DbManager::integrityCheckAsync(bool full, QObject* context, std::function<void(IntegrityResult)> complete) const {
    if (!complete) { DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "DB check scheduling failed: callback unavailable"); return false; }
    return readAsync(context, [full](DbReadView& view, QVariant* value) {
        IntegrityResult result = full ? view.integrityCheckFull() : view.integrityCheckQuick();
        int code = result.sqliteCode; *value = QVariant::fromValue(std::move(result)); return code;
    }, [complete = std::move(complete)](const db_async_result& status, const QVariant& value) {
        IntegrityResult result;
        if (status.status == DB_ASYNC_OK && value.canConvert<IntegrityResult>()) result = value.value<IntegrityResult>();
        else {
            result.sqliteCode = status.sqlite_code;
            result.message = status.status ? QString::fromUtf8(status.message) : QStringLiteral("Invalid database check response");
            DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "DB check failed status=%d rc=%d: %s", status.status, status.sqlite_code, qPrintable(result.message));
        }
        complete(std::move(result));
    });
}

/* Getter-ошибки и смена GUI-привязки проверяются одинаково для всех асинхронных снимков. */
bool DbManager::readAsync(QObject* context, std::function<int(DbReadView&, QVariant*)> work, GuiDbReadDone complete) const {
    if (!m_db || !m_asyncReader || !context || !work || !complete) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "DB read scheduling failed: database/backend/context/work/callback unavailable"); return false;
    }
    const QPointer<const DbManager> self(this); const auto generation = m_dbGeneration;
    return m_asyncReader(m_db, context, [work = std::move(work), path = m_dbPath](sqlite3* db, QVariant* value) {
        DbReadView view(db, path); const int rc = work(view, value);
        return rc == SQLITE_OK ? view.sqlError() : rc;
    }, [self, generation, complete = std::move(complete)](const db_async_result& status, const QVariant& value) {
        if (!self || self->m_dbGeneration != generation) {
            db_async_result changed{}; changed.status = DB_ASYNC_CLOSED; changed.sqlite_code = SQLITE_ABORT;
            snprintf(changed.message, sizeof(changed.message), "Database changed or stopped");
            DEBUG_WARN(DEBUG_CATEGORY_CONFIG, "DB read result discarded: manager/database changed"); complete(changed, {}); return;
        }
        complete(status, value);
    });
}

IntegrityResult DbManager::integrityOptimize() const {
    return runPragma(m_db, "PRAGMA optimize", PragmaMode::Command);
}

/* Настоящее ядро и SQLite: подписанные ответы, сохранение, медиа и окно старой истории. */
#include "../db/db_manager.h"
#include "../src/nodespage.h"
#include "../transport/gui_bridge.h"
#include <QtTest>
#include <QTemporaryDir>
extern "C" {
#include "utun_instance.h"
#include "chat/chat_core.h"
#include "chat/chat_event.h"
#include "chat/db_sync.h"
#include "chat/chat_log.h"
#include "group/group_protocol.h"
#include "media_async/attachment_send.h"
#include "routing_layer/topo_node_sqlite.h"
#include "config_parser.h"
#include "mem.h"
#include "debug_config.h"
}

static QHash<quint64, bool> results;
static void submitted(UTUN_INSTANCE*, int type, const uint8_t* data, int len) {
    if (type != CHAT_EVT_MSG_SUBMITTED || len != 9) return;
    quint64 id = 0; for (unsigned i = 0; i < 8; ++i) id |= quint64(data[i]) << (8 * i);
    results.insert(id, data[8] != 0);
}

class ChannelRepliesTest : public QObject {
    Q_OBJECT
    QTemporaryDir directory;
    UASYNC* ua = nullptr;
    QTimer poll;
    UTUN_INSTANCE* inst = nullptr;
    DbManager db;
    QString channel;
    QByteArray original;

    QByteArray send(const QByteArray& text, const QByteArray& reply = {}) {
        chat_msg_submit request{}; request.inst = inst;
        snprintf(request.channel_id, sizeof(request.channel_id), "%s", qPrintable(channel));
        strcpy(request.content_type, "text/plain"); request.data = (uint8_t*)text.constData(); request.data_len = text.size();
        if (reply.size() == 32) memcpy(request.reply_to, reply.constData(), 32);
        if (chat_core_submit_message(inst, &request) != 0) return {};
        return QByteArray(reinterpret_cast<const char*>(request.message_id), 32);
    }

private slots:
    void initTestCase() {
        debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
        debug_set_category_level(DEBUG_CATEGORY_DEBUG, DEBUG_LEVEL_INFO);
        gui_bridge_init();
        QVERIFY(directory.isValid()); ua = uasync_create(); QVERIFY(ua);
        const QByteArray config = "[global]\ndb_path=" + directory.path().toUtf8()
            + "\n[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n";
        inst = utun_instance_create_from_str(ua, config.constData()); QVERIFY(inst);
        QCOMPARE(utun_core_start(inst), 0); QCOMPARE(chat_service_start(inst), 0);
        chat_event_set_handler(inst, submitted);
        chat_core_create_channel_auto(inst, "Reply tests");
        db.setDb(inst->topo_sqlite_db);
        db.setAsyncReader(gui_bridge_db_read); uasync_mark_running(ua); gui_bridge_set_uasync(ua); gui_bridge_set_inst(inst);
        connect(&poll, &QTimer::timeout, this, [this] { uasync_poll(ua, 0); }); poll.start(1);
        const auto channels = db.getChannels(); QCOMPARE(channels.size(), 1); channel = channels.first().channelId;
        original = send("Original with Unicode: Привет \"world\"\nnext line"); QCOMPARE(original.size(), 32);
    }

    void signedReferenceAndRestart() {
        sqlite3_stmt* identity = nullptr;
        QCOMPARE(sqlite3_prepare_v2(inst->topo_sqlite_db, "SELECT x25519_pubkey,ed25519_pubkey FROM local_identity WHERE id=1",
                                  -1, &identity, nullptr), SQLITE_OK);
        const int identityRc = sqlite3_step(identity);
        const auto x25519 = DbManager::colBlob(identity, 0), ed25519 = DbManager::colBlob(identity, 1);
        sqlite3_finalize(identity);
        QCOMPARE(identityRc, SQLITE_ROW);
        QCOMPARE(x25519, QByteArray(reinterpret_cast<const char*>(inst->my_keys.public_key), 32));
        QCOMPARE(ed25519, QByteArray(reinterpret_cast<const char*>(inst->my_ed25519_pubkey), 32));
        const auto reply = send("Answer", original); QCOMPARE(reply.size(), 32);
        QCOMPARE(db.getMessageByKey(channel, reply).replyTo, original);
        const auto sql = QString("SELECT timestamp,data,author_signature,nonce,ver,edited_at,generation FROM msg_%1 WHERE message_id=?")
            .arg(channel).toUtf8();
        sqlite3_stmt* statement = nullptr;
        QCOMPARE(sqlite3_prepare_v2(inst->topo_sqlite_db, sql.constData(), -1, &statement, nullptr), SQLITE_OK);
        sqlite3_bind_blob(statement, 1, reply.constData(), reply.size(), SQLITE_STATIC);
        QCOMPARE(sqlite3_step(statement), SQLITE_ROW);
        uint64_t ts = sqlite3_column_int64(statement, 0);
        QByteArray body = DbManager::colBlob(statement, 1), signature = DbManager::colBlob(statement, 2);
        db_message_revision revision{}; const auto nonce = DbManager::colBlob(statement, 3); QCOMPARE(nonce.size(), 16);
        memcpy(revision.nonce, nonce.constData(), 16); revision.ver = sqlite3_column_int64(statement, 4);
        revision.edited_at = sqlite3_column_int64(statement, 5);
        const auto generation = DbManager::colBlob(statement, 6); QCOMPARE(generation.size(), 32);
        memcpy(revision.generation, generation.constData(), 32); sqlite3_finalize(statement);
        db_sync_record record{}; record.timestamp = ts; record.author = inst->node_id; record.revision = revision;
        memcpy(record.author_ed, inst->my_ed25519_pubkey, 32);
        record.data = body.constData(); record.size = size_t(body.size());
        memcpy(record.message_id, reply.constData(), 32); memcpy(record.signature, signature.constData(), 64);
        uint8_t wire[MEMBER_LOG_WIRE_MAX]; size_t wireSize = sizeof(wire); member_log_record block{};
        QCOMPARE(member_log_get(inst->topo_sqlite_db, qPrintable(channel), inst->my_ed25519_pubkey, revision.ver, wire, &wireSize), 1);
        QCOMPARE(member_log_decode(wire, wireSize, inst->my_ed25519_pubkey, &block), 0);
        QCOMPARE(QByteArray(reinterpret_cast<const char*>(block.scope), 32), reply);
        QCOMPARE(QByteArray(reinterpret_cast<const char*>(block.signature), 64), signature);
        QCOMPARE(chat_log_message_check(inst->topo_sqlite_db, qPrintable(channel), &record), 1);
        DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "reply signature verified against member log ch=%s seq=%llu bytes=%zu",
            qPrintable(channel), (unsigned long long)block.seq, wireSize);
        const auto byKey = db.getMessageByKey(channel, reply);
        const auto latest = db.getMessages(channel, 1); QCOMPARE(latest.size(), 1);
        const auto since = db.getMessagesSince(channel, byKey.id - 1); QCOMPARE(since.size(), 1);
        const auto byId = db.getMessageById(channel, byKey.id);
        const MessageRow readers[] = {byKey, byId, latest.first(), since.first()};
        const char* names[] = {"key", "id", "latest", "since"};
        for (unsigned i = 0; i < 4; i++) {
            const auto& row = readers[i];
            DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "history reader=%s author=%s local=%s outgoing=%d signature=%d id=%d attrs=%d",
                names[i], row.authorEd.toHex().constData(), db.localEd25519().toHex().constData(), row.isOutgoing,
                int(row.signature.size()), int(row.messageId.size()), int(row.localAttrs.size()));
            QCOMPARE(row.messageId, reply); QCOMPARE(row.replyTo, original); QCOMPARE(row.signature, signature);
            QCOMPARE(row.timestamp, qint64(ts)); QCOMPARE(row.data, QByteArray("Answer"));
            QVERIFY(row.isOutgoing); QVERIFY(row.localAttrs.isEmpty());
        }
        const int reference = body.indexOf(original.toHex()); QVERIFY(reference >= 0); body[reference] = body[reference] == 'a' ? 'b' : 'a';
        record.data = body.constData();
        QCOMPARE(chat_log_message_check(inst->topo_sqlite_db, qPrintable(channel), &record), 0);
        chat_service_stop(inst); QCOMPARE(chat_service_start(inst), 0);
        QCOMPARE(db.getMessageByKey(channel, reply).replyTo, original);
        sqlite3* reopened = nullptr;
        QCOMPARE(sqlite3_open_v2(qPrintable(directory.filePath("chats.db")), &reopened, SQLITE_OPEN_READONLY, nullptr), SQLITE_OK);
        DbManager reader; reader.setDb(reopened); QCOMPARE(reader.getMessageByKey(channel, reply).replyTo, original);
        reader.setDb(nullptr); QCOMPARE(sqlite3_close(reopened), SQLITE_OK);
    }

    void addressPresentation() {
        TOPO_SOCKMETA4 meta4{}; meta4.id = 5; meta4.config_type = CFG_SERVER_TYPE_PUBLIC;
        meta4.address_profile.source = ADDRESS_SOURCE_ETCP;
        TOPO_SOCKMETA6 meta6{}; meta6.id = 6; meta6.config_type = CFG_SERVER_TYPE_PUBLIC;
        meta6.address_profile = {ADDRESS_MAP_REAL_IP, ADDRESS_IN_UNKNOWN, ADDRESS_SOURCE_ETCP};
        TOPO_ADDR4 addr4{}; memcpy(addr4.addr, (uint8_t[]){194,63,142,140}, 4);
        addr4.port = 0; addr4.socket_id = 5; addr4.protocol = TOPO_PROTO_TCP; addr4.type = TOPO_ADDR_NAT;
        TOPO_ADDR6 addr6{}; addr6.addr[0] = 0x2a; addr6.addr[1] = 0x04; addr6.addr[2] = 0x52; addr6.addr[15] = 0xf;
        addr6.port = 443; addr6.socket_id = 6; addr6.protocol = TOPO_PROTO_TCP | TOPO_PROTO_REALITY;
        TOPO_NODE node{}; node.node_id = inst->node_id; memcpy(node.public_key, inst->my_keys.public_key, 32);
        node.v4_sock_meta = &meta4; node.v4_addrs = &addr4; node.v6_sock_meta = &meta6; node.v6_addrs = &addr6;
        QCOMPARE(topo_node_sqlite_addrs_put(inst->topo_sqlite_db, inst->node_id, &node), 0);
        NodesPage page(&db); page.refreshNodes();
        const auto* dump = page.findChild<QPlainTextEdit*>(); QVERIFY(dump);
        auto* refresh = page.findChild<QPushButton*>(); QVERIFY(refresh); QTRY_VERIFY(refresh->isEnabled());
        const QString text = dump->toPlainText();
        QVERIFY2(text.contains("194.63.142.140:0  TCP  NAT?"), qPrintable(text));
        QVERIFY2(text.contains("2a04:5200:0:0:0:0:0:f:443  TCP+REALITY  rtt=-ms\n"), qPrintable(text));
        meta6.address_profile = {};
        QCOMPARE(topo_node_sqlite_addrs_put(inst->topo_sqlite_db, inst->node_id, &node), 0);
        page.refreshNodes();
        QTRY_VERIFY(refresh->isEnabled());
        QCOMPARE(dump->toPlainText(), text);
    }

    void unavailableAndForeignReferencesFail() {
        QVERIFY(send("missing", QByteArray(32, 'z')).isEmpty());
        const QString saved = channel; chat_core_create_channel_auto(inst, "Other channel");
        for (const auto& row : db.getChannels()) if (row.channelId != saved) channel = row.channelId;
        QVERIFY(channel != saved); QVERIFY(send("wrong channel", original).isEmpty()); channel = saved;
        auto* request = (chat_msg_submit*)u_calloc(1, sizeof(chat_msg_submit)); QVERIFY(request);
        request->inst = inst; request->request_id = 101; strcpy(request->channel_id, "0"); strcpy(request->content_type, "text/plain");
        chat_core_submit_trampoline(request); QVERIFY(results.contains(101)); QVERIFY(!results.value(101));
    }

    void mediaKeepsReferenceThroughWorker() {
        auto* request = (attachment_send_req*)u_calloc(1, sizeof(attachment_send_req)); QVERIFY(request);
        request->inst = inst; request->request_id = 102; request->info.kind = ATTACHMENT_VOICE; request->preset = 1;
        snprintf(request->target, sizeof(request->target), "%s", qPrintable(channel)); memcpy(request->reply_to, original.constData(), 32);
        request->pcm = (int16_t*)u_calloc(48000, sizeof(int16_t)); QVERIFY(request->pcm); request->pcm_count = 48000;
        attachment_send_trampoline(request);
        QElapsedTimer timer; timer.start(); while (!results.contains(102) && timer.elapsed() < 10000) uasync_poll(ua, 20);
        QVERIFY(results.contains(102)); QVERIFY(results.value(102));
        const auto messages = db.getMessages(channel, 1); QCOMPARE(messages.size(), 1);
        QCOMPARE(messages.first().contentType, QString("audio/opus")); QCOMPARE(messages.first().replyTo, original);
        // Регистрация файла завершена до успешного подтверждения.
        QVERIFY(messages.first().localAttrs.contains("\"st\":\"fl\""));
    }

    void historyWindowAndDelayedOriginal() {
        for (int i = 0; i < 420; ++i) QCOMPARE(send(QByteArray::number(i)).size(), 32);
        const auto latest = db.getMessages(channel, 200); QCOMPARE(latest.size(), 200);
        for (const auto& row : latest) QVERIFY(row.messageId != original);
        const auto window = db.getMessages(channel, 200, original); QCOMPARE(window.size(), 200);
        bool found = false; for (const auto& row : window) found |= row.messageId == original; QVERIFY(found);
        const auto answer = send("Answer to old message", original); QCOMPARE(answer.size(), 32);
        const auto sql = QString("CREATE TEMP TABLE saved_original AS SELECT * FROM msg_%1 WHERE message_id=x'%2';"
                                "DELETE FROM msg_%1 WHERE message_id=x'%2'").arg(channel, QString::fromLatin1(original.toHex())).toUtf8();
        QCOMPARE(sqlite3_exec(inst->topo_sqlite_db, sql.constData(), nullptr, nullptr, nullptr), SQLITE_OK);
        QCOMPARE(db.getMessageByKey(channel, answer).replyTo, original); QVERIFY(db.getMessageByKey(channel, original).messageId.isEmpty());
        const auto restore = QString("INSERT INTO msg_%1 SELECT * FROM saved_original").arg(channel).toUtf8();
        QCOMPARE(sqlite3_exec(inst->topo_sqlite_db, restore.constData(), nullptr, nullptr, nullptr), SQLITE_OK);
        QCOMPARE(db.getMessageByKey(channel, original).messageId, original);
    }

    void cleanupTestCase() {
        poll.stop(); gui_bridge_set_inst(nullptr); gui_bridge_set_uasync(nullptr); if (ua) uasync_mark_stopped(ua);
        db.setDb(nullptr); if (inst) utun_instance_destroy(inst); if (ua) uasync_destroy(ua, 0);
    }

    void identityWriteFailureRejectsStartup() {
        group_service_stop(inst); // Идентичность принадлежит общим группам; остановка только CHAT их сохраняет.
        QCOMPARE(sqlite3_exec(inst->topo_sqlite_db, "CREATE TEMP TRIGGER reject_identity BEFORE INSERT ON local_identity"
            " BEGIN SELECT RAISE(ABORT,'injected local identity failure'); END", nullptr, nullptr, nullptr), SQLITE_OK);
        QVERIFY(chat_service_start(inst) < 0);
        QVERIFY(!inst->chat_started); QVERIFY(!inst->chat_core);
        QCOMPARE(sqlite3_exec(inst->topo_sqlite_db, "DROP TRIGGER reject_identity", nullptr, nullptr, nullptr), SQLITE_OK);
        QCOMPARE(chat_service_start(inst), 0); QVERIFY(inst->chat_started);
    }
};

QTEST_MAIN(ChannelRepliesTest)
#include "test_channel_replies.moc"

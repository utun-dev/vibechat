/* Настоящие core/storage, Qt worker и кеш: два канала, публикация после COMMIT и restart. */
#include "../src/avatars/avatarmanager.h"
#include "../src/memberlistmodel.h"
#include "../src/avatars/avatarcrop.h"
#include "../src/avatars/avatargallery.h"
#include "../src/imageviewer_window.h"
#include "../db/db_manager.h"
#include "../transport/gui_bridge.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QCryptographicHash>
#include <QPainter>
#include <QDialog>
#include <QPushButton>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QScopeGuard>
extern "C" {
#include "utun_instance.h"
#include "chat/chat_core.h"
#include "chat/chat_event.h"
#include "chat/chat_member.h"
#include "chat/member_sync.h"
#include "routing_layer/topo_group.h"
#include "routing_layer/topo_node_sqlite.h"
#include "member_files/member_files.h"
#include "json_flat.h"
#include "mem.h"
#include "debug_config.h"
}
static UTUN_INSTANCE* testInstance;
static unsigned downloads;
static QSet<quint64> downloadIds;
static QHash<quint64, int> finished;
static bool deferDownload;
static QByteArray delayedDownload;
static constexpr quint64 replicaNode = 0x1234567;
#define REPLICA_ED_HEX "822cf510b716f1a87b38d00ea5d487b164ff650448a4c4a328921701607e4732"
static bool replicaAvailable = true;
static unsigned replicaUploads, replicaPublications, ownPublications;
static QString replicaFile;
static QByteArray replicaGroup;
static void mockReplica(void (*fn)(void*), void* arg) {
    auto* manager = AvatarManager::instance(); if (!manager) { u_free(arg); return; }
    if (fn == member_files_gui_request) {
        auto* q = (mf_gui_request*)arg; mf_message reply = q->message;
        reply.kind = MF_RESPONSE;
        reply.status = replicaAvailable ? (reply.command == MF_STAT ? MF_NOT_FOUND : MF_OK) : MF_TIMEOUT;
        QByteArray bytes(MF_EVENT_RESPONSE_PREFIX + MF_WIRE_MAX, 0); memcpy(bytes.data(), q->group, 32);
        memcpy(bytes.data() + 32, q->server, MF_ADDRESS_SIZE);
        const int size = mf_encode(&reply, (uint8_t*)bytes.data() + MF_EVENT_RESPONSE_PREFIX, MF_WIRE_MAX);
        manager->onResponse((const uint8_t*)bytes.constData(), size + MF_EVENT_RESPONSE_PREFIX);
    } else if (fn == member_files_gui_transfer) {
        auto* q = (mf_transfer_request*)arg; replicaUploads++;
        QByteArray bytes(MF_EVENT_TRANSFER_SIZE, 0); auto* out = (uint8_t*)bytes.data();
        mf_put64(out, q->id); memcpy(out + MF_EVENT_TRANSFER_GROUP, q->group_key, 32); memcpy(out + MF_EVENT_TRANSFER_SERVER, q->server, 32); memcpy(out + MF_EVENT_TRANSFER_OWNER, q->owner, 32);
        out[MF_EVENT_TRANSFER_STATUS] = replicaAvailable ? MF_OK : MF_TIMEOUT; out[MF_EVENT_TRANSFER_STATE] = 2; manager->onTransfer(out, bytes.size());
    } else {
        auto* q = (chat_avatar_req*)arg; replicaPublications++; replicaFile = QString::fromLatin1(q->file);
        uint8_t wire[CHAT_MEMBER_DISPLAY_SIZE];
        const QByteArray channel = QByteArray(reinterpret_cast<const char*>(q->group), 32).toHex();
        Q_ASSERT(!chat_core_get_single_member(testInstance, channel.constData(), testInstance->my_ed25519_pubkey, wire));
        memcpy(wire + 71, q->server, sizeof(q->server)); memcpy(wire + 103, q->file, sizeof(q->file));
        manager->onMembers(QString::fromLatin1(channel), 1, wire, sizeof(wire), false);
        uint8_t result[CHAT_AVATAR_RESULT_SIZE] = {0}; mf_put64(result, q->id); memcpy(result + 8, q->group, 32); manager->onPublished(result, sizeof(result));
    }
    u_free(arg);
}
extern "C" int gui_bridge_is_uasync_ready(void) { return testInstance != nullptr; }
extern "C" UTUN_INSTANCE* gui_bridge_get_inst(void) { return testInstance; }
extern "C" int gui_bridge_self_ed(uint8_t ed[32]) {
    if (!testInstance) return -1;
    memcpy(ed, testInstance->my_ed25519_pubkey, 32); return 0;
}
extern "C" int gui_bridge_post_uasync_owned_fn(void (*fn)(void*), void* arg) {
    if (!testInstance) return -1;
    uasync_call_soon(testInstance->ua, arg, fn); return 0;
}
extern "C" void gui_bridge_post_uasync_fn(void (*fn)(void*), void* arg) {
    const uint8_t* file_server = fn == member_files_gui_request ? ((mf_gui_request*)arg)->server :
        fn == member_files_gui_transfer ? ((mf_transfer_request*)arg)->server : nullptr;
    const uint8_t* server = fn == chat_core_set_my_avatar_trampoline ? ((chat_avatar_req*)arg)->server : nullptr;
    const auto replicaEd = QByteArray::fromHex(REPLICA_ED_HEX);
    if (server && !memcmp(server, testInstance->my_ed25519_pubkey, 32) &&
        !memcmp(((chat_avatar_req*)arg)->group, replicaGroup.constData(), 32)) ownPublications++;
    if ((server && !memcmp(server, replicaEd.constData(), 32)) || (file_server && !memcmp(file_server, replicaEd.constData(), 32))) { QTimer::singleShot(0, [fn, arg] { mockReplica(fn, arg); }); return; }
    if (fn == member_files_gui_transfer && !((mf_transfer_request*)arg)->upload) {
        downloads++; downloadIds.insert(((mf_transfer_request*)arg)->id);
    }
    uasync_call_soon(testInstance->ua, arg, fn);
}
static void avatarEvent(UTUN_INSTANCE*, int type, const uint8_t* data, int length) {
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "avatar test: event type=%d bytes=%d", type, length);
    auto* manager = AvatarManager::instance(); if (!manager) return;
    if (type == CHAT_EVT_MEMBER_FILES_RESPONSE) manager->onResponse(data, length);
    if (type == CHAT_EVT_MEMBER_FILES_TRANSFER) {
        if (length == MF_EVENT_TRANSFER_SIZE && data[MF_EVENT_TRANSFER_STATE] == 2) {
            finished[mf_get64(data)] = data[MF_EVENT_TRANSFER_STATUS] | (int(data[MF_EVENT_TRANSFER_STATUS + 1]) << 8);
            if (deferDownload && downloadIds.contains(mf_get64(data)) && !finished[mf_get64(data)]) {
                delayedDownload = QByteArray((const char*)data, length); return;
            }
        }
        manager->onTransfer(data, length);
    }
    if (type == CHAT_EVT_AVATAR_RESULT) manager->onPublished(data, length);
    if (type == CHAT_EVT_MEMBER_UPDATED && length >= 2 + data[0] + CHAT_MEMBER_DISPLAY_SIZE)
        manager->onMembers(QString::fromLatin1((const char*)data + 1, data[0]), 1, data + 2 + data[0], CHAT_MEMBER_DISPLAY_SIZE, false);
    if (type == CHAT_EVT_MEMBER_LIST && length >= 3 + data[0]) {
        uint16_t count; memcpy(&count, data + 1 + data[0], 2);
        manager->onMembers(QString::fromLatin1((const char*)data + 1, data[0]), count, data + 3 + data[0], length - 3 - data[0]);
    }
}

class AvatarTest : public QObject {
    Q_OBJECT
private slots:
    void uploadPreparationLifetime_data() {
        QTest::addColumn<int>("stage");
        QTest::newRow("delete-from-preparing") << 0;
        QTest::newRow("delete-from-servers-changed") << 1;
        QTest::newRow("delete-from-uploading") << 2;
        QTest::newRow("clear-and-retry") << 3;
    }
    void uploadPreparationLifetime() {
        QFETCH(int, stage);
        debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
        QTemporaryDir directory; QVERIFY(directory.isValid()); UASYNC* ua = uasync_create(); QVERIFY(ua);
        DbManager db; QPointer<AvatarManager> manager;
        auto cleanup = qScopeGuard([&] {
            delete manager.data(); uasync_poll(ua, 0); db.setDb(nullptr);
            if (testInstance) utun_instance_destroy(testInstance);
            testInstance = nullptr; uasync_destroy(ua, 0);
        });
        const char* config = "[global]\n"
            "my_private_key=704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f\n"
            "my_public_key=7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f\n"
            "[chatserver]\ngroup_autoconnect=0\nmember_files_enabled=0\nstorage_autoload=0\n";
        testInstance = utun_instance_create_from_str(ua, config); QVERIFY(testInstance);
        QCOMPARE(utun_core_start(testInstance), 0); QCOMPARE(chat_service_start(testInstance), 0);
        db.setDb(testInstance->topo_sqlite_db); db.setDbPath(directory.path());
        manager = new AvatarManager(&db); manager->initialize(); QTRY_VERIFY_WITH_TIMEOUT(manager->ready(), 3000);
        QImage image(32, 16, QImage::Format_RGB32); image.fill(Qt::red);
        const QString first = directory.filePath("red.png"); QVERIFY(image.save(first));
        if (stage == 0 || stage == 2) connect(manager, &AvatarManager::statusChanged, this, [&](const QString& text) {
            if (text == QString::fromUtf8(stage == 0 ? "Preparing avatar…" : "Uploading avatar…")) delete manager.data();
        });
        if (stage == 1) connect(manager, &AvatarManager::serversChanged, this, [&] { delete manager.data(); });
        manager->uploadDefault(first, QByteArray((const char*)testInstance->my_ed25519_pubkey, 32));
        if (stage != 3) { QTRY_VERIFY_WITH_TIMEOUT(!manager, 3000); return; }
        manager->clearDefault(); image.fill(Qt::blue); const QString second = directory.filePath("blue.png"); QVERIFY(image.save(second));
        manager->uploadDefault(second, QByteArray((const char*)testInstance->my_ed25519_pubkey, 32));
        QTRY_VERIFY_WITH_TIMEOUT(!manager->defaultImage().isNull(), 3000);
        QCOMPARE(manager->defaultImage().pixelColor(128, 128), QColor(Qt::blue));
    }

    void cacheValidation() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        QVERIFY(AvatarCache::initialize(directory.path()));
        QImage source(600, 400, QImage::Format_RGB32); source.fill(Qt::red);
        const QString input = directory.path() + "/input.jpg"; QVERIFY(source.save(input));
        const auto prepared = AvatarCache::prepare(input, directory.path());
        QVERIFY2(!prepared.image.isNull(), qPrintable(prepared.error)); QCOMPARE(prepared.image.size(), QSize(256, 256));
        const QString path = directory.path() + '/' + prepared.hash + ".json";
        QFile original(input), stored(directory.path() + '/' + prepared.record.original);
        QVERIFY(original.open(QIODevice::ReadOnly)); QVERIFY(stored.open(QIODevice::ReadOnly)); QCOMPARE(stored.readAll(),original.readAll());
        QFile webp(directory.path() + '/' + prepared.record.preview); QVERIFY(webp.open(QIODevice::ReadOnly));
        const QByteArray encoded=webp.readAll(); QCOMPARE(encoded.left(4),QByteArray("RIFF")); QVERIFY(encoded.contains("VP8 "));
        QVERIFY(!AvatarCache::load(path, prepared.hash).image.isNull());
        QFile description(path); QVERIFY(description.open(QIODevice::ReadOnly));
        QJsonObject record=QJsonDocument::fromJson(description.readAll()).object(); description.close();
        record["original_bytes"]=1e99;
        const QByteArray oversized=QJsonDocument(record).toJson(QJsonDocument::Compact);
        const QString oversizedName=QCryptographicHash::hash(oversized,QCryptographicHash::Sha256).toHex()+".json";
        QString writeError; QVERIFY(AvatarCache::storeObject(directory.path(),oversizedName,oversized,writeError));
        QVERIFY(!AvatarCache::readObject(directory.path()+'/'+oversizedName,oversizedName).error.isEmpty());
        QVERIFY(AvatarCache::load(path, QString(64, '0')).image.isNull());
        QFile corrupt(path); QVERIFY(corrupt.open(QIODevice::Append)); QCOMPARE(corrupt.write("x"), 1LL); corrupt.close();
        QVERIFY(AvatarCache::load(path, prepared.hash).image.isNull());
        QImage nonSquare(32, 64, QImage::Format_RGB32); nonSquare.fill(Qt::green);
        QByteArray bytes; QBuffer buffer(&bytes); QVERIFY(buffer.open(QIODevice::WriteOnly)); QVERIFY(nonSquare.save(&buffer, "PNG"));
        const QString hash = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
        const QString bad = directory.path() + "/bad.download"; QFile output(bad); QVERIFY(output.open(QIODevice::WriteOnly));
        QCOMPARE(output.write(bytes), qint64(bytes.size())); output.close();
        QVERIFY(!AvatarCache::publish(bad, directory.path(), hash + ".webp").error.isEmpty()); QVERIFY(!QFile::exists(bad));
        QFile partial(directory.path() + "/x.png.utun-123.part"); QVERIFY(partial.open(QIODevice::WriteOnly)); partial.close();
        QVERIFY(AvatarCache::initialize(directory.path())); QVERIFY(!partial.exists());
    }

    void selectedSquarePreservesOriginal() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        QImage source(800,400,QImage::Format_RGB32); source.fill(Qt::blue);
        QPainter painter(&source); painter.fillRect(QRect(0,0,400,400),Qt::red); painter.end();
        const QString input=directory.path()+"/source.png"; QVERIFY(source.save(input));
        const auto prepared=AvatarCache::prepare(input,directory.path(),QRectF(0,0,0.5,1));
        QVERIFY2(prepared.error.isEmpty(),qPrintable(prepared.error)); QCOMPARE(prepared.record.crop,QRectF(0,0,0.5,1));
        const auto loaded=AvatarCache::load(directory.path()+'/'+prepared.hash+".json",prepared.hash);
        QVERIFY2(loaded.error.isEmpty(),qPrintable(loaded.error)); QVERIFY(loaded.image.pixelColor(128,128).red()>240);
        const auto full=AvatarCache::readObject(directory.path()+'/'+prepared.record.original,prepared.record.original);
        QCOMPARE(full.image,source);
        QVERIFY(!AvatarCache::prepare(input,directory.path(),QRectF(0.7,0,0.5,1)).error.isEmpty());
    }

    void exifRotationKeepsOriginalBytesAndSelectedArea() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        QImage image(800,400,QImage::Format_RGB32); image.fill(Qt::blue);
        QPainter painter(&image); painter.fillRect(QRect(0,0,400,400),Qt::red); painter.end();
        QByteArray jpeg; QBuffer buffer(&jpeg); QVERIFY(buffer.open(QIODevice::WriteOnly)); QVERIFY(image.save(&buffer,"JPEG",95));
        const QByteArray exif=QByteArray::fromHex("ffe1002245786966000049492a0008000000010012010300010000000600000000000000");
        const QByteArray original=jpeg.left(2)+exif+jpeg.mid(2);
        const QString path=directory.path()+"/rotated.jpg"; QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(original),qint64(original.size())); file.close();
        const auto prepared=AvatarCache::prepare(path,directory.path(),QRectF(0,0,1,0.5));
        QVERIFY2(prepared.error.isEmpty(),qPrintable(prepared.error)); QCOMPARE(prepared.record.size,QSize(400,800));
        const auto loaded=AvatarCache::load(directory.path()+'/'+prepared.hash+".json",prepared.hash);
        QVERIFY(loaded.image.pixelColor(128,128).red()>240);
        QFile stored(directory.path()+'/'+prepared.record.original); QVERIFY(stored.open(QIODevice::ReadOnly)); QCOMPARE(stored.readAll(),original);
    }

    void editorDragSelectsTheEdgeAndCancelDoesNotPrepareFiles() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        QImage image(800,400,QImage::Format_RGB32); image.fill(Qt::blue);
        const QString source=directory.path()+"/source.png"; QVERIFY(image.save(source));
        QTimer automate; automate.setInterval(10);
        connect(&automate,&QTimer::timeout,this,[] {
            auto* dialog=QApplication::activeModalWidget(); if (!dialog) return;
            auto* view=dialog->findChild<QWidget*>("avatarCropView"); auto* upload=dialog->findChild<QPushButton*>("avatarCropUpload");
            if (!view || !upload || !upload->isEnabled()) return;
            const QPoint center=view->rect().center();
            QTest::mousePress(view,Qt::LeftButton,Qt::NoModifier,center);
            const QPoint edge=center+QPoint(view->width()/2-5,0);
            QTest::mouseMove(view,edge); QTest::mouseRelease(view,Qt::LeftButton,Qt::NoModifier,edge);
            QTest::mouseClick(upload,Qt::LeftButton);
        });
        automate.start(); QRectF crop; QVERIFY(selectAvatarCrop(source,crop,nullptr)); automate.stop();
        QVERIFY(qAbs(crop.x())<0.001); QVERIFY(qAbs(crop.y())<0.001);
        QVERIFY(qAbs(crop.width()-0.5)<0.001); QVERIFY(qAbs(crop.height()-1)<0.001);
        const QString panorama=directory.path()+"/panorama.png";
        QImage wide(14003,1001,QImage::Format_RGB32); wide.fill(Qt::blue); QVERIFY(wide.save(panorama));
        automate.start(); QVERIFY(selectAvatarCrop(panorama,crop,nullptr)); automate.stop();
        const auto prepared=AvatarCache::prepare(panorama,directory.path(),crop);
        QVERIFY2(prepared.error.isEmpty(),qPrintable(prepared.error));
        QVERIFY(qAbs(prepared.record.crop.width()*14003-prepared.record.crop.height()*1001)<=1);
        const int preparedFiles=QDir(directory.path()).entryList({"*.json","*.webp"},QDir::Files).size();
        QTimer::singleShot(10,[] { if (auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget())) dialog->reject(); });
        QVERIFY(!selectAvatarCrop(source,crop,nullptr));
        QCOMPARE(QDir(directory.path()).entryList({"*.json","*.webp"},QDir::Files).size(),preparedFiles);
    }

    void uploadReplicateCacheRestartAndRemove() {
        debug_config_init(); debug_set_level(DEBUG_LEVEL_INFO);
        debug_set_category_level(DEBUG_CATEGORY_CHAT_SYNC, DEBUG_LEVEL_DEBUG);
        debug_set_category_level(DEBUG_CATEGORY_MEDIA, DEBUG_LEVEL_DEBUG);
        debug_set_category_level(DEBUG_CATEGORY_SYS, DEBUG_LEVEL_ERROR);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        UASYNC* ua = uasync_create(); QVERIFY(ua);
        const QByteArray config = "[global]\nmy_private_key=704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f\n"
            "my_public_key=7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f\n"
            "[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\nmember_files_path=" + directory.path().toUtf8() + "/storage\n";
        testInstance = utun_instance_create_from_str(ua, config.constData()); QVERIFY(testInstance);
        QVERIFY(utun_core_start(testInstance) == 0); QVERIFY(chat_service_start(testInstance) == 0);
        chat_core_create_channel_auto(testInstance, "one"); chat_core_create_channel_auto(testInstance, "two");
        DbManager db; db.setDb(testInstance->topo_sqlite_db); db.setDbPath(directory.path());
        const auto channels = db.getChannels(); QCOMPARE(channels.size(), 2);
        QTimer poll; connect(&poll, &QTimer::timeout, this, [ua] { uasync_poll(ua, 0); }); poll.start(1);
        chat_event_set_handler(testInstance, avatarEvent);
        const QString source = directory.path() + "/photo.png"; QImage image(300, 200, QImage::Format_RGB32); image.fill(Qt::blue);
        QVERIFY(image.save(source));
        QString cached;
        {
            AvatarManager manager(&db); manager.initialize();
            QTRY_VERIFY_WITH_TIMEOUT(manager.ready(), 5000);
            QTRY_COMPARE_WITH_TIMEOUT(manager.servers().size(), 1, 5000);
            QSignalSpy unchangedServers(&manager, &AvatarManager::serversChanged);
            for (const auto& channel : channels) {
                uint8_t wire[CHAT_MEMBER_DISPLAY_SIZE];
                QCOMPARE(chat_core_get_single_member(testInstance, channel.channelId.toLatin1().constData(), testInstance->my_ed25519_pubkey, wire), 0);
                manager.onMembers(channel.channelId, 1, wire, sizeof(wire), false);
            }
            QCOMPARE(unchangedServers.count(), 0);
            manager.uploadDefault(source, QByteArray((const char*)testInstance->my_ed25519_pubkey, 32));
            QTRY_VERIFY_WITH_TIMEOUT(!manager.defaultImage().isNull(), 5000);
            for (const auto& channel : channels) {
                uint8_t wire[CHAT_MEMBER_DISPLAY_SIZE];
                QTRY_VERIFY_WITH_TIMEOUT(chat_core_get_single_member(testInstance, channel.channelId.toLatin1().constData(), testInstance->my_ed25519_pubkey, wire) == 0 &&
                    !memcmp(wire + 71, testInstance->my_ed25519_pubkey, 32), 10000);
                chat_member_display member{}; chat_core_deserialize_member(wire, &member);
                QVERIFY(mf_avatar_file_valid(member.avatar_file));
                QCOMPARE(manager.storageLocation(channel.channelId, QByteArray((const char*)testInstance->my_ed25519_pubkey, 32)), QString("On your device"));
                MemberListModel model; model.setChannel(channel.channelId, true);
                member.flags |= CHAT_MEMBER_FLAG_SUPERNODE; member.storage = 1;
                chat_core_serialize_member(&member, wire); model.onMemberList(channel.channelId, 1, wire, sizeof(wire));
                const QImage rowAvatar = model.data(model.index(0), Qt::DecorationRole).value<QIcon>().pixmap(34,34).toImage();
                QVERIFY(rowAvatar.pixelColor(17,17).blue() > 240);

                QVERIFY(QFile::exists(directory.path() + "/storage/" + channel.channelId + '/' + QString::fromLatin1(QByteArray((const char*)testInstance->my_ed25519_pubkey, 32).toHex()) +
                    "/avatars/" + member.avatar_file));
                const QString root=directory.path()+"/storage/"+channel.channelId+'/'+QString::fromLatin1(QByteArray((const char*)testInstance->my_ed25519_pubkey, 32).toHex())+"/avatars/";
                const auto bundle=AvatarCache::readObject(root+member.avatar_file,member.avatar_file);
                QVERIFY2(bundle.error.isEmpty(),qPrintable(bundle.error));
                QFile original(source), uploaded(root+bundle.record.original);
                QVERIFY(original.open(QIODevice::ReadOnly)); QVERIFY(uploaded.open(QIODevice::ReadOnly)); QCOMPARE(uploaded.readAll(),original.readAll());
                QVERIFY(QFile::exists(root+bundle.record.preview));
                QVERIFY(!manager.image(channel.channelId, QByteArray((const char*)testInstance->my_ed25519_pubkey, 32)).isNull());
                QVERIFY(!manager.image("pm:test", QByteArray((const char*)testInstance->my_ed25519_pubkey, 32)).isNull());
                cached = directory.path() + "/avatar_cache/" + QString::fromLatin1(QByteArray((const char*)testInstance->my_ed25519_pubkey, 32).toHex()) + '/' + member.avatar_file;
            }
            auto* limited = (mf_transfer_request*)u_calloc(1, sizeof(mf_transfer_request)); QVERIFY(limited);
            limited->inst = testInstance; limited->id = 41; memcpy(limited->group_key, QByteArray::fromHex(channels[0].channelId.toLatin1()).constData(), 32);
            memcpy(limited->server, testInstance->my_ed25519_pubkey, 32); memcpy(limited->owner, limited->server, 32); limited->max_bytes = 1;
            const QByteArray remote = "avatars/" + QFileInfo(cached).fileName().toLatin1();
            const QString rejected = directory.path() + "/rejected.png";
            const QByteArray local = rejected.toUtf8(); memcpy(limited->remote, remote.constData(), remote.size());
            memcpy(limited->local, local.constData(), local.size()); gui_bridge_post_uasync_fn(member_files_gui_transfer, limited);
            QTRY_VERIFY_WITH_TIMEOUT(finished.contains(41), 5000); QCOMPARE(finished.take(41), int(MF_INVALID_RANGE));
            QVERIFY(!QFile::exists(rejected));
            chat_core_update_my_name(testInstance, "quote\"\\name"); chat_core_sync_my_addresses(testInstance);
            for (const auto& channel : channels) {
                char profile[MS_USERINFO_MAX + 1], name[128]; uint8_t signature[64]; uint64_t timestamp;
                QCOMPARE(chat_core_get_my_profile(testInstance, channel.channelId.toLatin1().constData(), profile, sizeof(profile), signature, &timestamp), 0);
                QCOMPARE(json_flat_get(profile, "name", name, sizeof(name)), 0); QCOMPARE(QString::fromLatin1(name), QString("quote\"\\name"));
                uint8_t joinId[32], signedBytes[MS_USERINFO_MAX + 41];
                QCOMPARE(member_sync_join_id_read(testInstance->topo_sqlite_db, channel.channelId.toLatin1().constData(),
                    testInstance->my_ed25519_pubkey, testInstance->my_keys.public_key, joinId), 0);
                int length = member_sync_build_update_msg(joinId, timestamp, profile, signedBytes, sizeof(signedBytes)); QVERIFY(length > 0);
                QCOMPARE(sc_ed25519_verify(testInstance->my_ed25519_pubkey, signedBytes, length, signature), SC_OK);
                uint8_t wire[CHAT_MEMBER_DISPLAY_SIZE]; QCOMPARE(chat_core_get_single_member(testInstance, channel.channelId.toLatin1().constData(), testInstance->my_ed25519_pubkey, wire), 0);
                QCOMPARE(memcmp(wire + 71, testInstance->my_ed25519_pubkey, 32), 0);
                QVERIFY(manager.image(channel.channelId, QByteArray((const char*)testInstance->my_ed25519_pubkey, 32)).pixelColor(128,128).blue()>240);
            }
            uasync_poll(ua, 0);
        }
        downloads = 0;
        {
            AvatarManager manager(&db); manager.initialize();
            QTRY_VERIFY_WITH_TIMEOUT(!manager.image(channels[0].channelId, QByteArray((const char*)testInstance->my_ed25519_pubkey, 32)).isNull(), 5000);
            QCOMPARE(downloads, 0u);
        }
        QVERIFY(QFile::remove(cached)); downloads = 0;
        {
            AvatarManager manager(&db); manager.initialize();
            deferDownload = true; delayedDownload.clear();
            QTRY_VERIFY_WITH_TIMEOUT((manager.image(channels[0].channelId, QByteArray((const char*)testInstance->my_ed25519_pubkey, 32)), !delayedDownload.isEmpty()), 10000);
            QCOMPARE(downloads, 1u);
            const QString replacement = directory.path() + "/replacement.png"; image.fill(Qt::red); QVERIFY(image.save(replacement));
            manager.uploadDefault(replacement, QByteArray((const char*)testInstance->my_ed25519_pubkey, 32));
            auto isRed = [&manager, &db](const QString& channel) {
                const QImage current = manager.image(channel, QByteArray((const char*)testInstance->my_ed25519_pubkey, 32));
                return !current.isNull() && current.pixelColor(128,128).red()>240;
            };
            for (const auto& channel : channels)
                QTRY_VERIFY_WITH_TIMEOUT(isRed(channel.channelId), 10000);
            deferDownload = false; manager.onTransfer((const uint8_t*)delayedDownload.constData(), delayedDownload.size());
            QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(cached), 5000);
            for (const auto& channel : channels) QVERIFY(manager.image(channel.channelId,QByteArray((const char*)testInstance->my_ed25519_pubkey, 32)).pixelColor(128,128).red()>240);
            manager.openGallery(channels[0].channelId,QByteArray((const char*)testInstance->my_ed25519_pubkey, 32));
            QTRY_COMPARE_WITH_TIMEOUT(manager.gallery().size(),2,5000);
            const QString selected=manager.galleryCurrent();
            QTRY_VERIFY_WITH_TIMEOUT(!manager.galleryImage(selected,true).isNull(),10000);
            QCOMPARE(manager.galleryImage(selected,true).size(),QSize(300,200));
            QVERIFY(manager.galleryImage(selected,true).pixelColor(0,0).red()>240);
            manager.closeGallery();
            showAvatarGallery(channels[0].channelId, QByteArray((const char*)testInstance->my_ed25519_pubkey, 32));
            QDialog* gallery=nullptr;
            for (auto* widget : QApplication::topLevelWidgets()) if (widget->windowTitle()=="Avatars") gallery=qobject_cast<QDialog*>(widget);
            QVERIFY(gallery); auto* viewer=gallery->findChild<ImageViewerWindow*>(); QVERIFY(viewer);
            QTRY_VERIFY_WITH_TIMEOUT(viewer->grab().toImage().pixelColor(viewer->rect().center()).red()>240,5000);
            for (int i=0;i<5;i++) QVERIFY(gallery->findChild<QPushButton*>(QString("avatarPreview%1").arg(i)));
            QTRY_COMPARE_WITH_TIMEOUT(manager.gallery().size(),2,5000);
            auto* next=gallery->findChild<QPushButton*>(manager.gallery().indexOf(selected)==0 ? "avatarNext" : "avatarPrevious");
            QVERIFY(next); QTest::mouseClick(next,Qt::LeftButton);
            QTRY_VERIFY_WITH_TIMEOUT(viewer->grab().toImage().pixelColor(viewer->rect().center()).blue()>240,5000);
            gallery->close(); QTRY_VERIFY_WITH_TIMEOUT(manager.gallery().isEmpty(),5000);
            QHash<QString,QString> previous;
            auto reference=[&](const QString& channel) {
                uint8_t wire[CHAT_MEMBER_DISPLAY_SIZE]; chat_member_display member{};
                if (chat_core_get_single_member(testInstance,channel.toLatin1().constData(),testInstance->my_ed25519_pubkey,wire)<0) return QString();
                chat_core_deserialize_member(wire,&member); return QString::fromLatin1(member.avatar_file);
            };
            for (const auto& channel : channels) previous[channel.channelId]=reference(channel.channelId);
            for (int i=0;i<4;i++) {
                image.fill(QColor::fromHsv(60+i*40,255,230)); const QString path=directory.path()+QString("/extra-%1.png").arg(i);
                QVERIFY(image.save(path)); manager.uploadDefault(path,QByteArray((const char*)testInstance->my_ed25519_pubkey, 32));
                auto published=[&] {
                    for (const auto& channel : channels) if (reference(channel.channelId)==previous[channel.channelId]) return false;
                    return true;
                };
                QTRY_VERIFY_WITH_TIMEOUT(published(),10000);
                for (const auto& channel : channels) previous[channel.channelId]=reference(channel.channelId);
            }
            showAvatarGallery(channels[0].channelId, QByteArray((const char*)testInstance->my_ed25519_pubkey, 32)); gallery=nullptr;
            for (auto* widget : QApplication::topLevelWidgets()) if (widget->windowTitle()=="Avatars") gallery=qobject_cast<QDialog*>(widget);
            QVERIFY(gallery); QTRY_COMPARE_WITH_TIMEOUT(manager.gallery().size(),6,5000);
            const auto album=manager.gallery(); int cursor=album.indexOf(manager.galleryCurrent());
            while (cursor!=2) {
                auto* button=gallery->findChild<QPushButton*>(cursor<2 ? "avatarNext" : "avatarPrevious"); QVERIFY(button);
                QTest::mouseClick(button,Qt::LeftButton); cursor+=cursor<2 ? 1 : -1;
            }
            for (int i=0;i<5;i++) QVERIFY(gallery->findChild<QPushButton*>(QString("avatarPreview%1").arg(i))->isEnabled());
            QTRY_VERIFY_WITH_TIMEOUT(!manager.galleryImage(album[2],true).isNull(),5000);
            const QColor expected=manager.galleryImage(album[2],true).pixelColor(0,0); viewer=gallery->findChild<ImageViewerWindow*>();
            QTRY_COMPARE_WITH_TIMEOUT(viewer->grab().toImage().pixelColor(viewer->rect().center()),expected,5000);
            gallery->close(); QTRY_VERIFY_WITH_TIMEOUT(manager.gallery().isEmpty(),5000);
            // GUI-машина публикации: узел storage без super не выбирается; сбой сохраняет ссылку.
            const QString channel = channels[0].channelId; replicaGroup = QByteArray::fromHex(channel.toLatin1()); ownPublications = 0;
            QCOMPARE(sqlite3_exec(testInstance->topo_sqlite_db,
                "INSERT OR IGNORE INTO nodes(node_id,x25519_pubkey,ed25519_pubkey) VALUES(19088743,zeroblob(32),x'" REPLICA_ED_HEX "')",
                nullptr, nullptr, nullptr), SQLITE_OK);
            chat_member_display super{}; super.storage_role = 1;
            memcpy(super.ed25519, QByteArray::fromHex(REPLICA_ED_HEX).constData(), 32);
            uint8_t superWire[CHAT_MEMBER_DISPLAY_SIZE]; chat_core_serialize_member(&super, superWire);
            replicaUploads = replicaPublications = 0; replicaAvailable = false;
            manager.onMembers(channel, 1, superWire, sizeof(superWire), false);
            QTest::qWait(100); QCOMPARE(replicaUploads, 0u);
            super.flags = CHAT_MEMBER_FLAG_SUPERNODE; super.storage_role = 0; chat_core_serialize_member(&super, superWire);
            manager.onMembers(channel, 1, superWire, sizeof(superWire), false);
            QTest::qWait(100); QCOMPARE(replicaUploads, 0u);
            super.storage_role = 1; chat_core_serialize_member(&super, superWire);
            manager.onMembers(channel, 1, superWire, sizeof(superWire), false);
            QTest::qWait(100); QCOMPARE(replicaPublications, 0u);
            replicaAvailable = true;
            QTRY_COMPARE_WITH_TIMEOUT(replicaPublications, 1u, 12000); QCOMPARE(replicaUploads, 3u);
            QVERIFY(manager.storageLocation(channel, QByteArray((const char*)testInstance->my_ed25519_pubkey, 32)).startsWith("On node:"));
            const QString publishedFile = replicaFile; const unsigned ownBefore = ownPublications;
            replicaAvailable = false;
            image.fill(Qt::magenta); const QString pending = directory.path()+"/pending.png"; QVERIFY(image.save(pending));
            manager.uploadDefault(pending, QByteArray((const char*)testInstance->my_ed25519_pubkey, 32));
            QTRY_COMPARE_WITH_TIMEOUT(manager.defaultImage().pixelColor(128,128), QColor(Qt::magenta), 5000);
            QTest::qWait(2500);
            QVERIFY(manager.storageLocation(channel, QByteArray((const char*)testInstance->my_ed25519_pubkey, 32)).startsWith("New avatar saved on your device; published: On node:"));
            QCOMPARE(replicaPublications, 1u); QCOMPARE(replicaFile, publishedFile); QCOMPARE(ownPublications, ownBefore);
            replicaAvailable = true;
            QTRY_COMPARE_WITH_TIMEOUT(replicaPublications, 2u, 12000); QVERIFY(replicaFile != publishedFile);
            QCOMPARE(replicaUploads, 6u);
            super.flags |= CHAT_MEMBER_FLAG_DELETED; chat_core_serialize_member(&super, superWire);
            manager.onMembers(channel, 1, superWire, sizeof(superWire), false);
            QTest::qWait(2200); QCOMPARE(ownPublications, ownBefore); QCOMPARE(replicaPublications, 2u);
            manager.clearDefault();
            for (const auto& channel : channels) {
                uint8_t wire[CHAT_MEMBER_DISPLAY_SIZE];
                QTRY_VERIFY_WITH_TIMEOUT(chat_core_get_single_member(testInstance, channel.channelId.toLatin1().constData(), testInstance->my_ed25519_pubkey, wire) == 0 &&
                    mf_zero(wire + 71, 32), 5000);
                QVERIFY(manager.image(channel.channelId, QByteArray((const char*)testInstance->my_ed25519_pubkey, 32)).isNull());
            }
        }
        poll.stop(); chat_event_set_handler(testInstance, nullptr); db.setDb(nullptr);
        utun_instance_destroy(testInstance); testInstance = nullptr; uasync_destroy(ua, 0);
    }
};
QTEST_MAIN(AvatarTest)
#include "test_avatars.moc"

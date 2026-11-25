#include "member_files_fixture.h"
/* Реальный браузер Qt с захватом bridge-команд: выбор подписанного storage через
 * NAT, права чужого UID и отбрасывание старого LIST после смены каталога. */
#include "../src/member_files/browser.h"
#include "../db/db_manager.h"
#include "../transport/gui_bridge.h"
#include "../src/channellist.h"
#include "../src/channeldelegate.h"
#include <QtTest>
#include <QComboBox>
#include <QTreeWidget>
#include <QPushButton>
#include <QLabel>
#include <QLocale>
#include <QInputDialog>
#include <QMessageBox>
#include <QTemporaryDir>
#include <QFile>
#include <QMimeData>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QLineEdit>
#include <QAction>
#include <QClipboard>
#include <QDrag>
#include <QMouseEvent>
#include <QScopeGuard>
#include <QChildEvent>
#include <QMenu>
#include <functional>
extern "C" {
#include "member_files/member_files.h"
#include "member_files/storage.h"
#include "mem.h"
#include "debug_config.h"
}
static mf_gui_request captured;
static mf_gui_request quotaCaptured;
static QList<mf_transfer_request> transfers;
static QList<quint64> cancelled;
static bool ready = true;
static std::function<void(mf_transfer_request)> transferStarted;
static std::function<void(mf_gui_request)> requestStarted;
extern "C" int gui_bridge_is_uasync_ready(void) { return ready; }
extern "C" UTUN_INSTANCE* gui_bridge_get_inst(void) { return nullptr; }
extern "C" int gui_bridge_post_uasync_owned_fn(void (*)(void*), void*) { return -1; }
extern "C" void gui_bridge_post_uasync_fn(void (*function)(void*), void* arg) {
    if (function == member_files_gui_request) {
        auto* request = (mf_gui_request*)arg;
        if (request->message.command == MF_QUOTA) quotaCaptured = *request;
        else captured = *request;
        if (requestStarted) requestStarted(*request);
    }
    if (function == member_files_gui_transfer) {
        auto request = *(mf_transfer_request*)arg; transfers.append(request);
        if (transferStarted) transferStarted(request);
    }
    if (function == member_files_gui_cancel) cancelled.append(((mf_cancel_request*)arg)->id);
    u_free(arg);
}
extern "C" int gui_bridge_self_ed(uint8_t ed[32]) { memcpy(ed, fileKey(22).constData(), 32); return 0; }
extern "C" int gui_bridge_member_files_request(const uint8_t group[32], const uint8_t server[32], const mf_message* message) {
    if (!ready) return -1;
    mf_gui_request request{}; memcpy(request.group, group, 32); memcpy(request.server, server, 32); request.message = *message;
    if (message->command == MF_QUOTA) quotaCaptured = request;
    else captured = request;
    if (requestStarted) requestStarted(request);
    return 0;
}
extern "C" int gui_bridge_member_files_transfer(const mf_transfer_request* request) {
    if (!ready) return -1;
    transfers.append(*request); if (transferStarted) transferStarted(*request);
    return 0;
}
extern "C" int gui_bridge_member_files_cancel(uint64_t id) {
    if (!id) return 0;
    if (!ready) return -1;
    cancelled.append(id); return 0;
}

static void answerDialog(int result) {
    QTimer::singleShot(0, [result] {
        if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) box->button((QMessageBox::StandardButton)result)->click();
        else if (auto* input = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) input->done(result);
    });
}

static void respond(MemberFileBrowser& browser, const mf_gui_request& request, uint16_t status, uint8_t type = MF_FILE) {
    mf_message r = request.message; r.kind = MF_RESPONSE; r.status = status; r.type = type;
    memset(r.version, 7, sizeof(r.version));
    uint8_t bytes[MF_EVENT_RESPONSE_PREFIX + MF_WIRE_MAX]; memcpy(bytes, request.group, 32); memcpy(bytes + 32, request.server, 32);
    int size = mf_encode(&r, bytes + MF_EVENT_RESPONSE_PREFIX, MF_WIRE_MAX); QVERIFY(size > 0); browser.onResponse(bytes, size + MF_EVENT_RESPONSE_PREFIX);
}

static void complete(MemberFileBrowser& browser, const mf_transfer_request& request, uint16_t status = MF_OK) {
    uint8_t bytes[MF_EVENT_TRANSFER_SIZE]{}; mf_put64(bytes, request.id); memcpy(bytes + MF_EVENT_TRANSFER_GROUP, request.group_key, 32);
    memcpy(bytes + MF_EVENT_TRANSFER_SERVER, request.server, 32); memcpy(bytes + MF_EVENT_TRANSFER_OWNER, request.owner, 32);
    bytes[MF_EVENT_TRANSFER_STATUS] = status; bytes[MF_EVENT_TRANSFER_STATUS + 1] = status >> 8; bytes[MF_EVENT_TRANSFER_STATE] = 2; browser.onTransfer(bytes, sizeof(bytes));
}

static QPushButton* cancelButton(MemberFileBrowser& browser) {
    for (auto* button : browser.findChildren<QPushButton*>()) if (button->text() == "Cancel transfer") return button;
    return nullptr;
}

// Offscreen не имеет native drag. Получатель запрашивает реальный MIME при публикации QDrag приложению.
class DragReceiver : public QObject {
public:
    std::function<void(const QMimeData*)> receive;
protected:
    bool eventFilter(QObject* object, QEvent* event) override {
        if (object == QCoreApplication::instance() && event->type() == QEvent::ChildAdded) {
            auto* drag = qobject_cast<QDrag*>(static_cast<QChildEvent*>(event)->child());
            if (drag && drag->mimeData()) receive(drag->mimeData());
        }
        return false;
    }
};

class BrowserTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
        debug_set_category_level(DEBUG_CATEGORY_MEDIA, DEBUG_LEVEL_DEBUG);
    }
    void init() { transfers.clear(); cancelled.clear(); ready = true; transferStarted = {}; requestStarted = {}; }

    void selectionAndRecursiveClipboard() {
        QTemporaryDir cache; QVERIFY(cache.isValid()); QByteArray oldCache = qgetenv("XDG_CACHE_HOME");
        qputenv("XDG_CACHE_HOME", cache.path().toUtf8());
        auto restore = qScopeGuard([oldCache] { qputenv("XDG_CACHE_HOME", oldCache); QApplication::clipboard()->clear(); });
        sqlite3* sql = nullptr; QCOMPARE(sqlite3_open(":memory:", &sql), SQLITE_OK);
        QCOMPARE(sqlite3_exec(sql, "CREATE TABLE accounts(node_id INTEGER,display_name TEXT);"
            "INSERT INTO accounts VALUES(22,'Me'),(33,'Server');"
            "CREATE TABLE peers_" TEST_FILE_GROUP "(node_id INTEGER,adm_tags TEXT,adm_tags_sig BLOB,deleted INTEGER,source INTEGER);"
            "INSERT INTO peers_" TEST_FILE_GROUP " VALUES(22,'{}',NULL,0,0),(33,'{\"storage\":\"yes\"}',zeroblob(64),0,0);"
            "ALTER TABLE peers_" TEST_FILE_GROUP " ADD COLUMN record_state INTEGER NOT NULL DEFAULT 0;"
            "ALTER TABLE peers_" TEST_FILE_GROUP " ADD COLUMN ed25519_pubkey BLOB;"
            "ALTER TABLE peers_" TEST_FILE_GROUP " ADD COLUMN local_nick TEXT;"
            "UPDATE peers_" TEST_FILE_GROUP " SET ed25519_pubkey=CASE node_id WHEN 11 THEN x'" TEST_FILE_ED_11 "'"
            " WHEN 22 THEN x'" TEST_FILE_ED_22 "' WHEN 33 THEN x'" TEST_FILE_ED_33 "' END,"
            " local_nick=(SELECT display_name FROM accounts a WHERE a.node_id=peers_" TEST_FILE_GROUP ".node_id);",
            nullptr, nullptr, nullptr), SQLITE_OK);
        DbManager db; db.setDb(sql);
        {
            MemberFileBrowser browser(&db); browser.setChannel(TEST_FILE_GROUP, fileKey(22)); browser.show(); browser.activateWindow();
            auto* tree = browser.findChild<QTreeWidget*>();
            for (const auto& name : {"folder", "loose.txt", "skip.txt", "last.txt"}) {
                auto* item = new QTreeWidgetItem(tree, {name}); item->setData(0, Qt::UserRole, name);
                item->setData(0, Qt::UserRole + 1, QString(name) == "folder" ? MF_DIRECTORY : MF_FILE);
                item->setData(0, Qt::UserRole + 2, QByteArray(16, '\7'));
            }
            tree->setFocus(); QTRY_VERIFY(tree->hasFocus());
            auto click = [tree](int row, Qt::KeyboardModifiers modifiers) {
                QTest::mouseClick(tree->viewport(), Qt::LeftButton, modifiers, tree->visualItemRect(tree->topLevelItem(row)).center());
            };
            click(0, Qt::NoModifier); click(2, Qt::ShiftModifier); QCOMPARE(tree->selectedItems().size(), 3);
            click(1, Qt::ControlModifier); QCOMPARE(tree->selectedItems().size(), 2);
            QTest::keyClick(tree, Qt::Key_A, Qt::ControlModifier); QCOMPARE(tree->selectedItems().size(), 4);
            click(0, Qt::NoModifier); click(1, Qt::ShiftModifier); QCOMPARE(tree->selectedItems().size(), 2);
            QTimer::singleShot(0, [] { if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget())) menu->close(); });
            tree->customContextMenuRequested(tree->visualItemRect(tree->topLevelItem(1)).center());
            QCOMPARE(tree->selectedItems().size(), 2);
            QMap<QString, QStringList> directories{{"folder", {"empty", "nested", "inside.txt"}}, {"folder/empty", {}},
                                                  {"folder/nested", {"deep.txt"}}};
            int pages = 0, verified = 0;
            requestStarted = [&](mf_gui_request request) {
                QTimer::singleShot(0, &browser, [&, request] {
                    mf_message r = request.message; r.kind = MF_RESPONSE; r.status = MF_OK; r.data_len = 0;
                    QString path = QString::fromUtf8(r.path); r.type = directories.contains(path) ? MF_DIRECTORY : MF_FILE;
                    memset(r.version, 7, 16);
                    if (r.command == MF_LIST) {
                        pages++; auto names = directories.value(path);
                        if (r.cursor < quint64(names.size())) {
                            QByteArray name = names[int(r.cursor)].toUtf8(); QString child = path + '/' + QString::fromUtf8(name);
                            r.data[0] = uint8_t(name.size()); r.data[2] = directories.contains(child) ? MF_DIRECTORY : MF_FILE;
                            memset(r.data + 19, 7, 16); memcpy(r.data + MF_ENTRY_HEADER, name.constData(), name.size());
                            r.data_len = MF_ENTRY_HEADER + name.size(); r.cursor++; r.more = r.cursor < quint64(names.size());
                        }
                    } else if (r.command == MF_STAT) verified++;
                    else return;
                    uint8_t bytes[MF_EVENT_RESPONSE_PREFIX + MF_WIRE_MAX]; memcpy(bytes, request.group, 32); memcpy(bytes + 32, request.server, 32);
                    int size = mf_encode(&r, bytes + MF_EVENT_RESPONSE_PREFIX, MF_WIRE_MAX); QVERIFY(size > 0); browser.onResponse(bytes, size + MF_EVENT_RESPONSE_PREFIX);
                });
            };
            transferStarted = [&](mf_transfer_request request) {
                QTimer::singleShot(0, &browser, [&, request] {
                    QFile file(QString::fromUtf8(request.local)); QVERIFY(file.open(QIODevice::WriteOnly));
                    QByteArray bytes(request.remote); QCOMPARE(file.write(bytes), qint64(bytes.size())); file.close(); complete(browser, request);
                });
            };
            auto* copy = browser.findChild<QAction*>("memberFilesCopy"); copy->trigger();
            QTRY_VERIFY(copy->isEnabled()); QCOMPARE(pages, 5); QCOMPARE(verified, 3); QCOMPARE(transfers.size(), 3);
            auto urls = QApplication::clipboard()->mimeData()->urls(); QCOMPARE(urls.size(), 2);
            QString folder = urls[0].toLocalFile(); QVERIFY(QFileInfo(folder + "/empty").isDir());
            QFile deep(folder + "/nested/deep.txt"); QVERIFY(deep.open(QIODevice::ReadOnly));
            QCOMPARE(deep.readAll(), QByteArray("folder/nested/deep.txt"));
            QVERIFY(QFileInfo(urls[1].toLocalFile()).isFile());
            // Опциональная интеграция с настоящим системным менеджером: координаты читает внешний XTest-драйвер.
            QString nativeDestination = qEnvironmentVariable("UTUN_MEMBER_FILES_NATIVE_DEST");
            if (!nativeDestination.isEmpty()) {
                for (int i = 0; i < 120; ++i) directories["folder"].append(QString("many-%1.txt").arg(i));
                browser.setWindowTitle("uTun native file copy test"); browser.resize(700, 600); browser.move(20, 40);
                browser.raise(); browser.activateWindow();
                QTRY_VERIFY(browser.isActiveWindow());
                QPoint source = tree->viewport()->mapToGlobal(tree->visualItemRect(tree->topLevelItem(0)).center());
                QFile control(nativeDestination + "/source-position"); QVERIFY(control.open(QIODevice::WriteOnly));
                control.write(QByteArray::number(source.x()) + ' ' + QByteArray::number(source.y())); control.close();
                auto allCopied = [&] {
                    QStringList files{"loose.txt", "folder/inside.txt", "folder/nested/deep.txt"};
                    for (int i = 0; i < 120; ++i) files.append(QString("folder/many-%1.txt").arg(i));
                    for (const auto& path : files) {
                        QFile received(nativeDestination + '/' + path);
                        if (!received.open(QIODevice::ReadOnly) || received.readAll() != path.toUtf8()) return false;
                    }
                    return QFileInfo(nativeDestination + "/folder/empty").isDir();
                };
                QTRY_VERIFY_WITH_TIMEOUT(allCopied(), 60000);
                QCOMPARE(transfers.size(), 126); // По три исходных файла и ещё 120 в native drag.
            }
            requestStarted = {}; transferStarted = {};
        }
        db.setDb(nullptr); QCOMPARE(sqlite3_close(sql), SQLITE_OK);
    }

    void recursiveUploadAndPreflight() {
        QTemporaryDir root; QVERIFY(root.isValid()); QVERIFY(QDir(root.path()).mkpath("folder/empty"));
        QVERIFY(QDir(root.path()).mkpath("folder/nested"));
        QString source = root.filePath("folder/nested/file.txt");
        { QFile file(source); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write("data"), 4LL); }
        MemberFileBatch batch(QByteArray::fromHex(TEST_FILE_GROUP), fileKey(33), fileKey(22), MemberFileBatch::Upload, nullptr);
        bool done = false, success = false; QString summary; QStringList created;
        batch.progress = [](const QString&, int) {};
        batch.conflict = [](const QString&, bool) { return int(QMessageBox::Cancel); };
        batch.finished = [&](bool ok, const QString& status, const QStringList&) { done = true; success = ok; summary = status; };
        requestStarted = [&](mf_gui_request request) {
            QTimer::singleShot(0, &batch, [&, request] {
                mf_message response = request.message; response.kind = MF_RESPONSE;
                response.status = response.command == MF_STAT ? MF_NOT_FOUND : MF_OK;
                if (response.command == MF_MKDIR) created.append(QString::fromUtf8(response.path));
                QVERIFY(batch.response(response));
            });
        };
        transferStarted = [&](mf_transfer_request request) {
            QCOMPARE(QString::fromUtf8(request.remote), QString("destination/folder/nested/file.txt"));
            QVERIFY(created.contains("destination/folder/nested")); QCOMPARE(QString::fromUtf8(request.local), source);
            QTimer::singleShot(0, &batch, [&, request] {
                uint8_t bytes[MF_EVENT_TRANSFER_SIZE]{}; mf_put64(bytes, request.id); bytes[MF_EVENT_TRANSFER_STATE] = 2; QVERIFY(batch.transfer(bytes, sizeof(bytes)));
            });
        };
        batch.upload({root.filePath("folder"), source}, "destination");
        QTRY_VERIFY(done); QVERIFY2(success, qPrintable(summary)); QCOMPARE(transfers.size(), 1);
        QVERIFY(created.contains("destination/folder/empty")); QCOMPARE(created.size(), 3);
        requestStarted = {}; transferStarted = {};
    }

    void changedTreeRemovesUnpublishedCopy() {
        QTemporaryDir root; QVERIFY(root.isValid());
        MemberFileBatch batch(QByteArray::fromHex(TEST_FILE_GROUP), fileKey(33), fileKey(22), MemberFileBatch::Download, nullptr);
        bool done = false, success = true; QString summary, downloaded;
        batch.progress = [](const QString&, int) {};
        batch.conflict = [](const QString&, bool) { return int(QMessageBox::Cancel); };
        batch.finished = [&](bool ok, const QString& status, const QStringList& roots) {
            done = true; success = ok; summary = status; QVERIFY(roots.isEmpty());
        };
        requestStarted = [&](mf_gui_request request) {
            QTimer::singleShot(0, &batch, [&, request] {
                mf_message response = request.message; response.kind = MF_RESPONSE; response.type = MF_DIRECTORY;
                if (response.command == MF_STAT) response.status = MF_STALE;
                else {
                    response.data[0] = 8; response.data[2] = MF_FILE; memset(response.data + 19, 7, 16);
                    memcpy(response.data + MF_ENTRY_HEADER, "file.txt", 8); response.data_len = MF_ENTRY_HEADER + 8; response.cursor = 1;
                }
                QVERIFY(batch.response(response));
            });
        };
        transferStarted = [&](mf_transfer_request request) {
            downloaded = QString::fromUtf8(request.local);
            QFile file(downloaded); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("complete file"); file.close();
            QTimer::singleShot(0, &batch, [&, request] {
                uint8_t bytes[MF_EVENT_TRANSFER_SIZE]{}; mf_put64(bytes, request.id); bytes[MF_EVENT_TRANSFER_STATE] = 2; QVERIFY(batch.transfer(bytes, sizeof(bytes)));
            });
        };
        batch.start({{"folder", {}, QByteArray(16, '\7'), true}}, root.path(), true);
        QTRY_VERIFY(done); QVERIFY(!success); QVERIFY(!downloaded.isEmpty()); QVERIFY(!QFileInfo::exists(downloaded));
        QVERIFY(summary.contains("folder")); QCOMPARE(QDir(root.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size(), 0);
        requestStarted = {}; transferStarted = {};
    }

    void recursiveDeletion_data() {
        QTest::addColumn<QString>("scenario");
        for (const auto* scenario : {"complete", "cancel-scan", "cancel-remove", "changed-file", "new-child", "changed-page", "server-busy"})
            QTest::newRow(scenario) << QString::fromLatin1(scenario);
    }

    void recursiveDeletion() {
        QFETCH(QString, scenario);
        QTemporaryDir root; QVERIFY(root.isValid());
        QString user = root.path() + "/" TEST_FILE_GROUP "/" + QString::fromLatin1(fileKey(22).toHex());
        QVERIFY(QDir().mkpath(user + "/folder/nested/empty")); QVERIFY(QDir().mkpath(user + "/other"));
        auto write = [&](const QString& path, const QByteArray& contents) {
            QFile file(user + '/' + path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(contents), qint64(contents.size()));
        };
        // Длинные имена вынуждают настоящий storage выдавать несколько страниц LIST.
        for (int i = 0; i < (scenario == "server-busy" ? 70 : 40); i++) write("folder/" + QString::number(i) + QString(220, 'x'), "payload");
        write("folder/nested/file.txt", "nested"); write("loose.txt", "selected"); write("keep.txt", "unselected");
        auto* storage = mf_storage_create(root.path().toUtf8().constData(), 1000000, 200000);
        QVERIFY(storage); auto destroy = qScopeGuard([&] { mf_storage_destroy(storage); });
        QCOMPARE(mf_storage_initialize(storage), int(MF_OK));
        QList<MemberFileEntry> entries;
        for (const auto* path : {"folder", "loose.txt", "other"}) {
            mf_message q{}, r{}; q.kind = MF_REQUEST; q.command = MF_STAT; memcpy(q.owner, fileKey(22).constData(), 32); q.request_id = entries.size() + 1;
            strcpy(q.path, path); mf_storage_execute(storage, TEST_FILE_GROUP, q.owner, &q, &r); QCOMPARE(r.status, uint16_t(MF_OK));
            entries.append({QString::fromUtf8(path), {}, QByteArray((const char*)r.version, 16), r.type == MF_DIRECTORY});
        }
        MemberFileBatch batch(QByteArray::fromHex(TEST_FILE_GROUP), fileKey(33), fileKey(22), MemberFileBatch::Remove, nullptr);
        bool done = false, success = false, injected = false; QString summary, changed;
        int pages = 0, removed = 0, requests = 0, rootStats = 0;
        batch.progress = [](const QString&, int) {};
        batch.finished = [&](bool ok, const QString& status, const QStringList&) { done = true; success = ok; summary = status; };
        auto reset = qScopeGuard([] { requestStarted = {}; });
        requestStarted = [&](mf_gui_request request) {
            requests++;
            QTimer::singleShot(0, &batch, [&, request] {
                const auto& q = request.message; QString path = QString::fromUtf8(q.path);
                if (q.command == MF_STAT && path == "folder") {
                    rootStats++;
                    if (scenario == "new-child" && rootStats == 2) { write("folder/arrived.txt", "new arrival"); injected = true; }
                }
                if (q.command == MF_LIST) {
                    QCOMPARE(removed, 0); pages++;
                    QVERIFY(!mf_zero(q.version, 16));
                    if (scenario == "changed-page" && q.cursor) { write("folder/arrived.txt", "new arrival"); injected = true; }
                }
                if (q.command == MF_REMOVE_PATH) {
                    QVERIFY(pages >= 4); // folder: две страницы, nested и empty: по одной.
                    if (scenario == "changed-file" && !injected) {
                        changed = "folder/nested/file.txt"; write(changed, "changed contents"); injected = true;
                    }
                }
                mf_message response{}; mf_storage_execute(storage, fileKeyValue(request.group).toHex().constData(), request.message.owner, &q, &response);
                if (q.command == MF_REMOVE_PATH && !response.status) {
                    removed++; QVERIFY(!QFileInfo::exists(user + '/' + path));
                }
                if (scenario == "cancel-scan" && response.more) {
                    batch.cancel(); QVERIFY(!batch.response(response)); return;
                }
                QVERIFY(batch.response(response));
                if (scenario == "cancel-remove" && removed == 1) { batch.cancel(); QVERIFY(!batch.response(response)); }
            });
        };
        batch.start(entries); QTRY_VERIFY(done);
        int stopped = requests; QCoreApplication::processEvents(); QCOMPARE(requests, stopped);
        QFile keep(user + "/keep.txt"); QVERIFY(keep.open(QIODevice::ReadOnly)); QCOMPARE(keep.readAll(), QByteArray("unselected"));
        if (scenario == "complete") {
            QVERIFY2(success, qPrintable(summary)); QCOMPARE(removed, 46);
            QVERIFY(!QFileInfo::exists(user + "/folder")); QVERIFY(!QFileInfo::exists(user + "/other"));
            QVERIFY(!QFileInfo::exists(user + "/loose.txt")); QVERIFY(summary.contains("Completed: 46"));
        } else {
            QVERIFY(!success); QVERIFY(QFileInfo::exists(user + "/folder"));
            if (scenario == "cancel-scan" || scenario == "changed-page") QCOMPARE(removed, 0);
            if (scenario == "cancel-remove") { QCOMPARE(removed, 1); QVERIFY(summary.contains("Completed: 1")); }
            if (scenario.startsWith("cancel-")) QVERIFY(summary.contains("Deletion cancelled"));
            if (scenario == "changed-file") { QVERIFY(injected); QVERIFY(QFileInfo::exists(user + '/' + changed)); }
            if (scenario == "new-child") {
                QVERIFY(injected); QVERIFY(QFileInfo::exists(user + "/folder/arrived.txt"));
                QVERIFY(summary.contains(QString::fromUtf8(mf_status_text(MF_DIRECTORY_NOT_EMPTY))));
            }
            if (scenario == "changed-page") QVERIFY(summary.contains(QString::fromUtf8(mf_status_text(MF_STALE))));
            if (scenario == "server-busy") { QCOMPARE(removed, 64); QVERIFY(summary.contains(QString::fromUtf8(mf_status_text(MF_BUSY)))); }
        }
    }

    void deleteAndParentShortcuts() {
        sqlite3* sql = nullptr; QCOMPARE(sqlite3_open(":memory:", &sql), SQLITE_OK);
        QCOMPARE(sqlite3_exec(sql, "CREATE TABLE accounts(node_id INTEGER,display_name TEXT);"
            "INSERT INTO accounts VALUES(11,'Other'),(22,'Me'),(33,'Server');"
            "CREATE TABLE peers_" TEST_FILE_GROUP "(node_id INTEGER,adm_tags TEXT,adm_tags_sig BLOB,deleted INTEGER,source INTEGER);"
            "INSERT INTO peers_" TEST_FILE_GROUP " VALUES(11,'{}',NULL,0,0),(22,'{}',NULL,0,0),"
            "(33,'{\"storage\":\"yes\"}',zeroblob(64),0,0);"
            "ALTER TABLE peers_" TEST_FILE_GROUP " ADD COLUMN record_state INTEGER NOT NULL DEFAULT 0;"
            "ALTER TABLE peers_" TEST_FILE_GROUP " ADD COLUMN ed25519_pubkey BLOB;"
            "ALTER TABLE peers_" TEST_FILE_GROUP " ADD COLUMN local_nick TEXT;"
            "UPDATE peers_" TEST_FILE_GROUP " SET ed25519_pubkey=CASE node_id WHEN 11 THEN x'" TEST_FILE_ED_11 "'"
            " WHEN 22 THEN x'" TEST_FILE_ED_22 "' WHEN 33 THEN x'" TEST_FILE_ED_33 "' END,"
            " local_nick=(SELECT display_name FROM accounts a WHERE a.node_id=peers_" TEST_FILE_GROUP ".node_id);", nullptr, nullptr, nullptr), SQLITE_OK);
        DbManager db; db.setDb(sql);
        {
            MemberFileBrowser browser(&db); browser.setChannel(TEST_FILE_GROUP, fileKey(22)); browser.show(); browser.activateWindow();
            auto* tree = browser.findChild<QTreeWidget*>(); auto* location = browser.findChild<QLineEdit*>();
            location->setText("documents/nested"); QTest::keyClick(location, Qt::Key_Return);
            auto* item = new QTreeWidgetItem(tree, {"remote.txt"});
            item->setData(0, Qt::UserRole, "remote.txt"); item->setData(0, Qt::UserRole + 1, MF_FILE);
            item->setData(0, Qt::UserRole + 2, QByteArray(16, '\7')); tree->setCurrentItem(item);
            tree->setFocus(); QTRY_VERIFY(tree->hasFocus());
            auto previous = captured.message.request_id;
            answerDialog(QMessageBox::No); QTest::keyClick(tree, Qt::Key_Delete);
            QCOMPARE(captured.message.request_id, previous);
            // Offscreen не восстанавливает активное окно после закрытия модального диалога.
            browser.activateWindow(); QTRY_VERIFY(browser.isActiveWindow()); QTRY_VERIFY(tree->hasFocus());
            answerDialog(QMessageBox::Yes); QTest::keyClick(tree, Qt::Key_Delete);
            QCOMPARE(captured.message.command, uint8_t(MF_STAT)); respond(browser, captured, MF_OK);
            QCOMPARE(captured.message.command, uint8_t(MF_REMOVE_PATH)); QCOMPARE(fileKeyValue(captured.message.owner), fileKey(22));
            QCOMPARE(QString::fromUtf8(captured.message.path), QString("documents/nested/remote.txt"));
            QCOMPARE(QByteArray((const char*)captured.message.version, 16), QByteArray(16, '\7'));
            respond(browser, captured, MF_OK); QTRY_VERIFY(!cancelButton(browser)->isEnabled());
            browser.activateWindow(); QTRY_VERIFY(browser.isActiveWindow());
            location->setFocus(); location->setText("abc"); location->setCursorPosition(1);
            previous = captured.message.request_id;
            QTest::keyClick(location, Qt::Key_Delete); QCOMPARE(location->text(), QString("ac"));
            QTest::keyClick(location, Qt::Key_Backspace); QCOMPARE(location->text(), QString("c"));
            QCOMPARE(captured.message.request_id, previous);
            tree->setFocus(); QTest::keyClick(tree, Qt::Key_Backspace);
            QCOMPARE(captured.message.command, uint8_t(MF_LIST)); QCOMPARE(QString::fromUtf8(captured.message.path), QString("documents"));
            QTest::keyClick(tree, Qt::Key_Backspace); QCOMPARE(QString::fromUtf8(captured.message.path), QString());
            QPushButton* up = nullptr;
            for (auto* button : browser.findChildren<QPushButton*>()) {
                QVERIFY(!button->autoDefault());
                if (button->text() == "Up") up = button;
            }
            QVERIFY(up);
            for (auto key : {Qt::Key_Return, Qt::Key_Enter}) {
                item = new QTreeWidgetItem(tree, {"folder"}); item->setData(0, Qt::UserRole, "folder");
                item->setData(0, Qt::UserRole + 1, MF_DIRECTORY); tree->setCurrentItem(item);
                if (key == Qt::Key_Return) {
                    previous = captured.message.request_id;
                    QTimer::singleShot(0, [] {
                        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()); QVERIFY(box);
                        bool recursive = box->text().contains("all its contents"); box->button(QMessageBox::No)->click(); QVERIFY(recursive);
                    });
                    QTest::keyClick(tree, Qt::Key_Delete); QCOMPARE(captured.message.request_id, previous);
                    browser.activateWindow(); QTRY_VERIFY(browser.isActiveWindow()); QTRY_VERIFY(tree->hasFocus());
                }
                QTest::keyClick(tree, key);
                QCOMPARE(captured.message.command, uint8_t(MF_LIST)); QCOMPARE(QString::fromUtf8(captured.message.path), QString("folder"));
                up->setFocus(); QTest::mouseClick(up, Qt::LeftButton);
                QVERIFY(tree->hasFocus()); QCOMPARE(QString::fromUtf8(captured.message.path), QString());
            }
            QTest::keyClick(tree, Qt::Key_Backspace); QCOMPARE(fileKeyValue(captured.message.owner), fileKey(0));
            browser.setChannel(TEST_FILE_GROUP, fileKey(11));
            item = new QTreeWidgetItem(tree, {"foreign.txt"}); item->setData(0, Qt::UserRole, "foreign.txt");
            item->setData(0, Qt::UserRole + 1, MF_FILE); tree->setCurrentItem(item);
            previous = captured.message.request_id;
            QTest::keyClick(tree, Qt::Key_Delete); QCOMPARE(captured.message.request_id, previous);
            QVERIFY(!QApplication::activeModalWidget());
        }
        db.setDb(nullptr); QCOMPARE(sqlite3_close(sql), SQLITE_OK);
    }

    void exportToSystemClipboardAndSingleDrag_data() {
        QTest::addColumn<int>("dragStatus");
        QTest::newRow("success") << int(MF_OK);
        QTest::newRow("failure") << int(MF_IO_ERROR);
        QTest::newRow("cancel") << int(MF_CANCELLED);
    }

    void exportToSystemClipboardAndSingleDrag() {
        QFETCH(int, dragStatus);
        QTemporaryDir cache; QVERIFY(cache.isValid()); QByteArray oldCache = qgetenv("XDG_CACHE_HOME");
        qputenv("XDG_CACHE_HOME", cache.path().toUtf8());
        auto restoreCache = qScopeGuard([oldCache] { qputenv("XDG_CACHE_HOME", oldCache); QApplication::clipboard()->clear(); });
        sqlite3* sql = nullptr; QCOMPARE(sqlite3_open(":memory:", &sql), SQLITE_OK);
        QCOMPARE(sqlite3_exec(sql, "CREATE TABLE accounts(node_id INTEGER,display_name TEXT);"
            "INSERT INTO accounts VALUES(11,'Other'),(22,'Me'),(33,'Server');"
            "CREATE TABLE peers_" TEST_FILE_GROUP "(node_id INTEGER,adm_tags TEXT,adm_tags_sig BLOB,deleted INTEGER,source INTEGER);"
            "INSERT INTO peers_" TEST_FILE_GROUP " VALUES(11,'{}',NULL,0,0),(22,'{}',NULL,0,0),"
            "(33,'{\"storage\":\"yes\"}',zeroblob(64),0,0);"
            "ALTER TABLE peers_" TEST_FILE_GROUP " ADD COLUMN record_state INTEGER NOT NULL DEFAULT 0;"
            "ALTER TABLE peers_" TEST_FILE_GROUP " ADD COLUMN ed25519_pubkey BLOB;"
            "ALTER TABLE peers_" TEST_FILE_GROUP " ADD COLUMN local_nick TEXT;"
            "UPDATE peers_" TEST_FILE_GROUP " SET ed25519_pubkey=CASE node_id WHEN 11 THEN x'" TEST_FILE_ED_11 "'"
            " WHEN 22 THEN x'" TEST_FILE_ED_22 "' WHEN 33 THEN x'" TEST_FILE_ED_33 "' END,"
            " local_nick=(SELECT display_name FROM accounts a WHERE a.node_id=peers_" TEST_FILE_GROUP ".node_id);", nullptr, nullptr, nullptr), SQLITE_OK);
        DbManager db; db.setDb(sql); QString exported;
        {
            MemberFileBrowser browser(&db); browser.setChannel(TEST_FILE_GROUP, fileKey(11)); browser.show();
            auto* tree = browser.findChild<QTreeWidget*>();
            auto* item = new QTreeWidgetItem(tree, {"remote.txt"});
            item->setData(0, Qt::UserRole, "remote.txt"); item->setData(0, Qt::UserRole + 1, MF_FILE);
            item->setData(0, Qt::UserRole + 2, QByteArray(16, '\7')); tree->setCurrentItem(item);
            auto* copy = browser.findChild<QAction*>("memberFilesCopy"); QVERIFY(copy); QVERIFY(copy->isEnabled());
            QApplication::clipboard()->setText("unchanged"); copy->trigger();
            QTRY_COMPARE(transfers.size(), 1); auto request = transfers.last();
            QCOMPARE(request.upload, 0); QCOMPARE(fileKeyValue(request.owner), fileKey(11)); QCOMPARE(fileKeyValue(request.group_key).toHex(), QByteArray(TEST_FILE_GROUP)); QCOMPARE(fileKeyValue(request.server), fileKey(33));
            QCOMPARE(QString::fromUtf8(request.remote), QString("remote.txt"));
            QCOMPARE(QByteArray((const char*)request.version, 16), QByteArray(16, '\7'));
            QCOMPARE(QApplication::clipboard()->text(), QString("unchanged"));
            exported = QString::fromUtf8(request.local);
            { QFile file(exported); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write("downloaded bytes"), 16LL); }
            complete(browser, request);
            QTRY_VERIFY(copy->isEnabled());
            const auto* mime = QApplication::clipboard()->mimeData(); QVERIFY(mime->hasUrls());
            QCOMPARE(mime->urls(), QList<QUrl>{QUrl::fromLocalFile(exported)});
            QVERIFY(mime->data("x-special/gnome-copied-files").startsWith("copy\n"));

            item->setData(0, Qt::UserRole + 2, QByteArray(16, '\10'));
            QApplication::clipboard()->setText("keep on error"); copy->trigger(); QTRY_COMPARE(transfers.size(), 2);
            complete(browser, transfers.last(), MF_IO_ERROR); QTRY_VERIFY(copy->isEnabled());
            QCOMPARE(QApplication::clipboard()->text(), QString("keep on error"));
            copy->trigger(); QTRY_COMPARE(transfers.size(), 3); auto cancelledRequest = transfers.last();
            cancelButton(browser)->click(); complete(browser, cancelledRequest, MF_CANCELLED); QTRY_VERIFY(copy->isEnabled());
            QCOMPARE(QApplication::clipboard()->text(), QString("keep on error"));

            bool committed = false, received = false;
            transferStarted = [&browser, &committed, dragStatus](mf_transfer_request transfer) {
                QTimer::singleShot(50, &browser, [&browser, &committed, transfer, dragStatus] {
                    if (!dragStatus) {
                        QFile file(QString::fromUtf8(transfer.local)); QVERIFY(file.open(QIODevice::WriteOnly));
                        QCOMPARE(file.write("drag bytes"), 10LL); file.close();
                    }
                    committed = true;
                    if (dragStatus == MF_CANCELLED) cancelButton(browser)->click();
                    complete(browser, transfer, dragStatus);
                });
            };
            DragReceiver receiver;
            receiver.receive = [&](const QMimeData* data) {
                QVERIFY(data->hasUrls()); QVERIFY(!committed);
                // Запрос системного менеджера приходит до скачивания и возвращается лишь после commit.
                auto urls = data->urls(); QVERIFY(committed);
                if (!dragStatus) {
                    QCOMPARE(urls.size(), 1); QFile file(urls.first().toLocalFile());
                    QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), QByteArray("drag bytes"));
                } else QVERIFY(urls.isEmpty());
                received = true;
            };
            QCoreApplication::instance()->installEventFilter(&receiver);
            QTimer::singleShot(3000, &browser, [] { QDrag::cancel(); });
            tree->setFocus(); QCoreApplication::processEvents(); QPoint point = tree->visualItemRect(item).center();
            QTest::mousePress(tree->viewport(), Qt::LeftButton, Qt::NoModifier, point);
            QMouseEvent move(QEvent::MouseMove, QPointF(point + QPoint(40, 0)), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(tree->viewport(), &move);
            QTest::mouseRelease(tree->viewport(), Qt::LeftButton, Qt::NoModifier, point + QPoint(40, 0));
            QVERIFY(received); QCOMPARE(transfers.size(), 4); transferStarted = {};
            if (dragStatus == MF_CANCELLED) {
                QPointer<MemberFileBrowser> closing = new MemberFileBrowser(&db);
                closing->setChannel(TEST_FILE_GROUP, fileKey(11)); closing->show();
                auto* closingTree = closing->findChild<QTreeWidget*>();
                auto* closingItem = new QTreeWidgetItem(closingTree, {"pending.txt"});
                closingItem->setData(0, Qt::UserRole, "pending.txt"); closingItem->setData(0, Qt::UserRole + 1, MF_FILE);
                closingItem->setData(0, Qt::UserRole + 2, QByteArray(16, '\7')); closingTree->setCurrentItem(closingItem);
                closing->findChild<QAction*>("memberFilesCopy")->trigger(); QTRY_COMPARE(transfers.size(), 5);
                auto pending = transfers.last(); QString temporary = QFileInfo(QString::fromUtf8(pending.local)).absolutePath();
                closing->close(); QVERIFY(closing); QVERIFY(cancelled.contains(pending.id));
                complete(*closing, pending, MF_CANCELLED); QTRY_VERIFY(closing.isNull()); QVERIFY(!QFileInfo::exists(temporary));
            }
        }
        QVERIFY(QFile::exists(exported)); // Закрытие Group files не ломает локальный URI.
        db.setDb(nullptr); QCOMPARE(sqlite3_close(sql), SQLITE_OK);
    }

    void browserDropsIntoCurrentDirectory() {
        sqlite3* sql = nullptr; QCOMPARE(sqlite3_open(":memory:", &sql), SQLITE_OK);
        QCOMPARE(sqlite3_exec(sql, "CREATE TABLE accounts(node_id INTEGER,display_name TEXT);"
            "INSERT INTO accounts VALUES(11,'Other'),(22,'Me'),(33,'Server');"
            "CREATE TABLE peers_" TEST_FILE_GROUP "(node_id INTEGER,adm_tags TEXT,adm_tags_sig BLOB,deleted INTEGER,source INTEGER);"
            "INSERT INTO peers_" TEST_FILE_GROUP " VALUES(11,'{}',NULL,0,0),(22,'{}',NULL,0,0),"
            "(33,'{\"storage\":\"yes\"}',zeroblob(64),0,0);"
            "ALTER TABLE peers_" TEST_FILE_GROUP " ADD COLUMN record_state INTEGER NOT NULL DEFAULT 0;"
            "ALTER TABLE peers_" TEST_FILE_GROUP " ADD COLUMN ed25519_pubkey BLOB;"
            "ALTER TABLE peers_" TEST_FILE_GROUP " ADD COLUMN local_nick TEXT;"
            "UPDATE peers_" TEST_FILE_GROUP " SET ed25519_pubkey=CASE node_id WHEN 11 THEN x'" TEST_FILE_ED_11 "'"
            " WHEN 22 THEN x'" TEST_FILE_ED_22 "' WHEN 33 THEN x'" TEST_FILE_ED_33 "' END,"
            " local_nick=(SELECT display_name FROM accounts a WHERE a.node_id=peers_" TEST_FILE_GROUP ".node_id);", nullptr, nullptr, nullptr), SQLITE_OK);
        DbManager db; db.setDb(sql);
        QTemporaryDir dir; QVERIFY(dir.isValid()); QString path = dir.filePath("dropped.txt");
        { QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write("test"), 4LL); }
        QMimeData mime; mime.setUrls({QUrl::fromLocalFile(path)});
        {
            MemberFileBrowser browser(&db); browser.show();
            auto* tree = browser.findChild<QTreeWidget*>(); auto* location = browser.findChild<QLineEdit*>(); QVERIFY(location);
            auto* cancel = cancelButton(browser); QVERIFY(cancel);
            QList<QWidget*> targets{&browser, tree->viewport(), location};
            for (auto* button : browser.findChildren<QPushButton*>()) if (button->text().startsWith("Upload")) targets.append(button);
            for (auto* target : targets) {
                browser.setChannel(TEST_FILE_GROUP, fileKey(22)); location->setText("documents"); location->returnPressed();
                QDragEnterEvent enter(QPoint(3, 3), Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(target, &enter);
                DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "test browser drop: target=%s acceptDrops=%d accepted=%d",
                           target->metaObject()->className(), target->acceptDrops(), enter.isAccepted());
                QVERIFY(enter.isAccepted()); QCOMPARE(enter.dropAction(), Qt::CopyAction);
                QVERIFY(browser.findChild<QLabel*>("memberFilesDropHint")->isVisible());
                QDropEvent drop(QPointF(3, 3), Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(target, &drop); QVERIFY(drop.isAccepted()); QCOMPARE(drop.dropAction(), Qt::CopyAction);
                QVERIFY(!browser.findChild<QLabel*>("memberFilesDropHint")->isVisible());
                browser.setChannel(TEST_FILE_OTHER_GROUP, fileKey(11)); // Назначение уже зафиксировано до отложенного запуска очереди.
                QTRY_COMPARE(captured.message.command, uint8_t(MF_STAT));
                QCOMPARE(fileKeyValue(captured.group).toHex(), QByteArray(TEST_FILE_GROUP)); QCOMPARE(fileKeyValue(captured.server), fileKey(33)); QCOMPARE(fileKeyValue(captured.message.owner), fileKey(22));
                QCOMPARE(QString::fromUtf8(captured.message.path), QString("documents/dropped.txt"));
                int count = transfers.size(); respond(browser, captured, MF_NOT_FOUND); QCOMPARE(transfers.size(), count + 1);
                QCOMPARE(QString::fromUtf8(transfers.last().remote), QString("documents/dropped.txt"));
                complete(browser, transfers.last()); QTRY_VERIFY(!cancel->isEnabled());
            }
            browser.setChannel(TEST_FILE_GROUP, fileKey(11));
            QDragEnterEvent other(QPoint(3, 3), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(tree->viewport(), &other); QVERIFY(!other.isAccepted());
            browser.setChannel(TEST_FILE_GROUP, fileKey(22));
            QDragEnterEvent enter(QPoint(3, 3), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(tree->viewport(), &enter); QVERIFY(enter.isAccepted());
            QDropEvent drop(QPointF(3, 3), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(tree->viewport(), &drop); QVERIFY(drop.isAccepted());
            quint64 previous = captured.message.request_id; cancel->click(); QCoreApplication::processEvents();
            QCOMPARE(captured.message.request_id, previous); QVERIFY(!cancel->isEnabled());
            ready = false;
            QDragEnterEvent stopped(QPoint(3, 3), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(tree->viewport(), &stopped); QVERIFY(!stopped.isAccepted()); ready = true;
        }
        db.setDb(nullptr); QCOMPARE(sqlite3_close(sql), SQLITE_OK);
    }

    void footerDropsCopyAndCaptureGroup() {
        sqlite3* sql = nullptr; QCOMPARE(sqlite3_open(":memory:", &sql), SQLITE_OK);
        auto closeDb = qScopeGuard([&] { sqlite3_close(sql); });
        DbManager db; db.setDb(sql); ChannelList list(&db); list.resize(350, 400); list.show();
        auto* view = list.findChild<QListView*>(); auto* model = qobject_cast<QStandardItemModel*>(view->model()); QVERIFY(model);
        for (const auto& id : {QString(TEST_FILE_GROUP), QString(TEST_FILE_OTHER_GROUP), QString("dm:abc")}) {
            auto* item = new QStandardItem; item->setData(id, ChannelChannelIdRole); model->appendRow(item);
        }
        auto* footer = list.findChild<QWidget*>("channelFooter"); QVERIFY(footer);
        QSignalSpy drops(&list, &ChannelList::ownFilesDropped);
        QMimeData mime; mime.setUrls({QUrl::fromLocalFile("/tmp/a.txt"), QUrl::fromLocalFile("/tmp/b.txt")});
        list.selectChannel(TEST_FILE_GROUP);
        auto targets = footer->findChildren<QPushButton*>(); QVERIFY(!targets.isEmpty());
        QList<QWidget*> widgets{footer}; for (auto* button : targets) widgets.append(button);
        for (auto* target : widgets) {
            list.selectChannel(TEST_FILE_GROUP);
            QDragEnterEvent enter(QPoint(3, 3), Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(target, &enter); QVERIFY(enter.isAccepted()); QCOMPARE(enter.dropAction(), Qt::CopyAction);
            QVERIFY(list.findChild<QLabel*>("ownFilesDropHint")->isVisible());
            QDropEvent drop(QPointF(3, 3), Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(target, &drop); QVERIFY(drop.isAccepted()); QCOMPARE(drop.dropAction(), Qt::CopyAction);
            list.selectChannel(TEST_FILE_OTHER_GROUP); QCoreApplication::processEvents();
            QCOMPARE(drops.last()[0].toString(), QString(TEST_FILE_GROUP));
            QCOMPARE(drops.last()[1].toStringList(), QStringList({"/tmp/a.txt", "/tmp/b.txt"}));
            QVERIFY(!list.findChild<QLabel*>("ownFilesDropHint")->isVisible());
        }
        list.selectChannel("dm:abc");
        QDragEnterEvent dm(QPoint(3, 3), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(footer, &dm); QVERIFY(!dm.isAccepted());
        list.selectChannel(TEST_FILE_GROUP); ready = false;
        QDragEnterEvent stopped(QPoint(3, 3), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(footer, &stopped); QVERIFY(!stopped.isAccepted()); ready = true;
        mime.setUrls({QUrl("https://example.com/file")});
        QDragEnterEvent remote(QPoint(3, 3), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(footer, &remote); QVERIFY(!remote.isAccepted());
    }

    void uploadQueueConflictsAndCancellation() {
        sqlite3* sql = nullptr; QCOMPARE(sqlite3_open(":memory:", &sql), SQLITE_OK);
        QCOMPARE(sqlite3_exec(sql, "CREATE TABLE accounts(node_id INTEGER,display_name TEXT);"
            "INSERT INTO accounts VALUES(22,'Me'),(33,'Server');"
            "CREATE TABLE peers_" TEST_FILE_GROUP "(node_id INTEGER,adm_tags TEXT,adm_tags_sig BLOB,deleted INTEGER,source INTEGER);"
            "INSERT INTO peers_" TEST_FILE_GROUP " VALUES(22,'{}',NULL,0,0),(33,'{\"storage\":\"yes\"}',zeroblob(64),0,0);"
            "ALTER TABLE peers_" TEST_FILE_GROUP " ADD COLUMN record_state INTEGER NOT NULL DEFAULT 0;"
            "ALTER TABLE peers_" TEST_FILE_GROUP " ADD COLUMN ed25519_pubkey BLOB;"
            "ALTER TABLE peers_" TEST_FILE_GROUP " ADD COLUMN local_nick TEXT;"
            "UPDATE peers_" TEST_FILE_GROUP " SET ed25519_pubkey=CASE node_id WHEN 11 THEN x'" TEST_FILE_ED_11 "'"
            " WHEN 22 THEN x'" TEST_FILE_ED_22 "' WHEN 33 THEN x'" TEST_FILE_ED_33 "' END,"
            " local_nick=(SELECT display_name FROM accounts a WHERE a.node_id=peers_" TEST_FILE_GROUP ".node_id);",
            nullptr, nullptr, nullptr), SQLITE_OK);
        DbManager db; db.setDb(sql);
        QTemporaryDir dir; QVERIFY(dir.isValid()); QStringList paths;
        for (const auto& name : {"a.txt", "b.txt", "c.txt"}) {
            QString path = dir.filePath(name); QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write("test"), 4LL); paths.append(path);
        }
        {
            MemberFileBrowser browser(&db); auto* cancel = cancelButton(browser); QVERIFY(cancel);
            auto* status = browser.findChild<QLabel*>("memberFilesUploadStatus"); QVERIFY(status);
            answerDialog(QDialog::Accepted); browser.uploadOwnFiles(TEST_FILE_GROUP, paths);
            QTRY_COMPARE(captured.message.command, uint8_t(MF_STAT));
            QCOMPARE(fileKeyValue(captured.message.owner), fileKey(22)); QCOMPARE(fileKeyValue(captured.server), fileKey(33)); QVERIFY(transfers.isEmpty());
            quint64 firstStat = captured.message.request_id;
            browser.setChannel(TEST_FILE_OTHER_GROUP, fileKey(11)); QCOMPARE(captured.message.request_id, firstStat);
            for (auto* selector : browser.findChildren<QComboBox*>()) QVERIFY(!selector->isEnabled());
            respond(browser, captured, MF_NOT_FOUND); QCOMPARE(transfers.size(), 1);
            QCOMPARE(QString::fromUtf8(transfers[0].remote), QString("a.txt")); QCOMPARE(fileKeyValue(transfers[0].owner), fileKey(22));
            QCOMPARE(QString::fromUtf8(transfers[0].local), paths[0]); QCOMPARE(fileKeyValue(transfers[0].group_key).toHex(), QByteArray(TEST_FILE_GROUP));
            QCOMPARE(QByteArray((const char*)transfers[0].version, 16), QByteArray(16, '\0'));
            complete(browser, transfers[0]); QTRY_COMPARE(QString::fromUtf8(captured.message.path), QString("b.txt"));
            answerDialog(QMessageBox::No); respond(browser, captured, MF_OK);
            QCOMPARE(transfers.size(), 1); QTRY_COMPARE(QString::fromUtf8(captured.message.path), QString("c.txt"));
            answerDialog(QMessageBox::Yes); respond(browser, captured, MF_OK); QCOMPARE(transfers.size(), 2);
            QCOMPARE(QByteArray((const char*)transfers[1].version, 16), QByteArray(16, '\7'));
            complete(browser, transfers[1]); QTRY_VERIFY(!cancel->isEnabled()); QVERIFY(status->text().contains("Completed: 2, skipped: 1"));
            for (const auto& path : paths) QVERIFY(QFile::exists(path));

            answerDialog(QDialog::Accepted); browser.uploadOwnFiles(TEST_FILE_GROUP, paths);
            QTRY_COMPARE(captured.message.command, uint8_t(MF_STAT)); mf_gui_request stale = captured;
            cancel->click(); QVERIFY(cancelled.contains(stale.message.request_id));
            respond(browser, stale, MF_NOT_FOUND); QCOMPARE(transfers.size(), 2); QVERIFY(!cancel->isEnabled());

            answerDialog(QDialog::Accepted); browser.uploadOwnFiles(TEST_FILE_GROUP, paths);
            QTRY_COMPARE(captured.message.command, uint8_t(MF_STAT)); respond(browser, captured, MF_NOT_FOUND);
            QCOMPARE(transfers.size(), 3); auto active = transfers.last(); cancel->click(); QVERIFY(cancelled.contains(active.id));
            QVERIFY(cancel->isEnabled()); complete(browser, active, MF_CANCELLED); QVERIFY(!cancel->isEnabled());
            QCOMPARE(transfers.size(), 3); QVERIFY(status->text().contains("cancelled"));

            answerDialog(QDialog::Accepted); browser.uploadOwnFiles(TEST_FILE_GROUP, paths);
            QTRY_COMPARE(captured.message.command, uint8_t(MF_STAT)); respond(browser, captured, MF_NOT_FOUND);
            QCOMPARE(transfers.size(), 4); complete(browser, transfers.last(), MF_NO_SPACE);
            QVERIFY(status->text().contains("a.txt")); QVERIFY(!cancel->isEnabled()); QCOMPARE(transfers.size(), 4);

            QString invalid = dir.filePath(".unsupported"); { QFile file(invalid); QVERIFY(file.open(QIODevice::WriteOnly)); }
            answerDialog(QDialog::Accepted); browser.uploadOwnFiles(TEST_FILE_GROUP, {dir.path()});
            QTRY_VERIFY(status->text().contains("Cannot copy this path")); QVERIFY(!cancel->isEnabled()); QCOMPARE(transfers.size(), 4);

            QCOMPARE(sqlite3_exec(sql, "DELETE FROM peers_" TEST_FILE_GROUP " WHERE node_id=33", nullptr, nullptr, nullptr), SQLITE_OK);
            answerDialog(QMessageBox::Ok); browser.uploadOwnFiles(TEST_FILE_GROUP, paths); QVERIFY(!cancel->isEnabled()); QCOMPARE(transfers.size(), 4);
        }
        db.setDb(nullptr); QCOMPARE(sqlite3_close(sql), SQLITE_OK);
    }

    void rolePermissionsAndLateResponse() {
        sqlite3* sql = nullptr; QCOMPARE(sqlite3_open(":memory:", &sql), SQLITE_OK);
        QCOMPARE(sqlite3_exec(sql, "CREATE TABLE accounts(node_id INTEGER,display_name TEXT);"
            "INSERT INTO accounts VALUES(11,'Author'),(22,'Reader'),(33,'Server');"
            "CREATE TABLE peers_" TEST_FILE_GROUP "(node_id INTEGER,storage INTEGER,adm_tags TEXT,adm_tags_sig BLOB,deleted INTEGER,source INTEGER);"
            "INSERT INTO peers_" TEST_FILE_GROUP " VALUES(11,0,'{}',NULL,0,0),(22,0,'{}',NULL,0,0),"
            "(33,0,'{\"storage\":\"yes\"}',zeroblob(64),0,0);"
            "ALTER TABLE peers_" TEST_FILE_GROUP " ADD COLUMN record_state INTEGER NOT NULL DEFAULT 0;"
            "ALTER TABLE peers_" TEST_FILE_GROUP " ADD COLUMN ed25519_pubkey BLOB;"
            "ALTER TABLE peers_" TEST_FILE_GROUP " ADD COLUMN local_nick TEXT;"
            "UPDATE peers_" TEST_FILE_GROUP " SET ed25519_pubkey=CASE node_id WHEN 11 THEN x'" TEST_FILE_ED_11 "'"
            " WHEN 22 THEN x'" TEST_FILE_ED_22 "' WHEN 33 THEN x'" TEST_FILE_ED_33 "' END,"
            " local_nick=(SELECT display_name FROM accounts a WHERE a.node_id=peers_" TEST_FILE_GROUP ".node_id);", nullptr, nullptr, nullptr), SQLITE_OK);
        DbManager db; db.setDb(sql);
        {
            MemberFileBrowser browser(&db); browser.setChannel(TEST_FILE_GROUP, fileKey(11));
            auto selectors = browser.findChildren<QComboBox*>(); QCOMPARE(selectors.size(), 2);
            QCOMPARE(selectors[0]->count(), 1); QCOMPARE(selectors[0]->currentData().toByteArray(), fileKey(33));
            QCOMPARE(fileKeyValue(captured.message.owner), fileKey(11)); QCOMPARE(captured.message.command, (uint8_t)MF_LIST);
            QCOMPARE(fileKeyValue(quotaCaptured.message.owner), fileKey(11)); QCOMPARE(quotaCaptured.message.command, (uint8_t)MF_QUOTA);
            auto* quota = browser.findChild<QLabel*>("memberFilesQuota"); QVERIFY(quota);
            mf_gui_request previousQuota = quotaCaptured;
            mf_message stats = quotaCaptured.message; stats.kind = MF_RESPONSE; stats.data_len = MF_QUOTA_BYTES;
            mf_put64(stats.data, 2 * 1024 * 1024); mf_put64(stats.data + 8, 8 * 1024 * 1024); mf_put64(stats.data + 16, 1024 * 1024);
            mf_put64(stats.data + 24, 42); mf_put64(stats.data + 32, 7);
            uint8_t quotaPacket[MF_EVENT_RESPONSE_PREFIX + MF_WIRE_MAX]; memcpy(quotaPacket, QByteArray::fromHex(TEST_FILE_GROUP).constData(), 32); memcpy(quotaPacket + 32, fileKey(33).constData(), 32);
            int quotaLength = mf_encode(&stats, quotaPacket + MF_EVENT_RESPONSE_PREFIX, MF_WIRE_MAX); browser.onResponse(quotaPacket, quotaLength + MF_EVENT_RESPONSE_PREFIX);
            QCOMPARE(quota->text(), QString("Storage: %1 used / %2 total · Files: 42 · Directories: 7 · Reserved for uploads: %3")
                .arg(QLocale().formattedDataSize(2 * 1024 * 1024, 2), QLocale().formattedDataSize(8 * 1024 * 1024, 2),
                     QLocale().formattedDataSize(1024 * 1024, 2)));
            QVERIFY(quota->toolTip().contains("2097152"));
            mf_gui_request previous = captured;
            mf_message r = previous.message; r.kind = MF_RESPONSE; r.type = MF_DIRECTORY;
            const char name[] = "readme.txt"; unsigned n = sizeof(name) - 1;
            r.data[0] = (uint8_t)n; r.data[2] = MF_FILE; mf_put64(r.data + 3, 123);
            memcpy(r.data + MF_ENTRY_HEADER, name, n); r.data_len = MF_ENTRY_HEADER + n;
            uint8_t packet[MF_EVENT_RESPONSE_PREFIX + MF_WIRE_MAX]; memcpy(packet, QByteArray::fromHex(TEST_FILE_GROUP).constData(), 32); memcpy(packet + 32, fileKey(33).constData(), 32);
            int length = mf_encode(&r, packet + MF_EVENT_RESPONSE_PREFIX, MF_WIRE_MAX); browser.onResponse(packet, length + MF_EVENT_RESPONSE_PREFIX);
            auto* table = browser.findChild<QTreeWidget*>(); QCOMPARE(table->topLevelItemCount(), 1);
            table->setCurrentItem(table->topLevelItem(0));
            for (auto* button : browser.findChildren<QPushButton*>()) {
                if (button->text().startsWith("Upload") || button->text().startsWith("Rename") || button->text() == "Delete")
                    QVERIFY(!button->isEnabled());
                if (button->text().startsWith("Download")) QVERIFY(button->isEnabled());
            }
            int own = selectors[1]->findData(fileKey(22)); QVERIFY(own >= 0);
            selectors[1]->setCurrentIndex(own); QCOMPARE(fileKeyValue(captured.message.owner), fileKey(22)); QCOMPARE(table->topLevelItemCount(), 0);
            QCOMPARE(fileKeyValue(quotaCaptured.message.owner), fileKey(22)); QVERIFY(quotaCaptured.message.request_id != previousQuota.message.request_id);
            browser.onResponse(quotaPacket, quotaLength + MF_EVENT_RESPONSE_PREFIX); QCOMPARE(quota->text(), QString("Loading quota…"));
            stats = quotaCaptured.message; stats.kind = MF_RESPONSE; stats.data_len = MF_QUOTA_BYTES;
            mf_put64(stats.data, 3 * 1024 * 1024); mf_put64(stats.data + 8, 8 * 1024 * 1024);
            mf_put64(stats.data + 24, 12); mf_put64(stats.data + 32, 3);
            quotaLength = mf_encode(&stats, quotaPacket + MF_EVENT_RESPONSE_PREFIX, MF_WIRE_MAX); browser.onResponse(quotaPacket, quotaLength + MF_EVENT_RESPONSE_PREFIX);
            QString ownQuota = QString("Storage: %1 used / %2 total · Files: 12 · Directories: 3")
                .arg(QLocale().formattedDataSize(3 * 1024 * 1024, 2), QLocale().formattedDataSize(8 * 1024 * 1024, 2));
            QCOMPARE(quota->text(), ownQuota);
            browser.onResponse(packet, length + MF_EVENT_RESPONSE_PREFIX); QCOMPARE(table->topLevelItemCount(), 0);
            for (auto* button : browser.findChildren<QPushButton*>()) if (button->text().startsWith("Upload")) QVERIFY(button->isEnabled());
            mf_message busy = captured.message; busy.kind = MF_RESPONSE; busy.status = MF_BUSY;
            length = mf_encode(&busy, packet + MF_EVENT_RESPONSE_PREFIX, MF_WIRE_MAX); browser.onResponse(packet, length + MF_EVENT_RESPONSE_PREFIX);
            QTRY_VERIFY_WITH_TIMEOUT(captured.message.request_id != busy.request_id, 1500);
            QCOMPARE(fileKeyValue(captured.message.owner), fileKey(22));
            selectors[1]->setCurrentIndex(0); QCOMPARE(quota->text(), QString("Select a member to see storage quota"));
            browser.onResponse(quotaPacket, quotaLength + MF_EVENT_RESPONSE_PREFIX);
            QCOMPARE(quota->text(), QString("Select a member to see storage quota"));
        }
        db.setDb(nullptr); QCOMPARE(sqlite3_close(sql), SQLITE_OK);
    }
};
QTEST_MAIN(BrowserTest)
#include "test_member_files_browser.moc"

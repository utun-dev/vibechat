/* Настоящий файловый core/DB worker: GUI не исполняет SQL, отмена и ошибки завершают запрос. */
#include "../transport/db_read.h"
#include "../transport/gui_bridge.h"
#include "../db/db_manager.h"
#include "../src/nodespage.h"
#include "../src/kanbanview.h"
#include "../src/inputbar.h"
#include "../src/messagedelegate.h"
#include <QClipboard>
#include <QRadioButton>
#include <QToolButton>
#include "../src/kanban_card_list.h"
#include "../src/avatars/groupavatarmanager.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QtTest>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QListWidget>
#include <QTextEdit>
#include <QCheckBox>
#include <QDateTimeEdit>
#include <QPushButton>
#include <QLabel>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
extern "C" {
#include "utun_instance.h"
#include "u_async.h"
#include "db_async.h"
#include "mem.h"
#include "debug_config.h"
#include "chat/chat_core.h"
#include "chat/chat_event.h"
#include "dm/dm_core.h"
#include "kanban/kanban.h"
#include "routing_layer/topo_group.h"
}

#ifdef Q_OS_LINUX
static std::atomic<int> failure{0};
extern "C" void* __real_u_malloc_impl(uint32_t, const char*);
extern "C" void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
extern "C" void* __wrap_u_malloc_impl(uint32_t bytes, const char* where) {
    int expected = 1;
    if (strstr(where, "gui_bridge_impl.cpp:") && failure.compare_exchange_strong(expected, 0)) return nullptr;
    return __real_u_malloc_impl(bytes, where);
}
extern "C" void* __wrap_u_calloc_impl(uint32_t count, uint32_t bytes, const char* where) {
    int expected = strstr(where, "gui_bridge_impl.cpp:") ? 2 : strstr(where, "db_async.c:") ? 3 : 0;
    if (expected && failure.compare_exchange_strong(expected, 0)) return nullptr;
    return __real_u_calloc_impl(count, bytes, where);
}
#endif

struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    std::atomic<bool> entered{false};
    bool released = false;
    void wait() {
        std::unique_lock<std::mutex> lock(mutex); entered = true;
        changed.wait(lock, [this] { return released; });
    }
    void release() { std::lock_guard<std::mutex> lock(mutex); released = true; changed.notify_all(); }
};

static std::shared_ptr<Gate> heldReadGate;
static bool pausedReader(sqlite3* db, QObject* context, GuiDbReadWork work, GuiDbReadDone done) {
    return gui_bridge_db_read(db, context, [work = std::move(work), gate = heldReadGate](sqlite3* handle, QVariant* value) {
        const int rc = work(handle, value); gate->wait(); return rc;
    }, std::move(done));
}

struct Core {
    size_t baseline = u_get_allocated_count();
    QTemporaryDir directory;
    UASYNC* ua = nullptr;
    UTUN_INSTANCE* instance = nullptr;
    std::shared_ptr<Gate> gate;
    DbManager manager;
    bool start() {
        if (!directory.isValid() || !(ua = uasync_create())) return false;
        const auto config = QByteArray("[global]\nauto_sockets=no\n") +
            "my_private_key=704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f\n"
            "my_public_key=7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f\n"
            "db_path=" + directory.path().toUtf8() + "\n[ntp]\nenabled=no\n"
            "[chatserver]\ngroup_autoconnect=0\nmember_files_enabled=0\nstorage_autoload=0\n";
        instance = utun_instance_create_from_str(ua, config.constData());
        if (!instance || !instance->db_worker) return false;
        uasync_mark_running(ua); attach();
        manager.setDbPath(directory.path()); manager.setDb(instance->topo_sqlite_db); manager.setAsyncReader(gui_bridge_db_read); return true;
    }
    void attach() { gui_bridge_set_uasync(ua); gui_bridge_set_inst(instance); }
    void pulse() { uasync_poll(ua, 0); QCoreApplication::processEvents(); QTest::qWait(1); }
    template<class Predicate> bool wait(Predicate ready) {
        QElapsedTimer timer; timer.start();
        do { pulse(); if (ready()) return true; } while (timer.elapsed() < 5000);
        return false;
    }
    void shutdown() {
        if (gate) gate->release();
        gui_bridge_set_inst(nullptr); gui_bridge_set_uasync(nullptr); manager.setDb(nullptr);
        if (ua) uasync_mark_stopped(ua);
        if (instance) { utun_instance_destroy(instance); instance = nullptr; }
    }
    ~Core() {
#ifdef Q_OS_LINUX
        failure = 0;
#endif
        shutdown(); if (ua) uasync_destroy(ua, 0); QCoreApplication::processEvents();
        Q_ASSERT(u_get_allocated_count() == baseline);
    }
};

static int selectValue(sqlite3* db, QVariant* value) {
    sqlite3_stmt* statement = nullptr;
    int rc = sqlite3_prepare_v2(db, "SELECT 42", -1, &statement, nullptr);
    if (rc == SQLITE_OK) rc = sqlite3_step(statement);
    if (rc == SQLITE_ROW) *value = sqlite3_column_int(statement, 0);
    int final = sqlite3_finalize(statement);
    return rc == SQLITE_ROW ? final : rc;
}

class GuiDbReadTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); gui_bridge_init(); }
    void resultRunsInGuiAndCallbackCanSubmitAgain() {
        Core core; QVERIFY(core.start()); QObject context; int calls = 0;
        const auto gui = QThread::currentThreadId();
        auto work = [gui](sqlite3* db, QVariant* value) {
            if (QThread::currentThreadId() == gui) return SQLITE_MISUSE;
            return selectValue(db, value);
        };
        QVERIFY(gui_bridge_db_read(core.instance->topo_sqlite_db, &context, work,
            [&](const db_async_result& result, const QVariant& value) {
                QCOMPARE(QThread::currentThreadId(), gui); QCOMPARE(result.status, DB_ASYNC_OK); QCOMPARE(value.toInt(), 42); calls++;
                QVERIFY(gui_bridge_db_read(core.instance->topo_sqlite_db, &context, work,
                    [&](const db_async_result& next, const QVariant& answer) {
                        QCOMPARE(next.status, DB_ASYNC_OK); QCOMPARE(answer.toInt(), 42); calls++;
                    }));
            }));
        QCOMPARE(calls, 0); QVERIFY(core.wait([&] { return calls == 2; }));
        for (int i = 0; i < 3; i++) core.pulse(); QCOMPARE(calls, 2);
    }
    void sqlFailureAndExceptionAllowNextRead() {
        Core core; QVERIFY(core.start()); QObject context;
        for (int type = 0; type < 2; type++) {
            int calls = 0;
            QVERIFY(gui_bridge_db_read(core.instance->topo_sqlite_db, &context,
                [type](sqlite3* db, QVariant* value) -> int {
                    *value = QStringLiteral("partial");
                    if (type) throw std::runtime_error("injected reader failure");
                    return sqlite3_exec(db, "SELECT * FROM missing_gui_table", nullptr, nullptr, nullptr);
                }, [&](const db_async_result& result, const QVariant& value) {
                    QCOMPARE(result.status, DB_ASYNC_SQL); QVERIFY(!value.isValid()); calls++;
                }));
            QVERIFY(core.wait([&] { return calls == 1; }));
            QVERIFY(gui_bridge_db_read(core.instance->topo_sqlite_db, &context, selectValue,
                [&](const db_async_result& result, const QVariant& value) {
                    QCOMPARE(result.status, DB_ASYNC_OK); QCOMPARE(value.toInt(), 42); calls++;
                }));
            QVERIFY(core.wait([&] { return calls == 2; }));
        }
    }
    void cancelBeforeExecutionAndDestroyContext() {
        Core core; QVERIFY(core.start()); QObject context; int cancelled = 0;
        auto executed = std::make_shared<std::atomic<int>>(0);
        QVERIFY(gui_bridge_db_read(core.instance->topo_sqlite_db, &context,
            [executed](sqlite3* db, QVariant* value) { (*executed)++; return selectValue(db, value); },
            [&](const db_async_result& result, const QVariant&) { QCOMPARE(result.status, DB_ASYNC_CLOSED); cancelled++; }));
        gui_bridge_set_inst(nullptr); QCoreApplication::processEvents(); QCOMPARE(cancelled, 1); QCOMPARE(executed->load(), 0);
        core.attach();
        auto* temporary = new QObject; int late = 0;
        QVERIFY(gui_bridge_db_read(core.instance->topo_sqlite_db, temporary,
            [executed](sqlite3* db, QVariant* value) { (*executed)++; return selectValue(db, value); },
            [&](const db_async_result&, const QVariant&) { late++; }));
        delete temporary;
        QVERIFY(core.wait([&] { return executed->load() == 1 && !db_async_pending(core.instance->db_worker); }));
        QCOMPARE(late, 0); QCOMPARE(cancelled, 1);
    }
    void slowSqlKeepsGuiAliveAndShutdownCompletesActiveAndQueued() {
        Core core; QVERIFY(core.start()); QObject context; int active = 0, queued = 0;
        core.gate = std::make_shared<Gate>(); auto gate = core.gate;
        QVERIFY(gui_bridge_db_read(core.instance->topo_sqlite_db, &context,
            [gate](sqlite3* db, QVariant* value) { gate->wait(); return selectValue(db, value); },
            [&](const db_async_result& result, const QVariant& value) {
                QCOMPARE(result.status, DB_ASYNC_OK); QCOMPARE(value.toInt(), 42); active++;
            }));
        QVERIFY(core.wait([&] { return gate->entered.load(); }));
        QVERIFY(gui_bridge_db_read(core.instance->topo_sqlite_db, &context, selectValue,
            [&](const db_async_result& result, const QVariant&) { QCOMPARE(result.status, DB_ASYNC_CLOSED); queued++; }));
        QVERIFY(core.wait([&] { return db_async_pending(core.instance->db_worker) == 2; }));
        bool tick = false; QTimer::singleShot(0, &context, [&] { tick = true; });
        QVERIFY(core.wait([&] { return tick; })); QCOMPARE(active, 0); QCOMPARE(queued, 0);
        core.shutdown(); QCoreApplication::processEvents(); QCOMPARE(active, 1); QCOMPARE(queued, 1);
    }
    void quickAndFullUseTheCommonBackend() {
        Core core; QVERIFY(core.start()); QObject context;
        for (bool full : {false, true}) {
            int calls = 0;
            QVERIFY(core.manager.integrityCheckAsync(full, &context, [&](IntegrityResult result) {
                QVERIFY2(result.ok, qPrintable(result.message)); QCOMPARE(result.sqliteCode, SQLITE_OK); calls++;
            }));
            QCOMPARE(calls, 0); QVERIFY(core.wait([&] { return calls == 1; }));
        }
        int stale = 0;
        QVERIFY(core.manager.integrityCheckAsync(false, &context, [&](IntegrityResult result) {
            QVERIFY(!result.ok); QCOMPARE(result.sqliteCode, SQLITE_ABORT); stale++;
        }));
        core.manager.setDb(nullptr); core.manager.setDb(core.instance->topo_sqlite_db);
        QVERIFY(core.wait([&] { return stale == 1; }));
        core.manager.setAsyncReader(nullptr);
        QVERIFY(!core.manager.integrityCheckAsync(false, &context, [](IntegrityResult) { QFAIL("rejected request completed"); }));
    }
    void readViewFreezesMetadataAndPreservesTheFirstGetterError() {
        Core core; QVERIFY(core.start()); QObject context; int calls = 0;
        const auto path = core.directory.path(); core.manager.setDbPath(path);
        const auto gui = QThread::currentThreadId();
        QVERIFY(core.manager.readAsync(&context, [path, gui](DbReadView& view, QVariant* value) {
            if (view.dbPath() != path || QThread::currentThreadId() == gui) return SQLITE_MISUSE;
            return selectValue(view.m_db, value);
        }, [&](const db_async_result& status, const QVariant& value) {
            QCOMPARE(status.status, DB_ASYNC_OK); QCOMPARE(value.toInt(), 42); calls++;
        }));
        core.manager.setDbPath("changed");
        QVERIFY(core.wait([&] { return calls == 1; }));
        QVERIFY(core.manager.readAsync(&context, [](DbReadView& view, QVariant* value) {
            view.getMessages("nonexistent"); view.getUiState("absent"); *value = QStringLiteral("partial"); return SQLITE_OK;
        }, [&](const db_async_result& status, const QVariant& value) {
            QCOMPARE(status.status, DB_ASYNC_SQL); QCOMPARE(status.sqlite_code, SQLITE_ERROR); QVERIFY(!value.isValid()); calls++;
        }));
        QVERIFY(core.wait([&] { return calls == 2; }));
        QVERIFY(core.manager.readAsync(&context, [](DbReadView& view, QVariant* value) { return selectValue(view.m_db, value); },
            [&](const db_async_result& status, const QVariant& value) {
                QCOMPARE(status.status, DB_ASYNC_OK); QCOMPARE(value.toInt(), 42); calls++;
            }));
        QVERIFY(core.wait([&] { return calls == 3; }));
    }
    void navigationReturnsOwnedGroupsPreviewsAvatarsAndConversations() {
        Core core; QVERIFY(core.start()); QCOMPARE(utun_core_start(core.instance), 0); QCOMPARE(chat_service_start(core.instance), 0);
        const auto group = chat_core_create_channel_auto(core.instance, "Chat"); QVERIFY(group);
        QVERIFY(group_core_create_auto(core.instance, "Board", "kanban"));
        const QByteArray id(topo_groups_channel(core.instance->topo_groups, group)); QCOMPARE(id.size(), 64);
        chat_msg_submit message{}; memcpy(message.channel_id, id.constData(), size_t(id.size()));
        memcpy(message.content_type, "text/plain", 11); uint8_t body[] = "Hello from a snapshot";
        message.data = body; message.data_len = sizeof(body) - 1; QCOMPARE(chat_core_submit_message(core.instance, &message), 0);
        const auto peer = QByteArray::fromHex("7eed321823c7cb73564d958d516c37d7cb4b2455b6222cd5a0aab5b6bdcacb5d");
        uint8_t peerX[32];
        QCOMPARE(sc_identity_public_x("GUI navigation test", (const uint8_t*)peer.constData(), peerX), SC_OK);
        const uint64_t peerId = sc_derive_node_id_from_pubkey(peerX);
        QCOMPARE(dm_start(core.instance, peerId, peerX, (const uint8_t*)peer.constData(),
            "PM", nullptr, DM_MODE_DEVICE, nullptr), 0);
        // Подписанное удаление: непустая версия и пустой BLOB должны сохраниться как самостоятельное значение.
        QCOMPARE(sqlite3_exec(core.instance->topo_sqlite_db,
            "UPDATE channels SET avatar_version=1,avatar_data=x'',avatar_hash="
            "x'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855' WHERE name='Chat'",
            nullptr, nullptr, nullptr), SQLITE_OK);
        NavigationSnapshot snapshot; int calls = 0; QObject context;
        QVERIFY(core.manager.readAsync(&context, [](DbReadView& view, QVariant* value) {
            *value = QVariant::fromValue(view.getNavigation()); return SQLITE_OK;
        }, [&](const db_async_result& status, const QVariant& value) {
            QCOMPARE(status.status, DB_ASYNC_OK); snapshot = value.value<NavigationSnapshot>(); calls++;
        }));
        QVERIFY(core.wait([&] { return calls == 1; }));
        QCOMPARE(snapshot.channels.size(), 2); QCOMPARE(snapshot.conversations.size(), 1);
        QCOMPARE(snapshot.channels[0].groupType, QString("kanban")); QCOMPARE(snapshot.channels[1].groupType, QString("chat"));
        QVERIFY(snapshot.previews.value(QString::fromLatin1(id)).contains("Hello from a snapshot"));
        QVERIFY(snapshot.previews.value(snapshot.channels[0].channelId).isEmpty());
        QVERIFY(snapshot.avatars.contains(QString::fromLatin1(id))); QVERIFY(snapshot.avatars.value(QString::fromLatin1(id)).isEmpty());
        core.shutdown(); QCOMPARE(snapshot.channels[1].name, QString("Chat")); QCOMPARE(snapshot.conversations[0].peerName, QString("PM"));
    }
    void nodesPageReadsInWorkerAndRecoversFromSqlFailureAndDbReset() {
        Core core; QVERIFY(core.start()); NodesPage page(&core.manager);
        auto* table = page.findChild<QTableWidget*>(); auto* text = page.findChild<QPlainTextEdit*>();
        auto* refresh = page.findChild<QPushButton*>(); QVERIFY(table && text && refresh);
        core.gate = std::make_shared<Gate>(); auto gate = core.gate; QObject context; int blocker = 0;
        QVERIFY(core.manager.readAsync(&context, [gate](DbReadView& view, QVariant* value) {
            gate->wait(); return selectValue(view.m_db, value);
        }, [&](const db_async_result& status, const QVariant&) { QCOMPARE(status.status, DB_ASYNC_OK); blocker++; }));
        QVERIFY(core.wait([&] { return gate->entered.load(); }));
        page.refreshNodes(); page.refreshNodes(); QVERIFY(!refresh->isEnabled()); QCOMPARE(table->rowCount(), 0);
        bool tick = false; QTimer::singleShot(0, &page, [&] { tick = true; });
        QVERIFY(core.wait([&] { return tick; })); QCOMPARE(blocker, 0);
        gate->release(); QVERIFY(core.wait([&] { return refresh->isEnabled() && table->rowCount() == 1; }));
        QCOMPARE(blocker, 1);
        QCOMPARE(sqlite3_exec(core.instance->topo_sqlite_db,
            "ALTER TABLE node_addresses RENAME TO hidden_addresses", nullptr, nullptr, nullptr), SQLITE_OK);
        page.refreshNodes(); QVERIFY(core.wait([&] { return refresh->isEnabled(); }));
        QCOMPARE(table->rowCount(), 0); QCOMPARE(text->toPlainText(), QString("Query error"));
        QCOMPARE(sqlite3_exec(core.instance->topo_sqlite_db,
            "ALTER TABLE hidden_addresses RENAME TO node_addresses", nullptr, nullptr, nullptr), SQLITE_OK);
        page.refreshNodes(); QVERIFY(core.wait([&] { return refresh->isEnabled() && table->rowCount() == 1; }));
        page.refreshNodes(); core.manager.setDb(nullptr); page.refreshNodes();
        QCOMPARE(table->rowCount(), 0); QCOMPARE(text->toPlainText(), QString("Database not connected"));
        core.manager.setDb(core.instance->topo_sqlite_db); page.refreshNodes();
        QVERIFY(core.wait([&] { return refresh->isEnabled() && table->rowCount() == 1; }));
    }
    void kanbanSelectionDetailsDraftsAndDiscussion() {
        Core core; QVERIFY(core.start()); QCOMPARE(utun_core_start(core.instance), 0); QCOMPARE(kanban_service_start(core.instance), 0);
        const auto group = group_core_create_auto(core.instance, "Card details", "kanban"); QVERIFY(group);
        const QByteArray channel(topo_groups_channel(core.instance->topo_groups, group)); QCOMPARE(channel.size(), 64);
        auto execute = [&](QJsonObject command) {
            struct Reply { bool done = false; QJsonObject value; } reply;
            const auto bytes = QJsonDocument(command).toJson(QJsonDocument::Compact);
            kanban_execute_submit(core.instance, channel.constData(), bytes.constData(), [](void* arg, const kanban_execute_result* result) {
                auto* reply = static_cast<Reply*>(arg); reply->value = QJsonDocument::fromJson(result->json).object(); reply->done = true;
            }, &reply);
            if (!core.wait([&] { return reply.done; })) qFatal("card command did not complete");
            return reply.value;
        };
        auto snapshot = execute({{"action", "snapshot"}}); QVERIFY(snapshot["ok"].toBool());
        const auto columns = snapshot["board"].toObject()["columns"].toArray(); QVERIFY(!columns.isEmpty());
        const auto column = columns[0].toObject()["id"].toString();
        const auto first = execute({{"action", "card_create"}, {"column", column}, {"text", "first"}})["id"].toString(); QVERIFY(!first.isEmpty());
        const auto second = execute({{"action", "card_create"}, {"column", column}, {"text", "second"}})["id"].toString(); QVERIFY(!second.isEmpty());
        chat_event_set_handler(core.instance, [](UTUN_INSTANCE*, int type, const uint8_t* bytes, int length) { gui_bridge_post(type, bytes, length); });
        KanbanView board(nullptr, &core.manager); board.resize(900, 500); board.show();
        QSignalSpy selection(&board, &KanbanView::cardSelected); board.setGroup(QString::fromLatin1(channel));
        auto cards = [&]() -> QListWidget* {
            for (auto* list : board.findChildren<QListWidget*>())
                if (list->count() == 2 && list->item(0)->data(Qt::UserRole).toString() == first) return list;
            return nullptr;
        };
        QVERIFY(core.wait([&] { return cards(); }));
        auto* list = cards(); QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, list->visualItemRect(list->item(0)).center());
        QVERIFY(core.wait([&] { return !selection.isEmpty() && selection.last()[0].toBool(); }));
        auto* detail = board.detailsPanel(); QVERIFY(!detail->findChild<QTextEdit*>("kanbanDetailText"));
        auto cardText = [&](int row) { auto* current = cards(); return current ? current->item(row)->text() : QString(); };
        QCOMPARE(cardText(0), QString("first")); QVERIFY(!board.findChild<QWidget*>("kanbanInlineEditor"));
        auto* input = detail->findChild<QTextEdit*>("kanbanChatInput"); QVERIFY(input); input->setPlainText("draft on first");
        auto* due = detail->findChild<QDateTimeEdit*>(); auto* hasDue = detail->findChild<QCheckBox*>(); QVERIFY(due && hasDue);
        hasDue->setChecked(true); const auto deadline = QDateTime::currentDateTime().addDays(3); due->setDateTime(deadline);
        execute({{"action", "card_edit"}, {"entity", first}, {"text", "remote"}});
        QVERIFY(core.wait([&] { return cardText(0) == "remote"; }));
        QVERIFY(hasDue->isChecked()); QCOMPARE(due->dateTime(), deadline);
        list = cards(); QVERIFY(list);
        QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, list->visualItemRect(list->item(0)).center());
        QVERIFY(core.wait([&] { return board.findChild<QTextEdit*>("kanbanInlineText"); }));
        auto* editor = board.findChild<QTextEdit*>("kanbanInlineText"); QCOMPARE(editor->toPlainText(), QString("remote"));
        editor->setPlainText("edited inline");
        list = cards(); QVERIFY(list); QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, list->visualItemRect(list->item(1)).center());
        QVERIFY(core.wait([&] { return input->toPlainText().isEmpty() && !board.findChild<QWidget*>("kanbanInlineEditor"); }));
        QVERIFY(input->toPlainText().isEmpty());
        list = cards(); QVERIFY(list); QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, list->visualItemRect(list->item(0)).center());
        QCOMPARE(cardText(0), QString("edited inline")); QVERIFY(!board.findChild<QWidget*>("kanbanInlineEditor"));
        QCOMPARE(input->toPlainText(), QString("draft on first")); QVERIFY(hasDue->isChecked());
        input->setPlainText("hello https://example.com <script>");
        QTest::keyClick(input, Qt::Key_Return);
        auto* messages = detail->findChild<QListView*>("kanbanMessages"); QVERIFY(messages);
        QVERIFY(core.wait([&] { return messages->model()->rowCount() == 1 && input->toPlainText().isEmpty(); }));
        QCOMPARE(messages->model()->index(0, 0).data(Qt::UserRole + 2).toString(), QString("hello https://example.com <script>"));
        static KanbanView* submitting = nullptr; submitting = &board;
        gui_bridge_set_msg_submitted_cb([](const uint8_t* data, int size) { if (submitting) submitting->onMessageSubmitted(data, size); });
        const auto unhook = qScopeGuard([] { submitting = nullptr; gui_bridge_set_msg_submitted_cb(nullptr); });
        QImage screenshot(240, 120, QImage::Format_RGB32); screenshot.fill(QColor("#2575c0"));
        QApplication::clipboard()->setImage(screenshot); QTest::keyClick(input, Qt::Key_V, Qt::ControlModifier);
        auto* png = detail->findChild<QRadioButton*>("clipboardPngChoice"); QVERIFY(png);
        QVERIFY(core.wait([&] { return png->isEnabled(); })); png->click(); QTest::keyClick(input, Qt::Key_Return);
        QVERIFY(core.wait([&] { return messages->model()->rowCount() == 2; }));
        auto photo = [&] { return messages->model()->index(1, 0); }; QCOMPARE(photo().data(MsgContentTypeRole).toString(), QString("image/png"));
        const auto localFile = photo().data(MsgVoiceFileRole).toString();
        QVERIFY2(QFileInfo::exists(localFile), qPrintable(QStringLiteral("Missing local Kanban attachment: %1").arg(localFile)));
        QVERIFY(core.wait([&] { return !photo().data(MsgImagePreviewRole).value<QImage>().isNull(); }));
        QCOMPARE(photo().data(MsgImagePreviewRole).value<QImage>().size(), screenshot.size());
        // Историческое перемещение в конец не должно вытолкнуть следующую созданную карточку вверх.
        QVERIFY(execute({{"action", "card_move"}, {"entity", second}, {"column", column}, {"before", ""}})["ok"].toBool());
        detail->findChild<QToolButton*>("kanbanCloseCard")->click(); QVERIFY(!selection.last()[0].toBool());
        list = cards(); auto* createList = dynamic_cast<CardList*>(list); QVERIFY(createList);
        QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, createList->newCardRect().center());
        QVERIFY(core.wait([&] { return board.findChild<QTextEdit*>("kanbanInlineText"); }));
        board.findChild<QTextEdit*>("kanbanInlineText")->setPlainText("new with one click"); board.finishEditing();
        QVERIFY(core.wait([&] {
            for (auto* candidate : board.findChildren<QListWidget*>()) if (candidate->count() == 3 && !board.findChild<QWidget*>("kanbanInlineEditor")) return true;
            return false;
        }));
        for (auto* candidate : board.findChildren<QListWidget*>()) if (candidate->count() == 3)
            QCOMPARE(candidate->item(2)->text(), QString("new with one click"));
    }
    void kanbanWidthReadDoesNotOverwriteAResizeAndCacheResetsWithDatabase() {
        Core core; QVERIFY(core.start()); QCOMPARE(utun_core_start(core.instance), 0); QCOMPARE(kanban_service_start(core.instance), 0);
        const auto group = group_core_create_auto(core.instance, "Board", "kanban"); QVERIFY(group);
        const QByteArray channel(topo_groups_channel(core.instance->topo_groups, group)); QCOMPARE(channel.size(), 64);
        char* json = nullptr;
        QCOMPARE(kanban_snapshot(core.instance, channel.constData(), &json), 0); QVERIFY(json);
        const auto columns = QJsonDocument::fromJson(json).object()["columns"].toArray(); u_free(json); QVERIFY(columns.size() >= 2);
        const auto column = columns[0].toObject()["id"].toString();
        const auto key = QString("kanban_%1_%2_width").arg(QString::fromLatin1(channel), column).toUtf8();
        const auto save = [&](int width) {
            sqlite3_stmt* statement = nullptr;
            int rc = sqlite3_prepare_v2(core.instance->topo_sqlite_db, "INSERT OR REPLACE INTO ui_state(key,value) VALUES(?1,?2)",
                -1, &statement, nullptr);
            if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 1, key.constData(), key.size(), SQLITE_STATIC);
            if (rc == SQLITE_OK) rc = sqlite3_bind_int(statement, 2, width);
            if (rc == SQLITE_OK) rc = sqlite3_step(statement);
            const int final = sqlite3_finalize(statement); return rc == SQLITE_DONE ? final : rc;
        };
        QCOMPARE(save(450), SQLITE_OK); core.gate = std::make_shared<Gate>(); heldReadGate = core.gate;
        core.manager.setAsyncReader(pausedReader);
        chat_event_set_handler(core.instance, [](UTUN_INSTANCE*, int type, const uint8_t* bytes, int length) {
            gui_bridge_post(type, bytes, length);
        });
        KanbanView board(nullptr, &core.manager); board.resize(900, 500); board.show(); board.setGroup(QString::fromLatin1(channel));
        QVERIFY(core.wait([&] { return core.gate->entered.load(); }));
        auto* pane = board.findChild<QWidget*>("kanbanColumn_" + column); auto* grip = board.findChild<QWidget*>("kanbanColumnGrip");
        QVERIFY(pane && grip); QCOMPARE(pane->width(), 290);
        const auto point = grip->rect().center(); QTest::mousePress(grip, Qt::LeftButton, Qt::NoModifier, point);
        const QPoint moved = point + QPoint(80, 0); QTest::mouseMove(grip, moved); QTest::mouseRelease(grip, Qt::LeftButton, Qt::NoModifier, point);
        const int resized = pane->width(); QVERIFY(resized > 290); QVERIFY(resized != 450);
        core.gate->release(); QVERIFY(core.wait([&] { return !db_async_pending(core.instance->db_worker); }));
        QCOMPARE(pane->width(), resized);
        QCOMPARE(save(660), SQLITE_OK); core.manager.setDb(nullptr); core.manager.setDb(core.instance->topo_sqlite_db);
        board.setGroup(QString::fromLatin1(channel));
        QVERIFY(core.wait([&] {
            const auto* current = board.findChild<QWidget*>("kanbanColumn_" + column); return current && current->width() == 660;
        }));
        heldReadGate.reset();
    }

    void kanbanLinksOpenOldMessageAndLastRequestWins() {
        Core core; QVERIFY(core.start()); QCOMPARE(utun_core_start(core.instance), 0); QCOMPARE(kanban_service_start(core.instance), 0);
        const auto group = group_core_create_auto(core.instance, "Link board", "kanban"); QVERIFY(group);
        const QByteArray channel(topo_groups_channel(core.instance->topo_groups, group)); QCOMPARE(channel.size(), 64);
        auto execute = [&](QJsonObject command) {
            struct Reply { bool done = false; QJsonObject value; } reply;
            const auto bytes = QJsonDocument(command).toJson(QJsonDocument::Compact);
            kanban_execute_submit(core.instance, channel.constData(), bytes.constData(), [](void* arg, const kanban_execute_result* result) {
                auto* reply = static_cast<Reply*>(arg); reply->value = QJsonDocument::fromJson(result->json).object(); reply->done = true;
            }, &reply);
            if (!core.wait([&] { return reply.done; })) qFatal("link fixture command did not complete");
            return reply.value;
        };
        const auto snapshot = execute({{"action", "snapshot"}}); QVERIFY(snapshot["ok"].toBool());
        const auto columns = snapshot["board"].toObject()["columns"].toArray(); QVERIFY(!columns.isEmpty());
        const auto column = columns[0].toObject()["id"].toString();
        const auto first = execute({{"action", "card_create"}, {"column", column}, {"text", "first"}})["id"].toString();
        const auto second = execute({{"action", "card_create"}, {"column", column}, {"text", "second"}})["id"].toString();
        const auto old = execute({{"action", "message_create"}, {"entity", first}, {"text", "old target"}})["id"].toString();
        QVERIFY(old.size() == 64 && first.size() == 64 && second.size() == 64);
        for (int i = 0; i < 55; i++) QVERIFY(execute({{"action", "message_create"}, {"entity", first}, {"text", QString::number(i)}})["ok"].toBool());
        const auto other = execute({{"action", "message_create"}, {"entity", second}, {"text", "other target"}})["id"].toString();
        chat_event_set_handler(core.instance, [](UTUN_INSTANCE*, int type, const uint8_t* bytes, int length) { gui_bridge_post(type, bytes, length); });
        KanbanView board(nullptr, &core.manager); board.resize(900, 500); board.show(); board.setGroup(QString::fromLatin1(channel));
        auto* messages = board.detailsPanel()->findChild<QListView*>("kanbanMessages"); QVERIFY(messages);
        QSignalSpy missing(&board, &KanbanView::targetUnavailable);
        auto highlighted = [&](const QString& id) {
            for (int row = 0; row < messages->model()->rowCount(); row++) {
                const auto index = messages->model()->index(row, 0);
                if (index.data(MsgMessageKeyRole).toString() == id && index.data(MsgHighlightRole).toBool()) return true;
            }
            return false;
        };
        board.openTarget(first, old); QVERIFY(core.wait([&] { return highlighted(old); }));
        QVERIFY(messages->model()->rowCount() <= 50); QCOMPARE(missing.size(), 0);
        QVERIFY(execute({{"action", "message_create"}, {"entity", first}, {"text", "newer while viewing old target"}})["ok"].toBool());
        QVERIFY(core.wait([&] {
            for (int row = 0; row < messages->model()->rowCount(); row++)
                if (messages->model()->index(row, 0).data(MsgMessageKeyRole).toString() == old) return true;
            return false;
        }));
        auto* input = board.detailsPanel()->findChild<QTextEdit*>("kanbanChatInput"); QVERIFY(input); input->setPlainText("keep draft");
        board.openTarget(first, old); board.openTarget(second, other);
        QVERIFY(core.wait([&] { return highlighted(other); })); QCOMPARE(missing.size(), 0);
        board.openTarget(first, old); QVERIFY(core.wait([&] { return highlighted(old); })); QCOMPARE(input->toPlainText(), QString("keep draft"));
        board.openTarget(second, old); QVERIFY(core.wait([&] { return missing.size() == 1; }));
        board.openTarget(QString(64, '0')); QVERIFY(core.wait([&] { return missing.size() == 2; }));
        board.openTarget(first, old); board.setGroup({});
        for (int i = 0; i < 20; i++) core.pulse();
        QCOMPARE(messages->model()->rowCount(), 0); QCOMPARE(missing.size(), 2);
    }
    void avatarReadsCoalesceAndCannotReplaceANewerExplicitSnapshot() {
        Core core; QVERIFY(core.start()); QCOMPARE(utun_core_start(core.instance), 0); QCOMPARE(chat_service_start(core.instance), 0);
        const auto group = chat_core_create_channel_auto(core.instance, "Avatars"); QVERIFY(group);
        const QString channel = QString::fromLatin1(topo_groups_channel(core.instance->topo_groups, group)); QCOMPARE(channel.size(), 64);
        QVERIFY(core.wait([&] { return !db_async_pending(core.instance->db_worker); }));
        auto jpeg = [](const QColor& color) {
            QImage image(32, 32, QImage::Format_RGB32); image.fill(color); QByteArray bytes; QBuffer buffer(&bytes);
            if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "JPEG")) return QByteArray(); return bytes;
        };
        const auto red = jpeg(Qt::red), blue = jpeg(Qt::blue); QVERIFY(!red.isEmpty() && !blue.isEmpty());
        const auto blueHash = QCryptographicHash::hash(blue, QCryptographicHash::Sha256);
        auto save = [&](int version, const QByteArray& bytes) {
            const auto hash = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256); const auto id = channel.toLatin1();
            sqlite3_stmt* statement = nullptr;
            int rc = sqlite3_prepare_v2(core.instance->topo_sqlite_db,
                "UPDATE channels SET avatar_version=?1,avatar_data=?2,avatar_hash=?3 WHERE channel_id=?4", -1, &statement, nullptr);
            if (rc == SQLITE_OK) rc = sqlite3_bind_int(statement, 1, version);
            if (rc == SQLITE_OK) rc = sqlite3_bind_blob(statement, 2, bytes.constData(), bytes.size(), SQLITE_STATIC);
            if (rc == SQLITE_OK) rc = sqlite3_bind_blob(statement, 3, hash.constData(), hash.size(), SQLITE_STATIC);
            if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 4, id.constData(), id.size(), SQLITE_STATIC);
            if (rc == SQLITE_OK) rc = sqlite3_step(statement);
            const int final = sqlite3_finalize(statement); return rc == SQLITE_DONE ? final : rc;
        };
        QCOMPARE(save(1, red), SQLITE_OK); core.gate = std::make_shared<Gate>(); heldReadGate = core.gate;
        core.manager.setAsyncReader(pausedReader); GroupAvatarManager avatars(&core.manager); QSignalSpy changed(&avatars, &GroupAvatarManager::changed);
        QVERIFY(avatars.image(channel).isNull()); QVERIFY(avatars.image(channel).isNull());
        QVERIFY(core.wait([&] { return core.gate->entered.load(); })); QCOMPARE(db_async_pending(core.instance->db_worker), 1u);
        QCOMPARE(save(2, blue), SQLITE_OK); QVERIFY(avatars.image(channel, 2, blueHash, &blue).isNull());
        QVERIFY(core.wait([&] { return !avatars.currentImage(channel).isNull(); }));
        QVERIFY(avatars.currentImage(channel).pixelColor(10, 10).blue() > 240); QCOMPARE(changed.count(), 1);
        core.gate->release(); QVERIFY(core.wait([&] { return !db_async_pending(core.instance->db_worker); }));
        QVERIFY(avatars.currentImage(channel).pixelColor(10, 10).blue() > 240); QCOMPARE(changed.count(), 1);
        core.manager.setDb(nullptr); core.manager.setDb(core.instance->topo_sqlite_db); QVERIFY(avatars.currentImage(channel).isNull());
        QVERIFY(avatars.image(channel).isNull());
        QVERIFY(core.wait([&] { return !avatars.currentImage(channel).isNull(); }));
        QVERIFY(avatars.currentImage(channel).pixelColor(10, 10).blue() > 240); heldReadGate.reset();
    }
#ifdef Q_OS_LINUX
    void allocationErrorsReleaseRequestsAndReportDbRejection() {
        Core core; QVERIFY(core.start()); QObject context;
        QVERIFY(core.wait([&] { return !db_async_pending(core.instance->db_worker); }));
        for (int mode = 1; mode <= 3; mode++) {
            const auto before = u_get_allocated_count(); int calls = 0; failure = mode;
            bool accepted = gui_bridge_db_read(core.instance->topo_sqlite_db, &context, selectValue,
                [&](const db_async_result& result, const QVariant&) { QCOMPARE(result.status, DB_ASYNC_NOMEM); calls++; });
            QCOMPARE(accepted, mode == 3);
            if (accepted) QVERIFY(core.wait([&] { return calls == 1; })); else QCOMPARE(calls, 0);
            QCOMPARE(failure.load(), 0); QCOMPARE(u_get_allocated_count(), before);
        }
    }
#endif
};

QTEST_MAIN(GuiDbReadTest)
#include "test_gui_db_read.moc"

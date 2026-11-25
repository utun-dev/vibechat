/* Формат канального события: live/history сохраняются при передаче в GUI, битые события отбрасываются. */
#include "../transport/gui_bridge.h"
#include "../src/connmonitorwindow.h"
#include "../../../lib/debug_config.h"
#include "../../../src/chat/chat_event.h"
#include "../../../src/chat/chat_core.h"
#include "../../../src/utun_instance.h"
#include "../../../lib/u_async.h"
#include "../../../lib/mem.h"
#include <QtTest>
#include <cstring>
#include <thread>
#include <future>
#include <QPointer>
#include <QScopeGuard>

#ifdef UTUN_TEST_BRIDGE_ALLOC_FAILURES
static int failAllocation;
extern "C" void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
extern "C" void* __wrap_u_calloc_impl(uint32_t count, uint32_t size, const char* where) {
    if (failAllocation && strstr(where, "gui_bridge_impl.cpp") && --failAllocation == 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "[INJECT] GUI bridge allocation failure at %s", where); return nullptr;
    }
    return __real_u_calloc_impl(count, size, where);
}
#endif

struct ReceivedMessage {
    QString channel;
    QByteArray author;
    bool initialSync;
    bool guiThread;
};
static QList<ReceivedMessage> received;
static QList<int> cpuLoads;
static bool cpuOnGuiThread = true;
static QStringList changedChannels;
static QList<QByteArray> edits;
static QList<QByteArray> submitted;
static int readyEvents;
static constexpr char testChannel[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

struct BoundCore {
    UASYNC* ua = uasync_create();
    UTUN_INSTANCE instance{};
    ~BoundCore() {
        gui_bridge_set_inst(nullptr); gui_bridge_set_uasync(nullptr);
        if (ua) { uasync_mark_stopped(ua); uasync_destroy(ua, 0); }
    }
    void attach() {
        instance.ua = ua; uasync_mark_running(ua);
        gui_bridge_set_uasync(ua); gui_bridge_set_inst(&instance);
    }
};

struct MonitorGuard {
    QPointer<ConnMonitorWindow> window = ConnMonitorWindow::showMonitor();
    MonitorGuard() { window->findChild<QTimer*>()->stop(); }
    ~MonitorGuard() { delete window.data(); }
};

static QByteArray connection_list(std::initializer_list<quint64> nodes) {
    QByteArray bytes(2 + 46 * nodes.size(), '\0');
    uint16_t count = nodes.size(); memcpy(bytes.data(), &count, 2);
    int index = 0;
    for (quint64 node : nodes) {
        char* row = bytes.data() + 2 + 46 * index++;
        memcpy(row, &node, 8); row[8] = 3; row[9] = 1; memcpy(row + 14, "peer", 4);
    }
    return bytes;
}

static int pending_count(UASYNC* ua) {
    int count = 0;
    for (auto* task = ua->posted_tasks_head; task; task = task->next) count++;
    return count;
}

static void cpu_received(int percent) {
    cpuLoads.append(percent);
    cpuOnGuiThread &= QThread::currentThread() == qApp->thread();
}

static void message_received(const char* channel, int length, const uint8_t author[32], int initial_sync) {
    received.append({QString::fromUtf8(channel, length), QByteArray(reinterpret_cast<const char*>(author), 32), initial_sync != 0, QThread::currentThread() == qApp->thread()});
}

static QByteArray channel_event() {
    return QByteArray(1, char(sizeof(testChannel) - 1)) + testChannel;
}

static QByteArray message_event(char author, bool initialSync) {
    QByteArray event = channel_event();
    event += QByteArray(32, author);
    event += char(initialSync);
    return event;
}

static void post_message(const QByteArray& event) {
    gui_bridge_post(GUI_EVT_MSG_RECEIVED, reinterpret_cast<const uint8_t*>(event.constData()), event.size());
}

class MessageEventTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        debug_config_init();
        debug_set_level(DEBUG_LEVEL_DEBUG);
        gui_bridge_init();
        gui_bridge_set_msg_received_cb(message_received);
    }
    void cleanupTestCase() { gui_bridge_set_msg_received_cb(nullptr); gui_bridge_set_cpu_load_cb(nullptr); }
    void init() { received.clear(); }

    void liveAndHistory() {
        post_message(message_event(42, false));
        post_message(message_event(43, true));
        QTRY_COMPARE(received.size(), 2);
        QCOMPARE(received[0].channel, QString::fromLatin1(testChannel));
        QCOMPARE(received[0].author, QByteArray(32, 42));
        QVERIFY(!received[0].initialSync);
        QVERIFY(received[0].guiThread);
        QCOMPARE(received[1].author, QByteArray(32, 43));
        QVERIFY(received[1].initialSync);
        QVERIFY(received[1].guiThread);
    }

    void rejectsMalformedEvents() {
        QByteArray event = message_event(42, false);
        event.chop(1);
        post_message(event);
        event = message_event(42, false);
        event[event.size() - 1] = 2;
        post_message(event);
        event = message_event(42, false);
        event[0] = char(255);
        post_message(event);
        post_message({});
        QTest::qWait(100);
        QVERIFY(received.isEmpty());
    }

    void editIsDeliveredSeparatelyFromNewMessage() {
        edits.clear();
        gui_bridge_set_msg_updated_cb([](const uint8_t* data, int len) {
            QVERIFY(QThread::currentThread() == qApp->thread()); edits.append(QByteArray((const char*)data, len));
        });
        QByteArray event = channel_event() + QByteArray(32, 'k');
        gui_bridge_post(CHAT_EVT_MSG_UPDATED, (const uint8_t*)event.constData(), event.size());
        QTRY_COMPARE(edits.size(), 1); QCOMPARE(edits.first(), event); QVERIFY(received.isEmpty());
        event.chop(1); gui_bridge_post(CHAT_EVT_MSG_UPDATED, (const uint8_t*)event.constData(), event.size());
        QTest::qWait(30); QCOMPARE(edits.size(), 1); QVERIFY(received.isEmpty());
        gui_bridge_set_msg_updated_cb(nullptr);
    }

    void cpuLoadDeliveredToGuiThread() {
        cpuLoads.clear(); cpuOnGuiThread = true;
        gui_bridge_set_cpu_load_cb(cpu_received);
        std::thread worker([] {
            const uint8_t samples[][2] = {{1, 0}, {1, 50}, {1, 100}, {0, 90}, {1, 101}};
            for (const auto& sample : samples) gui_bridge_post(CHAT_EVT_CPU_LOAD, sample, sizeof(sample));
            gui_bridge_post(CHAT_EVT_CPU_LOAD, nullptr, 0);
        });
        worker.join();
        QVERIFY(cpuLoads.isEmpty()); // Worker не вызывает UI напрямую.
        QTRY_COMPARE(cpuLoads.size(), 6);
        QCOMPARE(cpuLoads, QList<int>({0, 50, 100, 0, 0, 0}));
        QVERIFY(cpuOnGuiThread);
        gui_bridge_set_cpu_load_cb(nullptr);
        const uint8_t sample[] = {1, 75};
        gui_bridge_post(CHAT_EVT_CPU_LOAD, sample, sizeof(sample));
        QCoreApplication::processEvents();
        QCOMPARE(cpuLoads.size(), 6);
    }

    void deletionRefreshesChannelsOnlyAfterValidEvent() {
        changedChannels.clear();
        gui_bridge_set_channel_updated_cb([](const char* channel, int length) {
            QVERIFY(QThread::currentThread() == qApp->thread());
            changedChannels.append(QString::fromUtf8(channel, length));
        });
        std::thread worker([] {
            const QByteArray event = channel_event();
            const auto* data = reinterpret_cast<const uint8_t*>(event.constData());
            gui_bridge_post(CHAT_EVT_CHANNEL_DELETED, data, event.size());
            gui_bridge_post(CHAT_EVT_CHANNEL_DELETED, data, event.size() - 1);
            const uint8_t empty[] = {0};
            gui_bridge_post(CHAT_EVT_CHANNEL_DELETED, empty, sizeof(empty));
            gui_bridge_post(CHAT_EVT_CHANNEL_DELETED, nullptr, 0);
        });
        worker.join();
        QTRY_COMPARE(changedChannels, QStringList({QString::fromLatin1(testChannel)}));
        QCoreApplication::processEvents();
        QCOMPARE(changedChannels.size(), 1);
        gui_bridge_set_channel_updated_cb(nullptr);
    }

    void shutdownCancelsOwnedCommandsAndKeepsBorrowedArguments() {
        const size_t before = u_get_allocated_count();
        {
            BoundCore core; QVERIFY(core.ua); core.attach();
            const size_t baseline = u_get_allocated_count();
            auto* req = static_cast<chat_msg_submit*>(u_calloc(1, sizeof(chat_msg_submit))); QVERIFY(req);
            QCOMPARE(gui_bridge_submit_message(req), 0);
            QCOMPARE(gui_bridge_delete_dm("123", 1, 1), 0);
            const uint8_t muteEd[32] = {7};
            QCOMPARE(gui_bridge_toggle_member_radio_mute(testChannel, muteEd), 0);
            QCOMPARE(gui_bridge_request_cpu_load(), 1);
            int calls = 0;
            gui_bridge_post_uasync_fn([](void* p) { ++*static_cast<int*>(p); }, &calls);
            gui_bridge_set_inst(nullptr);
            QVERIFY(!gui_bridge_is_uasync_ready());
            uasync_poll(core.ua, 0);
            QCOMPARE(calls, 0);
            QCOMPARE(u_get_allocated_count(), baseline);
        }
        QCOMPARE(u_get_allocated_count(), before);
    }

    void submissionSizeErrorReachesComposer() {
        submitted.clear();
        const auto resetCallback = qScopeGuard([] { gui_bridge_set_msg_submitted_cb(nullptr); });
        gui_bridge_set_msg_submitted_cb([](const uint8_t* data, int len) { submitted.append(QByteArray((const char*)data, len)); });
        QByteArray event = QByteArray::fromHex("080706050403020102");
        gui_bridge_post(CHAT_EVT_MSG_SUBMITTED, reinterpret_cast<const uint8_t*>(event.constData()), event.size());
        QCoreApplication::processEvents(); QCOMPARE(submitted, QList<QByteArray>{event});
        event[8] = 3;
        gui_bridge_post(CHAT_EVT_MSG_SUBMITTED, reinterpret_cast<const uint8_t*>(event.constData()), event.size());
        QCoreApplication::processEvents(); QCOMPARE(submitted.size(), 1);
    }

    void cancelledSubmissionReturnsOneFailureAndFreesRequest() {
        BoundCore core; QVERIFY(core.ua); core.attach(); submitted.clear();
        const auto resetCallback = qScopeGuard([] { gui_bridge_set_msg_submitted_cb(nullptr); });
        gui_bridge_set_msg_submitted_cb([](const uint8_t* data, int len) { submitted.append(QByteArray((const char*)data, len)); });
        const size_t before = u_get_allocated_count();
        auto* request = static_cast<chat_msg_submit*>(u_calloc(1, sizeof(chat_msg_submit))); QVERIFY(request);
        request->request_id = UINT64_C(0x0102030405060708);
        QCOMPARE(gui_bridge_submit_message(request), 0); gui_bridge_set_inst(nullptr);
        QCoreApplication::processEvents();
        QCOMPARE(submitted, QList<QByteArray>({QByteArray::fromHex("080706050403020100")}));
        uasync_poll(core.ua, 0); QCoreApplication::processEvents(); QCOMPARE(submitted.size(), 1);
        QCOMPARE(u_get_allocated_count(), before);
    }

    void invalidPostArgumentsDoNotTurnIntoEmptyEvents() {
        readyEvents = 0;
        gui_bridge_set_db_ready_cb([] { readyEvents++; });
        gui_bridge_post(GUI_EVT_DB_READY, nullptr, -1);
        gui_bridge_post(GUI_EVT_DB_READY, nullptr, 10);
        QCoreApplication::processEvents();
        gui_bridge_set_db_ready_cb(nullptr);
        QCOMPARE(readyEvents, 0);
    }

    void repeatedInitKeepsQueuedEventsAndJoinRequiresOutput() {
        post_message(message_event(42, false));
        gui_bridge_init();
        QTRY_COMPARE(received.size(), 1); QCOMPARE(received[0].author, QByteArray(32, 42));
        QVERIFY(gui_bridge_join_link("", 0, nullptr) < 0);
        QVERIFY(gui_bridge_offer_link(testChannel, "", 0, nullptr) < 0);
        uint64_t request = 42;
        QVERIFY(gui_bridge_join_link("", 0, &request) < 0); QCOMPARE(request, uint64_t(42));
        QVERIFY(gui_bridge_offer_link(testChannel, "", 0, &request) < 0); QCOMPARE(request, uint64_t(42));
    }

    void ownedPostsDeliverOrCancelAndRejectedArgumentsStayWithCaller() {
        const size_t before = u_get_allocated_count();
        {
            BoundCore core; QVERIFY(core.ua); core.attach();
            const size_t baseline = u_get_allocated_count();
            auto* value = static_cast<int*>(u_malloc(sizeof(int))); QVERIFY(value); *value = 7;
            QCOMPARE(gui_bridge_post_uasync_owned_fn([](void* p) {
                QCOMPARE(*static_cast<int*>(p), 7); u_free(p);
            }, value), 0);
            uasync_poll(core.ua, 0); QCOMPARE(u_get_allocated_count(), baseline);
            value = static_cast<int*>(u_malloc(sizeof(int))); QVERIFY(value);
            QCOMPARE(gui_bridge_post_uasync_owned_fn([](void*) { QFAIL("cancelled command executed"); }, value), 0);
            gui_bridge_set_inst(nullptr);
            QCOMPARE(u_get_allocated_count(), baseline);
            value = static_cast<int*>(u_malloc(sizeof(int))); QVERIFY(value);
            QVERIFY(gui_bridge_post_uasync_owned_fn([](void*) {}, value) < 0);
            QCOMPARE(u_get_allocated_count(), baseline + 1); u_free(value);
            core.attach();
            QCOMPARE(gui_bridge_request_cpu_load(), 1); // Отмена прежнего запуска не повреждает очередь следующего.
        }
        QCOMPARE(u_get_allocated_count(), before);
    }

    void concurrentProducerCanOutliveCoreDetachAndUasyncDestroy() {
        const size_t before = u_get_allocated_count();
        BoundCore core; QVERIFY(core.ua); core.attach();
        const auto general = g_debug_config.category_levels[DEBUG_CATEGORY_GENERAL];
        const auto sys = g_debug_config.category_levels[DEBUG_CATEGORY_SYS];
        debug_set_category_level(DEBUG_CATEGORY_GENERAL, DEBUG_LEVEL_DISABLED);
        debug_set_category_level(DEBUG_CATEGORY_SYS, DEBUG_LEVEL_WARN);
        std::promise<void> first;
        auto queued = first.get_future();
        unsigned accepted = 0, rejected = 0;
        bool allocated = true;
        std::thread producer([&] {
            for (unsigned i = 0; i < 128; i++) {
                void* arg = u_malloc(1);
                if (!arg) { allocated = false; if (!i) first.set_value(); break; }
                if (gui_bridge_post_uasync_owned_fn([](void* p) { u_free(p); }, arg) == 0) accepted++;
                else { rejected++; u_free(arg); }
                if (!i) first.set_value();
            }
        });
        queued.wait();
        gui_bridge_set_inst(nullptr); gui_bridge_set_uasync(nullptr);
        uasync_mark_stopped(core.ua); uasync_destroy(core.ua, 0); core.ua = nullptr;
        producer.join();
        debug_set_category_level(DEBUG_CATEGORY_GENERAL, general);
        debug_set_category_level(DEBUG_CATEGORY_SYS, sys);
        QVERIFY(allocated); QCOMPARE(accepted + rejected, 128u);
        QCOMPARE(u_get_allocated_count(), before);
        DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "[PASS] bridge detach race: accepted=%u rejected=%u, producer joined, no leaks", accepted, rejected);
    }

    void monitorSelectionSendsOneRequestAndRecoversAfterRemoval() {
        BoundCore core; QVERIFY(core.ua); core.attach();
        MonitorGuard monitor;
        auto* window = monitor.window.data();
        auto* list = window->findChild<QListWidget*>(); QVERIFY(list);
        auto* details = window->findChild<QPlainTextEdit*>(); QVERIFY(details);
        auto snapshot = connection_list({0x112233445566abcd, 0x2222});
        window->onConnList(reinterpret_cast<const uint8_t*>(snapshot.constData()), snapshot.size());
        QCOMPARE(list->count(), 2); QCOMPARE(pending_count(core.ua), 1);
        QVERIFY(list->currentItem()->text().contains("[abcd]"));
        snapshot = connection_list({0x2222});
        window->onConnList(reinterpret_cast<const uint8_t*>(snapshot.constData()), snapshot.size());
        QCOMPARE(list->currentItem()->data(Qt::UserRole).toULongLong(), quint64(0x2222));
        QCOMPARE(pending_count(core.ua), 2);
        window->onConnMetrics(reinterpret_cast<const uint8_t*>("current metrics"), 15);
        snapshot = connection_list({});
        window->onConnList(reinterpret_cast<const uint8_t*>(snapshot.constData()), snapshot.size());
        QVERIFY(details->toPlainText().isEmpty());
        window->onConnMetrics(reinterpret_cast<const uint8_t*>("late metrics"), 12);
        QVERIFY(details->toPlainText().isEmpty());
    }

    void monitorRejectsIncompleteListsWithoutDestroyingExistingRows() {
        BoundCore core; QVERIFY(core.ua); core.attach();
        MonitorGuard monitor;
        auto* window = monitor.window.data();
        auto* list = window->findChild<QListWidget*>(); QVERIFY(list);
        auto snapshot = connection_list({7, 8});
        window->onConnList(reinterpret_cast<const uint8_t*>(snapshot.constData()), snapshot.size());
        window->onConnList(reinterpret_cast<const uint8_t*>(snapshot.constData()), 2);
        QCOMPARE(list->count(), 2);
        QCOMPARE(list->currentItem()->data(Qt::UserRole).toULongLong(), quint64(7));
    }

    void monitorFactoryReusesWindowAndReopensBeforeDeferredDelete() {
        BoundCore core; QVERIFY(core.ua); core.attach();
        MonitorGuard first;
        QCOMPARE(ConnMonitorWindow::showMonitor(), first.window.data());
        first.window->close();
        MonitorGuard second;
        QVERIFY(second.window);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(!first.window); QCOMPARE(ConnMonitorWindow::showMonitor(), second.window.data());
        const auto snapshot = connection_list({7});
        gui_bridge_post(GUI_EVT_CONN_LIST, reinterpret_cast<const uint8_t*>(snapshot.constData()), snapshot.size());
        QCoreApplication::processEvents();
        QCOMPARE(second.window->findChild<QListWidget*>()->count(), 1); // Деструктор старого окна не снимает новый callback.
    }

#ifdef UTUN_TEST_BRIDGE_ALLOC_FAILURES
    void allocationFailuresKeepOwnershipAndReleasePartialRequests() {
        BoundCore core; QVERIFY(core.ua); core.attach();
        const size_t baseline = u_get_allocated_count();
        auto* req = static_cast<chat_msg_submit*>(u_calloc(1, sizeof(chat_msg_submit))); QVERIFY(req);
        failAllocation = 1; QVERIFY(gui_bridge_submit_message(req) < 0); QCOMPARE(failAllocation, 0);
        QCOMPARE(u_get_allocated_count(), baseline + 1); u_free(req);
        for (int point : {1, 2}) {
            failAllocation = point; QVERIFY(gui_bridge_delete_dm("123", 1, 1) < 0); QCOMPARE(failAllocation, 0);
            QCOMPARE(u_get_allocated_count(), baseline);
            const uint8_t muteEd[32] = {7};
            failAllocation = point; QVERIFY(gui_bridge_toggle_member_radio_mute(testChannel, muteEd) < 0); QCOMPARE(failAllocation, 0);
            QCOMPARE(u_get_allocated_count(), baseline);
        }
        failAllocation = 1; QCOMPARE(gui_bridge_request_cpu_load(), 0); QCOMPARE(failAllocation, 0);
        QCOMPARE(u_get_allocated_count(), baseline);
    }
#endif
};

QTEST_MAIN(MessageEventTest)
#include "test_message_events.moc"

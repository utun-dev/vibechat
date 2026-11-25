/* Снимок uasync → GUI, динамика панелей/поколений, общий переключатель и реальные пиксели трёх графиков. */
#include "../src/networktrafficwindow.h"
#include "../src/connmonitorwindow.h"
#include "../transport/gui_bridge.h"
#include "../../../src/chat/chat_network_stats.h"
#include "../../../src/chat/chat_event.h"
#include "../../../src/utun_instance.h"
#include "../../../src/transport_layer/etcp.h"
#include "../../../src/transport_layer/etcp_connections.h"
#include "../../../lib/u_async.h"
#include "../../../lib/mem.h"
#include "../../../lib/debug_config.h"
#include <QtTest>
#include <QImage>
#include <QPointer>
#include <QToolButton>
#include <QTimer>
#include <cstring>

static QByteArray lastSnapshot;
static uint64_t lastRequest;
static void forward(UTUN_INSTANCE*, int type, const uint8_t* data, int len) {
    if (type != CHAT_EVT_NETWORK_STATS) return;
    lastSnapshot = QByteArray(reinterpret_cast<const char*>(data), len);
    chat_network_stats_header header; memcpy(&header, data, sizeof(header)); lastRequest = header.request_id;
    gui_bridge_post(type, data, len);
}

struct TestCore {
    UASYNC* ua = uasync_create();
    UTUN_INSTANCE instance{};
    char queueName[16] = "traffic_test";
    TestCore() {
        instance.ua = ua; instance.connections = queue_new(ua, 0, 0, 0, queueName);
        chat_event_set_handler(&instance, forward);
        uasync_mark_running(ua); gui_bridge_set_uasync(ua); gui_bridge_set_inst(&instance);
    }
    ~TestCore() {
        gui_bridge_set_inst(nullptr); gui_bridge_set_uasync(nullptr);
        queue_clear(instance.connections); queue_free(instance.connections);
        uasync_mark_stopped(ua); uasync_destroy(ua, 0);
    }
    void poll() { uasync_poll(ua, 0); QCoreApplication::processEvents(); }
};
struct WindowGuard {
    QPointer<NetworkTrafficWindow> window = NetworkTrafficWindow::showMonitor();
    WindowGuard() { window->findChild<QTimer*>()->stop(); }
    ~WindowGuard() { delete window.data(); }
};
struct TestConnection { chat_network_stats_conn value{}; QList<chat_network_stats_link> links; };

static TestConnection connection(quint64 id, unsigned sample = 0) {
    TestConnection result;
    result.value.id = id; result.value.peer_id = id + 100; result.value.up = 1;
    result.value.tx_bytes = result.value.rx_bytes = sample * 750; strcpy(result.value.name, "Test peer");
    for (unsigned index = 1; index <= 2; ++index) {
        chat_network_stats_link link{}; link.id = index; link.description.id = index; link.description.status = 1;
        strcpy(link.description.protocol, index == 1 ? "UDP" : "TCP+REALITY");
        link.tx_bytes = sample * index * 250; link.rx_bytes = sample * (3 - index) * 250;
        link.inflight = index == 1 ? 1000 : 3000; result.links.append(link);
    }
    return result;
}
static QByteArray snapshot(uint64_t at, const QList<TestConnection>& connections) {
    chat_network_stats_header header{}; header.at_ms = at; header.request_id = lastRequest; header.count = connections.size();
    QByteArray bytes(reinterpret_cast<const char*>(&header), sizeof(header));
    for (auto conn : connections) {
        conn.value.link_count = conn.links.size(); bytes.append(reinterpret_cast<const char*>(&conn.value), sizeof(conn.value));
        for (const auto& link : conn.links) bytes.append(reinterpret_cast<const char*>(&link), sizeof(link));
    }
    return bytes;
}
static void apply(NetworkTrafficWindow* window, const QByteArray& bytes) {
    window->onSnapshot(reinterpret_cast<const uint8_t*>(bytes.constData()), bytes.size());
    QCoreApplication::processEvents();
}
static QWidget* panel(NetworkTrafficWindow* window, quint64 id = 17) {
    return window->findChild<QWidget*>(QStringLiteral("trafficConnection_%1").arg(id, 0, 16));
}
static QImage render(QWidget* canvas) {
    QImage image(canvas->size(), QImage::Format_ARGB32_Premultiplied); image.fill(Qt::transparent); canvas->render(&image); return image;
}
static int coloredPixels(const QImage& image, const QColor& color, int row, double* averageY = nullptr) {
    int count = 0; double sum = 0;
    for (int y = row * 122 + 23; y < row * 122 + 102; ++y) for (int x = 80; x < image.width() - 18; ++x) {
        QColor pixel = image.pixelColor(x, y);
        if (qAbs(pixel.red() - color.red()) + qAbs(pixel.green() - color.green()) + qAbs(pixel.blue() - color.blue()) > 35) continue;
        ++count; sum += y;
    }
    if (averageY) *averageY = count ? sum / count : -1;
    return count;
}

class NetworkTrafficTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { debug_config_init(); debug_set_level(DEBUG_LEVEL_DEBUG); gui_bridge_init(); }

    void nativeSnapshotCopiesCountersAndPhysicalInflight() {
        TestCore core; QVERIFY(core.ua); QVERIFY(core.instance.connections);
        ETCP_CONN conn{}; ETCP_LINK first{}, second{}; char name[] = "pending transport";
        conn.instance = &core.instance; conn.name = name; conn.diagnostic_id = 123;
        conn.diagnostic_wire_tx_bytes = 1234567890123ULL; conn.diagnostic_wire_rx_bytes = 2345678901234ULL;
        first.etcp = second.etcp = &conn; first.next = &second; conn.links = &first;
        first.local_link_id = 1; first.diagnostic_id = 4; first.inflight_bytes = 100;
        second.local_link_id = 2; second.diagnostic_id = 9; second.inflight_bytes = 300;
        first.diagnostic_wire_tx_bytes = 77; second.diagnostic_wire_rx_bytes = 88;
        auto* entry = queue_entry_new(sizeof(conn_queue_entry)); QVERIFY(entry);
        auto* record = reinterpret_cast<conn_queue_entry*>(entry->data); record->conn = &conn;
        QCOMPARE(queue_data_put(core.instance.connections, entry), 0);
        uint64_t id = 0; QCOMPARE(gui_bridge_request_network_stats(&id), 1);
        QCOMPARE(gui_bridge_request_network_stats(&id), 2); core.poll();
        QCOMPARE(lastSnapshot.size(), qsizetype(sizeof(chat_network_stats_header) + sizeof(chat_network_stats_conn) +
            2 * sizeof(chat_network_stats_link)));
        chat_network_stats_header header; memcpy(&header, lastSnapshot.constData(), sizeof(header));
        QCOMPARE(header.count, 1u); QCOMPARE(header.failed, 0u); QCOMPARE(header.request_id, id);
        chat_network_stats_conn copied; memcpy(&copied, lastSnapshot.constData() + sizeof(header), sizeof(copied));
        QCOMPARE(copied.id, conn.diagnostic_id); QCOMPARE(copied.peer_id, uint64_t(0));
        QCOMPARE(copied.tx_bytes, conn.diagnostic_wire_tx_bytes); QCOMPARE(copied.rx_bytes, conn.diagnostic_wire_rx_bytes);
        chat_network_stats_link link;
        memcpy(&link, lastSnapshot.constData() + sizeof(header) + sizeof(copied), sizeof(link));
        QCOMPARE(link.id, first.diagnostic_id); QCOMPARE(link.tx_bytes, uint64_t(77)); QCOMPARE(link.inflight, uint64_t(100));
        QVERIFY(core.ua->posted_tasks_head == nullptr);
    }

    void legendControlsAllThreePlotsAndRatesUseElapsedTime() {
        TestCore core; WindowGuard guard; core.poll(); QVERIFY(lastRequest);
        auto* window = guard.window.data();
        for (unsigned i = 0; i <= 120; ++i) apply(window, snapshot(100000 + i * 250, {connection(17, i)}));
        auto* row = panel(window); QVERIFY(row);
        auto* canvas = row->findChild<QWidget*>("trafficCanvas"); QVERIFY(canvas);
        auto* total = row->findChild<QToolButton*>("trafficSeries_0"); QVERIFY(total);
        auto* first = row->findChild<QToolButton*>("trafficSeries_1"); QVERIFY(first);
        const QColor color = QColor::fromHsv(35, 190, 210);
        QImage before = render(canvas);
        // inflight=1000/5000, TX=1000/5000 B/s, RX=2000/5000 B/s; кривые на разных осях.
        for (int graph = 0; graph < 3; ++graph) {
            double average = 0; QVERIFY(coloredPixels(before, color, graph, &average) > 200);
            double expected = graph * 122 + 100 - 76 * (graph == 2 ? .4 : .2);
            QVERIFY2(qAbs(average - expected) < 1.5, qPrintable(QString("row=%1 actual=%2 expected=%3").arg(graph).arg(average).arg(expected)));
        }
        QTest::mouseClick(first, Qt::LeftButton); QVERIFY(!first->isChecked()); QVERIFY(total->isChecked());
        QImage hidden = render(canvas);
        for (int graph = 0; graph < 3; ++graph) QCOMPARE(coloredPixels(hidden, color, graph), 0);
        apply(window, snapshot(130250, {connection(17, 121)})); QVERIFY(!first->isChecked());
        QTest::mouseClick(first, Qt::LeftButton); QVERIFY(first->isChecked());
        QImage restored = render(canvas);
        for (int graph = 0; graph < 3; ++graph) QVERIFY(coloredPixels(restored, color, graph) > 200);
        QTest::mouseClick(total, Qt::LeftButton); QVERIFY(!total->isChecked()); QVERIFY(first->isChecked());
        window->grab().save("/tmp/network-traffic-preview.png");
    }

    void dynamicConnectionsAndReusedLinkNumberKeepSeparateHistory() {
        TestCore core; WindowGuard guard; core.poll(); auto* window = guard.window.data();
        apply(window, snapshot(100000, {connection(17), connection(18)}));
        QPointer<QWidget> firstPanel = panel(window), removed = panel(window, 18); QVERIFY(firstPanel); QVERIFY(removed);
        auto* button = firstPanel->findChild<QToolButton*>("trafficSeries_1"); QVERIFY(button);
        QTest::mouseClick(button, Qt::LeftButton); QPointer<QToolButton> old = button;
        auto updated = connection(17, 1); updated.links[0].id = 500; // LINK1 с новым поколением.
        apply(window, snapshot(100250, {updated}));
        QVERIFY(!removed); QVERIFY(!old); QCOMPARE(panel(window), firstPanel.data());
        auto* replacement = firstPanel->findChild<QToolButton*>("trafficSeries_500"); QVERIFY(replacement);
        QVERIFY(replacement->isChecked()); QCOMPARE(replacement->accessibleName(), QString("LINK1"));
        auto* other = firstPanel->findChild<QToolButton*>("trafficSeries_2"); QVERIFY(other);
        QVERIFY(replacement->x() < other->x()); // Порядок по номеру линка, независимо от поколения.
        updated.value.up = 0; apply(window, snapshot(100500, {updated})); QCOMPARE(panel(window), firstPanel.data());
        auto malformed = snapshot(100750, {}); malformed.append('x'); apply(window, malformed); QVERIFY(firstPanel);
        malformed = snapshot(100750, {updated}); malformed.chop(1); apply(window, malformed); QVERIFY(firstPanel);
        apply(window, snapshot(101000, {})); QVERIFY(!firstPanel);
    }

    void hideCloseDetachAndConnectionMonitorDoNotStealCallbacks() {
        TestCore core; WindowGuard first; core.poll(); auto* window = first.window.data();
        QPointer<ConnMonitorWindow> monitor = ConnMonitorWindow::showMonitor(); monitor->findChild<QTimer*>()->stop();
        apply(window, snapshot(100000, {connection(17)})); QVERIFY(panel(window));
        window->hide(); QVERIFY(!window->findChild<QTimer*>()->isActive());
        apply(window, snapshot(100250, {})); QVERIFY(panel(window));
        QCOMPARE(NetworkTrafficWindow::showMonitor(), window); window->findChild<QTimer*>()->stop(); core.poll();
        QVERIFY(!panel(window));
        window->close(); WindowGuard second;
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); QVERIFY(!first.window);
        core.poll(); apply(second.window, snapshot(101000, {connection(17)})); QVERIFY(panel(second.window));
        delete monitor.data();
        uint64_t id = 0; QCOMPARE(gui_bridge_request_network_stats(&id), 1);
        gui_bridge_set_inst(nullptr); QCOMPARE(gui_bridge_request_network_stats(&id), 0);
        gui_bridge_set_inst(&core.instance); QCOMPARE(gui_bridge_request_network_stats(&id), 1); core.poll();
    }
};

QTEST_MAIN(NetworkTrafficTest)
#include "test_network_traffic.moc"

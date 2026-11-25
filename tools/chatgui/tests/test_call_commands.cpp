/* Настоящие CallWindow/bridge/uasync: владение call-командами при отказе, отмене и исполнении.
 * Linux hooks наблюдают границу native trampoline и затем вызывают его без подмены освобождения payload. */
#include "../src/callwindow.h"
#include "../src/phone_media_dispatcher.h"
#include "../src/call_audio_graphs.h"
#include "call/call_history_store.h"
#include "../src/video_capture.h"
#include "../src/call_video_engine.h"
#include "../transport/gui_bridge.h"
#include <QtTest>
#include <QLabel>
#include <QPushButton>
#include <QPointer>
#include <QScopeGuard>
#include <QMenuBar>
#include <QMenu>
#include <QActionGroup>
#include <QSpinBox>
#include <QTextDocument>
#include <QTextCursor>
#include <atomic>
#include <thread>
#include <cstring>
extern "C" {
#include "utun_instance.h"
#include "transport_layer/etcp.h"
#include "chat/chat_core.h"
#include "call/call.h"
#include "call/call_priv.h"
#include "call/call_proto.h"
#include "ntp_time.h"
#include "mem.h"
#include "debug_config.h"
}

struct Observation {
    UTUN_INSTANCE* instance;
    uint64_t id;
    QByteArray peer;
    int action;
    QByteArray channel;
};
static QList<Observation> observed;
static UTUN_INSTANCE* executingCore = nullptr;
static bool bindingValid = true;

#ifdef UTUN_TEST_CALL_HOOKS
static std::atomic<int> failAllocation{0};
extern "C" void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
extern "C" void* __wrap_u_calloc_impl(uint32_t count, uint32_t size, const char* where) {
    if (failAllocation && strstr(where, "/gui_bridge_impl.cpp:") && --failAllocation == 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "[INJECT] call command allocation failure at %s", where); return nullptr;
    }
    return __real_u_calloc_impl(count, size, where);
}
extern "C" void __real_call_start_trampoline(void*);
extern "C" void __wrap_call_start_trampoline(void* arg) {
    auto* request = static_cast<call_start_arg*>(arg);
    bindingValid &= request->inst == executingCore;
    observed.append({request->inst, request->call_id, QByteArray(reinterpret_cast<const char*>(request->peer), 32), -1, request->ch_id});
    __real_call_start_trampoline(arg);
}
static void observeAction(void* arg, int action, void (*native)(void*)) {
    auto* request = static_cast<call_id_arg*>(arg);
    bindingValid &= request->inst == executingCore;
    observed.append({request->inst, request->call_id, {}, action, {}}); native(arg);
}
extern "C" void __real_call_accept_trampoline(void*);
extern "C" void __wrap_call_accept_trampoline(void* arg) { observeAction(arg, GUI_CALL_ACCEPT, __real_call_accept_trampoline); }
extern "C" void __real_call_decline_trampoline(void*);
extern "C" void __wrap_call_decline_trampoline(void* arg) { observeAction(arg, GUI_CALL_DECLINE, __real_call_decline_trampoline); }
extern "C" void __real_call_hangup_trampoline(void*);
extern "C" void __wrap_call_hangup_trampoline(void* arg) { observeAction(arg, GUI_CALL_HANGUP, __real_call_hangup_trampoline); }
extern "C" void __real_call_continue_trampoline(void*);
extern "C" void __wrap_call_continue_trampoline(void* arg) { observeAction(arg, GUI_CALL_CONTINUE, __real_call_continue_trampoline); }
#endif

struct CommandCore {
    UASYNC* ua = uasync_create();
    UTUN_INSTANCE instance{};
    ~CommandCore() {
        gui_bridge_set_inst(nullptr); gui_bridge_set_uasync(nullptr);
        if (ua) { uasync_mark_stopped(ua); uasync_destroy(ua, 0); }
    }
    void attach() {
        instance.ua = ua; uasync_mark_running(ua); gui_bridge_set_uasync(ua); gui_bridge_set_inst(&instance);
    }
    void poll() { executingCore = &instance; uasync_poll(ua, 0); executingCore = nullptr; }
};
static int pending(UASYNC* ua) {
    int count = 0;
    for (auto* task = ua->posted_tasks_head; task; task = task->next) ++count;
    return count;
}

struct IncomingEvents {
    QPointer<CallWindow> window;
    unsigned incoming = 0, ended = 0, declined = 0;
};
static void incomingEvent(UTUN_INSTANCE* instance, int type, const uint8_t*, int) {
    auto* events = static_cast<IncomingEvents*>(instance->test_user_ptr);
    if (type == CHAT_EVT_CALL_INCOMING) ++events->incoming;
    if (type == CHAT_EVT_CALL_DECLINED) ++events->declined;
    if (type == CHAT_EVT_CALL_ENDED) {
        ++events->ended;
        if (events->window) events->window->deleteLater(); // Как MainWindow::closeCallWindow.
    }
}
/* Граница сервиса после router: настоящий call_handle_invite создаёт сессию и её таймеры, внешнего пира нет. */
static bool deliverInvite(UTUN_INSTANCE* instance, uint64_t group, uint64_t id) {
    auto callback = instance->router_bindings.callbacks[ETCP_RT_ID_CALL];
    if (!callback) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "fixture: call service is not bound"); return false; }
    auto* entry = queue_entry_new(0);
    if (!entry) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "fixture: invite entry allocation failed"); return false; }
    entry->len = ROUTER_SVC_PAYLOAD_OFF + sizeof(call_pkt_invite);
    entry->dgram = static_cast<uint8_t*>(u_calloc(1, entry->len));
    if (!entry->dgram) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "fixture: invite payload allocation failed"); queue_entry_free(entry); return false;
    }
    entry->dgram[0] = ETCP_RT_ID_CALL;
    entry->dgram[ROUTER_SVC_FLAGS_OFF] = ROUTER_FLAG_SIGNED | ROUTER_FLAG_ENCRYPTED;
    uint8_t peerX[32], peerPrivate[32], peerEd[32], seed[32];
    if (sc_identity_generate(peerX, peerPrivate, peerEd, seed) != SC_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "fixture: identity generation failed");
        queue_dgram_free(entry); queue_entry_free(entry); return false;
    }
    memcpy(entry->dgram + ROUTER_SVC_SRC_ED_OFF, peerEd, 32);
    memcpy(entry->dgram + ROUTER_SVC_DST_ED_OFF, instance->my_ed25519_pubkey, 32);
    memcpy(entry->dgram + ROUTER_SVC_GROUP_OFF, &group, sizeof(group));
    call_pkt_invite packet{}; packet.subcmd = CALL_SUBCMD_INVITE; packet.call_id = id;
    packet.num_tracks = 1; packet.expires_us = ntp_time_get_us(instance) + 2000000;
    packet.tracks[0] = {CALL_TRACK_AUDIO, CALL_CODEC_OPUS, 48000, 1, 32000};
    memcpy(entry->dgram + ROUTER_SVC_PAYLOAD_OFF, &packet, sizeof(packet));
    ETCP_CONN connection{}; connection.instance = instance; callback(&connection, entry);
    auto* session = call_session_find(instance->call, id);
    return session && session->state == CALL_ST_RINGING && call_busy(instance);
}

class CallCommandsTest : public QObject {
    Q_OBJECT
    static void scenarios() {
        QTest::addColumn<QString>("scenario");
        for (const char* value : {"unavailable", "stopped", "cancelled", "executed"}) QTest::newRow(value) << QString(value);
#ifdef UTUN_TEST_CALL_HOOKS
        QTest::newRow("request-allocation") << QString("request-allocation");
        QTest::newRow("post-allocation") << QString("post-allocation");
#endif
    }
    static void prepare(CommandCore& core, const QString& scenario) {
        core.attach();
        if (scenario == "unavailable") gui_bridge_set_inst(nullptr);
        if (scenario == "stopped") uasync_mark_stopped(core.ua);
#ifdef UTUN_TEST_CALL_HOOKS
        failAllocation = scenario == "request-allocation" ? 1 : scenario == "post-allocation" ? 2 : 0;
#endif
    }
private slots:
    void mediaTransitionBarrier() {
        PhoneMediaDispatcher dispatcher(this);
        bool released = false, phoneReleased = false;
        int stops = 0, starts = 0, restores = 0;
        dispatcher.enter(1, [&] { ++stops; }, [&] { return released; }, [&] { ++starts; }, [] { QFAIL("unexpected failure"); });
        dispatcher.enter(1, [&] { ++stops; }, [] { return true; }, [&] { ++starts; }, [] { QFAIL("duplicate failure"); });
        QTest::qWait(80); QCOMPARE(stops, 1); QCOMPARE(starts, 0); QVERIFY(dispatcher.blocked());
        released = true; QTRY_COMPARE(starts, 1);
        dispatcher.finish(1, [&] { return phoneReleased; }, [&] { ++restores; });
        QTest::qWait(80); QCOMPARE(restores, 0); QVERIFY(dispatcher.blocked());
        phoneReleased = true; QTRY_COMPARE(restores, 1); QVERIFY(!dispatcher.blocked());
    }
    void mediaTransitionCancelledBeforeRelease() {
        PhoneMediaDispatcher dispatcher(this);
        bool released = false;
        int starts = 0, restores = 0;
        dispatcher.enter(1, [] {}, [&] { return released; }, [&] { ++starts; }, [] { QFAIL("unexpected failure"); });
        dispatcher.finish(1, [] { return true; }, [&] { ++restores; });
        QTest::qWait(80); QVERIFY(dispatcher.blocked()); QCOMPARE(restores, 0);
        released = true; QTRY_COMPARE(restores, 1); QCOMPARE(starts, 0); QVERIFY(!AudioCaptureLease::phoneReserved.load());
    }
    void mediaTransitionTimeoutRetainsOwners() {
        PhoneMediaDispatcher dispatcher(this, 20);
        bool released = false;
        int failures = 0, starts = 0;
        dispatcher.enter(1, [] {}, [&] { return released; }, [&] { ++starts; }, [&] { ++failures; });
        QTRY_COMPARE(failures, 1); QVERIFY(dispatcher.blocked()); QCOMPARE(starts, 0);
        dispatcher.finish(1, [] { return true; }, [] {});
        released = true; QTRY_VERIFY(!dispatcher.blocked()); QCOMPARE(failures, 1);
    }
    void initTestCase() {
        debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
        debug_set_category_level(DEBUG_CATEGORY_CALL, DEBUG_LEVEL_DEBUG);
        debug_set_category_level(DEBUG_CATEGORY_GENERAL, DEBUG_LEVEL_DEBUG); gui_bridge_init();
        { CallWindow warmup; } // Singleton audio engine создаётся до подсчёта командных аллокаций.
    }
    void cleanup() {
#ifdef UTUN_TEST_CALL_HOOKS
        failAllocation = 0;
#endif
        gui_bridge_set_inst(nullptr); gui_bridge_set_uasync(nullptr);
        observed.clear(); bindingValid = true;
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    void audioHistoryGraphs() {
        CallWindow window; window.setCallId(77); window.setState(CallWindow::Active); window.show();
        auto* button = window.findChild<QPushButton*>("callDiagnosticsButton"); QVERIFY(button); QVERIFY(button->isVisible());
        QVERIFY(!window.findChild<QPushButton*>("callAudioGraphsButton"));
        window.setConnectionType(1, QByteArray(32, '\x2a')); window.setBgpPath("A52 → Linux"); window.setAudioStats(40, 600, 100, 0, 1, 0, 750, 200);
        button->click();
        auto* child = window.findChild<QWidget*>("callAudioGraphs"); QVERIFY(child); QVERIFY(child->isVisible());
        auto* graphs = static_cast<CallAudioGraphs*>(child);
        auto* details = graphs->findChild<QWidget*>("callDiagnostics"); QVERIFY(details); QVERIFY(details->isVisible());
        CallHistoryStore history; uint64_t sent = 0;
        for (int i = 0; i < 600; ++i) {
            int depth = 500 + (int)(250 * std::sin(i / 25.0));
            bool missing = i >= 230 && i < 245;
            int buffered = missing ? (i - 230) * 20 : depth; // Во время паузы заполнение растёт, доступный запас остаётся нулевым.
            history.receive(i * 50, buffered, 200, 100, 2400, missing ? 2400 : 0, missing ? 0 : depth - 50);
            sent += i % 4 == 0 ? 500 : 0;
            history.transmit({(uint64_t)i * 50, (uint64_t)(i % 100) * 20, 800, 1200, 42, 9, sent});
        }
        call_audio_history snapshot; history.snapshot(29950, snapshot); graphs->setSnapshot(snapshot);
        QCoreApplication::processEvents(); QVERIFY(!graphs->grab().isNull());
        const QString screenshot = qEnvironmentVariable("UTUN_TEST_AUDIO_GRAPHS_SCREENSHOT");
        if (!screenshot.isEmpty()) QVERIFY(graphs->grab().save(screenshot));
        graphs->close(); QVERIFY(!graphs->isVisible()); QCOMPARE(window.state(), CallWindow::Active);
        button->click(); QVERIFY(graphs->isVisible());
        window.setState(CallWindow::Ended);
    }
    void videoSettingsMenus() {
        CallWindow window; window.setCallId(77); window.setPeerName(QStringLiteral("Video call")); window.setState(CallWindow::Active);
        auto* engine = window.findChild<CallVideoEngine*>(); QVERIFY(engine);
        VideoCaptureDevice first{QStringLiteral("Main camera"), QByteArray("main"), {}};
        stream_video_add_camera_modes(first.modes, QSize(1920, 1080), 29.97, 29.97);
        stream_video_add_camera_modes(first.modes, QSize(1280, 720), 15, 60);
        VideoCaptureDevice second{QStringLiteral("Secondary camera"), QByteArray("secondary"), {}};
        stream_video_add_camera_modes(second.modes, QSize(640, 480), 7, 7);
        emit engine->camerasChanged({first, second});
        QSignalBlocker blocked(engine); // Аппаратные события не меняют тестовые режимы; камера/звонок не открываются.
        auto getMenu = [&window](const char* name) { return window.findChild<QMenu*>(name); };
        auto chosen = [](QMenu* menu) { auto* action = menu->findChild<QActionGroup*>()->checkedAction(); return action ? action->data() : QVariant(); };
        auto choose = [](QMenu* menu, const QVariant& value) {
            for (auto* action : menu->actions()) if (action->isCheckable() && action->data() == value) { action->trigger(); return; }
            QFAIL("Missing video setting");
        };
        auto* camera = getMenu("callVideoCamera"); auto* resolution = getMenu("callVideoResolution"); auto* fps = getMenu("callVideoFps");
        QVERIFY(camera); QVERIFY(resolution); QVERIFY(fps); QCOMPARE(chosen(camera).toByteArray(), first.id);
        choose(resolution, QSize(1280, 720)); choose(fps, 60); QCOMPARE(chosen(fps).toInt(), 60);
        const QImage previousResolution = resolution->menuAction()->icon().pixmap(40, 36).toImage();
        choose(resolution, QSize(1920, 1080)); QCOMPARE(chosen(fps).toInt(), 30);
        QVERIFY(resolution->menuAction()->icon().pixmap(40, 36).toImage() != previousResolution);
        for (auto* action : fps->actions()) if (action->isCheckable())
            QCOMPARE(action->isEnabled(), action->data().toInt() <= 30);
        choose(camera, second.id); QCOMPARE(chosen(resolution).toSize(), QSize(640, 480)); QCOMPARE(chosen(fps).toInt(), 7);
        blocked.unblock(); emit engine->camerasChanged({second, first}); blocked.reblock();
        QCOMPARE(chosen(camera).toByteArray(), second.id); QCOMPARE(chosen(fps).toInt(), 7);
        choose(camera, first.id); choose(resolution, QSize(1920, 1080)); choose(getMenu("callVideoCodec"), 1);
        auto* minimum = window.findChild<QSpinBox*>("callVideoMinimumBitrate"); QVERIFY(minimum);
        auto* maximum = window.findChild<QSpinBox*>("callVideoMaximumBitrate"); QVERIFY(maximum);
        minimum->setValue(2000); maximum->setValue(1000); QCOMPARE(minimum->value(), 1000);
        minimum->setValue(300); maximum->setValue(4000);
        auto* settings = window.findChild<QPushButton*>("callVideoSettings"); QVERIFY(settings);
        settings->click(); window.show(); QTest::qWait(20);
        auto* bar = window.findChild<QMenuBar*>("callVideoControls"); QVERIFY(bar); QVERIFY(bar->isVisible());
        auto* avatar = window.findChild<QLabel*>("callAvatar"); QVERIFY(avatar); QVERIFY(avatar->isHidden());
        auto* diagnostics = window.findChild<QWidget*>("callDiagnostics"); QVERIFY(diagnostics); QVERIFY(!diagnostics->isVisible());
        auto* diagnosticButton = window.findChild<QPushButton*>("callDiagnosticsButton"); QVERIFY(diagnosticButton);
        QVERIFY(!diagnosticButton->isChecked());
        window.setAudioStats(40, 80, 100, 0, 0, 50, 90, 10); window.setBgpPath("node1 → node2");
        QVERIFY(!diagnostics->isVisible());
        diagnosticButton->click(); QVERIFY(diagnostics->isVisible()); QVERIFY(bar->isVisible()); QVERIFY(avatar->isHidden());
        auto* diagnosticWindow = window.findChild<QWidget*>("callAudioGraphs"); QVERIFY(diagnosticWindow);
        QVERIFY(diagnosticWindow->isAncestorOf(diagnostics));
        window.setAudioStats(41, 81, 100, 1, 0, 50, 90, 10); QVERIFY(diagnostics->isVisible());
        auto* stats = window.findChild<QLabel*>("callAudioStats"); QVERIFY(stats);
        QTextDocument metrics; metrics.setHtml(stats->text());
        auto metric = metrics.find("81 ms"); QVERIFY(!metric.isNull()); QVERIFY(metric.charFormat().fontWeight() >= QFont::Bold);
        QCOMPARE(metric.charFormat().foreground().color(), window.palette().color(QPalette::WindowText));
        auto caption = metrics.find("Buffer:"); QVERIFY(!caption.isNull()); QVERIFY(caption.charFormat().fontWeight() < QFont::Bold);
        const QString diagnosticScreenshot = qEnvironmentVariable("UTUN_TEST_CALL_DIAGNOSTICS_SCREENSHOT");
        if (!diagnosticScreenshot.isEmpty()) { QCoreApplication::processEvents(); QVERIFY(diagnosticWindow->grab().save(diagnosticScreenshot)); }
        diagnosticWindow->close(); window.setAudioStats(42, 82, 100, 1, 0, 50, 90, 10); QVERIFY(!diagnostics->isVisible());
        auto* footer = window.findChild<QWidget*>("callActions"); QVERIFY(footer);
        QCoreApplication::processEvents(); QCOMPARE(window.rect().bottom() - footer->geometry().bottom(), 12);
        window.resize(window.width() + 40, window.height() + 80); QCoreApplication::processEvents();
        QCOMPARE(window.rect().bottom() - footer->geometry().bottom(), 12);
        QRect previous;
        for (auto* action : bar->actions()) {
            QRect button = bar->actionGeometry(action); QCOMPARE(button.size(), QSize(48, 48));
            if (!previous.isNull()) QCOMPARE(button.left() - previous.right() - 1, 4);
            previous = button; QVERIFY(bar->rect().contains(button));
            QVERIFY(!action->menu()->actions().first()->text().isEmpty()); QVERIFY(!action->toolTip().isEmpty());
        }
        const QString screenshot = qEnvironmentVariable("UTUN_TEST_CALL_CONTROLS_SCREENSHOT");
        if (!screenshot.isEmpty()) QVERIFY(window.grab().save(screenshot));
        auto* action = camera->menuAction(); const QIcon original = action->icon();
        QPixmap marker(80, 72); marker.fill(Qt::magenta); action->setIcon(QIcon(marker));
        const QImage rendered = bar->grab().toImage(); QRect iconBounds;
        for (int y = 0; y < rendered.height(); y++) for (int x = 0; x < rendered.width(); x++) {
            QColor color = rendered.pixelColor(x, y);
            if (color.red() > 240 && color.blue() > 240 && color.green() < 15) iconBounds |= QRect(x, y, 1, 1);
        }
        action->setIcon(original); QCOMPARE(iconBounds.size(), QSize(40, 36) * rendered.devicePixelRatio());
        QTest::mouseClick(bar, Qt::LeftButton, {}, bar->actionGeometry(resolution->menuAction()).center()); QTRY_VERIFY(resolution->isVisible());
        QTest::mouseClick(bar, Qt::LeftButton, {}, bar->actionGeometry(fps->menuAction()).center());
        QTRY_VERIFY(fps->isVisible()); QVERIFY(!resolution->isVisible());
        QTest::mouseClick(bar, Qt::LeftButton, {}, bar->actionGeometry(fps->menuAction()).center()); QTRY_VERIFY(!fps->isVisible());
        settings->click(); QVERIFY(!bar->isVisible()); QVERIFY(!avatar->isHidden());
        QImage frame(160, 90, QImage::Format_RGB32); frame.fill(Qt::darkGreen);
        blocked.unblock(); emit engine->localImageChanged(frame); blocked.reblock(); QVERIFY(avatar->isHidden());
        auto* preview = window.findChild<QWidget*>("callLocalPreview"); QVERIFY(preview);
        auto* previewArea = window.findChild<QWidget*>("callPreviewArea"); QVERIFY(previewArea);
        QCoreApplication::processEvents(); QVERIFY(preview->isVisible());
        QCOMPARE(preview->size(), previewArea->contentsRect().size()); QCOMPARE(preview->width(), window.width() - 24);
        window.resize(900, 700); QCoreApplication::processEvents(); QVERIFY(preview->width() > 640);
        QCOMPARE(preview->size(), previewArea->contentsRect().size());
        QCOMPARE(window.rect().bottom() - footer->geometry().bottom(), 12);
        QImage previewImage = preview->grab().toImage(); QRect frameBounds;
        for (int y = 0; y < previewImage.height(); y++) for (int x = 0; x < previewImage.width(); x++) {
            QColor color = previewImage.pixelColor(x, y);
            if (color.green() > 80 && color.red() < 10 && color.blue() < 10) frameBounds |= QRect(x, y, 1, 1);
        }
        QVERIFY(frameBounds.width() == previewImage.width() || frameBounds.height() == previewImage.height());
        QVERIFY(std::abs(frameBounds.width() * frame.height() - frameBounds.height() * frame.width()) <= frame.width());
        const QString previewScreenshot = qEnvironmentVariable("UTUN_TEST_CALL_PREVIEW_SCREENSHOT");
        if (!previewScreenshot.isEmpty()) QVERIFY(window.grab().save(previewScreenshot));
        blocked.unblock(); emit engine->localImageChanged({}); blocked.reblock(); QVERIFY(!avatar->isHidden());
        blocked.unblock(); emit engine->imageChanged(frame, 0); blocked.reblock(); QVERIFY(avatar->isHidden());
        blocked.unblock(); emit engine->imageChanged({}, 0); blocked.reblock(); QVERIFY(!avatar->isHidden());
        auto* videoStatus = window.findChild<QLabel*>("callVideoStatus"); QVERIFY(videoStatus);
        blocked.unblock(); emit engine->statusChanged("Camera error"); blocked.reblock();
        QVERIFY(videoStatus->isVisible()); QVERIFY(!diagnostics->isVisible());
        blocked.unblock(); emit engine->statusChanged({}); blocked.reblock(); QVERIFY(!videoStatus->isVisible());
        window.setState(CallWindow::Ended); QVERIFY(!diagnostics->isVisible());
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "call settings verified: rendered_icon=40x36 buttons=48x48 spacing=4 source/FPS/menu switching passed");
    }
    void windowAction_data() {
        QTest::addColumn<QString>("scenario"); QTest::addColumn<QString>("action");
        QStringList failures{"unavailable", "stopped", "cancelled", "executed"};
#ifdef UTUN_TEST_CALL_HOOKS
        failures << "request-allocation" << "post-allocation";
#endif
        for (const auto& failure : failures)
            for (const char* action : {"onAccept", "onDecline", "onHangup", "continue", "close", "closeIncoming"})
                QTest::newRow(qPrintable(failure + '-' + action)) << failure << QString(action);
    }
    void windowAction() {
        QFETCH(QString, scenario); QFETCH(QString, action);
        const size_t before = u_get_allocated_count();
        {
            CommandCore core; QVERIFY(core.ua);
            CallWindow window; window.setCallId(0x123456789);
            const bool closing = action.startsWith("close");
            const auto state = action == "onAccept" || action == "onDecline" || action == "closeIncoming" ? CallWindow::Incoming :
                action == "continue" ? CallWindow::WaitDecision : CallWindow::Outgoing;
            window.setState(state); prepare(core, scenario);
            const size_t ready = u_get_allocated_count();
            if (action == "continue") {
                QPushButton* button = nullptr;
                for (auto* candidate : window.findChildren<QPushButton*>())
                    if (candidate->text() == QCoreApplication::translate("ChatGui", "Call anyway")) button = candidate;
                QVERIFY(button); button->click();
            } else if (closing) { window.show(); window.close(); QCOMPARE(window.state(), CallWindow::Ended); }
            else QVERIFY(QMetaObject::invokeMethod(&window, qPrintable(action), Qt::DirectConnection));
            const bool accepted = scenario == "executed" || scenario == "cancelled";
            QCOMPARE(pending(core.ua), accepted ? 1 : 0);
            if (!accepted && !closing) {
                QCOMPARE(window.state(), state);
                bool message = false;
                for (auto* label : window.findChildren<QLabel*>())
                    message |= label->text() == QCoreApplication::translate("ChatGui", "Could not send the call command");
                QVERIFY(message);
            }
            if (scenario == "executed") core.poll();
            else gui_bridge_set_inst(nullptr);
            QCOMPARE(pending(core.ua), 0); QCOMPARE(u_get_allocated_count(), ready);
#ifdef UTUN_TEST_CALL_HOOKS
            QCOMPARE(failAllocation.load(), 0); QCOMPARE(observed.size(), scenario == "executed" ? 1 : 0);
            if (!observed.isEmpty()) {
                const int expected = action == "onAccept" ? GUI_CALL_ACCEPT :
                    action == "onDecline" || action == "closeIncoming" ? GUI_CALL_DECLINE :
                    action == "continue" ? GUI_CALL_CONTINUE : GUI_CALL_HANGUP;
                QCOMPARE(observed.front().action, expected); QCOMPARE(observed.front().id, uint64_t(0x123456789));
            }
#endif
            QVERIFY(bindingValid);
        }
        QCOMPARE(u_get_allocated_count(), before);
    }
    void closeStates_data() {
        QTest::addColumn<int>("state");
        QTest::newRow("outgoing") << int(CallWindow::Outgoing); QTest::newRow("ringing") << int(CallWindow::Ringing);
        QTest::newRow("incoming") << int(CallWindow::Incoming); QTest::newRow("active") << int(CallWindow::Active);
        QTest::newRow("ended") << int(CallWindow::Ended); QTest::newRow("wait-decision") << int(CallWindow::WaitDecision);
    }
    void closeStates() {
        QFETCH(int, state); const size_t before = u_get_allocated_count();
        {
            CommandCore core; QVERIFY(core.ua); core.attach();
            CallWindow window; window.setCallId(77); window.setState(static_cast<CallWindow::State>(state));
            QSignalSpy closed(&window, &CallWindow::closed); window.show(); window.close();
            QCOMPARE(window.state(), CallWindow::Ended); QCOMPARE(closed.count(), 1);
            QCOMPARE(closed.front().front().toULongLong(), quint64(77));
            const int commands = state == CallWindow::Ended ? 0 : 1;
            QCOMPARE(pending(core.ua), commands);
            window.close(); QCOMPARE(pending(core.ua), commands); // До исполнения первой команды повтор не должен дублировать её.
            core.poll(); QCOMPARE(pending(core.ua), 0); QVERIFY(bindingValid);
#ifdef UTUN_TEST_CALL_HOOKS
            QCOMPARE(observed.size(), commands);
            if (commands) QCOMPARE(observed.front().action, state == CallWindow::Incoming ? GUI_CALL_DECLINE : GUI_CALL_HANGUP);
#endif
        }
        QCOMPARE(u_get_allocated_count(), before);
    }
    void incomingEndsSession_data() {
        QTest::addColumn<bool>("closeWindow");
        QTest::newRow("close") << true; QTest::newRow("decline-control") << false;
    }
    void incomingEndsSession() {
        QFETCH(bool, closeWindow); const size_t before = u_get_allocated_count();
        {
            IncomingEvents events; UASYNC* ua = uasync_create(); UTUN_INSTANCE* instance = nullptr;
            const auto release = qScopeGuard([&] {
                delete events.window.data(); gui_bridge_set_inst(nullptr); gui_bridge_set_uasync(nullptr);
                executingCore = nullptr;
                if (ua) uasync_mark_stopped(ua);
                if (instance) utun_instance_destroy(instance);
                if (ua) uasync_destroy(ua, 0);
            });
            QVERIFY(ua);
            instance = utun_instance_create_from_str(ua,
                "[global]\nauto_sockets=no\n[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n"
                "member_files_enabled=0\nlocal_invites_enabled=0\nlocal_broadcasts_enabled=0\n[ntp]\nenabled=no\n");
            QVERIFY(instance); QCOMPARE(utun_core_start(instance), 0); QCOMPARE(chat_service_start(instance), 0);
            instance->test_user_ptr = &events; chat_event_set_handler(instance, incomingEvent);
            uasync_mark_running(ua); gui_bridge_set_uasync(ua); gui_bridge_set_inst(instance);
            const uint64_t group = chat_core_create_channel_auto(instance, "Incoming close test"); QVERIFY(group);
            const uint64_t id = 101; QVERIFY(deliverInvite(instance, group, id)); QCOMPARE(events.incoming, 1u);
            events.window = new CallWindow(); events.window->setCallId(id); events.window->setState(CallWindow::Incoming);
            QObject::connect(events.window.data(), &CallWindow::closed, events.window.data(), [&events] {
                events.window->deleteLater();
            });
            events.window->show(); const int queued = pending(ua);
            if (closeWindow) events.window->close();
            else QVERIFY(QMetaObject::invokeMethod(events.window.data(), "onDecline", Qt::DirectConnection));
            QCOMPARE(pending(ua), queued + 1);
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            if (closeWindow) QVERIFY(events.window.isNull()); // Команда должна пережить GUI-владельца.
            executingCore = instance; uasync_poll(ua, 0); executingCore = nullptr;
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            auto* session = call_session_find(instance->call, id); QVERIFY(session);
            QCOMPARE(int(session->state), int(CALL_ST_ENDED)); QCOMPARE(int(session->reason), int(CALL_REASON_DECLINE));
            QCOMPARE(call_busy(instance), 0); QCOMPARE(events.ended, 1u); QCOMPARE(events.declined, 1u);
            QVERIFY(events.window.isNull()); QVERIFY(bindingValid);
            DEBUG_INFO(DEBUG_CATEGORY_CALL, "incoming %s verified: native=ENDED reason=DECLINE busy=0 window=deleted",
                closeWindow ? "close" : "decline");
        }
        QCOMPARE(u_get_allocated_count(), before);
    }
    void startLifecycle_data() { scenarios(); }
    void startLifecycle() {
        QFETCH(QString, scenario);
        const size_t before = u_get_allocated_count();
        {
            CommandCore core; QVERIFY(core.ua); prepare(core, scenario);
            const size_t ready = u_get_allocated_count();
            char channel[] = "dm:123456789abcdef0";
            const bool accepted = scenario == "executed" || scenario == "cancelled";
            uint8_t peer[32] = {42};
            QCOMPARE(gui_bridge_call_start(channel, peer, 73), accepted ? 0 : -1);
            memset(peer, 0, sizeof(peer));
            memset(channel, 'x', sizeof(channel) - 1); // После возврата caller может менять/освобождать строку.
            if (scenario == "executed") core.poll();
            else gui_bridge_set_inst(nullptr);
            QCOMPARE(pending(core.ua), 0); QCOMPARE(u_get_allocated_count(), ready);
#ifdef UTUN_TEST_CALL_HOOKS
            QCOMPARE(failAllocation.load(), 0); QCOMPARE(observed.size(), scenario == "executed" ? 1 : 0);
            if (!observed.isEmpty()) {
                QCOMPARE(observed.front().channel, QByteArray("dm:123456789abcdef0"));
                QCOMPARE(observed.front().peer, QByteArray(1, char(42)) + QByteArray(31, '\0')); QCOMPARE(observed.front().id, uint64_t(73));
            }
#endif
            QVERIFY(bindingValid);
        }
        QCOMPARE(u_get_allocated_count(), before);
    }
    void invalidArguments() {
        CommandCore core; QVERIFY(core.ua); core.attach(); const size_t before = u_get_allocated_count();
        char channel[64]; memset(channel, '1', sizeof(channel));
        const uint8_t peer[32] = {1};
        QCOMPARE(gui_bridge_call_start(nullptr, peer, 1), -1); QCOMPARE(gui_bridge_call_start("", peer, 1), -1);
        QCOMPARE(gui_bridge_call_start(channel, peer, 1), -1); QCOMPARE(gui_bridge_call_start("42", nullptr, 1), -1);
        QCOMPARE(gui_bridge_call_start("42", peer, 0), -1);
        QCOMPARE(gui_bridge_call_action(static_cast<gui_call_action>(99), 1), -1);
        QCOMPARE(gui_bridge_call_action(GUI_CALL_ACCEPT, 0), -1);
        QCOMPARE(pending(core.ua), 0); QCOMPARE(u_get_allocated_count(), before);
    }
    void concurrentRebind() {
        const size_t before = u_get_allocated_count();
        {
            CommandCore first, second; QVERIFY(first.ua); QVERIFY(second.ua); first.attach();
            std::thread producer([] {
                for (uint64_t id = 1; id <= 64; ++id) gui_bridge_call_action(GUI_CALL_HANGUP, id);
            });
            for (unsigned i = 0; i < 32; ++i) {
                gui_bridge_set_inst(nullptr);
                auto& core = i % 2 ? first : second; core.attach(); core.poll();
            }
            producer.join(); gui_bridge_set_inst(nullptr); first.attach();
            QCOMPARE(gui_bridge_call_action(GUI_CALL_HANGUP, 1000), 0); first.poll();
            QVERIFY(bindingValid); QCOMPARE(pending(first.ua), 0); QCOMPARE(pending(second.ua), 0);
#ifdef UTUN_TEST_CALL_HOOKS
            QVERIFY(!observed.isEmpty()); QCOMPARE(observed.back().instance, &first.instance);
#endif
        }
        QCOMPARE(u_get_allocated_count(), before);
    }
};

int main(int argc, char** argv) {
    if (argc >= 2 && !strcmp(argv[1], "--video-capture-worker")) return video_capture_worker_main(argc, argv);
    QApplication application(argc, argv); CallCommandsTest test; return QTest::qExec(&test, argc, argv);
}
#include "test_call_commands.moc"

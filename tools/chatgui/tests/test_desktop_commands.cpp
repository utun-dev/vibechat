/* Desktop MainWindow/MessageList/delegate/SQLite/bridge: отказы, отмена, OOM и привязка к владельцу.
 * Native trampoline исполняется без подмены; пустой каталог CHAT даёт штатный FINISHED/UNAVAILABLE для START. */
#include "../src/mainwindow.h"
#include "../src/kanbanview.h"
#include "../src/video_recording_window.h"
#include "../transport/gui_bridge.h"
#include "../transport/utun_node.h"
#include "../db/db_manager.h"
#include "../src/inputbar.h"
#include "../src/messagedelegate.h"
#include "../src/messagelist.h"
#include "../src/sound_manager.h"
#include "../src/imageviewer_window.h"
#include "../src/videoplayer_window.h"
#ifdef HAVE_STREAM_CAPTURE
#include "stream_capture_fixture.h"
#endif
#include <QScreen>
#include <QWindow>
#include <QJsonDocument>
#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QTextEdit>
#include <QLineEdit>
#include <QRadioButton>
#include <QClipboard>
#include <QDialog>
#include <QMenu>
#include <QListView>
#include <QLabel>
#include <QPushButton>
#include <QStandardItemModel>
#include <QPainter>
#include <QScopeGuard>
#include <atomic>
#include <thread>
#include <mutex>
#include <map>
#include <memory>
#include <cstring>
extern "C" {
#include "utun_instance.h"
#include "chat/chat_core.h"
#include "chat/chat_event.h"
#include "radio/radio_call.h"
#include "dm/dm_core.h"
#include "media_async/attachment_send.h"
#include "audio_compressor.h"
#include "mem.h"
#include "debug_config.h"
}

static UTUN_INSTANCE* executingCore = nullptr;
static bool bindingValid = true;
static QList<radio_call_request> observed;
static QMap<quint64, bool> submissions;

#ifdef UTUN_TEST_DESKTOP_HOOKS
#include "miniaudio.h"
static std::atomic<bool> watching{false};
static std::atomic<int> failAllocation{0};
static std::mutex allocationMutex;
static std::map<void*, const char*> outstanding;
static void track(void* pointer, const char* source) {
    if (!pointer || !watching || (!strstr(source, "/gui_bridge_impl.cpp:") && !strstr(source, "/mainwindow.cpp:") &&
        !strstr(source, "/messagelist.cpp:") && !strstr(source, "/messagefeedback.cpp:") &&
        !strstr(source, "/test_desktop_commands.cpp:"))) return;
    std::lock_guard<std::mutex> lock(allocationMutex); outstanding.emplace(pointer, source);
}
extern "C" void* __real_u_malloc_impl(uint32_t, const char*);
extern "C" void* __wrap_u_malloc_impl(uint32_t bytes, const char* source) {
    void* pointer = __real_u_malloc_impl(bytes, source); track(pointer, source); return pointer;
}
extern "C" void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
extern "C" void* __wrap_u_calloc_impl(uint32_t count, uint32_t bytes, const char* source) {
    if (failAllocation && strstr(source, "/gui_bridge_impl.cpp:") && --failAllocation == 0) {
        DEBUG_WARN(DEBUG_CATEGORY_RADIO, "[INJECT] radio command allocation failure at %s", source); return nullptr;
    }
    void* pointer = __real_u_calloc_impl(count, bytes, source); track(pointer, source); return pointer;
}
extern "C" void __real_u_free_impl(void*, const char*);
extern "C" void __wrap_u_free_impl(void* pointer, const char* source) {
    { std::lock_guard<std::mutex> lock(allocationMutex); outstanding.erase(pointer); }
    __real_u_free_impl(pointer, source);
}
extern "C" void __real_radio_call_trampoline(void*);
extern "C" void __wrap_radio_call_trampoline(void* arg) {
    auto* request = static_cast<radio_call_request*>(arg);
    if (request->inst != executingCore) {
        bindingValid = false;
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "Radio command binding mismatch id=%llu request_inst=%p executing_core=%p",
            (unsigned long long)request->id, static_cast<void*>(request->inst), static_cast<void*>(executingCore));
    }
    observed.append(*request);
    __real_radio_call_trampoline(arg);
}
// Настоящий mixer остаётся включённым; null backend устраняет зависимость от аппаратных устройств.
extern "C" ma_result __real_ma_context_init(const ma_backend*, ma_uint32, const ma_context_config*, ma_context*);
extern "C" ma_result __wrap_ma_context_init(const ma_backend*, ma_uint32, const ma_context_config* config, ma_context* context) {
    const ma_backend backend = ma_backend_null;
    return __real_ma_context_init(&backend, 1, config, context);
}
// Инъекции сохраняют настоящий finalize, чтобы даже ошибочный результат не оставлял statement.
static const char* sqlMatch = nullptr;
static int sqlStage = 0, sqlCalls = 0;
static bool failSql(sqlite3_stmt* statement, int stage) {
    const char* sql = sqlite3_sql(statement);
    if (sqlStage != stage || !sqlMatch || !sql || !strstr(sql, sqlMatch) || --sqlCalls) return false;
    sqlStage = 0; DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "[INJECT] SQLite stage=%d sql=%s", stage, sql); return true;
}
extern "C" int __real_sqlite3_step(sqlite3_stmt*);
extern "C" int __wrap_sqlite3_step(sqlite3_stmt* statement) {
    return failSql(statement, 1) ? SQLITE_IOERR : __real_sqlite3_step(statement);
}
extern "C" int __real_sqlite3_bind_text(sqlite3_stmt*, int, const char*, int, void(*)(void*));
extern "C" int __wrap_sqlite3_bind_text(sqlite3_stmt* statement, int index, const char* text, int bytes, void(*destroy)(void*)) {
    return failSql(statement, 2) ? SQLITE_NOMEM : __real_sqlite3_bind_text(statement, index, text, bytes, destroy);
}
extern "C" int __real_sqlite3_finalize(sqlite3_stmt*);
extern "C" int __wrap_sqlite3_finalize(sqlite3_stmt* statement) {
    const bool fail = failSql(statement, 3);
    const int rc = __real_sqlite3_finalize(statement); return fail ? SQLITE_IOERR : rc;
}
#endif

struct DesktopCore {
    QTemporaryDir directory;
    UASYNC* ua = uasync_create();
    UTUN_INSTANCE* inst = nullptr;
    unsigned results = 0;
    bool resultValid = true;
    ~DesktopCore() {
        gui_bridge_set_inst(nullptr); gui_bridge_set_uasync(nullptr);
        if (ua) uasync_mark_stopped(ua);
        if (inst) utun_instance_destroy(inst);
        if (ua) uasync_destroy(ua, 0);
    }
    bool start() {
        if (!ua || !directory.isValid()) {
            DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "fixture: no uasync/config directory"); return false;
        }
        const QByteArray config = "[global]\nauto_sockets=no\ndb_path=" + directory.path().toUtf8() +
            "\n[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n"
            "member_files_enabled=0\nlocal_invites_enabled=0\nlocal_broadcasts_enabled=0\n[ntp]\nenabled=no\n";
        const QString path = directory.filePath("utun.cfg"); QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(config) != config.size()) {
            DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "fixture: config write failed: %s", qPrintable(file.errorString())); return false;
        }
        file.close(); inst = utun_instance_create(ua, path.toUtf8().constData());
        if (!inst || utun_core_start(inst) || chat_service_start(inst)) {
            DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "fixture: core/chat startup failed"); return false;
        }
        inst->test_user_ptr = this;
        chat_event_set_handler(inst, [](UTUN_INSTANCE* instance, int type, const uint8_t* data, int len) {
            if (type == CHAT_EVT_MSG_SUBMITTED) gui_bridge_post(type, data, len);
            if (type != CHAT_EVT_RADIO_CALL) return;
            auto* core = static_cast<DesktopCore*>(instance->test_user_ptr); ++core->results;
            core->resultValid &= len == CHAT_RADIO_CALL_EVENT_SIZE && data[CHAT_RADIO_CALL_STATE] == RADIO_CALL_FINISHED &&
                data[CHAT_RADIO_CALL_STATE + 1] == RADIO_CALL_UNAVAILABLE && !data[CHAT_RADIO_CALL_STATE + 2];
        });
        attach(); poll(); return true;
    }
    void attach() { uasync_mark_running(ua); gui_bridge_set_uasync(ua); gui_bridge_set_inst(inst); }
    void poll() { executingCore = inst; uasync_poll(ua, 0); executingCore = nullptr; }
};
static unsigned pending(UASYNC* ua) {
    unsigned count = 0;
    for (auto* task = ua->posted_tasks_head; task; task = task->next) ++count;
    return count;
}

class DesktopCommandsTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        qApp->setQuitOnLastWindowClosed(false);
        debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); gui_bridge_init();
        debug_set_category_level(DEBUG_CATEGORY_RADIO, DEBUG_LEVEL_DEBUG);
        debug_set_category_level(DEBUG_CATEGORY_GENERAL, DEBUG_LEVEL_DEBUG);
    }
    void init() {
        observed.clear(); submissions.clear(); bindingValid = true;
        gui_bridge_set_msg_submitted_cb([](const uint8_t* data, int len) {
            if (len != 9) return;
            quint64 id = 0; for (int i = 0; i < 8; ++i) id |= quint64(data[i]) << (8 * i);
            submissions.insert(id, data[8] != 0);
        });
#ifdef UTUN_TEST_DESKTOP_HOOKS
        watching = false; failAllocation = 0; sqlMatch = nullptr; sqlStage = 0; sqlCalls = 0;
#endif
    }
    void cleanup() {
#ifdef UTUN_TEST_DESKTOP_HOOKS
        failAllocation = 0; sqlStage = 0; sqlMatch = nullptr;
#endif
        gui_bridge_set_inst(nullptr); gui_bridge_set_uasync(nullptr);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents(); gui_bridge_set_msg_submitted_cb(nullptr);
#ifdef UTUN_TEST_DESKTOP_HOOKS
        watching = false;
        std::lock_guard<std::mutex> lock(allocationMutex);
        for (const auto& item : outstanding)
            DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "Unfreed radio command ptr=%p source=%s", item.first, item.second);
        QCOMPARE(outstanding.size(), size_t(0));
#endif
    }
    void kanbanVideoRequestOpensSharedRecorder() {
        if (!VideoRecordingEngine::supported()) QSKIP("Capture unavailable in this build");
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString path = directory.filePath("video.cfg"); QFile config(path); QVERIFY(config.open(QIODevice::WriteOnly));
        const QByteArray text = "[global]\nauto_sockets=no\n"
            "my_private_key=704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f\n"
            "my_public_key=7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f\n"
            "[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\nmember_files_enabled=0\n"
            "local_invites_enabled=0\nlocal_broadcasts_enabled=0\n[ntp]\nenabled=no\n";
        QCOMPARE(config.write(text),text.size()); config.close();
        const auto logger = qScopeGuard([] { debug_disable_file_output(); });
        DbManager db; MainWindow window(nullptr,&db,path,directory.path(),directory.filePath("video.log"));
        QVERIFY(window.utunNode() && window.utunNode()->isRunning()); QTRY_VERIFY(db.isOpen());
        auto* board = window.findChild<KanbanView*>(); QVERIFY(board);
        auto* composer = board->detailsPanel()->findChild<InputBar*>("kanbanComposer"); QVERIFY(composer);
        composer->setEnabled(true); composer->setMediaEnabled(true);
        auto* button = composer->findChild<QPushButton*>("recordVideoButton"); QVERIFY(button && button->isEnabled());
        board->videoRecordingRequested("42",QString(64,QLatin1Char('a')),"Original card");
        auto* recorder = window.findChild<VideoRecordingWindow*>(); QVERIFY(recorder);
        bool named = false;
        for (auto* label : recorder->findChildren<QLabel*>()) if (label->text().contains("Original card")) named = true;
        QVERIFY(named);
        board->videoRecordingRequested("43",QString(64,QLatin1Char('b')),"Other card");
        QCOMPARE(window.findChildren<VideoRecordingWindow*>().size(),1); QCOMPARE(window.findChild<VideoRecordingWindow*>(),recorder);
        QSignalSpy stopped(window.utunNode(), &UtunNode::stopped);
        window.requestShutdown(); QTRY_COMPARE_WITH_TIMEOUT(stopped.size(),1,5000);
    }
    void chatWindowReactivationFocus_data() {
        QTest::addColumn<bool>("imageChoice");
        QTest::newRow("history") << false; QTest::newRow("image-format") << true;
    }
    void chatWindowReactivationFocus() {
        QFETCH(bool, imageChoice); QTemporaryDir directory; QVERIFY(directory.isValid());
        MainWindow window(nullptr, nullptr, directory.filePath("focus.cfg"));
        auto* history = window.findChild<MessageList*>(); QVERIFY(history); history->loadChannel("123");
        auto* composer = history->findChild<InputBar*>(); QVERIFY(composer); composer->setRecordingContext(directory.path(), "123");
        auto* editor = composer->findChild<QTextEdit*>(); QVERIFY(editor); QCOMPARE(history->focusProxy(), composer);
        window.show(); window.activateWindow(); QTRY_VERIFY(window.isActiveWindow()); QTRY_VERIFY(editor->hasFocus());
        const QString draft = "first line\nsecond line"; editor->setPlainText(draft);
        auto cursor = editor->textCursor(); cursor.setPosition(2); cursor.setPosition(7, QTextCursor::KeepAnchor); editor->setTextCursor(cursor);
        QRadioButton* jpeg = nullptr;
        const auto clearClipboard = qScopeGuard([&] { if (imageChoice) QApplication::clipboard()->clear(); });
        if (imageChoice) {
            QImage image(32, 24, QImage::Format_RGB32); image.fill(Qt::green); QApplication::clipboard()->setImage(image); editor->paste();
            jpeg = composer->findChild<QRadioButton*>("clipboardJpegChoice"); QVERIFY(jpeg);
            QTRY_VERIFY(jpeg->isVisible()); QTest::mouseClick(jpeg, Qt::LeftButton); QVERIFY(jpeg->isChecked()); jpeg->setFocus();
            QTRY_VERIFY(jpeg->hasFocus());
        } else {
            auto* view = history->findChild<QListView*>(); QVERIFY(view); view->setFocus(); QTRY_VERIFY(view->hasFocus());
        }
        QLineEdit other; other.show(); other.activateWindow(); other.setFocus();
        QTRY_VERIFY(other.isActiveWindow()); QTRY_VERIFY(other.hasFocus());
        QTest::keyClicks(&other, "other window"); QCOMPARE(other.text(), QString("other window"));
        window.activateWindow(); QTRY_VERIFY(window.isActiveWindow()); QTRY_VERIFY(editor->hasFocus());
        QCOMPARE(editor->toPlainText(), draft); QCOMPARE(editor->textCursor().position(), 7); QCOMPARE(editor->textCursor().anchor(), 2);
        if (imageChoice) QVERIFY(jpeg->isChecked());
        QTest::keyClicks(QApplication::focusWidget(), "x"); QCOMPARE(editor->toPlainText(), QString("fixine\nsecond line"));
    }
    void chatActivationRespectsCurrentWindow_data() {
        QTest::addColumn<QString>("state");
        for (const char* state : {"other-window", "disabled", "hidden", "dialog", "popup"}) QTest::newRow(state) << QString(state);
    }
    void chatActivationRespectsCurrentWindow() {
        QFETCH(QString, state); QTemporaryDir directory; QVERIFY(directory.isValid());
        MainWindow window(nullptr, nullptr, directory.filePath("focus.cfg"));
        auto* history = window.findChild<MessageList*>(); QVERIFY(history); history->loadChannel("123");
        auto* composer = history->findChild<InputBar*>(); QVERIFY(composer);
        auto* editor = composer->findChild<QTextEdit*>(); QVERIFY(editor);
        auto* view = history->findChild<QListView*>(); QVERIFY(view);
        window.show(); window.activateWindow(); QTRY_VERIFY(window.isActiveWindow()); QTRY_VERIFY(editor->hasFocus());
        view->setFocus(); QTRY_VERIFY(view->hasFocus());
        QEvent activation(QEvent::ActivationChange); QApplication::sendEvent(&window, &activation);
        QLineEdit other; QDialog dialog(&window); QLineEdit dialogInput(&dialog); QMenu popup(&window); popup.addAction("Menu item");
        if (state == "other-window") { other.show(); other.activateWindow(); other.setFocus(); QTRY_VERIFY(other.isActiveWindow()); }
        if (state == "disabled") composer->setEnabled(false);
        if (state == "hidden") window.hide();
        if (state == "dialog") { dialog.setModal(true); dialog.show(); dialog.activateWindow(); dialogInput.setFocus(); }
        if (state == "popup") popup.popup(window.mapToGlobal(QPoint(100, 100)));
        QCoreApplication::processEvents(); QVERIFY(!editor->hasFocus());
        if (state == "other-window") { QVERIFY(other.isActiveWindow()); QVERIFY(other.hasFocus()); }
        if (state == "dialog") { QCOMPARE(QApplication::activeModalWidget(), &dialog); QTRY_VERIFY(dialogInput.hasFocus()); }
        if (state == "popup") QCOMPARE(QApplication::activePopupWidget(), &popup);
    }

    void imagePreviewPreparedOutsidePaint() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString path = directory.filePath("image.png");
        QImage original(1200, 1800, QImage::Format_ARGB32); original.fill(Qt::red); QVERIFY(original.save(path));
        MessageList history; history.resize(600, 500); history.show();
        auto* model = history.findChild<QStandardItemModel*>(); QVERIFY(model);
        auto* view = history.findChild<QListView*>(); QVERIFY(view);
        auto* item = new QStandardItem;
        item->setData("image/png", MsgContentTypeRole); item->setData(path, MsgVoiceFileRole);
        item->setData(1200, MsgImageWidthRole); item->setData(1800, MsgImageHeightRole);
        item->setData(2, MsgFileDownloadStateRole); model->appendRow(item);
        QTRY_VERIFY_WITH_TIMEOUT(!item->data(MsgImagePreviewRole).value<QImage>().isNull(), 3000);
        const QImage preview = item->data(MsgImagePreviewRole).value<QImage>();
        QVERIFY(preview.width() <= 600 && preview.height() <= 600);
        QCOMPARE(preview.width() * 3, preview.height() * 2); QVERIFY(!item->data(MsgImageErrorRole).toBool());
        QVERIFY(QFile::remove(path)); // Paint обязан использовать подготовленное превью даже без файла на диске.
        QImage canvas(600, 400, QImage::Format_ARGB32); canvas.fill(Qt::black);
        QStyleOptionViewItem option; option.initFrom(view); option.rect = canvas.rect();
        QPainter painter(&canvas); view->itemDelegate()->paint(&painter, option, model->index(0, 0)); painter.end();
        int red = 0;
        for (int y = 0; y < canvas.height(); ++y) for (int x = 0; x < canvas.width(); ++x) {
            const QColor color = canvas.pixelColor(x, y);
            if (color.red() > 220 && color.green() < 30 && color.blue() < 30) ++red;
        }
        QVERIFY(red > 1000);
    }

    void boundCommand_data() {
        QTest::addColumn<QString>("scenario");
        for (const auto& name : {"unavailable", "stopped", "cancelled", "executed"}) QTest::newRow(name) << QString(name);
#ifdef UTUN_TEST_DESKTOP_HOOKS
        QTest::newRow("queue-oom") << QString("queue-oom");
#endif
    }
    void boundCommand() {
        QFETCH(QString, scenario); DesktopCore core; QVERIFY(core.start());
        if (scenario == "unavailable") gui_bridge_set_inst(nullptr);
        if (scenario == "stopped") uasync_mark_stopped(core.ua);
#ifdef UTUN_TEST_DESKTOP_HOOKS
        watching = true; failAllocation = scenario == "queue-oom" ? 1 : 0;
#endif
        int executed = 0; bool valid = false;
        struct Command { UTUN_INSTANCE* inst; int* executed; bool* valid; };
        auto* request = static_cast<Command*>(u_calloc(1, sizeof(Command))); QVERIFY(request);
        request->executed = &executed; request->valid = &valid;
        const int rc = gui_bridge_post_bound_fn([](void* arg) {
            auto* command = static_cast<Command*>(arg); ++*command->executed;
            *command->valid = command->inst && command->inst == executingCore; u_free(command);
        }, request, &request->inst);
        const bool accepted = scenario == "executed" || scenario == "cancelled";
        QCOMPARE(rc, accepted ? 0 : -1);
        if (rc) { QVERIFY(!request->inst); u_free(request); }
        if (scenario == "executed") core.poll();
        else gui_bridge_set_inst(nullptr);
        QCOMPARE(executed, scenario == "executed" ? 1 : 0);
        if (executed) QVERIFY(valid);
        QCOMPARE(pending(core.ua), 0u);
    }
    void attachmentOwnership_data() {
        QTest::addColumn<QString>("scenario"); QTest::addColumn<bool>("temporary");
        QStringList names{"unavailable", "stopped", "cancelled", "executed"};
#ifdef UTUN_TEST_DESKTOP_HOOKS
        names << "queue-oom";
#endif
        for (const auto& name : names) for (bool temporary : {false, true})
            QTest::newRow(qPrintable(name + (temporary ? "-owned-source" : "-borrowed-source"))) << name << temporary;
    }
    void attachmentOwnership() {
        QFETCH(QString, scenario); QFETCH(bool, temporary); std::atomic<int> releases{0}; DesktopCore core; QVERIFY(core.start());
        const quint64 channel = chat_core_create_channel_auto(core.inst, "Attachment ownership"); QVERIFY(channel);
        core.poll();
        const QByteArray source = core.directory.filePath("recording.raw").toUtf8();
        QFile file(QString::fromUtf8(source)); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write("source"), qint64(6));
        file.close();
#ifdef UTUN_TEST_DESKTOP_HOOKS
        watching = true;
#endif
        struct PcmOwner { std::atomic<int>* releases; int16_t samples[24000]; };
        auto* owner = static_cast<PcmOwner*>(u_calloc(1, sizeof(PcmOwner))); QVERIFY(owner); owner->releases = &releases;
        auto* request = static_cast<attachment_send_req*>(u_calloc(1, sizeof(attachment_send_req))); QVERIFY(request);
        request->pcm = owner->samples; request->pcm_count = 24000; request->pcm_owner = owner;
        request->pcm_release = [](void* arg) { auto* pcm = static_cast<PcmOwner*>(arg); ++*pcm->releases; u_free(pcm); };
        request->info.kind = ATTACHMENT_VOICE; request->request_id = 77;
        qstrncpy(request->target, topo_groups_channel(core.inst->topo_groups, channel), sizeof(request->target));
        qstrncpy(request->source, source.constData(), sizeof(request->source)); request->temporary_source = temporary;
        if (scenario == "unavailable") gui_bridge_set_inst(nullptr);
        if (scenario == "stopped") uasync_mark_stopped(core.ua);
#ifdef UTUN_TEST_DESKTOP_HOOKS
        failAllocation = scenario == "queue-oom" ? 1 : 0;
#endif
        const bool accepted = scenario == "executed" || scenario == "cancelled";
        const int rc = gui_bridge_submit_attachment(request); QCOMPARE(rc, accepted ? 0 : -1);
        if (rc) attachment_send_discard(request);
        if (scenario == "executed") {
            QTRY_VERIFY_WITH_TIMEOUT(([&] { core.poll(); return submissions.contains(77); })(), 5000);
            QVERIFY(submissions.value(77));
            DbManager db; db.setDb(chat_core_get_db(core.inst));
            const auto messages = db.getMessages(QString::fromLatin1(topo_groups_channel(core.inst->topo_groups, channel)), 1);
            QCOMPARE(messages.size(), 1); QCOMPARE(messages.first().contentType, QString("audio/opus"));
        } else {
            gui_bridge_set_inst(nullptr); QCoreApplication::processEvents();
            QCOMPARE(submissions.contains(77), scenario == "cancelled");
            if (submissions.contains(77)) QVERIFY(!submissions.value(77));
        }
        QCOMPARE(releases.load(), 1); QCOMPARE(QFile::exists(QString::fromUtf8(source)), !temporary);
    }
    void discardUnsubmittedCompressor() {
        const auto baseline = u_get_allocated_count();
        auto* request = static_cast<attachment_send_req*>(u_calloc(1, sizeof(attachment_send_req))); QVERIFY(request);
        auto discard = qScopeGuard([&] { attachment_send_discard(request); });
        request->compressor = audio_compressor_create(); QVERIFY(request->compressor);
        const audio_compressor_config_t settings{}; QCOMPARE(audio_compressor_configure(request->compressor, &settings), 0);
        const std::vector<int16_t> samples(24000, 0);
        QCOMPARE(audio_compressor_push(request->compressor, samples.data(), samples.size()), 0);
        attachment_send_discard(request); request = nullptr; QCOMPARE(u_get_allocated_count(), baseline);
    }
    void mediaWindowGeometry_data() {
        QTest::addColumn<int>("monitor"); QTest::addColumn<bool>("fullscreen"); QTest::addColumn<bool>("maximized");
        for (int monitor = 0; monitor < QGuiApplication::screens().size(); ++monitor)
            for (const QString& mode : {QString("normal"), QString("maximized"), QString("fullscreen")})
                QTest::newRow(qPrintable(QString("monitor-%1-%2").arg(monitor).arg(mode)))
                    << monitor << (mode == "fullscreen") << (mode == "maximized");
    }
    void mediaWindowGeometry() {
        QFETCH(int, monitor); QFETCH(bool, fullscreen); QFETCH(bool, maximized);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString path = directory.filePath("media.cfg"), log = directory.filePath("media.log");
        QFile config(path); QVERIFY(config.open(QIODevice::WriteOnly));
        const QByteArray text = "[global]\nauto_sockets=no\n"
            "my_private_key=704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f\n"
            "my_public_key=7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f\n"
            "[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n"
            "member_files_enabled=0\nlocal_invites_enabled=0\nlocal_broadcasts_enabled=0\n[ntp]\nenabled=no\n";
        QCOMPARE(config.write(text), text.size()); config.close();
        const QString photo = directory.filePath("photo.png");
        QImage image(32, 24, QImage::Format_RGB32); image.fill(Qt::green); QVERIFY(image.save(photo));
        const auto logger = qScopeGuard([] { debug_disable_file_output(); });
        const auto diagnostics = qScopeGuard([&] {
            if (QTest::currentTestFailed()) {
                directory.setAutoRemove(false);
                DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "Media geometry fixture failed; diagnostics: %s", qPrintable(log));
            }
        });
        auto findWindow = [](const QString& name) -> QWidget* {
            for (auto* widget : QApplication::topLevelWidgets())
                if (widget->objectName() == name && widget->isVisible()) return widget;
            return nullptr;
        };
        QScreen* display = QGuiApplication::screens()[monitor];
        QRect expected;
        for (int stage = 0; stage < 3; ++stage) {
            DbManager db; MainWindow window(nullptr, &db, path, directory.path(), log);
            auto* node = window.utunNode();
            if (!node || !node->isRunning()) directory.setAutoRemove(false);
            QVERIFY2(node && node->isRunning(), qPrintable(log)); QTRY_VERIFY(db.isOpen());
            debug_set_category_level(DEBUG_CATEGORY_GENERAL, DEBUG_LEVEL_DEBUG);
            QVERIFY(QMetaObject::invokeMethod(&window, "openImage", Q_ARG(QString, photo)));
            auto* viewer = findWindow("chatImageViewer"); QVERIFY(viewer);
            if (!stage) {
                QVERIFY(!viewer->isFullScreen());
                viewer->windowHandle()->setScreen(display);
                viewer->setGeometry(QRect(display->availableGeometry().topLeft() + QPoint(70, 65), QSize(520, 340)));
                QCoreApplication::processEvents();
                expected = viewer->normalGeometry();
            } else if (stage == 1) {
                QCOMPARE(viewer->screen(), display);
                QCOMPARE(viewer->normalGeometry(), expected);
                QCOMPARE(viewer->isFullScreen(), fullscreen);
                QCOMPARE(viewer->isMaximized(), maximized);
            } else {
                QCOMPARE(viewer->screen(), window.screen());
                const QRect area = window.screen()->availableGeometry(), actual = viewer->normalGeometry();
                const QMargins margins = viewer->windowHandle()->frameMargins();
                DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "Media fallback: area=%d,%d %dx%d normal=%d,%d %dx%d frame=%d,%d,%d,%d state=%d",
                           area.x(), area.y(), area.width(), area.height(), actual.x(), actual.y(), actual.width(), actual.height(),
                           margins.left(), margins.top(), margins.right(), margins.bottom(), int(viewer->windowState()));
                QVERIFY(window.screen()->availableGeometry().contains(viewer->normalGeometry()));
                QVERIFY(!viewer->isFullScreen());
            }
            QVERIFY(viewer->close());
            if (stage < 2) {
                QVERIFY(QMetaObject::invokeMethod(&window, "openVideo", Q_ARG(QString, directory.filePath("missing.mp4"))));
                auto* player = findWindow("chatVideoPlayer"); QVERIFY(player);
                QCOMPARE(player->screen(), display); QCOMPARE(player->normalGeometry(), expected);
                QCOMPARE(player->isFullScreen(), stage == 1 && fullscreen);
                QCOMPARE(player->isMaximized(), stage == 1 && maximized);
                if (!stage && fullscreen) player->showFullScreen();
                if (!stage && maximized) player->showMaximized();
                if (stage == 1) {
                    if (fullscreen) {
                        QTest::keyClick(player, Qt::Key_Escape);
                        QVERIFY(!player->isFullScreen()); QCOMPARE(player->geometry(), expected);
                    }
                    QVERIFY(player->close());
                    QJsonObject missing{{"x", 30000}, {"y", -30000}, {"width", 20000}, {"height", 20000},
                        {"mode", "normal"}, {"screen", "disconnected-monitor"}, {"serial", "missing-physical-monitor"},
                        {"screen_x", 0}, {"screen_y", 0}};
                    const QByteArray value = QJsonDocument(missing).toJson(QJsonDocument::Compact);
                    QCOMPARE(gui_bridge_post_setting(GUI_SETTING_UI_STATE, "media_window_geometry", value.constData()), 0);
                    QTRY_COMPARE(db.getUiState("media_window_geometry"), QString::fromUtf8(value));
                }
            }
            QSignalSpy stopped(node, &UtunNode::stopped);
            window.requestShutdown(); QTRY_COMPARE_WITH_TIMEOUT(stopped.size(), 1, 5000);
            QVERIFY(!db.isOpen());
        }
        DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "Media geometry verified: monitor=%s fullscreen=%d maximized=%d restart=1 missing_monitor_fallback=1",
                   qPrintable(display->name()), fullscreen, maximized);
    }

    void orderedWindowShutdownPersistsGeometry() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString path = directory.filePath("shutdown.cfg"), log = directory.filePath("shutdown.log");
        QFile config(path); QVERIFY(config.open(QIODevice::WriteOnly));
        const QByteArray text = "[global]\nauto_sockets=no\n"
            "my_private_key=704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f\n"
            "my_public_key=7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f\n"
            "[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n"
            "member_files_enabled=0\nlocal_invites_enabled=0\nlocal_broadcasts_enabled=0\n[ntp]\nenabled=no\n";
        QCOMPARE(config.write(text), text.size()); config.close();
        auto logger = qScopeGuard([] { debug_disable_file_output(); });
        DbManager db; MainWindow window(nullptr, &db, path, directory.path(), log);
        auto* node = window.utunNode();
        if (!node || !node->isRunning()) {
            directory.setAutoRemove(false);
            DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Shutdown fixture startup failed; diagnostics: %s", qPrintable(log));
        }
        QVERIFY(node && node->isRunning()); QTRY_VERIFY(db.isOpen());
        window.resize(1017, 713); const QRect geometry = window.normalGeometry(); QVERIFY(!geometry.isEmpty());
        const QString expected = QStringLiteral("%1,%2,%3,%4").arg(geometry.width()).arg(geometry.height()).arg(geometry.x()).arg(geometry.y());
        QSignalSpy stopping(node, &UtunNode::stopping), stopped(node, &UtunNode::stopped);
        window.requestShutdown(); QTRY_COMPARE_WITH_TIMEOUT(stopped.size(), 1, 5000);
        QCOMPARE(stopping.size(), 1); QVERIFY(!db.isOpen());
        sqlite3* saved = nullptr;
        QCOMPARE(sqlite3_open_v2(qPrintable(directory.filePath("chats.db")), &saved, SQLITE_OPEN_READONLY, nullptr), SQLITE_OK);
        auto close = qScopeGuard([&] { sqlite3_close(saved); });
        DbManager snapshot; snapshot.setDb(saved);
        QCOMPARE(snapshot.getUiState("window_geometry"), expected);
        QCOMPARE(snapshot.getUiState("window_maximized"), QString("0"));
        QVERIFY(!snapshot.getUiState("window_splitter").isEmpty()); snapshot.setDb(nullptr);
    }
    void dmAcknowledgment_data() {
        QTest::addColumn<QString>("scenario");
        for (const auto& name : {"executed", "stale-generation", "cancelled", "unavailable"})
            QTest::newRow(name) << QString(name);
#ifdef UTUN_TEST_DESKTOP_HOOKS
        QTest::newRow("queue-oom") << QString("queue-oom");
#endif
    }
    void dmAcknowledgment() {
        QFETCH(QString, scenario); DesktopCore core; QVERIFY(core.start());
        SC_MYKEYS peer{}; uint8_t ed[32], seed[32];
        QCOMPARE(sc_identity_generate(peer.public_key, peer.private_key, ed, seed), SC_OK);
        QCOMPARE(dm_start(core.inst, sc_derive_node_id_from_pubkey(peer.public_key), peer.public_key, ed,
            "Peer", nullptr, DM_MODE_DEVICE, nullptr), 0);
        DbManager db; db.setDb(chat_core_get_db(core.inst)); const auto conversations = db.getDmConversations();
        QCOMPARE(conversations.size(), 1); core.poll();
#ifdef UTUN_TEST_DESKTOP_HOOKS
        watching = true;
#endif
        auto* request = static_cast<dm_send_req*>(u_calloc(1, sizeof(dm_send_req) + 5)); QVERIFY(request);
        qstrncpy(request->conv_id, qPrintable(conversations.first().convId), sizeof(request->conv_id));
        request->generation = conversations.first().generation + (scenario == "stale-generation");
        request->request_id = 88; request->data_len = 5; request->data = reinterpret_cast<uint8_t*>(request + 1);
        memcpy(request->data, "hello", 5);
        if (scenario == "unavailable") gui_bridge_set_inst(nullptr);
#ifdef UTUN_TEST_DESKTOP_HOOKS
        failAllocation = scenario == "queue-oom" ? 1 : 0;
#endif
        const bool accepted = scenario != "unavailable" && scenario != "queue-oom";
        const int rc = gui_bridge_submit_dm_message(request); QCOMPARE(rc, accepted ? 0 : -1);
        if (rc) u_free(request);
        if (scenario == "cancelled") gui_bridge_set_inst(nullptr);
        else if (accepted) core.poll();
        if (accepted) {
            QTRY_VERIFY(submissions.contains(88)); QCOMPARE(submissions.value(88), scenario == "executed");
        } else { QCoreApplication::processEvents(); QVERIFY(!submissions.contains(88)); }
        QCOMPARE(db.getDmConversations().first().lastOutSeq, qint64(scenario == "executed" ? 1 : 0));
    }
    void dmComposerKeepsRejectedDraft() {
        InputBar input(nullptr, nullptr, nullptr); input.setDmMode(true); input.setRecordingContext(".", "dm:7", 1);
        auto* editor = input.findChild<QTextEdit*>(); QVERIFY(editor); editor->setPlainText("keep this draft");
        quint64 request = 0;
        connect(&input, &InputBar::sendMessage, &input, [&](const QString&, const QByteArray&, quint64 id) {
            request = id; input.submissionResult(id, false);
        });
        QVERIFY(QMetaObject::invokeMethod(&input, "onSendClicked", Qt::DirectConnection));
        QVERIFY(request); QCOMPARE(editor->toPlainText(), QString("keep this draft"));
        input.submissionResult(0, false); QCOMPARE(editor->toPlainText(), QString("keep this draft"));
    }
    void sqliteReadsAreAtomicSnapshots() {
        sqlite3* handle = nullptr; QCOMPARE(sqlite3_open(":memory:", &handle), SQLITE_OK);
        auto close = qScopeGuard([&] { sqlite3_close(handle); });
        QCOMPARE(sqlite3_exec(handle, "CREATE TABLE accounts(node_x,display_name,avatar_color,avatar_letter,is_contact);"
            "INSERT INTO accounts VALUES(1,'Alice','red','A',1),(2,'Bob','blue','B',1);"
            "CREATE TABLE ui_state(key,value);INSERT INTO ui_state VALUES('draft','original');"
            "CREATE TABLE attachment_keys(channel,message_id,ready,downloaded,decrypting,error,plain_path,info,content_type,key);"
            "CREATE TABLE attachment_key_requests(channel,message_id,incoming,state);"
            "INSERT INTO attachment_keys VALUES('42',zeroblob(32),1,1,0,0,'original.opus',X'010203','audio/opus',zeroblob(32));",
            nullptr, nullptr, nullptr), SQLITE_OK);
        DbManager db; db.setDb(handle);
        QCOMPARE(db.getAccounts().size(), 2); QCOMPARE(db.getUiState("draft"), QString("original"));
        QVERIFY(!db.prepareOrNull("DELETE FROM accounts")); QCOMPARE(db.getAccounts().size(), 2);
        const auto snapshot = db.getAttachment("42", QByteArray(32, '\0'));
        QVERIFY(snapshot.found && snapshot.ready && snapshot.downloaded && snapshot.hasKey);
        QCOMPARE(snapshot.info, QByteArray::fromHex("010203")); QCOMPARE(snapshot.requestState, -1);
        QCOMPARE(sqlite3_exec(handle, "UPDATE attachment_keys SET plain_path='new.opus',info=X'04'", nullptr, nullptr, nullptr), SQLITE_OK);
        QCOMPARE(snapshot.path, QString("original.opus")); QCOMPARE(snapshot.info, QByteArray::fromHex("010203"));
        QCOMPARE(db.getAttachment("42", QByteArray(32, '\0')).path, QString("new.opus"));
#ifdef UTUN_TEST_DESKTOP_HOOKS
        sqlMatch = "FROM accounts"; sqlStage = 1; sqlCalls = 2;
        QVERIFY(db.getAccounts().isEmpty()); QCOMPARE(sqlStage, 0);
        sqlMatch = "FROM ui_state"; sqlStage = 2; sqlCalls = 1;
        QVERIFY(db.getUiState("draft").isEmpty()); QCOMPARE(sqlStage, 0);
        sqlMatch = "FROM accounts"; sqlStage = 3; sqlCalls = 1;
        QVERIFY(db.getAccounts().isEmpty()); QCOMPARE(sqlStage, 0);
        sqlMatch = "FROM attachment_keys"; sqlStage = 2; sqlCalls = 1;
        QVERIFY(!db.getAttachment("42", QByteArray(32, '\0')).found); QCOMPARE(sqlStage, 0);
#endif
        QCOMPARE(db.getAccounts().size(), 2); QCOMPARE(db.getUiState("draft"), QString("original"));
        QVERIFY(!sqlite3_next_stmt(handle, nullptr)); db.setDb(nullptr);
    }
    void mediaGesture_data() {
        QTest::addColumn<QString>("gesture");
        for (const auto& name : {"release-only", "drag", "blank", "click"}) QTest::newRow(name) << QString(name);
    }
    void mediaGesture() {
        QFETCH(QString, gesture); QListView view; MessageDelegate delegate; QStandardItemModel model;
        auto* item = new QStandardItem; item->setData("video/mp4", MsgContentTypeRole);
        item->setData("42", MsgChannelIdRole); item->setData(1, MsgIdRole); model.appendRow(item);
        QStyleOptionViewItem option; option.widget = &view; option.rect = QRect(0, 0, 500, 200);
        int downloads = 0; delegate.setDownloadCallback([&](const QModelIndex&) { ++downloads; return true; });
        const QPointF begin = gesture == "blank" ? QPointF(450, 40) : QPointF(100, 40);
        const QPointF end = gesture == "drag" ? QPointF(220, 40) : begin;
        QMouseEvent press(QEvent::MouseButtonPress, begin, begin, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, end, end, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        if (gesture != "release-only") delegate.editorEvent(&press, &model, option, model.index(0, 0));
        delegate.editorEvent(&release, &model, option, model.index(0, 0));
        QCOMPARE(downloads, gesture == "click" ? 1 : 0);
    }
    void voiceTimerSurvivesViewDestructionAndStopsWithoutPaint() {
        auto* sound = SoundManager::instance(); QVERIFY(sound->init());
        auto shutdown = qScopeGuard([&] { MessageDelegate::stopAllAutoPlay(); sound->shutdown(); });
        auto pcm = std::make_shared<std::vector<int16_t>>(48000, 0);
        const auto id = sound->playRawPcm(pcm->data(), int(pcm->size()), 48000, 1, pcm); QVERIFY(id);
        QStandardItemModel model; auto* item = new QStandardItem;
        item->setData("audio/opus", MsgContentTypeRole); item->setData("voice-test.opus", MsgVoiceFileRole);
        item->setData("1.0s", MsgVoiceDurationRole); item->setData(2, MsgFileDownloadStateRole); model.appendRow(item);
        MessageDelegate delegate; auto* view = new QListView;
        QStyleOptionViewItem option; option.widget = view; option.rect = QRect(0, 0, 500, 200);
        QImage image(500, 200, QImage::Format_ARGB32_Premultiplied); image.fill(Qt::transparent);
        { QPainter painter(&image); delegate.paint(&painter, option, model.index(0, 0)); }
        MessageDelegate::startAutoPlay("voice-test.opus", 1.0f, id);
        auto* timer = qApp->findChild<QTimer*>("messageVoiceTimer"); QVERIFY(timer && timer->isActive());
        QTRY_VERIFY_WITH_TIMEOUT(sound->pcmCursor(id) > 0, 2000);
        // Ни paint, ни видимость view не должны требоваться для завершения прогресса.
        sound->stopPcm(id); QTRY_VERIFY_WITH_TIMEOUT(!timer->isActive(), 3000);
        MessageDelegate::startAutoPlay("voice-test.opus", 1.0f, 999);
        QVERIFY(timer->isActive()); delete view; QVERIFY(!timer->isActive()); QTest::qWait(30);
    }
    void lifecycle_data() {
        QTest::addColumn<int>("action"); QTest::addColumn<QString>("scenario"); QTest::addColumn<bool>("window");
        QStringList scenarios{"unavailable", "stopped", "cancelled", "executed"};
#ifdef UTUN_TEST_DESKTOP_HOOKS
        scenarios << "request-oom" << "queue-oom";
#endif
        for (int action = GUI_RADIO_CALL_START; action <= GUI_RADIO_CALL_CONTINUE; ++action)
            for (const auto& scenario : scenarios)
                QTest::newRow(qPrintable(QString("bridge-%1-%2").arg(action).arg(scenario))) << action << scenario << false;
        for (const auto& scenario : scenarios)
            QTest::newRow(qPrintable("window-" + scenario)) << int(GUI_RADIO_CALL_START) << scenario << true;
        QTest::newRow("window-destroyed-before-execution") << int(GUI_RADIO_CALL_START) << QString("destroyed") << true;
    }
    void lifecycle() {
        QFETCH(int, action); QFETCH(QString, scenario); QFETCH(bool, window);
        DesktopCore core; QVERIFY(core.start());
        std::unique_ptr<MainWindow> form;
        if (window) {
            form = std::make_unique<MainWindow>(nullptr, nullptr, QString()); // UtunNode остаётся владельцу fixture.
            QCoreApplication::processEvents(); core.poll(); // Отделить стартовые настройки media от проверяемой команды.
        }
        if (scenario == "unavailable") gui_bridge_set_inst(nullptr);
        if (scenario == "stopped") uasync_mark_stopped(core.ua);
#ifdef UTUN_TEST_DESKTOP_HOOKS
        watching = true; failAllocation = scenario == "request-oom" ? 1 : scenario == "queue-oom" ? 2 : 0;
#endif
        const bool accepted = scenario == "executed" || scenario == "cancelled" || scenario == "destroyed";
        const uint8_t key[32] = {42}, zero[32] = {};
        const uint8_t* group = action == GUI_RADIO_CALL_START ? key : nullptr;
        const uint8_t* peer = group ? key : nullptr;
        if (form) form->onRadioCallRequested(QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(key), 32).toHex()), QByteArray(reinterpret_cast<const char*>(key), sizeof(key)));
        else QCOMPARE(gui_bridge_radio_call(static_cast<gui_radio_call_action>(action), 73, group, peer), accepted ? 0 : -1);
        QCOMPARE(pending(core.ua), accepted ? 1u : 0u);
        if (scenario == "destroyed") form.reset();
        const bool executes = scenario == "executed" || scenario == "destroyed";
        if (executes) core.poll();
        else gui_bridge_set_inst(nullptr);
        QCOMPARE(pending(core.ua), 0u); QVERIFY(bindingValid);
        QCOMPARE(core.results, executes && action == GUI_RADIO_CALL_START ? 1u : 0u); QVERIFY(core.resultValid);
#ifdef UTUN_TEST_DESKTOP_HOOKS
        QCOMPARE(failAllocation.load(), 0); QCOMPARE(observed.size(), executes ? 1 : 0);
        if (executes) {
            static const int nativeActions[]{RADIO_CALL_START, RADIO_CALL_READY, RADIO_CALL_ACCEPT, RADIO_CALL_DECLINE,
                RADIO_CALL_CANCEL, RADIO_CALL_FAIL, RADIO_CALL_REJECT_BUSY, RADIO_CALL_CONTINUE};
            const auto& request = observed.front(); QCOMPARE(request.inst, core.inst);
            QCOMPARE(request.action, nativeActions[action]); QVERIFY(!memcmp(request.group, group ? group : zero, 32)); QVERIFY(!memcmp(request.peer, peer ? peer : zero, sizeof(request.peer)));
            if (window) QVERIFY(request.id);
            else QCOMPARE(request.id, uint64_t(73));
        }
#endif
        DEBUG_INFO(DEBUG_CATEGORY_RADIO, "Radio command verified: window=%d action=%d scenario=%s results=%u",
                   window, action, qPrintable(scenario), core.results);
    }
    void invalidArguments() {
        DesktopCore core; QVERIFY(core.start()); const auto baseline = u_get_allocated_count();
        QCOMPARE(gui_bridge_radio_call(static_cast<gui_radio_call_action>(99), 1, 0, 0), -1);
        const uint8_t key[32] = {42};
        QCOMPARE(gui_bridge_radio_call(GUI_RADIO_CALL_START, 0, key, key), -1);
        QCOMPARE(gui_bridge_radio_call(GUI_RADIO_CALL_START, 1, 0, key), -1);
        QCOMPARE(gui_bridge_radio_call(GUI_RADIO_CALL_START, 1, key, 0), -1);
        QCOMPARE(gui_bridge_radio_call(GUI_RADIO_CALL_CANCEL, 0, 0, 0), -1);
        QCOMPARE(gui_bridge_radio_call(GUI_RADIO_CALL_CANCEL, 1, key, 0), -1);
        QCOMPARE(gui_bridge_radio_call(GUI_RADIO_CALL_CANCEL, 1, 0, key), -1);
        QCOMPARE(pending(core.ua), 0u); QCOMPARE(u_get_allocated_count(), baseline);
    }
    void concurrentRebind() {
        DesktopCore first, second; QVERIFY(first.start()); gui_bridge_set_inst(nullptr); QVERIFY(second.start());
#ifdef UTUN_TEST_DESKTOP_HOOKS
        watching = true;
#endif
        std::atomic<unsigned> turn{0}, submitted{0};
        std::thread producer([&] {
            for (unsigned id = 1; id <= 96; ++id) {
                while (turn < id) std::this_thread::yield();
                gui_bridge_radio_call(GUI_RADIO_CALL_CANCEL, id, 0, 0); submitted = id;
            }
        });
        bool controlsAccepted = true;
        for (unsigned i = 1; i <= 96; ++i) {
            turn = i;
            gui_bridge_set_inst(nullptr); auto& core = i % 2 ? first : second; core.attach();
            while (submitted < i) std::this_thread::yield();
            first.poll(); second.poll();
            // Контроль исполняется у каждого владельца даже при отклонении/отмене конкурирующей команды.
            controlsAccepted &= gui_bridge_radio_call(GUI_RADIO_CALL_CANCEL, 1000 + i, 0, 0) == 0; core.poll();
        }
        producer.join(); gui_bridge_set_inst(nullptr); first.attach();
        const uint8_t key[32] = {42};
        QCOMPARE(gui_bridge_radio_call(GUI_RADIO_CALL_START, 2000, key, key), 0); first.poll();
        QVERIFY(controlsAccepted); QVERIFY(bindingValid); QCOMPARE(pending(first.ua), 0u); QCOMPARE(pending(second.ua), 0u);
        QCOMPARE(first.results, 1u); QCOMPARE(second.results, 0u); QVERIFY(first.resultValid);
#ifdef UTUN_TEST_DESKTOP_HOOKS
        size_t firstCount = 0, secondCount = 0;
        for (const auto& request : observed) { firstCount += request.inst == first.inst; secondCount += request.inst == second.inst; }
        QVERIFY(firstCount >= 49); QVERIFY(secondCount >= 48);
        QCOMPARE(observed.back().inst, first.inst); QCOMPARE(observed.back().id, uint64_t(2000));
        DEBUG_INFO(DEBUG_CATEGORY_RADIO, "Radio binding verified: first_executed=%zu second_executed=%zu binding_valid=%d",
                   firstCount, secondCount, bindingValid);
#endif
    }
};

int main(int argc, char** argv) {
#ifdef HAVE_STREAM_CAPTURE
    if (argc >= 4 && QByteArray(argv[1]) == "--video-capture-worker") {
        QGuiApplication app(argc,argv); debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
        return video_capture_worker_run(QString::fromLocal8Bit(argv[2]),std::make_unique<StreamCaptureFixture>("camera"),true);
    }
#endif
    QApplication app(argc,argv); DesktopCommandsTest test; return QTest::qExec(&test,argc,argv);
}
#include "test_desktop_commands.moc"

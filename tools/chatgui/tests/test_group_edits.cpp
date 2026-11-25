/* Настоящие MainWindow/uasync/SQLite: контекстное меню, composer и обновление существующей строки/цитаты. */
#include "../src/mainwindow.h"
#include "../src/kanbanview.h"
#include "../src/kanban_card_list.h"
#include "../src/messagelist.h"
#include "../src/messagedelegate.h"
#include "../src/chatview.h"
#include "../src/inputbar.h"
#include "../src/soundsettingspage.h"
#include "../src/channelsettingsdialog.h"
#include "../src/animtimer.h"
#include "../db/db_manager.h"
#include "../transport/utun_node.h"
#include "../transport/gui_bridge.h"
#include "clipboard_fixture.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QTextEdit>
#include <QMenu>
#include <QSlider>
#include <QCheckBox>
#include <QScopeGuard>
#include <QPointer>
#include <QClipboard>
#include <QImage>
#include <QPainter>
#include <QLabel>
#include <QScrollBar>
#include <QJsonArray>
#include <QJsonDocument>
#include <QDir>
#include <QRadioButton>
#include <QPushButton>
#include <QToolButton>
#include <QScrollArea>
#include <QWheelEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QListWidget>
#include <QLineEdit>
#include <QEnterEvent>
#include <QWindow>
#include <future>
#include <memory>
#include <atomic>
extern "C" {
#include "utun_instance.h"
#include "chat/chat_core.h"
#include "chat/chat_channel.h"
#include "chat/chat_poll.h"
#include "kanban/kanban.h"
#include "chat/chat_event.h"
#include "dm/dm_core.h"
#include "dm/dm_crypto.h"
#include "debug_config.h"
#include "mem.h"
}

static QPointer<MessageList> sendHistory;
#ifdef Q_OS_LINUX
static std::atomic<int> sendAllocationFailure{0}; // 1=text request, 2=запись очереди моста.
extern "C" void* __real_u_malloc_impl(uint32_t, const char*);
extern "C" void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
extern "C" void* __wrap_u_malloc_impl(uint32_t bytes, const char* where) {
    int expected = 1;
    if (strstr(where, "/messagelist.cpp:") && sendAllocationFailure.compare_exchange_strong(expected, 0)) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "[INJECT] text request allocation failure"); return nullptr;
    }
    return __real_u_malloc_impl(bytes, where);
}
extern "C" void* __wrap_u_calloc_impl(uint32_t count, uint32_t bytes, const char* where) {
    int expected = 2;
    if (strstr(where, "/gui_bridge_impl.cpp:") && sendAllocationFailure.compare_exchange_strong(expected, 0)) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "[INJECT] submission queue allocation failure"); return nullptr;
    }
    return __real_u_calloc_impl(count, bytes, where);
}
#endif

/* Детерминированный owner uasync: ядро выполняется только при pump, реальные виджеты получают события через мост. */
struct SendCore {
    QTemporaryDir databaseDirectory;
    UASYNC* ua = uasync_create();
    UTUN_INSTANCE* instance = nullptr;
    DbManager db;
    QPointer<MessageList> history;
    QString channel;
    QByteArray original;
    ~SendCore() {
#ifdef Q_OS_LINUX
        sendAllocationFailure = 0;
#endif
        delete history.data(); sendHistory.clear(); gui_bridge_set_inst(nullptr); gui_bridge_set_uasync(nullptr);
        gui_bridge_set_msg_submitted_cb(nullptr); db.setDb(nullptr);
        if (ua) uasync_mark_stopped(ua);
        if (instance) utun_instance_destroy(instance);
        if (ua) uasync_destroy(ua, 0);
        QCoreApplication::processEvents(); AnimTimer::shutdown(); MessageList::cleanupAnimations();
    }
    void attach() { uasync_mark_running(ua); gui_bridge_set_uasync(ua); gui_bridge_set_inst(instance); }
    void pump() { uasync_poll(ua, 0); QCoreApplication::processEvents(); }
    InputBar* input() const { return history->findChild<InputBar*>(); }
    QTextEdit* editor() const { return input()->findChild<QTextEdit*>(); }
    bool send() { return QMetaObject::invokeMethod(input(), "onSendClicked", Qt::DirectConnection); }
    void openHistory() {
        history = new MessageList(&db); sendHistory = history; history->loadChannel(channel);
    }
    bool start(bool reply = false, const QString& dbPath = {}, bool durable = false) {
        debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
        debug_set_category_level(DEBUG_CATEGORY_CHAT, DEBUG_LEVEL_DEBUG);
        debug_set_category_level(DEBUG_CATEGORY_GENERAL, DEBUG_LEVEL_DEBUG); gui_bridge_init();
        if (durable && !databaseDirectory.isValid()) return false;
        const QString path = durable && dbPath.isEmpty() ? databaseDirectory.path() : dbPath;
        const QByteArray config = "[global]\ndb_path=" + path.toUtf8() +
            "\nmy_private_key=704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f\n"
            "my_public_key=7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f\n"
            "tun_enabled=no\n[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\nmember_files_enabled=0\n"
            "local_invites_enabled=0\nlocal_broadcasts_enabled=0\n[ntp]\nenabled=no\n";
        instance = ua ? utun_instance_create_from_str(ua, config.constData()) : nullptr;
        if (!instance || utun_core_start(instance) || chat_service_start(instance)) return false;
        const auto id = chat_core_create_channel_auto(instance, "Send tests"); if (!id) return false;
        channel = QString::fromLatin1(topo_groups_channel(instance->topo_groups, id)); db.setDb(instance->topo_sqlite_db); db.setDbPath(path);
        if (reply) {
            chat_msg_submit request{}; request.inst = instance; strcpy(request.content_type, "text/plain");
            snprintf(request.channel_id, sizeof(request.channel_id), "%s", qPrintable(channel));
            request.data = (uint8_t*)"Original"; request.data_len = 8;
            if (chat_core_submit_message(instance, &request)) return false;
            original = QByteArray((const char*)request.message_id, 32);
        }
        chat_event_set_handler(instance, [](UTUN_INSTANCE*, int type, const uint8_t* data, int len) { gui_bridge_post(type, data, len); });
        gui_bridge_set_msg_submitted_cb([](const uint8_t* data, int len) { if (sendHistory) sendHistory->onMessageSubmitted(data, len); });
        attach();
        if (durable) db.setAsyncReader(gui_bridge_db_read);
        openHistory(); pump();
        if (reply) input()->setReply(original, "Member", "Original");
        return true;
    }
};

static int newMessages;
static int inCore(std::function<int(UTUN_INSTANCE*)> operation) {
    auto* task = new std::packaged_task<int()>([operation] { return operation(gui_bridge_get_inst()); });
    auto result = task->get_future();
    gui_bridge_post_uasync_fn([](void* arg) {
        std::unique_ptr<std::packaged_task<int()>> work(static_cast<std::packaged_task<int()>*>(arg)); (*work)();
    }, task);
    if (result.wait_for(std::chrono::seconds(5)) != std::future_status::ready) qFatal("Edit test: core task timed out");
    return result.get();
}

template<class Result>
static Result inCoreAsync(std::function<void(UTUN_INSTANCE*, void (*)(void*, const Result*), void*)> operation) {
    std::promise<Result> completion;
    auto result = completion.get_future();
    inCore([&](UTUN_INSTANCE* inst) {
        operation(inst, [](void* arg, const Result* outcome) { static_cast<std::promise<Result>*>(arg)->set_value(*outcome); }, &completion);
        return 0;
    });
    if (result.wait_for(std::chrono::seconds(5)) != std::future_status::ready) qFatal("Edit test: database task timed out");
    return result.get();
}

class GroupEditTest : public QObject {
    Q_OBJECT
private slots:
    void kanbanVideoButtonPinsSelectedCard() {
        SendCore core; QVERIFY(core.start()); QVERIFY(!kanban_service_start(core.instance));
        debug_set_category_level(DEBUG_CATEGORY_KANBAN, DEBUG_LEVEL_INFO);
        quint64 id = group_core_create_auto(core.instance, "Video board", "kanban"); QVERIFY(id);
        QString group = QString::fromLatin1(topo_groups_channel(core.instance->topo_groups, id)); char* result = nullptr;
        QVERIFY(!kanban_snapshot(core.instance,qPrintable(group),&result));
        auto columns = QJsonDocument::fromJson(result).object()["columns"].toArray(); u_free(result); QVERIFY(!columns.isEmpty());
        QStringList cards;
        for (const QString& title : {QStringLiteral("Original card"),QStringLiteral("Other card")}) {
            auto command = QJsonDocument(QJsonObject{{"action","card_create"},{"column",columns[0].toObject()["id"]},{"text",title}})
                .toJson(QJsonDocument::Compact);
            QVERIFY(!kanban_command(core.instance,qPrintable(group),command.constData(),&result));
            cards.append(QJsonDocument::fromJson(result).object()["id"].toString()); u_free(result);
        }
        KanbanView view(nullptr,&core.db); view.resize(1100,600); view.show(); view.setGroup(group); view.setVideoRecordingEnabled(true);
        auto pump = [&] { for (int i = 0; i < 12; ++i) core.pump(); }; pump();
        QListWidget* list = nullptr;
        for (auto* candidate : view.findChildren<QListWidget*>()) if (candidate->count() == 2) list = candidate;
        QVERIFY(list);
        auto selectCard = [&](const QString& card) {
            for (int i = 0; i < list->count(); ++i) if (list->item(i)->data(Qt::UserRole).toString() == card) list->setCurrentItem(list->item(i));
            pump();
        };
        selectCard(cards[0]);
        auto* composer = view.detailsPanel()->findChild<InputBar*>("kanbanComposer"); QVERIFY(composer);
        auto* button = composer->findChild<QPushButton*>("recordVideoButton"); QVERIFY(button && button->isEnabled());
        QSignalSpy requested(&view,&KanbanView::videoRecordingRequested); button->click(); QCOMPARE(requested.size(),1);
        QCOMPARE(requested[0][0].toString(),group); QCOMPARE(requested[0][1].toString(),cards[0]);
        QCOMPARE(requested[0][2].toString(),QString("Original card"));
        selectCard(cards[1]); QCOMPARE(requested[0][1].toString(),cards[0]);
        button->click(); QCOMPARE(requested.size(),2); QCOMPARE(requested[1][1].toString(),cards[1]);
        view.setVideoRecordingEnabled(false); QVERIFY(!button->isEnabled());
    }
    void kanbanPaletteKeepsCursorWithWindowMouseEvents() {
        SendCore core; QVERIFY(core.start()); QVERIFY(!kanban_service_start(core.instance));
        debug_set_category_level(DEBUG_CATEGORY_KANBAN, DEBUG_LEVEL_DEBUG);
        quint64 id = group_core_create_auto(core.instance, "Palette focus", "kanban"); QVERIFY(id);
        KanbanView view; view.resize(1100, 600); view.show(); view.setGroup(QString::fromLatin1(topo_groups_channel(core.instance->topo_groups, id)));
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        view.windowHandle()->requestActivate(); QVERIFY(QTest::qWaitForWindowActive(&view));
        auto pump = [&] { for (int i = 0; i < 12; i++) core.pump(); }; pump();
        auto* list = dynamic_cast<CardList*>(view.findChildren<QListWidget*>().first()); QVERIFY(list);
        QTest::mouseClick(view.windowHandle(), Qt::LeftButton, Qt::NoModifier, list->viewport()->mapTo(&view, list->newCardRect().center()));
        pump(); auto* text = view.findChild<QTextEdit*>("kanbanInlineText"); QVERIFY(text && text->hasFocus());
        text->setPlainText("Keep editing"); QTextCursor cursor = text->textCursor(); cursor.setPosition(4); text->setTextCursor(cursor);
        auto* inlineCard = view.findChild<QWidget*>("kanbanInlineEditor"); auto* bubble = inlineCard->findChild<QWidget*>("kanbanEditBubble");
        auto* blue = view.findChild<QPushButton*>("kanbanColor_8"); QVERIFY(blue && blue->isVisible());
        QRect blueRect(blue->mapTo(inlineCard, QPoint()), blue->size());
        QVERIFY(blueRect.top() > kanbanBubble(bubble->geometry()).bottom());
        QVERIFY(kanbanSelection(kanbanBubble(inlineCard->rect())).contains(blueRect));
        QPointer<QTextEdit> retained(text);
        QTest::mousePress(view.windowHandle(), Qt::LeftButton, Qt::NoModifier, blue->mapTo(&view, blue->rect().center()));
        pump(); QVERIFY(retained && retained->isVisible()); QVERIFY(!retained->isReadOnly());
        QTest::mouseRelease(view.windowHandle(), Qt::LeftButton, Qt::NoModifier, blue->mapTo(&view, blue->rect().center()));
        pump(); QVERIFY(retained && retained->isVisible()); QVERIFY(blue->isChecked()); QVERIFY(retained->hasFocus());
        QCOMPARE(retained->textCursor().position(), 4);
        for (int position : {1, 8, 3}) {
            QPoint point = retained->viewport()->mapTo(&view, QPoint(retained->fontMetrics().horizontalAdvance(retained->toPlainText().left(position)),
                retained->fontMetrics().height() / 2));
            QTest::mousePress(view.windowHandle(), Qt::LeftButton, Qt::NoModifier, point); pump();
            QVERIFY(retained && retained->isVisible() && !retained->isReadOnly());
            QTest::mouseRelease(view.windowHandle(), Qt::LeftButton, Qt::NoModifier, point); pump();
            QVERIFY(retained && retained->isVisible() && retained->hasFocus()); QCOMPARE(retained->textCursor().position(), position);
        }
        QTest::keyClicks(retained, "!"); QCOMPARE(retained->toPlainText(), QStringLiteral("Kee!p editing"));
        char* board = nullptr; QVERIFY(!kanban_snapshot(core.instance, qPrintable(QString::fromLatin1(topo_groups_channel(core.instance->topo_groups, id))), &board));
        QCOMPARE(QJsonDocument::fromJson(board).object()["cards"].toArray().size(), 0); u_free(board);
        if (qEnvironmentVariableIsSet("UTUN_KANBAN_PREVIEW")) QVERIFY(view.grab().save(qEnvironmentVariable("UTUN_KANBAN_PREVIEW") + ".palette.png"));
        QTest::mouseClick(view.windowHandle(), Qt::LeftButton, Qt::NoModifier, QPoint(view.width() - 20, view.height() - 20)); pump();
        QVERIFY(!retained || !retained->isVisible());
        QVERIFY(!kanban_snapshot(core.instance, qPrintable(QString::fromLatin1(topo_groups_channel(core.instance->topo_groups, id))), &board));
        auto cards = QJsonDocument::fromJson(board).object()["cards"].toArray(); u_free(board); QCOMPARE(cards.size(), 1);
        QCOMPARE(cards[0].toObject()["color"].toString(), QStringLiteral("#547ddb"));
        QCOMPARE(cards[0].toObject()["text"].toString(), QStringLiteral("Kee!p editing"));
    }
    void kanbanInlineSavesOnFocusLossAndKeepsDraftOnFailure() {
        SendCore core; QVERIFY(core.start()); QVERIFY(!kanban_service_start(core.instance));
        quint64 id = group_core_create_auto(core.instance, "Board", "kanban"); QVERIFY(id);
        KanbanView view; view.resize(1100, 600); view.show(); view.setGroup(QString::fromLatin1(topo_groups_channel(core.instance->topo_groups, id)));
        auto pump = [&] { for (int i = 0; i < 12; i++) core.pump(); };
        auto textEditor = [&]() -> QTextEdit* {
            for (auto* text : view.findChildren<QTextEdit*>("kanbanInlineText")) if (text->isVisible()) return text;
            return nullptr;
        };
        auto count = [&]() {
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(core.instance->topo_sqlite_db, "SELECT count(*) FROM kanban_ops WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1)", -1, &st, nullptr) != SQLITE_OK) return -1;
            sqlite3_bind_text(st, 1, qPrintable(QString::fromLatin1(topo_groups_channel(core.instance->topo_groups, id))), -1, SQLITE_TRANSIENT);
            int rows = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int(st, 0) : -1; sqlite3_finalize(st); return rows;
        };
        pump(); QCOMPARE(view.findChildren<QListWidget*>().size(), 3);
        auto* list = dynamic_cast<CardList*>(view.findChildren<QListWidget*>().first()); QVERIFY(list);
        QPoint cue = list->newCardRect().center(); QTest::mouseMove(list->viewport(), cue); QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, cue);
        pump(); auto* text = textEditor(); QVERIFY(text && text->hasFocus()); QCOMPARE(count(), 0);
        text->setPlainText("0123456789"); pump(); QCOMPARE(count(), 0);
        auto* inlineCard = view.findChild<QWidget*>("kanbanInlineEditor"); auto* palette = inlineCard->findChild<QWidget*>("kanbanPalette"); QVERIFY(palette);
        QCOMPARE(palette->findChildren<QPushButton*>().size(), 12);
        auto* blue = palette->findChild<QPushButton*>("kanbanColor_8"); QVERIFY(blue); QTest::mouseClick(blue, Qt::LeftButton); pump();
        QVERIFY(blue->isChecked()); QCOMPARE(count(), 0); QVERIFY(textEditor() == text);
        if (qEnvironmentVariableIsSet("UTUN_KANBAN_PREVIEW")) QVERIFY(view.grab().save(qEnvironmentVariable("UTUN_KANBAN_PREVIEW") + ".editing.png"));
        view.setFocus(); pump(); QVERIFY(!textEditor()); QCOMPARE(count(), 1);
        list = dynamic_cast<CardList*>(view.findChildren<QListWidget*>().first()); QVERIFY(list && list->count() == 1);
        int normalHeight = list->visualItemRect(list->item(0)).height();
        QPoint point = list->visualItemRect(list->item(0)).topLeft() + QPoint(16 + list->fontMetrics().horizontalAdvance("01234") + 1, 13 + list->fontMetrics().height() / 2);
        QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, point); pump(); text = textEditor(); QVERIFY(text);
        QCOMPARE(text->textCursor().position(), 5); QVERIFY(list->visualItemRect(list->item(0)).height() > normalHeight);
        text->setPlainText("Edited draft"); inlineCard = view.findChild<QWidget*>("kanbanInlineEditor"); palette = inlineCard->findChild<QWidget*>("kanbanPalette");
        blue = palette->findChild<QPushButton*>("kanbanColor_8"); QVERIFY(blue && blue->isChecked());
        QVERIFY(sqlite3_exec(core.instance->topo_sqlite_db, "CREATE TEMP TRIGGER reject_board_edit BEFORE INSERT ON kanban_ops"
            " WHEN NEW.action=2 BEGIN SELECT RAISE(ABORT,'injected editor failure'); END", nullptr, nullptr, nullptr) == SQLITE_OK);
        view.setFocus(); pump(); QVERIFY(textEditor() == text); QVERIFY(!text->isReadOnly()); QCOMPARE(text->toPlainText(), QStringLiteral("Edited draft"));
        QVERIFY(blue->isChecked() && blue->isEnabled()); auto* error = inlineCard->findChild<QLabel*>("kanbanEditError"); QVERIFY(error && !error->text().isEmpty());
        QCOMPARE(count(), 1); QVERIFY(sqlite3_exec(core.instance->topo_sqlite_db, "DROP TRIGGER reject_board_edit", nullptr, nullptr, nullptr) == SQLITE_OK);
        text->setFocus(); view.setFocus(); pump(); QVERIFY(!textEditor()); QCOMPARE(count(), 2);
        char* board = nullptr; QVERIFY(!kanban_snapshot(core.instance, qPrintable(QString::fromLatin1(topo_groups_channel(core.instance->topo_groups, id))), &board));
        QVERIFY(board && strstr(board, "Edited draft") && strstr(board, "\"color\":\"#547ddb\"")); u_free(board);
        list = dynamic_cast<CardList*>(view.findChildren<QListWidget*>().first());
        QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, list->newCardRect().center()); pump(); QVERIFY(textEditor());
        view.setFocus(); pump(); QCOMPARE(count(), 2); QVERIFY(!textEditor());
        list = dynamic_cast<CardList*>(view.findChildren<QListWidget*>().first());
        QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, list->visualItemRect(list->item(0)).center()); pump();
        text = textEditor(); QVERIFY(text); text->setPlainText("Saved before shutdown");
        view.finishEditing(); pump(); QCOMPARE(count(), 3); QVERIFY(!textEditor());
        if (qEnvironmentVariableIsSet("UTUN_KANBAN_PREVIEW")) QVERIFY(view.grab().save(qEnvironmentVariable("UTUN_KANBAN_PREVIEW")));
    }
    void kanbanInlineSwitchesCardsAfterPressWhileSaveIsPending() {
        SendCore core; QVERIFY(core.start()); QVERIFY(!kanban_service_start(core.instance));
        quint64 id = group_core_create_auto(core.instance, "Switch editors", "kanban"); QVERIFY(id);
        KanbanView view; view.resize(1100, 600); view.show(); view.setGroup(QString::fromLatin1(topo_groups_channel(core.instance->topo_groups, id)));
        auto pump = [&] { for (int i = 0; i < 12; i++) core.pump(); }; pump();
        auto lists = view.findChildren<QListWidget*>(); auto* first = dynamic_cast<CardList*>(lists[0]); auto* second = dynamic_cast<CardList*>(lists[1]);
        QVERIFY(first && second); QTest::mouseClick(first->viewport(), Qt::LeftButton, Qt::NoModifier, first->newCardRect().center()); pump();
        QTextEdit* text = nullptr; for (auto* edit : view.findChildren<QTextEdit*>("kanbanInlineText")) if (edit->isVisible()) text = edit;
        QVERIFY(text); text->setPlainText("First card");
        second = dynamic_cast<CardList*>(view.findChildren<QListWidget*>()[1]); QPointer<CardList> retained(second); QPoint target = second->newCardRect().center();
        QTest::mousePress(second->viewport(), Qt::LeftButton, Qt::NoModifier, target); pump(); QVERIFY(retained);
        QTest::mouseRelease(second->viewport(), Qt::LeftButton, Qt::NoModifier, target); pump();
        text = nullptr; for (auto* edit : view.findChildren<QTextEdit*>("kanbanInlineText")) if (edit->isVisible()) text = edit;
        QVERIFY(text); text->setPlainText("Second card"); view.setFocus(); pump();
        char* board = nullptr; QVERIFY(!kanban_snapshot(core.instance, qPrintable(QString::fromLatin1(topo_groups_channel(core.instance->topo_groups, id))), &board));
        QVERIFY(board && strstr(board, "First card") && strstr(board, "Second card")); u_free(board);
    }
    void kanbanColumnWidthsAreLocalAndPersistent() {
        SendCore core; QVERIFY(core.start()); QVERIFY(!kanban_service_start(core.instance));
        quint64 id = group_core_create_auto(core.instance, "Widths", "kanban"); QVERIFY(id);
        auto view = std::make_unique<KanbanView>(nullptr, &core.db); view->resize(1100, 600); view->show(); view->setGroup(QString::fromLatin1(topo_groups_channel(core.instance->topo_groups, id)));
        auto pump = [&] { for (int i = 0; i < 12; i++) core.pump(); }; pump();
        auto* grip = view->findChild<QWidget*>("kanbanColumnGrip"); QVERIFY(grip);
        QList<QWidget*> panes;
        for (auto* widget : view->findChildren<QWidget*>()) if (widget->objectName().startsWith("kanbanColumn_")) panes.append(widget);
        QCOMPARE(panes.size(), 3); QWidget* pane = panes.front(); int initial = pane->width(); QString name = pane->objectName();
        auto point = grip->rect().center(); QTest::mousePress(grip, Qt::LeftButton, Qt::NoModifier, point);
        QTest::mouseMove(grip, point + QPoint(80, 0)); QTest::mouseRelease(grip, Qt::LeftButton, Qt::NoModifier, point); pump();
        QCOMPARE(pane->width(), initial + 80);
        view.reset(); view = std::make_unique<KanbanView>(nullptr, &core.db); view->setGroup(QString::fromLatin1(topo_groups_channel(core.instance->topo_groups, id))); pump();
        auto* restored = view->findChild<QWidget*>(name); QVERIFY(restored); QCOMPARE(restored->width(), initial + 80);
        sqlite3_stmt* st = nullptr;
        QCOMPARE(sqlite3_prepare_v2(core.instance->topo_sqlite_db, "SELECT count(*) FROM kanban_ops WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1)", -1, &st, nullptr), SQLITE_OK);
        QCOMPARE(sqlite3_bind_text(st, 1, qPrintable(QString::fromLatin1(topo_groups_channel(core.instance->topo_groups, id))), -1, SQLITE_TRANSIENT), SQLITE_OK);
        QCOMPARE(sqlite3_step(st), SQLITE_ROW); QCOMPARE(sqlite3_column_int(st, 0), 0); QCOMPARE(sqlite3_finalize(st), SQLITE_OK);
        grip = view->findChild<QWidget*>("kanbanColumnGrip"); QVERIFY(grip);
        QTest::mousePress(grip, Qt::LeftButton, Qt::NoModifier, grip->rect().center()); view.reset(); pump();
    }
    void streamButtonExplicitListAndClickOpensAll() {
        SendCore core; QVERIFY(core.start()); auto* history = core.history.data();
        debug_set_category_level(DEBUG_CATEGORY_STREAM, DEBUG_LEVEL_DEBUG);
        history->resize(800, 420); history->show(); QApplication::processEvents();
        auto* button = history->findChild<QPushButton*>("streamButton");
        auto* menu = history->findChild<QMenu*>("streamSourceMenu"); QVERIFY(button && menu && button->isHidden());
        auto snapshot = [&](const QJsonArray& sources) {
            QByteArray event = QByteArray::fromHex(core.channel.toLatin1()); event.append(char(1));
            event.append(QJsonDocument(sources).toJson(QJsonDocument::Compact));
            history->onStreamSources(reinterpret_cast<const uint8_t*>(event.constData()), event.size());
        };
        const QByteArray aliceKey(32, '\x11'), bobKey(32, '\x12'), selfKey(32, '\x13');
        const QJsonArray sources{
            QJsonObject{{"id", QString::fromLatin1(aliceKey.toHex())}, {"epoch", "a1"}, {"name", "Alice"}},
            QJsonObject{{"id", QString::fromLatin1(bobKey.toHex())}, {"epoch", "b2"}, {"name", "Bob"}},
            QJsonObject{{"id", QString::fromLatin1(selfKey.toHex())}, {"epoch", "c3"}, {"name", "Self"}, {"self", 1}}};
        snapshot(sources); QApplication::processEvents(); QVERIFY(button->isVisible());
        QSignalSpy all(history, &MessageList::allStreamsRequested), one(history, &MessageList::streamRequested);
        const QPoint point = button->rect().center();
        QEnterEvent enter(point, button->mapTo(history->window(), point), button->mapToGlobal(point));
        QApplication::sendEvent(button, &enter); QTest::qWait(550);
        QVERIFY(!menu->isVisible()); QCOMPARE(all.size(), 0); QCOMPARE(one.size(), 0);
        auto openList = [&] { QTest::mouseClick(button, Qt::RightButton); };
        openList(); QTRY_VERIFY(menu->isVisible()); QCOMPARE(menu->actions().size(), 2);
        QCOMPARE(all.size(), 0); QCOMPARE(one.size(), 0);
        QVERIFY(menu->pos().y() >= button->mapToGlobal(QPoint(0, button->height())).y());
        auto* alice = menu->actions().front(); QVERIFY(alice->text().contains("Alice"));
        QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier, menu->actionGeometry(alice).center());
        QCOMPARE(one.size(), 1); QCOMPARE(one.last()[0].toString(), core.channel);
        QCOMPARE(one.last()[1].toByteArray(), aliceKey); QCOMPARE(one.last()[2].toULongLong(), quint64(0xa1));
        QCOMPARE(all.size(), 0); QVERIFY(!menu->isVisible());
        openList(); QTRY_VERIFY(menu->isVisible());
        // Открытое меню удерживает мышь: нажатие над самой кнопкой всё равно открывает все, ровно один раз.
        QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier, menu->mapFromGlobal(button->mapToGlobal(button->rect().center())));
        QCOMPARE(all.size(), 1); QCOMPARE(all.last()[0].toString(), core.channel);
        QCOMPARE(one.size(), 1); QVERIFY(!menu->isVisible());
        QTest::mouseClick(button, Qt::LeftButton); QCOMPARE(all.size(), 2);
        QTest::mouseClick(button, Qt::RightButton); QTRY_VERIFY(menu->isVisible()); QCOMPARE(all.size(), 2);
        const QPoint anchor = menu->mapFromGlobal(button->mapToGlobal(button->rect().center()));
        QTest::mouseClick(menu, Qt::RightButton, Qt::NoModifier, anchor); QVERIFY(menu->isVisible()); QCOMPARE(all.size(), 2);
        QTest::keyClick(menu, Qt::Key_Escape);
        // Сенсорное удержание: первое отпускание не выбирает пункт и не открывает всё.
        QTest::mousePress(button, Qt::LeftButton); QTest::qWait(550); QTRY_VERIFY(menu->isVisible());
        QTest::mouseRelease(menu, Qt::LeftButton, Qt::NoModifier, anchor);
        QCOMPARE(all.size(), 2); QCOMPARE(one.size(), 1); QVERIFY(menu->isVisible() && !button->isDown());
        // То же удержание, когда список уже открыт и мышью владеет popup.
        QTest::mousePress(menu, Qt::LeftButton, Qt::NoModifier, anchor); QTest::qWait(550);
        QTest::mouseRelease(menu, Qt::LeftButton, Qt::NoModifier, anchor);
        QCOMPARE(all.size(), 2); QVERIFY(menu->isVisible());
        QTest::keyClick(menu, Qt::Key_Escape);
        QTest::keyClick(button, Qt::Key_Down); QTRY_VERIFY(menu->isVisible());
        QTest::keyClick(menu, Qt::Key_Escape); QVERIFY(!menu->isVisible());
        openList(); QTRY_VERIFY(menu->isVisible()); snapshot(sources); QVERIFY(menu->isVisible());
        auto* oldSelection = menu->actions().front();
        snapshot(QJsonArray{sources[1]}); QVERIFY(!menu->isVisible());
        oldSelection->trigger(); QCOMPARE(one.size(), 1); // Источник, исчезнувший из свежего снимка, не открывается.
        openList(); QTRY_VERIFY(menu->isVisible()); QCOMPARE(menu->actions().size(), 1);
        oldSelection = menu->actions().front(); history->loadChannel(QString());
        QVERIFY(!menu->isVisible() && button->isHidden()); oldSelection->trigger(); QCOMPARE(one.size(), 1);
    }
    void headerActionsStartManagePollAndOpenOwnFiles() {
        SendCore core; QVERIFY(core.start()); auto* history = core.history.data();
        history->resize(700, 420); history->show(); QApplication::processEvents();
        auto* publish = history->findChild<QPushButton*>("streamPublishButton");
        auto* radio = history->findChild<QPushButton*>("radioButton");
        auto* poll = history->findChild<QPushButton*>("createPollButton");
        auto* ownFiles = history->findChild<QPushButton*>("ownFilesButton");
        QVERIFY(publish && radio && poll && ownFiles);
        for (auto* button : {publish, radio, poll, ownFiles}) { QVERIFY(button->isVisible()); QVERIFY(!button->icon().isNull()); }
        QVERIFY(publish->geometry().right() < radio->geometry().left());
        QVERIFY(radio->geometry().right() < poll->geometry().left());
        QVERIFY(poll->geometry().right() < ownFiles->geometry().left());
        QSignalSpy streams(history, &MessageList::streamPublishRequested), files(history, &MessageList::ownFilesRequested);
        const auto normal = publish->icon().pixmap(24).toImage();
        QTest::mouseClick(publish, Qt::LeftButton); QCOMPARE(streams.size(), 1);
        QCOMPARE(streams.last()[0].toString(), core.channel);
        // Своя трансляция может идти в другом канале: кнопка открывает именно её управление.
        const QString activeGroup(64, 'd'); history->setStreamPublisherGroup(activeGroup);
        const auto active = publish->icon().pixmap(24).toImage(); QVERIFY(active != normal);
        auto* viewers = publish->findChild<QLabel*>("streamViewerBadge"); QVERIFY(viewers); QVERIFY(viewers->isHidden());
        history->setStreamPublisherViewers(activeGroup, 1); QVERIFY(viewers->isVisible()); QCOMPARE(viewers->text(), QStringLiteral("1"));
        const int oneDigitWidth = viewers->width();
        history->setStreamPublisherViewers(activeGroup, 123); QCOMPARE(viewers->text(), QStringLiteral("123"));
        QVERIFY(viewers->width() > oneDigitWidth); QVERIFY(publish->rect().contains(viewers->geometry()));
        history->setStreamPublisherGroup(activeGroup); QCOMPARE(viewers->text(), QStringLiteral("123"));
        history->setStreamPublisherViewers(core.channel, 5); QCOMPARE(viewers->text(), QStringLiteral("123"));
        const QString screenshot = qEnvironmentVariable("UTUN_TEST_STREAM_VIEWERS_SCREENSHOT");
        if (!screenshot.isEmpty()) QVERIFY(publish->grab().save(screenshot));
        QTest::mouseClick(publish, Qt::LeftButton); QCOMPARE(streams.size(), 2);
        QCOMPARE(streams.last()[0].toString(), activeGroup);
        QTest::mouseClick(publish, Qt::LeftButton, Qt::NoModifier, viewers->geometry().center()); QCOMPARE(streams.size(), 3);
        history->setStreamPublisherViewers(activeGroup, 0); QVERIFY(viewers->isHidden());
        history->setStreamPublisherViewers(activeGroup, 5); QVERIFY(viewers->isVisible());
        QPalette palette = history->palette(); palette.setColor(QPalette::ButtonText, QColor("#abcdef")); history->setPalette(palette);
        QCOMPARE(publish->icon().pixmap(24).toImage(), active);
        history->setStreamPublisherGroup({}); QVERIFY(publish->isVisible()); QVERIFY(publish->icon().pixmap(24).toImage() != active);
        QVERIFY(viewers->isHidden());
        QTest::mouseClick(ownFiles, Qt::LeftButton); QCOMPARE(files.size(), 1); QCOMPARE(files.last()[0].toString(), core.channel);
        bool pollOpened = false;
        QTimer::singleShot(0, history, [&] {
            if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
                pollOpened = dialog->windowTitle() == QCoreApplication::translate("ChatGui", "Create poll"); dialog->reject();
            }
        });
        QTest::mouseClick(poll, Qt::LeftButton); QVERIFY(pollOpened);
        history->loadChannel(QString());
        for (auto* button : {publish, poll, ownFiles}) QVERIFY(button->isHidden());
    }
    void newMessageScroll_data() {
        QTest::addColumn<bool>("dm"); QTest::addColumn<QString>("scenario");
        for (bool dm : {false, true}) for (const QString& scenario : {"bottom", "near", "boundary", "outside", "far", "own", "unchanged"})
            QTest::newRow(qPrintable(QString("%1-%2").arg(dm ? "pm" : "group", scenario))) << dm << scenario;
    }
    void newMessageScroll() {
        QFETCH(bool, dm); QFETCH(QString, scenario);
        SendCore core; QVERIFY(core.start(false, {}, !dm));
        auto* history = core.history.data(); history->resize(500, 420); history->show();
        auto* view = history->findChild<ChatView*>(); QVERIFY(view);
        auto* model = qobject_cast<QStandardItemModel*>(view->model()); QVERIFY(model);
        QJsonArray messages; QString conversation; quint64 generation = 0;
        if (dm) {
            SC_MYKEYS peer{}; uint8_t ed[32], seed[32]; QCOMPARE(sc_identity_generate(peer.public_key, peer.private_key, ed, seed), 0);
            QCOMPARE(dm_start(core.instance, sc_derive_node_id_from_pubkey(peer.public_key), peer.public_key, ed,
                "Scroll peer", nullptr, DM_MODE_DEVICE, nullptr), 0);
            const auto conversations = core.db.getDmConversations(); QCOMPARE(conversations.size(), 1);
            conversation = conversations.first().convId; generation = conversations.first().generation;
            history->loadChannel("dm:" + conversation);
        }
        const auto append = [&](int seq, const QString& text, bool outgoing = false) {
            if (dm) {
                messages.append(QJsonObject{{"seq", seq}, {"ts", seq * 1000}, {"dir", outgoing ? 1 : 0},
                    {"author", QString::fromLatin1(QByteArray((const char*)core.instance->my_ed25519_pubkey, 32).toHex())}, {"data", text}, {"content_type", "text/plain"}});
                return true;
            }
            const auto bytes = text.toUtf8(); chat_msg_submit request{};
            snprintf(request.channel_id, sizeof(request.channel_id), "%s", qPrintable(core.channel));
            strcpy(request.content_type, "text/plain"); request.data = (uint8_t*)bytes.constData(); request.data_len = bytes.size();
            request.timestamp = 1000000 + seq * 1000;
            return chat_core_submit_message(core.instance, &request) == 0;
        };
        const auto snapshot = [&] {
            QByteArray packet; packet.append(char(conversation.toUtf8().size())); packet.append(conversation.toUtf8());
            packet.append(reinterpret_cast<const char*>(&generation), sizeof(generation));
            packet.append(QJsonDocument(messages).toJson(QJsonDocument::Compact)); history->onDmMessages((const uint8_t*)packet.constData(), packet.size());
        };
        for (int i = 1; i <= 60; ++i) QVERIFY(append(i, QString("message %1\nsecond line").arg(i)));
        if (dm) snapshot(); else history->loadChannel(core.channel);
        QTest::qWait(30); view->doItemsLayout(); view->scrollToBottom();
        auto* scrollbar = view->verticalScrollBar(); const int line = view->fontMetrics().lineSpacing();
        QVERIFY(scrollbar->maximum() > 30 * line);
        if (!dm && scenario == "own") {
            QScopedValueRollback<debug_level_t> logging(g_debug_config.category_levels[DEBUG_CATEGORY_GENERAL], DEBUG_LEVEL_DEBUG);
        auto* delegate = qobject_cast<MessageDelegate*>(view->itemDelegate()); QVERIFY(delegate);
            delegate->replyRequested(model->index(0, 0).data(MsgMessageKeyRole).toString());
            QCoreApplication::processEvents();
        }
        const int distance = scenario == "bottom" ? 0 : scenario == "boundary" ? 10 * line : scenario == "outside" ? 10 * line + 1 :
            scenario == "far" || scenario == "own" ? 30 * line : 5 * line;
        scrollbar->setValue(scrollbar->maximum() - distance); const int previous = scrollbar->value();
        const QString text = "```cpp\n" + QString("int value = 1;\n").repeated(30) + "```";
        if (scenario != "unchanged") {
            if (!dm && scenario == "own") {
                core.editor()->setPlainText(text); QVERIFY(core.send()); core.pump();
                QCOMPARE(scrollbar->value(), scrollbar->maximum());
            } else QVERIFY(append(61, text, scenario == "own"));
        }
        if (dm) snapshot(); else history->refresh();
        QCoreApplication::processEvents(); view->doItemsLayout();
        const bool follow = scenario != "outside" && scenario != "far" && scenario != "unchanged";
        if (follow) QCOMPARE(scrollbar->value(), scrollbar->maximum());
        else QCOMPARE(scrollbar->value(), previous);
        DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "scroll verified: dm=%d scenario=%s previous=%d current=%d maximum=%d", dm, qPrintable(scenario), previous,
                  scrollbar->value(), scrollbar->maximum());
        // Отложенная прокрутка предыдущего канала не должна сдвинуть новую историю.
        if (scenario == "near") {
            if (dm) { QVERIFY(append(62, "next")); snapshot(); }
            else { QVERIFY(append(62, "next")); history->refresh(); }
            history->loadChannel(QString());
            QCoreApplication::processEvents(); QCOMPARE(model->rowCount(), 0);
        }
    }

    void clipboardImageCommitsThroughCore_data() {
        QTest::addColumn<bool>("dm"); QTest::addColumn<bool>("jpeg"); QTest::addColumn<bool>("manual");
        QTest::newRow("png-group-reply") << false << false << false;
        QTest::newRow("png-pm") << true << false << false;
        QTest::newRow("jpeg-group-reply") << false << true << false;
        QTest::newRow("jpeg-pm") << true << true << false;
        QTest::newRow("manual-jpeg-group") << false << true << true;
        QTest::newRow("manual-png-pm") << true << false << true;
    }
    void clipboardImageCommitsThroughCore() {
        QFETCH(bool, dm); QFETCH(bool, jpeg); QFETCH(bool, manual);
        QTemporaryDir directory; QVERIFY(directory.isValid()); SendCore core;
        QVERIFY(core.start(!dm, directory.path())); QString conversation;
        if (dm) {
            SC_MYKEYS peer{}; uint8_t ed[32], seed[32]; QCOMPARE(sc_identity_generate(peer.public_key, peer.private_key, ed, seed), 0);
            const auto peerId = sc_derive_node_id_from_pubkey(peer.public_key);
            QCOMPARE(dm_start(core.instance, peerId, peer.public_key, ed, "Clipboard peer", nullptr, DM_MODE_DEVICE, nullptr), 0);
            conversation = QString::number(dm_derive_conv_id(core.instance->my_ed25519_pubkey, ed));
            core.history->loadChannel("dm:" + conversation);
        }
        core.history->show();
        auto* thumbnail = core.input()->findChild<QLabel*>("clipboardImageThumbnail"); QVERIFY(thumbnail);
        QSignalSpy errors(core.input(), &InputBar::attachmentRejected);
        const QStringList patterns{"vibechat-clipboard-*.png", "vibechat-clipboard-*.jpg"};
        const auto before = QDir(QDir::tempPath()).entryList(patterns, QDir::Files);
        const auto clear = qScopeGuard([] { QApplication::clipboard()->clear(); });
        const QImage image = clipboardCodecFixture(jpeg != manual ? 64 : 0);
        QApplication::clipboard()->setImage(image); core.editor()->paste(); QTRY_VERIFY(!thumbnail->isHidden());
        auto* choice = core.input()->findChild<QRadioButton*>(jpeg ? "clipboardJpegChoice" : "clipboardPngChoice"); QVERIFY(choice);
        if (manual) QTest::mouseClick(choice, Qt::LeftButton);
        QVERIFY(choice->isChecked());
        gui_bridge_set_inst(nullptr); QVERIFY(core.send()); QCOMPARE(errors.size(), 1); QVERIFY(!thumbnail->isHidden());
        core.attach(); QVERIFY(core.send());
        QTRY_VERIFY_WITH_TIMEOUT((core.pump(), thumbnail->isHidden()), 5000);
        QCOMPARE(errors.size(), 1);
        QCOMPARE(QDir(QDir::tempPath()).entryList(patterns, QDir::Files), before);
        if (dm) {
            char json[8192]; size_t length = 0;
            QCOMPARE(dm_list_messages_json(core.instance, qPrintable(conversation), 20, 0, json, sizeof(json), &length), 0);
            const QByteArray messages(json, int(length));
            QVERIFY(messages.contains(jpeg ? "\"content_type\":\"image/jpeg\"" : "\"content_type\":\"image/png\""));
            QVERIFY(messages.contains("\"width\":320,\"height\":240")); QVERIFY(messages.contains("clipboard-"));
            QVERIFY(messages.contains(jpeg ? ".jpg" : ".png"));
        } else {
            const auto messages = core.db.getMessages(core.channel, 10); QCOMPARE(messages.size(), 2);
            bool found = false;
            for (const auto& message : messages) if (message.contentType == (jpeg ? "image/jpeg" : "image/png")) {
                found = true; QCOMPARE(message.replyTo, core.original);
                const int separator = message.data.indexOf('|'); QVERIFY(separator > 1); QVERIFY(message.data.startsWith('*'));
                const auto name = QByteArray::fromBase64(message.data.mid(1, separator - 1)); QVERIFY(name.startsWith("clipboard-"));
                QVERIFY(name.endsWith(jpeg ? ".jpg" : ".png")); QVERIFY(message.data.endsWith("|w=320|h=240"));
            }
            QVERIFY(found);
            const QDir media(directory.filePath("media/" + core.channel));
            const auto files = media.entryList({jpeg ? "*.jpg" : "*.png"}, QDir::Files); QCOMPARE(files.size(), 1);
            const QImage decoded(media.filePath(files.front()));
            QCOMPARE(decoded.size(), image.size());
            if (!jpeg) QCOMPARE(decoded.pixelColor(0, 0), image.pixelColor(0, 0));
        }
    }

    void channelSend_data() {
        QTest::addColumn<bool>("reply"); QTest::addColumn<QString>("failure");
        QStringList failures{"none", "unavailable", "stopped", "cancelled", "sql"};
#ifdef Q_OS_LINUX
        failures << "text-allocation" << "queue-allocation";
#endif
        for (bool reply : {false, true}) for (const auto& failure : failures)
            QTest::newRow(qPrintable(QString("%1-%2").arg(reply ? "reply" : "ordinary", failure))) << reply << failure;
    }
    void channelSend() {
        QFETCH(bool, reply); QFETCH(QString, failure); const auto baseline = u_get_allocated_count();
        {
            SendCore core; QVERIFY(core.start(reply, {}, true)); QSignalSpy errors(core.input(), &InputBar::attachmentRejected);
            QSignalSpy sends(core.input(), &InputBar::sendMessage), accepted(core.input(), &InputBar::messageAccepted);
            const auto before = u_get_allocated_count();
            if (failure == "unavailable") gui_bridge_set_inst(nullptr);
            if (failure == "stopped") uasync_mark_stopped(core.ua);
            QByteArray table = core.db.msgTableName(core.channel).toUtf8();
            const auto clearAuthorizer = qScopeGuard([&] { sqlite3_set_authorizer(core.db.m_db, nullptr, nullptr); });
            if (failure == "sql") QCOMPARE(sqlite3_set_authorizer(core.db.m_db,
                [](void* arg, int action, const char* table, const char*, const char*, const char*) {
                    if (action == SQLITE_INSERT && table && !strcmp(table, static_cast<const char*>(arg))) {
                        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "[INJECT] message INSERT refused"); return SQLITE_DENY;
                    }
                    return SQLITE_OK;
                }, table.data()), SQLITE_OK);
#ifdef Q_OS_LINUX
            if (failure == "text-allocation") sendAllocationFailure = 1;
            if (failure == "queue-allocation") sendAllocationFailure = 2;
#endif
            const QString text = QString::fromUtf8("Канальный текст 👍"); core.editor()->setPlainText(text);
            QVERIFY(core.send()); QCOMPARE(sends.size(), 1); QVERIFY(sends[0][2].toULongLong());
            QCOMPARE(core.editor()->toPlainText(), text); // До pump сохранение ещё не подтверждено.
            if (failure == "cancelled") {
                gui_bridge_set_inst(nullptr); QCOMPARE(u_get_allocated_count(), before); QCoreApplication::processEvents();
            } else if (failure != "unavailable" && failure != "stopped") core.pump();
            const bool success = failure == "none";
            QCOMPARE(accepted.size(), success ? 1 : 0);
            QCOMPARE(errors.size(), success ? 0 : 1);
            QCOMPARE(core.editor()->toPlainText(), success ? QString() : text);
            QCOMPARE(core.db.getMessages(core.channel, 200).size(), int(reply) + int(success));
            QCOMPARE(sqlite3_set_authorizer(core.db.m_db, nullptr, nullptr), SQLITE_OK);
            if (!success) {
#ifdef Q_OS_LINUX
                QCOMPARE(sendAllocationFailure.load(), 0);
#endif
                core.attach(); QVERIFY(core.send()); QCOMPARE(sends.size(), 2);
                QVERIFY(sends[0][2] != sends[1][2]); core.pump(); QVERIFY(core.editor()->toPlainText().isEmpty());
                QCOMPARE(accepted.size(), 1);
                QCOMPARE(core.db.getMessages(core.channel, 200).size(), int(reply) + 1);
            }
        }
        QCOMPARE(u_get_allocated_count(), baseline);
    }

    void quoteSelectionThroughMenu() {
        QTemporaryDir directory; QVERIFY(directory.isValid()); SendCore core; QVERIFY(core.start(true, directory.path()));
        const QByteArray source = "Original https://example.org/quote-test suffix";
        const auto original = core.db.getMessageByKey(core.channel, core.original);
        QCOMPARE(chat_core_edit_message(core.instance, qPrintable(core.channel), (const uint8_t*)core.original.constData(),
            original.version, source.constData(), size_t(source.size())), 0);
        core.history->resize(700, 600); core.history->show(); core.history->loadChannel(core.channel); core.pump();
        auto* view = core.history->findChild<ChatView*>(); QVERIFY(view);
        const auto index = view->model()->index(0, 0); auto* delegate = qobject_cast<MessageDelegate*>(view->itemDelegate()); QVERIFY(delegate);
        QCOMPARE(index.data(MsgTextRole).toString(), QString::fromUtf8(source));
        QStyleOptionViewItem option; option.initFrom(view); option.widget = view; option.rect = view->visualRect(index);
        QPoint first;
        for (int y = option.rect.top(); y < option.rect.bottom() && first.isNull(); y += 3)
            for (int x = 20; x < view->viewport()->width() - 50; x += 3)
                if (!delegate->linkAt(option, index, QPoint(x, y)).isEmpty()) { first = QPoint(x, y); break; }
        QVERIFY(!first.isNull()); const QPoint last = first + QPoint(28, 0);
        view->viewport()->setUpdatesEnabled(false);
        QTest::mousePress(view->viewport(), Qt::LeftButton, Qt::NoModifier, first);
        QMouseEvent drag(QEvent::MouseMove, last, view->viewport()->mapToGlobal(last), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(view->viewport(), &drag);
        QTest::mouseRelease(view->viewport(), Qt::LeftButton, Qt::NoModifier, last);
        const QString fragment = view->selectedText(index); QVERIFY(!fragment.isEmpty());
        QVERIFY(QString::fromUtf8(source).contains(fragment)); // Нет зависимости от paint и раскрытия ссылки.
        view->viewport()->setUpdatesEnabled(true); core.editor()->setPlainText("Unsent draft");
        bool chosen = false;
        QTimer::singleShot(0, [&] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget()); if (!menu) return;
            for (auto* action : menu->actions()) if (action->text() == "Quote selection") {
                chosen = true; action->trigger(); menu->close(); return;
            }
            menu->close();
        });
        QVERIFY(QMetaObject::invokeMethod(view, "customContextMenuRequested", Qt::DirectConnection, Q_ARG(QPoint, last)));
        QVERIFY(chosen); QCOMPARE(core.editor()->toPlainText(), QString("Unsent draft"));
        core.editor()->setPlainText("Menu reply"); QVERIFY(core.send()); core.pump();
        const auto reply = core.db.getMessages(core.channel, 200).last();
        QCOMPARE(reply.replyTo, core.original); QCOMPARE(reply.replyQuote, fragment);
    }

    void quoteSnapshotAndSizeFailure() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        SendCore core; QVERIFY(core.start(true, directory.path()));
        core.history->resize(700, 700); core.history->show(); core.pump();
        const QString quote = QString::fromUtf8("  Выделено 😀\nстрока с **буквальными** маркерами\n") + QString("long ").repeated(50);
        core.input()->setReply(core.original, "Member", "Original", quote);
        core.editor()->setPlainText("Quoted reply"); QVERIFY(core.send()); core.pump();
        const auto rows = core.db.getMessages(core.channel, 200); QCOMPARE(rows.size(), 2);
        const auto reply = rows.last(); QCOMPARE(reply.replyTo, core.original); QCOMPARE(reply.replyQuote, quote);
        core.history->refresh(true);
        auto* view = core.history->findChild<ChatView*>(); QVERIFY(view);
        const auto index = view->model()->index(1, 0);
        QCOMPARE(index.data(MsgReplyQuoteRole).toString(), quote); QCOMPARE(index.data(MsgQuoteTextRole).toString(), quote);
        auto* delegate = qobject_cast<MessageDelegate*>(view->itemDelegate()); QVERIFY(delegate);
        QStyleOptionViewItem option; option.initFrom(view); option.rect = QRect(0, 0, view->viewport()->width(), 2000); option.widget = view;
        const int fullHeight = delegate->sizeHint(option, index).height();
        option.rect.setHeight(fullHeight);
        QImage preview(option.rect.size(), QImage::Format_ARGB32); preview.fill(Qt::white);
        { QPainter painter(&preview); delegate->paint(&painter, option, index); }
        QVERIFY(preview.save(QDir::tempPath() + "/utun-quote-preview.png"));
        view->model()->setData(index, QString(), MsgReplyQuoteRole);
        QVERIFY(fullHeight > delegate->sizeHint(option, index).height() + 3 * QFontMetrics(view->font()).height());
        view->model()->setData(index, quote, MsgReplyQuoteRole);
        const auto original = core.db.getMessageByKey(core.channel, core.original);
        QCOMPARE(chat_core_edit_message(core.instance, qPrintable(core.channel), (const uint8_t*)core.original.constData(),
            original.version, "Changed", 7), 0);
        core.history->refresh();
        QCOMPARE(index.data(MsgQuoteTextRole).toString(), quote); // Снимок не заменяется новым содержимым оригинала.
        QSignalSpy errors(core.input(), &InputBar::attachmentRejected);
        const QString large(3900, 'q'); core.input()->setReply(core.original, "Member", {}, large);
        core.editor()->setPlainText("This reply makes the signed JSON exceed the limit"); QVERIFY(core.send()); core.pump();
        QCOMPARE(errors.size(), 1); QVERIFY(errors.last()[0].toString().contains("4000"));
        QCOMPARE(core.editor()->toPlainText(), QString("This reply makes the signed JSON exceed the limit"));
        QCOMPARE(core.db.getMessages(core.channel, 200).size(), 2);
        core.editor()->setPlainText("retry"); core.input()->setReply(core.original, "Member", {}, quote);
        QVERIFY(core.send()); core.pump(); QCOMPARE(core.db.getMessages(core.channel, 200).last().replyQuote, quote);
    }

    void lateSendResultsKeepNewDraft() {
        SendCore core; QVERIFY(core.start(false, {}, true)); QSignalSpy sends(core.input(), &InputBar::sendMessage);
        core.editor()->setPlainText("first"); QVERIFY(core.send()); QVERIFY(core.send()); QCOMPARE(sends.size(), 1);
        core.editor()->setPlainText("next draft"); core.pump(); QCOMPARE(core.editor()->toPlainText(), QString("next draft"));
        QCOMPARE(core.db.getMessages(core.channel, 200).size(), 1);
    }

    void lateResultCannotAcknowledgeNewComposer() {
        SendCore core; QVERIFY(core.start(false, {}, true)); core.editor()->setPlainText("old composer"); QVERIFY(core.send());
        uasync_poll(core.ua, 0); // Старый success уже в Qt-очереди, новый запрос ещё не выполнялся.
        delete core.history.data(); core.openHistory(); core.editor()->setPlainText("new composer"); QVERIFY(core.send());
        QCoreApplication::processEvents(); QCOMPARE(core.editor()->toPlainText(), QString("new composer"));
        core.pump(); QVERIFY(core.editor()->toPlainText().isEmpty()); QCOMPARE(core.db.getMessages(core.channel, 200).size(), 2);
    }

    void channelSettingsCanLoseMainWindow() {
        debug_config_init(); gui_bridge_init(); QTemporaryDir directory; QVERIFY(directory.isValid());
        directory.setAutoRemove(false);
        auto diagnostics = qScopeGuard([&] { directory.setAutoRemove(!QTest::currentTestFailed()); });
        const QString path = directory.filePath("node.cfg"); QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray config = "[global]\n"
            "my_private_key=704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f\n"
            "my_public_key=7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f\n"
            "tun_enabled=no\n[server:local]\naddr=127.0.0.1:0\ntype=private\n"
            "[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\nlocal_invites_enabled=0\nlocal_broadcasts_enabled=0\n"
            "[ntp]\nenabled=no\n";
        QCOMPARE(file.write(config), config.size()); file.close(); DbManager db;
        QPointer<MainWindow> window = new MainWindow(nullptr, &db, path, directory.filePath("db"), directory.filePath("dialog.log"), "warn");
        auto release = qScopeGuard([&] { delete window.data(); });
        QVERIFY2(window->utunNode()->isRunning(), qPrintable(window->utunNode()->startupError()));
        QTRY_VERIFY_WITH_TIMEOUT(db.isOpen(), 3000);
        QPointer<ChannelSettingsDialog> settings; bool observed = false;
        QTimer::singleShot(0, [&] {
            settings = qobject_cast<ChannelSettingsDialog*>(QApplication::activeModalWidget()); observed = bool(settings);
            delete window.data();
        });
        QVERIFY(QMetaObject::invokeMethod(window.data(), "showChannelSettings", Qt::DirectConnection, Q_ARG(QString, QString("1"))));
        QVERIFY(observed); QVERIFY(!window); QVERIFY(!settings);
    }

    void editThroughMenuAndCore() {
        debug_config_init(); gui_bridge_init(); QTemporaryDir directory; QVERIFY(directory.isValid());
        directory.setAutoRemove(false);
        auto diagnostics = qScopeGuard([&] { directory.setAutoRemove(!QTest::currentTestFailed()); });
        const auto configPath = directory.filePath("node.cfg"); QFile config(configPath); QVERIFY(config.open(QIODevice::WriteOnly));
        const QByteArray settings = "[global]\n"
            "my_private_key=704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f\n"
            "my_public_key=7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f\n"
            "tun_enabled=no\n[server:local]\naddr=127.0.0.1:0\ntype=private\n"
            "[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\nlocal_invites_enabled=0\nlocal_broadcasts_enabled=0\n"
            "[ntp]\nenabled=no\n";
        QCOMPARE(config.write(settings), settings.size()); config.close(); DbManager db;
        db.setAsyncReader(gui_bridge_db_read);
        MainWindow window(nullptr, &db, configPath, directory.filePath("db"), directory.filePath("edit.log"), "warn", "chat_sync=debug");
        window.show(); QFile log(directory.filePath("edit.log"));
        const QByteArray startupLog = log.open(QIODevice::ReadOnly) ? log.readAll() : QByteArray("log unavailable");
        QVERIFY2(window.utunNode()->isRunning(), qPrintable(window.utunNode()->startupError() + "\n" + startupLog));
        QTRY_VERIFY_WITH_TIMEOUT(db.isOpen(), 3000);
        QCOMPARE(inCoreAsync<chat_channel_result>([](auto* inst, auto done, void* arg) {
            chat_channel_create_auto_submit(inst, "Editable", "chat", done, arg);
        }).status, 0);
        QTRY_COMPARE(db.getChannels().size(), 1); const auto channel = db.getChannels().first().channelId;
        chat_msg_submit req{}; strcpy(req.content_type, "text/plain");
        snprintf(req.channel_id, sizeof(req.channel_id), "%s", qPrintable(channel));
        auto submit = [&](auto* inst, auto done, void* arg) { chat_core_submit_message_async(inst, &req, done, arg); };
        req.data = (uint8_t*)"Original"; req.data_len = 8;
        const auto original = inCoreAsync<chat_message_result>(submit); QCOMPARE(original.status, 0);
        const QByteArray id((const char*)original.message_id, 32);
        memcpy(req.reply_to, original.message_id, 32); req.data = (uint8_t*)"Reply"; req.data_len = 5;
        const auto reply = inCoreAsync<chat_message_result>(submit); QCOMPARE(reply.status, 0);
        const QByteArray replyId((const char*)reply.message_id, 32);
        auto* history = window.findChild<MessageList*>(); QVERIFY(history); history->loadChannel(channel);
        auto* view = history->findChild<ChatView*>(); auto* input = history->findChild<InputBar*>(); QVERIFY(view && input);
        auto* editor = input->findChild<QTextEdit*>(); QVERIFY(editor);
        QTRY_COMPARE(view->model()->rowCount(), 2);
        auto indexFor = [&](const QByteArray& key) {
            for (int row = 0; row < view->model()->rowCount(); row++) {
                const auto index = view->model()->index(row, 0);
                if (index.data(MsgMessageKeyRole).toString() == QString::fromLatin1(key.toHex())) return index;
            }
            return QModelIndex();
        };
        QCOMPARE(indexFor(id).data(MsgTextRole).toString(), QString("Original"));
        auto* delegate = qobject_cast<MessageDelegate*>(view->itemDelegate()); QVERIFY(delegate);
        auto openClickMenu = [&]() -> QMenu* {
            const auto index = indexFor(id);
            delegate->messageMenuRequested(index, view->viewport()->mapToGlobal(view->visualRect(index).center()));
            return history->findChild<QMenu*>("messageClickMenu");
        };
        QPointer<QMenu> clickMenu = openClickMenu(); QVERIFY(clickMenu && clickMenu->isVisible());
        auto* commands = clickMenu->findChild<QMenu*>("messageCommands"); QVERIFY(commands);
        auto* reactions = clickMenu->findChild<QScrollArea*>("reactionStrip"); QVERIFY(reactions);
        QCOMPARE(commands->actions().size(), 2);
        QCOMPARE(commands->actions()[0]->text(), QString("Copy")); QCOMPARE(commands->actions()[1]->text(), QString("Reply"));
        QVERIFY(commands->width() < reactions->width());
        QVERIFY(commands->mapTo(clickMenu, QPoint()).y() >= reactions->mapTo(clickMenu, QPoint(0, reactions->height())).y() + 6);
        auto* bar = reactions->horizontalScrollBar(); QVERIFY(bar->maximum() > 0); QVERIFY(bar->height() <= 4);
        const auto buttons = reactions->findChildren<QToolButton*>(); QVERIFY(!buttons.isEmpty());
        auto* emojiButton = buttons.first(); const QPointF wheelPoint = emojiButton->rect().center();
        auto wheel = [&](const QPoint& pixels, const QPoint& angle) {
            QWheelEvent event(wheelPoint, emojiButton->mapToGlobal(wheelPoint.toPoint()), pixels, angle,
                Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QApplication::sendEvent(emojiButton, &event);
        };
        wheel({}, QPoint(0, -120)); QVERIFY(bar->value() > 0);
        wheel({}, QPoint(0, 120)); QCOMPARE(bar->value(), 0);
        wheel(QPoint(-12, 0), {}); QCOMPARE(bar->value(), 12);
        QVERIFY(clickMenu->grab().save("/tmp/chatgui-message-menu-preview.png"));
        QCOMPARE(clickMenu->findChildren<QToolButton*>().size(), CHAT_REACTION_COUNT);
        QTest::mouseClick(commands, Qt::LeftButton, Qt::NoModifier, commands->actionGeometry(commands->actions()[0]).center());
        QCOMPARE(QApplication::clipboard()->text(), QString("Original")); QVERIFY(!clickMenu || !clickMenu->isVisible());
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        clickMenu = openClickMenu(); QVERIFY(clickMenu);
        commands = clickMenu->findChild<QMenu*>("messageCommands"); QVERIFY(commands);
        QTest::mouseClick(commands, Qt::LeftButton, Qt::NoModifier, commands->actionGeometry(commands->actions()[1]).center());
        bool replyVisible = false;
        for (auto* label : input->findChildren<QLabel*>()) if (label->isVisible() && label->text().contains("Original")) replyVisible = true;
        QVERIFY(replyVisible); input->clearReply();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        clickMenu = openClickMenu(); QVERIFY(clickMenu && clickMenu->isVisible());
        history->loadChannel(channel); QVERIFY(!clickMenu || !clickMenu->isVisible());
        editor->setPlainText("Unsent draft");
        auto openEditor = [&] {
            QCoreApplication::processEvents(); const auto original = indexFor(id); QVERIFY(original.isValid());
            view->scrollTo(original);
            const auto pos = view->visualRect(original).center();
            QCOMPARE(view->indexAt(pos), original);
            QTimer::singleShot(0, [] {
                auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
                if (!menu) qFatal("Edit menu missing");
                for (auto* action : menu->actions()) QVERIFY(action->text() != "React");
                for (auto* action : menu->actions()) if (action->text() == "Edit message") { action->trigger(); menu->close(); return; }
                qFatal("Edit action missing");
            });
            QMetaObject::invokeMethod(view, "customContextMenuRequested", Qt::DirectConnection, Q_ARG(QPoint, pos));
        };
        openEditor(); QCOMPARE(editor->toPlainText(), QString("Original"));
        gui_bridge_set_msg_received_cb([](const char*, int, const uint8_t*, int) { ++newMessages; }); newMessages = 0;
        editor->setPlainText(QString::fromUtf8("Изменено 👍")); QTest::keyClick(editor, Qt::Key_Return);
        QTRY_COMPARE_WITH_TIMEOUT(indexFor(id).data(MsgTextRole).toString(), QString::fromUtf8("Изменено 👍"), 3000);
        QCOMPARE(view->model()->rowCount(), 2);
        const auto editedVersion = indexFor(id).data(MsgVersionRole).toULongLong(); QVERIFY(editedVersion > original.ver);
        QVERIFY(indexFor(id).data(MsgTimeRole).toString().contains("edited "));
        QCOMPARE(indexFor(replyId).data(MsgQuoteTextRole).toString(), QString::fromUtf8("Изменено 👍"));
        QTRY_COMPARE(editor->toPlainText(), QString("Unsent draft")); QCOMPARE(newMessages, 0);
        openEditor(); editor->setPlainText("Stale editor draft");
        req.expected_ver = editedVersion; memcpy(req.message_id, id.constData(), 32);
        req.data = (uint8_t*)"Remote edit"; req.data_len = 11;
        const auto remoteEdit = inCoreAsync<chat_message_result>(submit); QCOMPARE(remoteEdit.status, 0);
        QTRY_COMPARE(indexFor(id).data(MsgVersionRole).toULongLong(), quint64(remoteEdit.ver));
        QCOMPARE(editor->toPlainText(), QString("Stale editor draft"));
        QSignalSpy failures(input, &InputBar::attachmentRejected); QTest::keyClick(editor, Qt::Key_Return);
        QTRY_COMPARE(failures.size(), 1); QCOMPARE(editor->toPlainText(), QString("Stale editor draft"));
        QCOMPARE(indexFor(id).data(MsgTextRole).toString(), QString("Remote edit")); QCOMPARE(newMessages, 0);
        QTest::keyClick(editor, Qt::Key_Escape); QCOMPARE(editor->toPlainText(), QString("Unsent draft"));
        SoundSettingsPage sounds(&db); sounds.loadFromDb();
        const auto checks = sounds.findChildren<QCheckBox*>(); const auto sliders = sounds.findChildren<QSlider*>();
        QCOMPARE(checks.size(), 9); QCOMPARE(sliders.size(), 9);
        for (int i = 0; i < 9; i++) { checks[i]->setChecked(i % 2 == 0); sliders[i]->setValue(20 + i); }
        checks[0]->setChecked(false); sounds.applyAndSave();
        QTRY_COMPARE(db.getUiStateInt("sound_enabled", -1), 0); QTRY_COMPARE(db.getUiStateInt("sound_volume", -1), 20);
        const char* events[] = {"msg_incoming", "msg_outgoing", "connect", "disconnect", "call_incoming", "call_connecting", "call_ringing", "radio_incoming"};
        for (int i = 0; i < 8; i++) {
            QCOMPARE(db.getUiStateInt(QString("sound_%1_on").arg(events[i]), -1), (i + 1) % 2 == 0 ? 1 : 0);
            QCOMPARE(db.getUiStateInt(QString("sound_%1_vol").arg(events[i]), -1), 21 + i);
        }
        gui_bridge_set_msg_received_cb(nullptr);
    }
};
QTEST_MAIN(GroupEditTest)
#include "test_group_edits.moc"

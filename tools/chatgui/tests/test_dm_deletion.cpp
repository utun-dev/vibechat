/* Настоящие MainWindow/uasync/SQLite: подтверждение, rollback, скрытие PM и старые поколения GUI. */
#include "../src/mainwindow.h"
#include "../src/channellist.h"
#include "../src/channeldelegate.h"
#include "../src/messagelist.h"
#include "../src/messagedelegate.h"
#include "../src/chatview.h"
#include "../src/inputbar.h"
#include "../db/db_manager.h"
#include "../transport/utun_node.h"
#include "../transport/gui_bridge.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QTextEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QMenu>
#include <QScopeGuard>
#include <future>
#include <memory>
extern "C" {
#include "utun_instance.h"
#include "db_async.h"
#include "dm/dm_core.h"
#include "dm/dm_history.h"
#include "debug_config.h"
}

static MessageList* historyView;
static QList<QByteArray> snapshots;

/* Полный путь UI с файловой БД. Состояние теста меняется после результата записи. */
using CoreResult = std::promise<int>;
using CoreOperation = std::function<void(UTUN_INSTANCE*, CoreResult*)>;
static int inCore(CoreOperation operation) {
    auto result = std::make_unique<CoreResult>();
    auto future = result->get_future();
    auto* task = new std::pair<CoreOperation, CoreResult*>(std::move(operation), result.release());
    gui_bridge_post_uasync_fn([](void* arg) {
        std::unique_ptr<std::pair<CoreOperation, CoreResult*>> work(static_cast<std::pair<CoreOperation, CoreResult*>*>(arg));
        work->first(gui_bridge_get_inst(), work->second);
    }, task);
    if (future.wait_for(std::chrono::seconds(5)) != std::future_status::ready) qFatal("DM test: core task timed out");
    return future.get();
}

template<typename Result>
static void operationDone(void* arg, const Result* result) {
    std::unique_ptr<CoreResult> done(static_cast<CoreResult*>(arg));
    done->set_value(result->status);
}

static int testSql(const char* sql) {
    return inCore([sql](UTUN_INSTANCE* inst, CoreResult* result) {
        auto* query = new std::pair<const char*, CoreResult*>(sql, result);
        db_async_submit(inst->db_worker, "dm-test-delete-trigger", DB_ASYNC_WRITE, [](sqlite3* db, void* arg) {
            return sqlite3_exec(db, static_cast<std::pair<const char*, CoreResult*>*>(arg)->first, nullptr, nullptr, nullptr);
        }, query, [](void* arg, const db_async_result* outcome) {
            std::unique_ptr<std::pair<const char*, CoreResult*>> query(static_cast<std::pair<const char*, CoreResult*>*>(arg));
            std::unique_ptr<CoreResult> done(query->second);
            done->set_value(outcome->sqlite_code);
        });
    });
}

static void answerConfirmation(bool remove) {
    QTimer::singleShot(0, [remove] {
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (!box) qFatal("DM test: confirmation missing");
        for (auto* button : box->buttons()) if (button->text() == (remove ? "Delete" : "Cancel")) { button->click(); return; }
        qFatal("DM test: confirmation button missing");
    });
}

static void dismissError() {
    auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
    QVERIFY(box);
    QCOMPARE(box->text(), QString("Could not delete the conversation. Please try again."));
    box->accept();
}

class DmDeletionTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { debug_config_init(); gui_bridge_init(); }

    void mediaPreviews() {
        QTemporaryDir directory; QVERIFY(directory.isValid()); debug_set_category_level(DEBUG_CATEGORY_MEDIA, DEBUG_LEVEL_DEBUG);
        const QString picture = directory.filePath("picture.png"); QImage pixels(1200, 800, QImage::Format_ARGB32);
        pixels.fill(QColor(30, 150, 70, 90)); QVERIFY(pixels.save(picture, "PNG"));
        QFile fixture(QFINDTESTDATA("data/preview_video.b64")); QVERIFY(fixture.open(QIODevice::ReadOnly));
        QFile video(directory.filePath("video.mp4")); QVERIFY(video.open(QIODevice::WriteOnly));
        QByteArray mp4 = QByteArray::fromBase64(fixture.readAll()); QCOMPARE(video.write(mp4), mp4.size()); video.close();
        DbManager db; db.setDbPath(directory.path());
        auto seed = [&](MessageList& history, int count) {
            auto* view = history.findChild<ChatView*>();
            auto* model = qobject_cast<QStandardItemModel*>(view->model());
            for (int row = 0; row < count; ++row) {
                auto* item = new QStandardItem; item->setData(row + 1, MsgIdRole); item->setData("Alice", MsgAuthorRole);
                item->setData("12:34", MsgTimeRole); item->setData(row % 2 ? "video/mp4" : "image/png", MsgContentTypeRole);
                item->setData(row % 2 ? video.fileName() : picture, MsgVoiceFileRole); item->setData(2, MsgFileDownloadStateRole);
                item->setData(1200, MsgImageWidthRole); item->setData(800, MsgImageHeightRole); item->setData(1000, MsgFileSizeRole);
                model->appendRow(item);
            }
            history.resize(640, 480); history.show(); view->scrollToTop(); return model;
        };
        const QString cache = directory.filePath("media_previews/v1"); QHash<QString, QDateTime> dates;
        {
            MessageList history(&db); auto* model = seed(history, 24); auto* view = history.findChild<ChatView*>();
            QTRY_VERIFY_WITH_TIMEOUT(!model->item(0)->data(MsgImagePreviewRole).value<QImage>().isNull(), 5000);
            QTRY_VERIFY_WITH_TIMEOUT(!model->item(1)->data(MsgImagePreviewRole).value<QImage>().isNull(), 5000);
            QCOMPARE(QDir(cache).entryList({"*.preview"}, QDir::Files).size(), 2);
            view->scrollToBottom();
            QTRY_VERIFY_WITH_TIMEOUT(!model->item(23)->data(MsgImagePreviewRole).value<QImage>().isNull(), 5000);
            QVERIFY(!model->item(0)->data(MsgImagePreviewRole).isValid());
            for (const auto& file : QDir(cache).entryInfoList({"*.preview"}, QDir::Files)) dates[file.fileName()] = file.lastModified();
        }
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        {
            MessageList reopened(&db); auto* model = seed(reopened, 2);
            QTRY_VERIFY_WITH_TIMEOUT(!model->item(0)->data(MsgImagePreviewRole).value<QImage>().isNull(), 5000);
            QTRY_VERIFY_WITH_TIMEOUT(!model->item(1)->data(MsgImagePreviewRole).value<QImage>().isNull(), 5000);
            QCOMPARE(QDir(cache).entryList({"*.preview"}, QDir::Files).size(), 2);
            for (const auto& file : QDir(cache).entryInfoList({"*.preview"}, QDir::Files)) QCOMPARE(file.lastModified(), dates.value(file.fileName()));
        }
    }

    void deleteThroughWindowAndCore() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        directory.setAutoRemove(false);
        auto diagnostics = qScopeGuard([&directory] {
            directory.setAutoRemove(!QTest::currentTestFailed());
            if (QTest::currentTestFailed()) DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "DM test diagnostics retained: %s", qPrintable(directory.path()));
        });
        const QString configPath = directory.filePath("node.cfg");
        QFile config(configPath); QVERIFY(config.open(QIODevice::WriteOnly));
        const QByteArray text = "[global]\n"
            "my_private_key=704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f\n"
            "my_public_key=7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f\n"
            "tun_enabled=no\n[server:local]\naddr=127.0.0.1:0\ntype=private\n"
            "[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\nlocal_invites_enabled=0\nlocal_broadcasts_enabled=0\n"
            "[ntp]\nenabled=no\n";
        QCOMPARE(config.write(text), text.size()); config.close();
        DbManager db; db.setAsyncReader(gui_bridge_db_read); // Как main.cpp: файловому списку нужен назначенный backend.
        MainWindow window(nullptr, &db, configPath, directory.filePath("db"), directory.filePath("dm.log"),
                          "warn", "dm=debug,general=info");
        window.show();
        QVERIFY2(window.utunNode()->isRunning(), qPrintable(window.utunNode()->startupError()));
        QTRY_VERIFY_WITH_TIMEOUT(db.isOpen(), 3000);
        auto* channels = window.findChild<ChannelList*>(); QVERIFY(channels);
        auto* history = window.findChild<MessageList*>(); QVERIFY(history);
        auto* view = history->findChild<ChatView*>(); QVERIFY(view);
        auto* input = history->findChild<InputBar*>(); QVERIFY(input);
        auto* editor = input->findChild<QTextEdit*>(); QVERIFY(editor);
        historyView = history;
        gui_bridge_set_dm_messages_cb([](const uint8_t* data, int len) {
            snapshots.append(QByteArray(reinterpret_cast<const char*>(data), len));
            historyView->onDmMessages(data, len);
        });

        SC_MYKEYS peer{}; uint8_t ed[32], seed[32];
        QCOMPARE(sc_identity_generate(peer.public_key, peer.private_key, ed, seed), SC_OK);
        const auto startPeer = [ed](UTUN_INSTANCE* inst, CoreResult* result) {
            dm_start_query query{}; query.peer_ed = ed; query.name = "Alice"; query.mode = DM_MODE_DEVICE;
            dm_start_submit(inst, &query, operationDone<dm_start_result>, result);
        };
        QCOMPARE(inCore(startPeer), 0);
        QTRY_COMPARE_WITH_TIMEOUT(db.getDmConversations().size(), 1, 3000);
        const QString conv = db.getDmConversations().first().convId;
        QCOMPARE(db.getDmConversations().first().generation, quint64(1));
        const auto send = [conv](UTUN_INSTANCE* inst, CoreResult* result) {
            const QByteArray conversation = conv.toLatin1();
            dm_send_query query{}; query.conv = conversation.constData(); query.type = "text";
            query.data = reinterpret_cast<const uint8_t*>("one"); query.size = 3;
            dm_send_submit(inst, &query, operationDone<dm_send_result>, result);
        };
        QCOMPARE(inCore(send), 0);
        QTRY_COMPARE_WITH_TIMEOUT(view->model()->rowCount(), 1, 3000);
        QVERIFY(input->isEnabled()); QVERIFY(!snapshots.isEmpty());
        const QByteArray oldHistory = snapshots.last();

        editor->setPlainText("keep draft on cancellation");
        answerConfirmation(false); window.onDmDeleteRequested(conv, 1, "Alice");
        QVERIFY(input->isEnabled()); QCOMPARE(editor->toPlainText(), QString("keep draft on cancellation"));
        QCOMPARE(db.getDmConversations().size(), 1);

        QCOMPARE(testSql("CREATE TEMP TRIGGER refuse_delete BEFORE DELETE ON dm_messages"
            " BEGIN SELECT RAISE(ABORT,'injected UI deletion failure'); END"), SQLITE_OK);
        answerConfirmation(true); window.onDmDeleteRequested(conv, 1, "Alice"); QVERIFY(!input->isEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(QApplication::activeModalWidget() != nullptr, 3000); dismissError();
        QVERIFY(input->isEnabled()); QCOMPARE(db.getDmConversations().first().generation, quint64(1));
        QCOMPARE(editor->toPlainText(), QString("keep draft on cancellation"));
        QCOMPARE(testSql("DROP TRIGGER refuse_delete"), SQLITE_OK);

        // ПКМ запускает тот же диалог; после подтверждения отправки заблокированы до результата.
        auto* list = channels->findChild<QListView*>(); QVERIFY(list);
        QModelIndex dmIndex;
        for (int row = 0; row < list->model()->rowCount(); ++row) {
            auto index = list->model()->index(row, 0);
            if (index.data(ChannelChannelIdRole).toString() == "dm:" + conv) dmIndex = index;
        }
        QVERIFY(dmIndex.isValid()); QCOMPARE(dmIndex.data(ChannelDmGenerationRole).toULongLong(), quint64(1));
        QSignalSpy requested(channels, &ChannelList::dmDeleteRequested);
        QTimer::singleShot(0, [] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            if (!menu) qFatal("DM test: PM context menu missing");
            auto* action = menu->findChild<QAction*>("deleteDmAction");
            if (!action || !action->isEnabled()) qFatal("DM test: delete action unavailable");
            menu->setActiveAction(action); answerConfirmation(true); QTest::keyClick(menu, Qt::Key_Return);
        });
        QMetaObject::invokeMethod(list, "customContextMenuRequested", Qt::DirectConnection,
                                  Q_ARG(QPoint, list->visualRect(dmIndex).center()));
        QCOMPARE(requested.size(), 1); QVERIFY(!input->isEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(db.getDmConversations().isEmpty(), 3000);
        QTRY_COMPARE_WITH_TIMEOUT(view->model()->rowCount(), 0, 3000);
        QVERIFY(editor->toPlainText().isEmpty()); QVERIFY(!input->isEnabled());
        history->onDmMessages(reinterpret_cast<const uint8_t*>(oldHistory.constData()), oldHistory.size());
        QCOMPARE(view->model()->rowCount(), 0);

        QCOMPARE(inCore(startPeer), 0); QCOMPARE(inCore(send), 0);
        QTRY_COMPARE_WITH_TIMEOUT(db.getDmConversations().size(), 1, 3000);
        QCOMPARE(db.getDmConversations().first().generation, quint64(2));
        QTRY_COMPARE_WITH_TIMEOUT(view->model()->rowCount(), 1, 3000); QVERIFY(input->isEnabled());
        editor->setPlainText("new generation draft");
        history->onDmMessages(reinterpret_cast<const uint8_t*>(oldHistory.constData()), oldHistory.size());
        QCOMPARE(view->model()->rowCount(), 1);
        QCOMPARE(view->model()->index(0, 0).data(MsgIdRole).toLongLong(), qint64(2));
        answerConfirmation(true); window.onDmDeleteRequested(conv, 1, "Alice");
        QTRY_VERIFY_WITH_TIMEOUT(QApplication::activeModalWidget() != nullptr, 3000); dismissError();
        QCOMPARE(db.getDmConversations().first().generation, quint64(2));
        QCOMPARE(editor->toPlainText(), QString("new generation draft")); QVERIFY(input->isEnabled());

        // Удаление другой беседы сохраняет выбранный PM и его черновик.
        SC_MYKEYS other{}; uint8_t otherEd[32];
        QCOMPARE(sc_identity_generate(other.public_key, other.private_key, otherEd, seed), SC_OK);
        QCOMPARE(inCore([otherEd](UTUN_INSTANCE* inst, CoreResult* result) {
            dm_start_query query{}; query.peer_ed = otherEd; query.name = "Bob"; query.mode = DM_MODE_DEVICE;
            dm_start_submit(inst, &query, operationDone<dm_start_result>, result);
        }), 0);
        QTRY_COMPARE_WITH_TIMEOUT(db.getDmConversations().size(), 2, 3000);
        QCoreApplication::processEvents(); channels->selectChannel("dm:" + conv);
        QTRY_VERIFY(input->isEnabled()); editor->setPlainText("selected PM stays");
        QString otherConv;
        for (const auto& dm : db.getDmConversations()) if (dm.convId != conv) otherConv = dm.convId;
        QVERIFY(!otherConv.isEmpty());
        answerConfirmation(true); window.onDmDeleteRequested(otherConv, 1, "Bob");
        QVERIFY(input->isEnabled());
        QTRY_COMPARE_WITH_TIMEOUT(db.getDmConversations().size(), 1, 3000);
        QCoreApplication::processEvents(); QCOMPARE(editor->toPlainText(), QString("selected PM stays"));
        QCOMPARE(list->currentIndex().data(ChannelChannelIdRole).toString(), "dm:" + conv);
        QVERIFY(input->isEnabled());
        DEBUG_INFO(DEBUG_CATEGORY_DM, "DM desktop: cancel, SQL rollback, context menu, generations and unselected deletion PASS");
    }
};

QTEST_MAIN(DmDeletionTest)
#include "test_dm_deletion.moc"

/* Настоящие Qt-меню, БД ядра и bridge: обновления списка и отмена его команд. */
#include "../src/channellist.h"
#include "../src/channeldelegate.h"
#include "../src/clipboard_exchange.h"
#include "../db/db_manager.h"
#include "../transport/gui_bridge.h"
#include <QtTest>
#include <QMenu>
#include <QPointer>
#include <QScopeGuard>
#include <QPushButton>
#include <QLabel>
#include <QToolButton>
#include <QSplitter>
#include <QToolTip>
#include <QEnterEvent>
#include <QDialog>
#include <QClipboard>
#include <QTemporaryDir>
#include <atomic>
#include <condition_variable>
#include <limits>
#include <memory>
#include <mutex>
extern "C" {
#include "utun_instance.h"
#include "chat/chat_core.h"
#include "chat/chat_clipboard.h"
#include "dm/dm_core.h"
#include "transport_layer/secure_channel.h"
#include "mem.h"
#include "debug_config.h"
}

static int navigationReads;
static bool failNavigationRead;
struct NavigationGate {
    std::mutex mutex;
    std::condition_variable changed;
    std::atomic<int> entered{0};
    int released = 0;
    void wait() {
        std::unique_lock<std::mutex> lock(mutex); const int turn = ++entered;
        changed.wait(lock, [this, turn] { return released >= turn; });
    }
    void release(int turn) { std::lock_guard<std::mutex> lock(mutex); released = turn; changed.notify_all(); }
};
static std::shared_ptr<NavigationGate> navigationGate;
static bool navigationReader(sqlite3* db, QObject* context, GuiDbReadWork work, GuiDbReadDone done) {
    navigationReads++;
    if (failNavigationRead) {
        failNavigationRead = false;
        work = [](sqlite3*, QVariant*) { return SQLITE_BUSY; };
    }
    if (navigationGate) work = [gate = navigationGate, read = std::move(work)](sqlite3* handle, QVariant* value) {
        const int rc = read(handle, value); gate->wait(); return rc;
    };
    return gui_bridge_db_read(db, context, std::move(work), std::move(done));
}

struct ListCore {
    UASYNC* ua = uasync_create();
    UTUN_INSTANCE* inst = nullptr;
    DbManager db;
    QString channel;
    QTimer poll;
    bool polling = false;
    ~ListCore() {
        poll.stop();
        gui_bridge_set_inst(nullptr); gui_bridge_set_uasync(nullptr);
        if (ua) uasync_mark_stopped(ua);
        db.setDb(nullptr);
        if (inst) utun_instance_destroy(inst);
        if (ua) uasync_destroy(ua, 0);
    }
    bool start(const QString& path = {}) {
        if (!ua) return false;
        const QByteArray config = QByteArray("[global]\n") + (path.isEmpty() ? QByteArray() : "db_path=" + path.toUtf8() + '\n') +
            "my_private_key=704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f\n"
            "my_public_key=7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f\n"
            "auto_sockets=no\n[ntp]\nenabled=no\n"
            "[chatserver]\ngroup_autoconnect=0\nmember_files_enabled=0\nstorage_autoload=0\n";
        inst = utun_instance_create_from_str(ua, config.constData());
        if (!inst || utun_core_start(inst) || chat_service_start(inst)) return false;
        chat_core_create_channel_auto(inst, "Group");
        db.setDb(inst->topo_sqlite_db);
        if (db.getChannels().size() != 1) return false;
        channel = db.getChannels()[0].channelId;
        if (!path.isEmpty()) {
            if (!inst->db_worker) return false;
            db.setAsyncReader(navigationReader); uasync_mark_running(ua);
            gui_bridge_set_uasync(ua); gui_bridge_set_inst(inst);
            QObject::connect(&poll, &QTimer::timeout, &poll, [this] { polling = true; uasync_poll(ua, 0); polling = false; });
            poll.start(1);
        }
        uint8_t seed[32] = {7}, peerX[32], peerEd[32], privateX[32];
        if (sc_identity_from_seed(seed, peerX, privateX, peerEd) != SC_OK) return false;
        return dm_start(inst, sc_derive_node_id_from_pubkey(peerX), peerX, peerEd, "PM", nullptr, DM_MODE_DEVICE, nullptr) == 0;
    }
};

class ChannelListTest : public QObject {
    Q_OBJECT
private slots:
    void radioPttDoesNotSelectChannelAndReleasesOnDisable() {
        ListCore core; QVERIFY(core.start());
        ChannelList list(&core.db); list.resize(350, 400); list.show(); list.loadChannels();
        auto* view = list.findChild<QListView*>(); QVERIFY(view);
        auto* button = list.findChild<QPushButton*>("channelRadioPtt"); QVERIFY(button);
        list.setActiveRadioChannel(core.channel); list.setRadioPttState(true, false, false);
        QTRY_VERIFY(button->isVisible());
        QModelIndex dm;
        for (int row = 0; row < view->model()->rowCount(); ++row) {
            const auto index = view->model()->index(row, 0);
            if (index.data(ChannelChannelIdRole).toString().startsWith("dm:")) dm = index;
        }
        QVERIFY(dm.isValid()); view->setCurrentIndex(dm);
        QSignalSpy selected(&list, &ChannelList::channelSelected);
        QSignalSpy pressed(&list, &ChannelList::radioPttPressed);
        QSignalSpy released(&list, &ChannelList::radioPttReleased);
        QSignalSpy locked(&list, &ChannelList::radioPttLockRequested);
        QTest::mousePress(button, Qt::LeftButton); QCOMPARE(pressed.count(), 1);
        QTest::mouseRelease(button, Qt::LeftButton); QCOMPARE(released.count(), 1);
        QTest::mouseDClick(button, Qt::LeftButton); QCOMPARE(locked.count(), 1);
        QTest::mouseRelease(button, Qt::LeftButton);
        QCOMPARE(view->currentIndex(), dm); QCOMPARE(selected.count(), 0);
        list.setRadioPttState(true, true, true);
        QVERIFY(button->toolTip().contains("locked"));
        list.loadChannels(); QTRY_VERIFY(button->isVisible());
        QModelIndex active;
        for (int row = 0; row < view->model()->rowCount(); ++row) {
            const auto index = view->model()->index(row, 0);
            if (index.data(ChannelChannelIdRole).toString() == core.channel) active = index;
        }
        QVERIFY(active.data(ChannelRadioTransmittingRole).toBool());
        QTest::mousePress(button, Qt::LeftButton);
        const int beforeDisable = released.count();
        list.setRadioPttState(false, false, false);
        QCOMPARE(released.count(), beforeDisable + 1); QVERIFY(!button->isEnabled()); QVERIFY(!button->isDown());
        list.setRadioPttState(true, false, false);
        QTest::mousePress(button, Qt::LeftButton);
        const int beforeHide = released.count();
        list.setActiveRadioChannel(QString());
        QCOMPARE(released.count(), beforeHide + 1); QVERIFY(!button->isVisible());
    }
    void activeRadioSurvivesReload() {
        ListCore core; QVERIFY(core.start());
        ChannelList list(&core.db);
        list.setActiveRadioChannel(core.channel);
        list.loadChannels();
        auto* view = list.findChild<QListView*>(); QVERIFY(view);
        const auto active = [&]() {
            for (int row = 0; row < view->model()->rowCount(); ++row) {
                const auto index = view->model()->index(row, 0);
                if (index.data(ChannelChannelIdRole).toString() == core.channel) return index.data(ChannelRadioActiveRole).toBool();
            }
            return false;
        };
        QTRY_VERIFY(active());
        list.loadChannels(); QTRY_VERIFY(active());
        list.setActiveRadioChannel(QString()); QVERIFY(!active());
        list.loadChannels(); QTRY_VERIFY(!active());
    }
    void initTestCase() {
        debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); debug_set_category_level(DEBUG_CATEGORY_SYS, DEBUG_LEVEL_ERROR);
        gui_bridge_init();
    }
    void clipboardSendSitsLeftOfCpu() {
        ChannelList list; list.resize(340, 200); list.show(); QApplication::processEvents();
        auto* send = list.findChild<QPushButton*>("sendClipboardButton"); auto* cpu = list.findChild<QWidget*>("cpuLoadIndicator");
        QVERIFY(send && cpu); QVERIFY(send->isVisible() && cpu->isVisible());
        QVERIFY(send->mapTo(&list, QPoint(send->width(), 0)).x() <= cpu->mapTo(&list, QPoint()).x());
    }

    void memberClipboardBelongsToGroupRowAndOpensOnClick() {
        ListCore core; QVERIFY(core.start()); ChannelList list(&core.db); list.loadChannels(); list.resize(350, 400); list.show();
        auto* view = list.findChild<QListView*>(); QVERIFY(view); auto* panel = list.clipboard();
        panel->setNameResolver([](const QString&, const QByteArray&) { return QString("Sender"); }); QApplication::clipboard()->setText("local");
        QByteArray bytes(CHAT_CLIPBOARD_EVENT_HEADER, 0); bytes[0] = 1; bytes[CHAT_CLIPBOARD_EVENT_PEER_ED_OFF] = 2; bytes[CHAT_CLIPBOARD_EVENT_TYPE_OFF] = CHAT_CLIPBOARD_TEXT;
        bytes[CHAT_CLIPBOARD_EVENT_SCOPE_OFF] = CHAT_CLIPBOARD_MEMBER;
        const auto group = core.channel;
        bytes.replace(CHAT_CLIPBOARD_EVENT_GROUP_OFF, 32, QByteArray::fromHex(group.toLatin1())); bytes += "remote";
        panel->receive(reinterpret_cast<const uint8_t*>(bytes.constData()), bytes.size());
        QTRY_COMPARE(panel->memberType(group), 1); QCOMPARE(QApplication::clipboard()->text(), QString("local")); list.loadChannels();
        QModelIndex row;
        for (int i = 0; i < view->model()->rowCount(); ++i) {
            auto index = view->model()->index(i, 0);
            if (index.data(ChannelChannelIdRole).toString() == core.channel) row = index;
            else QVERIFY(qvariant_cast<QIcon>(index.data(ChannelClipboardIconRole)).isNull());
        }
        QVERIFY(row.isValid()); QCOMPARE(row.data(ChannelClipboardSenderRole).toString(), QString("Sender"));
        QVERIFY(!qvariant_cast<QIcon>(row.data(ChannelClipboardIconRole)).isNull());
        QStyleOptionViewItem option; option.rect = QRect(0, 0, 350, 56); const auto wide = ChannelDelegate::clipboardRect(option, row);
        QVERIFY(wide.width() >= 40); QVERIFY(option.rect.contains(wide)); option.rect.setWidth(180);
        QCOMPARE(ChannelDelegate::clipboardRect(option, row).width(), 28);
        QApplication::processEvents(); option.rect = view->visualRect(row);
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, ChannelDelegate::clipboardRect(option, row).center());
        auto* dialog = panel->findChild<QDialog*>("memberClipboardDialog"); QVERIFY(dialog);
        QVERIFY(dialog->findChild<QLabel*>("memberClipboardDescription")->text().contains("Sender"));
        QCOMPARE(QApplication::clipboard()->text(), QString("local"));
        dialog->findChild<QToolButton*>("memberClipboardCopy")->click(); QCOMPARE(QApplication::clipboard()->text(), QString("remote"));
        delete dialog;
    }

    void narrowingHidesOptionalActionsThenCollapsesAndRestores() {
        struct Splitter : QSplitter { using QSplitter::moveSplitter; } splitter;
        splitter.setHandleWidth(1); auto* list = new ChannelList;
        splitter.addWidget(list); splitter.addWidget(new QWidget); splitter.resize(800, 300); splitter.show();
        const QStringList order{"channelVersion", "networkTrafficButton", "connectionMonitorButton"};
        QStringList hidden;
        auto* footer = list->findChild<QWidget*>("channelFooter"); QVERIFY(footer);
        auto* title = list->findChild<QLabel*>("channelTitle"); QVERIFY(title);
        const int minimumWidth = footer->minimumWidth();
        for (int width = 400; width >= minimumWidth; --width) {
            splitter.setSizes({width, 799 - width}); QApplication::processEvents();
            QCOMPARE(list->width(), width);
            bool visibleSeen = false;
            for (const auto& name : order) {
                auto* widget = list->findChild<QWidget*>(name); QVERIFY(widget);
                if (!widget->isHidden()) visibleSeen = true;
                else {
                    QVERIFY(!visibleSeen);
                    if (!hidden.contains(name)) hidden.append(name);
                }
            }
            QList<QWidget*> shown;
            for (auto* widget : footer->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly)) {
                if (widget->isHidden()) continue;
                QVERIFY2(footer->rect().contains(widget->geometry()), qPrintable(widget->objectName()));
                for (auto* other : shown) QVERIFY(!widget->geometry().intersects(other->geometry()));
                shown.append(widget);
            }
            for (const char* name : {"settingsButton", "createGroupButton", "joinGroupButton", "openLinkButton"})
                QVERIFY(!list->findChild<QPushButton*>(name)->isHidden());
        }
        QCOMPARE(hidden, order);
        // Принятый буфер добавляет ещё один значок в header и тоже должен помещаться в узкой панели.
        auto* received = list->findChild<QToolButton*>("receivedClipboardButton"); QVERIFY(received); received->show();
        QApplication::processEvents(); QCOMPARE(list->width(), minimumWidth);
        for (const char* name : {"sendClipboardButton", "receivedClipboardButton", "cpuLoadIndicator"}) {
            auto* widget = list->findChild<QWidget*>(name); QVERIFY(widget && widget->isVisible());
            QVERIFY(list->rect().contains(QRect(widget->mapTo(list, QPoint()), widget->size())));
        }
        splitter.moveSplitter(0, 1); QApplication::processEvents(); QCOMPARE(splitter.sizes()[0], 0);
        splitter.moveSplitter(400, 1); QApplication::processEvents(); QVERIFY(splitter.sizes()[0] >= 390);
        QVERIFY(!title->text().isEmpty());
        for (const auto& name : order) QVERIFY(!list->findChild<QWidget*>(name)->isHidden());
    }

    void actionTooltipsAppearImmediatelyAndClicksKeepTheirMeaning() {
        ChannelList list; list.resize(340, 180); list.show(); QApplication::processEvents();
        QSignalSpy created(&list, &ChannelList::createGroupRequested), joined(&list, &ChannelList::joinRequested);
        QSignalSpy monitored(&list, &ChannelList::connMonitorRequested);
        QSignalSpy opened(&list, &ChannelList::openLinkRequested);
        for (const char* name : {"createGroupButton", "joinGroupButton", "connectionMonitorButton", "openLinkButton"}) {
            auto* button = list.findChild<QPushButton*>(name); QVERIFY(button);
            QVERIFY(button->isVisible()); QVERIFY(!button->icon().isNull());
            const QPoint center = button->rect().center();
            QEnterEvent enter(center, button->mapTo(&list, center), button->mapToGlobal(center));
            QApplication::sendEvent(button, &enter);
            // Проверяем синхронно, без ожидания стандартного tooltip timeout.
            QCOMPARE(QToolTip::text(), button->toolTip()); QVERIFY(QToolTip::isVisible());
            QTest::mouseClick(button, Qt::LeftButton);
            QCOMPARE(created.count(), 1); QCOMPARE(joined.count(), name == QByteArray("createGroupButton") ? 0 : 1);
            QCOMPARE(monitored.count(), name == QByteArray("connectionMonitorButton") || name == QByteArray("openLinkButton") ? 1 : 0);
            QCOMPARE(opened.count(), name == QByteArray("openLinkButton") ? 1 : 0);
            QEvent leave(QEvent::Leave); QApplication::sendEvent(button, &leave);
            QTRY_VERIFY(!QToolTip::isVisible());
        }
    }

    void reloadKeepsConnectionState() {
        ListCore core; QVERIFY(core.start());
        ChannelList list(&core.db); list.loadChannels();
        auto* view = list.findChild<QListView*>(); QVERIFY(view);
        list.setChannelPeersOnline(core.channel, 3);
        list.setChannelPeersOnline(core.channel, -1);
        list.setAutoConnectActive(true);
        const auto dm = core.db.getDmConversations()[0];
        list.selectChannel("dm:" + dm.convId);
        QVERIFY(!view->currentIndex().data(ChannelAutoConnectActiveRole).toBool());
        list.selectChannel(core.channel);
        list.loadChannels();
        QCOMPARE(view->currentIndex().data(ChannelPeersOnlineRole).toInt(), 3);
        QVERIFY(view->currentIndex().data(ChannelAutoConnectActiveRole).toBool());
        QSignalSpy selected(&list, &ChannelList::channelSelected);
        list.selectChannel(QString());
        QCOMPARE(view->currentIndex().data(ChannelChannelIdRole).toString(), core.channel);
        QVERIFY(selected.isEmpty());
    }

    void fileNavigationCoalescesPreservesSelectionAndIgnoresStoppedResults() {
        QTemporaryDir directory; QVERIFY(directory.isValid()); ListCore core; QVERIFY(core.start(directory.path()));
        navigationReads = 0; failNavigationRead = false;
        int guiSqlReads = 0;
        struct Observer { ListCore* core; int* reads; } observer{&core, &guiSqlReads};
        QCOMPARE(sqlite3_set_authorizer(core.inst->topo_sqlite_db,
            [](void* data, int action, const char*, const char*, const char*, const char*) {
                auto* observer = static_cast<Observer*>(data);
                if (action == SQLITE_READ && !observer->core->polling) (*observer->reads)++;
                return SQLITE_OK;
            }, &observer), SQLITE_OK);
        auto reset = qScopeGuard([&] { sqlite3_set_authorizer(core.inst->topo_sqlite_db, nullptr, nullptr); });
        ChannelList list(&core.db); auto* view = list.findChild<QListView*>(); QVERIFY(view);
        list.loadChannels(); QCOMPARE(view->model()->rowCount(), 0); list.selectChannel(core.channel);
        for (int i = 0; i < 20; i++) list.loadChannels();
        QTRY_COMPARE(view->model()->rowCount(), 3); QCOMPARE(navigationReads, 2);
        bool initialBarrier = false;
        QVERIFY(core.db.integrityCheckAsync(false, &list, [&](IntegrityResult result) { QVERIFY(result.ok); initialBarrier = true; }));
        QTRY_VERIFY(initialBarrier);
        QCOMPARE(view->currentIndex().data(ChannelChannelIdRole).toString(), core.channel);
        const QString conversation = view->model()->index(2, 0).data(ChannelChannelIdRole).toString(); QVERIFY(conversation.startsWith("dm:"));
        QSignalSpy changed(view->model(), &QAbstractItemModel::modelReset);
        list.loadChannels(); list.selectChannel(conversation); list.setChannelPeersOnline(core.channel, 9);
        QTRY_COMPARE(changed.count(), 1); QCOMPARE(navigationReads, 4);
        QCOMPARE(view->currentIndex().data(ChannelChannelIdRole).toString(), conversation);
        QCOMPARE(view->model()->index(0, 0).data(ChannelPeersOnlineRole).toInt(), 9);
        failNavigationRead = true; list.loadChannels(); bool barrier = false;
        QVERIFY(core.db.integrityCheckAsync(false, &list, [&](IntegrityResult result) { QVERIFY(result.ok); barrier = true; }));
        QTRY_VERIFY(barrier); QCOMPARE(changed.count(), 1); QCOMPARE(view->model()->rowCount(), 3);
        // Старый Qt-result не должен очистить флаг нового READ после повторного подключения того же handle.
        list.loadChannels(); core.db.setDb(nullptr); list.loadChannels(); QCOMPARE(view->model()->rowCount(), 0);
        core.db.setDb(core.inst->topo_sqlite_db); list.loadChannels(); list.selectChannel(core.channel);
        QTRY_COMPARE(view->model()->rowCount(), 3); QCOMPARE(view->currentIndex().data(ChannelChannelIdRole).toString(), core.channel);
        QCOMPARE(guiSqlReads, 0);
    }

    void repeatedUpdatesStillPublishSnapshotsAndKeepThePendingSelection() {
        QTemporaryDir directory; QVERIFY(directory.isValid()); ListCore core; QVERIFY(core.start(directory.path()));
        auto gate = std::make_shared<NavigationGate>();
        auto release = qScopeGuard([gate] { gate->release(std::numeric_limits<int>::max()); navigationGate.reset(); });
        navigationGate = gate; ChannelList list(&core.db); auto* view = list.findChild<QListView*>(); QVERIFY(view);
        list.loadChannels(); QTRY_COMPARE(gate->entered.load(), 1);
        for (int i = 0; i < 20; i++) list.loadChannels();
        gate->release(1); QTRY_COMPARE(gate->entered.load(), 2);
        // Следующее чтение уже задержано: первый согласованный снимок обязан быть виден под непрерывными обновлениями.
        QCOMPARE(view->model()->rowCount(), 3);
        const auto created = chat_core_create_channel_auto(core.inst, "Next"); QVERIFY(created);
        const auto target = QString::fromLatin1(topo_groups_channel(core.inst->topo_groups, created)); list.loadChannels(); list.selectChannel(target);
        gate->release(2); QTRY_COMPARE(gate->entered.load(), 3); QCOMPARE(view->model()->rowCount(), 3);
        gate->release(3); QTRY_COMPARE(view->model()->rowCount(), 4);
        QCOMPARE(view->currentIndex().data(ChannelChannelIdRole).toString(), target);
    }

    void unreadNotificationIsUnique() {
        ListCore core; QVERIFY(core.start());
        ChannelList list(&core.db); list.loadChannels();
        auto* view = list.findChild<QListView*>(); QVERIFY(view);
        QSignalSpy changes(view->model(), &QAbstractItemModel::dataChanged);
        list.setChannelUnreadCount(core.channel, 7);
        QCOMPARE(list.getChannelUnreadCount(core.channel), 7);
        QCOMPARE(changes.count(), 1);
        list.setChannelUnreadCount(core.channel, 7);
        QCOMPARE(changes.count(), 1);
        list.setChannelUnreadCount(core.channel, -1);
        QCOMPARE(list.getChannelUnreadCount(core.channel), 7);
        QCOMPARE(changes.count(), 1);
    }

    void presenceRequestsAreCancelledWithoutLeaks() {
        ListCore core; QVERIFY(core.start());
        ChannelList list(&core.db);
        UTUN_INSTANCE instance{};
        instance.ua = uasync_create(); QVERIFY(instance.ua);
        auto release = qScopeGuard([&] {
            gui_bridge_set_inst(nullptr); gui_bridge_set_uasync(nullptr); uasync_destroy(instance.ua, 1);
        });
        uasync_mark_running(instance.ua);
        gui_bridge_set_uasync(instance.ua); gui_bridge_set_inst(&instance);
        list.loadChannels();
        const auto allocations = u_get_allocated_count();
        list.refreshDmPresence(); list.refreshDmPresence();
        QTest::qWait(100);
        QVERIFY(instance.ua->posted_tasks_head != nullptr);
        gui_bridge_set_inst(nullptr);
        QVERIFY(instance.ua->posted_tasks_head == nullptr);
        QCOMPARE(u_get_allocated_count(), allocations);
    }

    void menuAction_data() {
        QTest::addColumn<bool>("dm"); QTest::addColumn<int>("change");
        QTest::newRow("group-current") << false << 0;
        QTest::newRow("group-removed") << false << 1;
        QTest::newRow("pm-current") << true << 0;
        QTest::newRow("pm-removed") << true << 1;
        QTest::newRow("pm-generation-changed") << true << 2;
        QTest::newRow("pm-deletion-started") << true << 3;
    }

    void menuAction() {
        QFETCH(bool, dm); QFETCH(int, change);
        ListCore core; QVERIFY(core.start());
        ChannelList list(&core.db); list.loadChannels(); list.resize(300, 400); list.show();
        const auto conversation = core.db.getDmConversations()[0];
        const QString target = dm ? "dm:" + conversation.convId : core.channel;
        list.selectChannel(target);
        auto* view = list.findChild<QListView*>(); QVERIFY(view);
        QSignalSpy removed(&list, &ChannelList::dmDeleteRequested);
        QSignalSpy files(&list, &ChannelList::filesRequested);
        QTimer::singleShot(0, &list, [&] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget()); QVERIFY(menu);
            QCOMPARE(menu->actions().size(), 1); QVERIFY(menu->actions()[0]->isEnabled());
            if (change == 1) view->model()->removeRow(view->currentIndex().row());
            if (change == 2)
                view->model()->setData(view->currentIndex(), QVariant::fromValue(conversation.generation + 1), ChannelDmGenerationRole);
            if (change == 3) list.setDmDeleting(conversation.convId, conversation.generation, true);
            QTest::keyClick(menu, Qt::Key_Down); QTest::keyClick(menu, Qt::Key_Return);
        });
        view->customContextMenuRequested(view->visualRect(view->currentIndex()).center());
        QCOMPARE(removed.count(), int(dm && !change));
        QCOMPARE(files.count(), int(!dm && !change));
        if (dm && !change) {
            QCOMPARE(removed[0][0].toString(), conversation.convId);
            QCOMPARE(removed[0][1].toULongLong(), conversation.generation);
        } else if (!dm && !change) QCOMPARE(files[0][0].toString(), target);
    }

    void menuSurvivesOwnerDeletion() {
        ListCore core; QVERIFY(core.start());
        QPointer<ChannelList> list = new ChannelList(&core.db);
        list->loadChannels(); list->resize(300, 400); list->show();
        auto* view = list->findChild<QListView*>(); QVERIFY(view);
        const QPoint point = view->visualRect(view->currentIndex()).center();
        QTimer::singleShot(0, [&] { delete list.data(); });
        view->customContextMenuRequested(point);
        QVERIFY(!list);
    }

    void borrowedDbCanDisappear() {
        QPointer<DbManager> db = new DbManager;
        ChannelList list(db); delete db.data();
        list.loadChannels(); list.refreshDmPresence();
        QTest::qWait(100);
        QCOMPARE(list.findChild<QListView*>()->model()->rowCount(), 0);
    }
};

QTEST_MAIN(ChannelListTest)
#include "test_channel_list.moc"

#include "kanbanview.h"
#include <QStackedWidget>
#include "devgroupdialog.h"
#include <QCoreApplication>
#include "mainwindow.h"
#include "../../../src/chat/chat_local.h"
extern "C" {
#include "../../../src/utun_instance.h"
}
#include "single_instance.h"
#include "streamplayer_window.h"
#include "stream_playback_preferences.h"
#include "streamgrid_window.h"
#include "stream_publish_engine.h"
#include "stream_publish_window.h"
#include "video_recording_window.h"
#include "phone_media_dispatcher.h"
#include "channellist.h"
#include "clipboard_exchange.h"
#include "messagelist.h"
#include "messagedelegate.h"
#include "accountlist.h"
#include "memberlistmodel.h"
#include "invitedialog.h"
#include "dminvitedialog.h"
#include "debug_ui.h"
#include "inviteby.h"
#include <QInputDialog>
#include "joindialog.h"
#include "settingsdialog.h"
#include "channelsettingsdialog.h"
#include "connmonitorwindow.h"
#include "networktrafficwindow.h"
#include "videoplayer_window.h"
#include "imageviewer_window.h"
#include "member_files/browser.h"
#include "avatars/avatarmanager.h"
#include "avatars/groupavatarmanager.h"
extern "C" {
#include "chat/chat_member.h"
}
#include "creategroupdialog.h"
#include "invite_link.h"
#include "../../../src/chat/object_link.h"
#include "sound_manager.h"
#include "voiceplayback.h"
#include "audiorecorder.h"
#include "voicemessageencoder.h"
#include "callwindow.h"
#include "call_audio_engine.h"
#include "radio_audio_engine.h"
#include "ptt_hotkey_manager.h"
#include "message_notification.h"

extern "C" {
#include "../../lib/audio_compressor.h"
}
#include "animtimer.h"
#include "../db/db_manager.h"
#include "../transport/utun_node.h"
#include "../transport/gui_bridge.h"
#include <QSplitter>
#include <QCloseEvent>
#include <QElapsedTimer>
#include <QApplication>
#include <QScreen>
#include <QWindow>
#include <QClipboard>
#include <QStyle>
#include <QMessageBox>
#include <QPointer>
#include <QScopeGuard>
#include <QPushButton>
#include <QDesktopServices>
#include <QUrl>
#include <QDebug>
#include <QFile>
#include <QTextStream>
#include <QDir>
#ifdef _WIN32
#include <QFileInfo>
#endif
#include <cmath>
#include <utility>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <csignal>

extern "C" {
#include "chat/chat_core.h"
#include "transport_layer/secure_channel.h"
#include "chat/chat_attachment.h"
#include "media_async/attachment_send.h"
#include "call/call.h"
#include "call/call_proto.h"
#include "radio/radio_call.h"
#include "dm/dm_core.h"
#include "../../lib/mem.h"
#include "../../lib/debug_config.h"
}

static MainWindow* s_mainWindow = nullptr;

#ifndef _WIN32
static volatile std::sig_atomic_t g_signalFlag = 0;
static void unixSignalHandler(int) { g_signalFlag = 1; }
#endif

/* Bridge копирует key/value и владеет запросом до записи ui_state в utun-потоке либо отмены. */
static void postUiStateKv(const char* key, const QString& value) {
    if (gui_bridge_post_setting(GUI_SETTING_UI_STATE, key, value.toUtf8().constData()) < 0)
        DEBUG_WARN(DEBUG_CATEGORY_CONFIG, "UI setting was not queued key=%s", key);
}

static void saveLastChannelId(const QString& channelId) {
    postUiStateKv("last_channel_id", channelId);
}

static void onMsgReceivedCallback(const char* ch_id, int ch_id_len, const uint8_t author[32], int initial_sync) {
    if (s_mainWindow)
        s_mainWindow->onMessageReceived(ch_id, ch_id_len, author, initial_sync != 0);
}

static void onChannelUpdatedCallback(const char* ch_id, int ch_id_len) {
    Q_UNUSED(ch_id_len);
    Q_UNUSED(ch_id);
    if (s_mainWindow)
        s_mainWindow->reloadChannels();
    if (AvatarManager::instance()) AvatarManager::instance()->refreshMembers();
}

static void onMemberListCallback(const uint8_t* data, int len) {
    if (len < 3) return;
    uint8_t cl = data[0];
    if (len < 1 + (int)cl + 2) return;
    uint16_t count; memcpy(&count, data + 1 + cl, 2);
    if (s_mainWindow)
        s_mainWindow->onMemberList(QString::fromUtf8((const char*)data + 1, cl), count, data + 1 + cl + 2, len - 1 - cl - 2);
}

static void onMemberUpdatedCallback(const uint8_t* data, int len) {
    if (len < 1) return;
    uint8_t cl = data[0];
    if (len < 1 + (int)cl + 1 + CHAT_MEMBER_DISPLAY_SIZE) return;
    if (s_mainWindow)
        s_mainWindow->onMemberUpdated(QString::fromUtf8((const char*)data + 1, cl), data + 1 + cl + 1);
}

static void onMemberRemovedCallback(const uint8_t* data, int len) {
    if (len < 1) return;
    uint8_t cl = data[0];
    if (!cl || len != 1 + (int)cl + 32) return;
    if (s_mainWindow)
        s_mainWindow->onMemberRemoved(QString::fromUtf8((const char*)data + 1, cl), QByteArray((const char*)data + 1 + cl, 32));
}

static void onAutoConnectStatusCallback(uint8_t status, uint16_t total_tried,
                                         uint16_t node_count, uint16_t connected) {
    if (s_mainWindow)
        s_mainWindow->onAutoConnectStatus(status, total_tried, node_count, connected);
}

static void onChannelPeersOnlineCallback(const char* ch_id, int ch_id_len, uint16_t online) {
    if (s_mainWindow)
        s_mainWindow->onChannelPeersOnline(ch_id, ch_id_len, online);
}

MainWindow::MainWindow(QWidget *parent, DbManager* db, const QString& cfgPath,
                       const QString& dbPath, const QString& debugFile,
                       const QString& debugLevel, const QString& debugCategories)
    : QMainWindow(parent)
    , m_channelList(nullptr), m_messageList(nullptr), m_accountList(nullptr)
    , m_trayIcon(nullptr), m_trayMenu(nullptr)
    , m_db(db), m_cfgPath(cfgPath), m_cfgDbPath(dbPath)
    , m_cfgDebugFile(debugFile), m_cfgDebugLevel(debugLevel)
    , m_cfgDebugCategories(debugCategories)
    , m_recorder(nullptr)
    , m_autoPlayTimer(new QTimer(this))
{
    setWindowTitle("vibechat");
    resize(900, 600);
    if (m_db) m_db->setDbPath(m_cfgDbPath);
    new AvatarManager(m_db, this);
    new GroupAvatarManager(m_db, this);
    setupUi();
    setupTray();
    initDevgroupUi(this);
    setupSoundFromConfig();
    if (m_db) {
        setupBridgeCallbacks();
        setupMessaging();
        setupNode();
        if (m_node->isRunning()) setupConnects();
    }
    connect(m_autoPlayTimer, &QTimer::timeout, this, &MainWindow::autoPlayTimerTick);
    m_cpuLoadTimer = new QTimer(this);
    m_cpuLoadTimer->setTimerType(Qt::PreciseTimer);
    connect(m_cpuLoadTimer, &QTimer::timeout, this, [this] {
        if (!m_node || !m_node->isRunning() || !gui_bridge_is_uasync_ready()) {
            m_cpuLoadPending = false; m_channelList->setCpuLoad(0); return;
        }
        if (m_cpuLoadPending) return;
        m_cpuLoadPending = gui_bridge_request_cpu_load() != 0;
    });
    m_cpuLoadTimer->start(333);
    DEBUG_DEBUG(DEBUG_CATEGORY_SYS, "desktop CPU indicator polling started: period=333ms");

#ifndef _WIN32
    std::signal(SIGINT, unixSignalHandler);
    std::signal(SIGTERM, unixSignalHandler);
    m_shutdownTimer = new QTimer(this);
    connect(m_shutdownTimer, &QTimer::timeout, this, &MainWindow::onShutdownCheck);
    m_shutdownTimer->start(200);
#endif
}

MainWindow::~MainWindow() {
    delete m_videoRecording.data();
    delete m_phoneMedia; m_phoneMedia = nullptr;
    delete m_videoPlayer.data();
    delete m_imageViewer.data();
    gui_bridge_set_radio_call_cb(nullptr);
    if (m_radioCallWindow) { m_radioCallId = 0; delete m_radioCallWindow.data(); }
    if (m_streamPublishWindow) delete m_streamPublishWindow.data();
    delete m_streamPublisher; m_streamPublisher = nullptr;
    if (m_streamPlayer) delete m_streamPlayer.data();
    if (m_streamGrid) delete m_streamGrid.data();
    gui_bridge_set_stream_sources_cb(nullptr);
    s_mainWindow = nullptr;
    m_cpuLoadTimer->stop();
    gui_bridge_set_cpu_load_cb(nullptr);
    gui_bridge_set_local_invite_cb(nullptr);
    DEBUG_DEBUG(DEBUG_CATEGORY_SYS, "desktop CPU indicator polling stopped");
    if (m_messageNotification) m_messageNotification->dismiss("shutdown");
    m_messageNotification.reset();
    delete AvatarManager::instance();
    delete GroupAvatarManager::instance();
    gui_bridge_set_group_avatar_result_cb(nullptr);
    gui_bridge_set_feedback_changed_cb(nullptr);
    gui_bridge_set_attachment_keys_cb(nullptr);
    gui_bridge_set_feedback_result_cb(nullptr);
    gui_bridge_set_msg_submitted_cb(nullptr);
    gui_bridge_set_avatar_result_cb(nullptr);
    if (m_fileBrowser) delete m_fileBrowser.data();
    gui_bridge_set_member_files_response_cb(nullptr);
    gui_bridge_set_member_files_transfer_cb(nullptr);
    m_autoPlayTimer->stop();
    gui_bridge_set_network_changed_cb(nullptr);
    gui_bridge_set_dm_call_route_cb(nullptr);
    gui_bridge_set_dm_conv_updated_cb(nullptr);
    gui_bridge_set_dm_start_result_cb(nullptr);
    gui_bridge_set_dm_msg_received_cb(nullptr);
    gui_bridge_set_dm_messages_cb(nullptr);
    gui_bridge_set_dm_members_cb(nullptr);
    gui_bridge_set_dm_delete_result_cb(nullptr);
    gui_bridge_set_msg_received_cb(nullptr);
    gui_bridge_set_channel_updated_cb(nullptr);
    gui_bridge_set_member_list_cb(nullptr);
    gui_bridge_set_member_updated_cb(nullptr);
    gui_bridge_set_member_removed_cb(nullptr);
    gui_bridge_set_radio_talk_cb(nullptr);
    gui_bridge_set_auto_connect_status_cb(nullptr);
    gui_bridge_set_channel_peers_online_cb(nullptr);
    gui_bridge_set_db_ready_cb(nullptr);
    gui_bridge_set_attachment_downloaded_cb(nullptr);
    CallAudioEngine::instance()->stop();
    RadioAudioEngine::instance()->stop();
    if (m_recorder) { m_recorder->shutdown(); m_recorder = nullptr; }
    SoundManager::instance()->shutdown();
    DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "MainWindow: ~MainWindow — saving channel state");
    if (m_messageList && m_node && m_node->isRunning()) m_messageList->saveChannelState();
    if (m_kanban && m_node && m_node->isRunning()) m_kanban->finishEditing();
    DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "MainWindow: ~MainWindow — stopping utun node");
    if (m_node) { m_node->stop(); delete m_node; m_node = nullptr; }
    DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "MainWindow: ~MainWindow — node stop+delete done");
    AnimTimer::shutdown();
    MessageList::cleanupAnimations();
    DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "MainWindow: ~MainWindow — animations cleaned");
    DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "MainWindow: ~MainWindow done");
}

void MainWindow::setupUi() {
    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setObjectName(QStringLiteral("mainColumns"));
    splitter->setHandleWidth(1);
    splitter->setStyleSheet("QSplitter#mainColumns::handle:horizontal { background: palette(mid); }");
    m_splitter = splitter;

    m_leftColumns = new QSplitter(Qt::Vertical, splitter);
    m_leftColumns->setObjectName("kanbanLeftSections"); m_leftColumns->setHandleWidth(1);
    m_channelList = new ChannelList(m_db, m_leftColumns);
    m_recorder = new AudioRecorder(this);
    m_contentStack = new QStackedWidget(splitter);
    m_messageList = new MessageList(m_db, m_recorder, m_contentStack);
    m_kanban = new KanbanView(m_contentStack, m_db);
    m_unknownGroup = new QLabel(QCoreApplication::translate("ChatGui", "Waiting for group information…"), m_contentStack);
    m_contentStack->addWidget(m_messageList); m_contentStack->addWidget(m_kanban); m_contentStack->addWidget(m_unknownGroup);
    connect(m_kanban, &KanbanView::inviteRequested, this, &MainWindow::showShareInviteDialog);
    connect(m_kanban, &KanbanView::settingsRequested, this, &MainWindow::showChannelSettings);
    m_rightColumns = new QStackedWidget(splitter);
    m_accountList = new AccountList(m_db, m_rightColumns);
    m_rightColumns->addWidget(m_accountList); m_rightColumns->addWidget(m_kanban->detailsPanel());
    m_rightColumns->setCurrentWidget(m_accountList);
    m_kanban->setMembers(m_accountList->model());
    m_accountList->setAssignmentState([board = QPointer<KanbanView>(m_kanban)](const QString& ed) { return board ? board->assignmentState(ed) : -1; });
    connect(m_accountList, &AccountList::assignmentRequested, m_kanban, &KanbanView::toggleAssignee);
    connect(m_kanban, &KanbanView::assignmentsChanged, m_accountList, &AccountList::refreshAssignments);
    m_messageList->setRadioMembers(m_accountList->model());

    splitter->addWidget(m_leftColumns);
    splitter->addWidget(m_contentStack);
    splitter->addWidget(m_rightColumns);
    connect(m_kanban, &KanbanView::cardSelected, this, [this](bool selected) {
        if (selected == m_cardLayout) return;
        if (m_cardLayout) m_cardColumnSizes = m_splitter->sizes(); else m_normalColumnSizes = m_splitter->sizes();
        m_cardLayout = selected;
        m_accountList->setDetailsEnabled(!selected);
        if (selected) {
            m_leftColumns->addWidget(m_accountList); m_accountList->show();
            m_leftColumns->setSizes(m_cardLeftSizes.size() == 2 ? m_cardLeftSizes :
                QList<int>{m_leftColumns->height() / 2, m_leftColumns->height() / 2});
            m_rightColumns->setCurrentWidget(m_kanban->detailsPanel()); m_kanban->detailsPanel()->show();
        } else {
            m_cardLeftSizes = m_leftColumns->sizes();
            m_rightColumns->addWidget(m_accountList); m_rightColumns->setCurrentWidget(m_accountList);
        }
        const auto sizes = selected ? m_cardColumnSizes : m_normalColumnSizes;
        if (sizes.size() == 3) m_splitter->setSizes(sizes);
        else if (selected) m_splitter->setSizes({220, qMax(300, width() - 600), 380});
        DEBUG_DEBUG(DEBUG_CATEGORY_KANBAN, "desktop card layout selected=%d members_left=%d member_details=%d", selected, selected, !selected);
    });

    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 5);
    splitter->setStretchFactor(2, 1);

    splitter->setSizes({220, 480, 180});

    setCentralWidget(splitter);
    DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "Main columns: continuous vertical separators width=1px color=palette(mid), including headers and details");
}

void MainWindow::saveWindowGeometry() {
    QRect g = normalGeometry();
    if (g.isEmpty()) g = geometry();
    postUiStateKv("window_geometry",
        QStringLiteral("%1,%2,%3,%4").arg(g.width()).arg(g.height()).arg(g.x()).arg(g.y()));
    postUiStateKv("window_maximized", isMaximized() ? QStringLiteral("1") : QStringLiteral("0"));

    if (m_splitter) {
        const auto normal = m_cardLayout ? m_normalColumnSizes : m_splitter->sizes();
        const auto card = m_cardLayout ? m_splitter->sizes() : m_cardColumnSizes;
        if (normal.size() == 3) postUiStateKv("window_splitter", QStringLiteral("%1,%2,%3").arg(normal[0]).arg(normal[1]).arg(normal[2]));
        if (card.size() == 3) postUiStateKv("window_splitter_card", QStringLiteral("%1,%2,%3").arg(card[0]).arg(card[1]).arg(card[2]));
        const auto left = m_cardLayout ? m_leftColumns->sizes() : m_cardLeftSizes;
        if (left.size() == 2) postUiStateKv("window_splitter_card_left", QStringLiteral("%1,%2").arg(left[0]).arg(left[1]));
    }
}

void MainWindow::restoreWindowGeometry() {
    if (!m_db) return;
    const QStringList p = m_db->getUiState("window_geometry").split(',');
    if (p.size() != 4) return;
    int w = p[0].toInt(), h = p[1].toInt(), x = p[2].toInt(), y = p[3].toInt();
    if (w <= 0 || h <= 0) return;

    QRect target(x, y, w, h);
    bool visible = false;
    for (const QScreen* s : QGuiApplication::screens()) {
        if (s->availableGeometry().intersects(target)) { visible = true; break; }
    }
    if (!visible) target.moveTo(0, 0);

    setGeometry(target);
    // Поздний ответ БД сохраняет свёрнутость при --minimized и не показывает скрытое в tray окно.
    if (m_db->getUiStateInt("window_maximized", 0))
        setWindowState(windowState() | Qt::WindowMaximized);
    GUI_DEBUG("window geometry restored: maximized=%d minimized=%d visible=%d", isMaximized(), isMinimized(), isVisible());

    if (m_splitter) {
        const auto leftSizes = m_db->getUiState("window_splitter_card_left").split(',');
        if (leftSizes.size() == 2 && leftSizes[0].toInt() > 0 && leftSizes[1].toInt() > 0)
            m_cardLeftSizes = {leftSizes[0].toInt(), leftSizes[1].toInt()};
        const QStringList cardSizes = m_db->getUiState("window_splitter_card").split(',');
        if (cardSizes.size() == 3 && cardSizes[0].toInt() > 0 && cardSizes[1].toInt() > 0 && cardSizes[2].toInt() > 0)
            m_cardColumnSizes = {cardSizes[0].toInt(), cardSizes[1].toInt(), cardSizes[2].toInt()};
        const QStringList sp = m_db->getUiState("window_splitter").split(',');
        if (sp.size() == 3) {
            int s0 = sp[0].toInt(), s1 = sp[1].toInt(), s2 = sp[2].toInt();
            if (s0 > 0 && s1 > 0 && s2 > 0)
                m_splitter->setSizes({s0, s1, s2});
        }
    }
}

/* Один снимок на профиль; кэш обновляется до асинхронной записи, поэтому быстрое переоткрытие видит новую геометрию. */
void MainWindow::saveMediaWindowGeometry(QWidget* window) {
    const QRect normal = window->normalGeometry();
    const QScreen* display = window->screen();
    if (!normal.isValid() || !display) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "MediaWindow: cannot save geometry window=%s valid=%d screen=%d",
                    qPrintable(window->objectName()), normal.isValid(), display != nullptr);
        return;
    }
    const QString mode = window->isFullScreen() ? "fullscreen" : window->isMaximized() ? "maximized" : "normal";
    m_mediaWindowState = {{"x", normal.x()}, {"y", normal.y()}, {"width", normal.width()}, {"height", normal.height()},
        {"mode", mode}, {"screen", display->name()}, {"serial", display->serialNumber()},
        {"screen_x", display->availableGeometry().x()}, {"screen_y", display->availableGeometry().y()}};
    m_mediaWindowStateLoaded = true;
    const QByteArray value = QJsonDocument(m_mediaWindowState).toJson(QJsonDocument::Compact);
    const int queued = gui_bridge_post_setting(GUI_SETTING_UI_STATE, "media_window_geometry", value.constData());
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "MediaWindow: cached %s rect=%d,%d %dx%d mode=%s monitor=%s serial=%s queued=%d",
               qPrintable(window->objectName()), normal.x(), normal.y(), normal.width(), normal.height(), qPrintable(mode),
               qPrintable(display->name()), qPrintable(display->serialNumber()), queued == 0);
    if (queued < 0) DEBUG_WARN(DEBUG_CATEGORY_CONFIG, "MediaWindow: geometry remains cached; database write was not queued");
}

/* Имя/serial определяют физический монитор; относительное смещение сохраняется и при перестановке мониторов. */
void MainWindow::restoreMediaWindowGeometry(QWidget* window) {
    if (!m_mediaWindowStateLoaded && m_db && m_db->isOpen()) {
        m_mediaWindowStateLoaded = true;
        const QByteArray value = m_db->getUiState("media_window_geometry").toUtf8();
        if (!value.isEmpty()) {
            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(value, &error);
            if (error.error != QJsonParseError::NoError || !document.isObject())
                DEBUG_WARN(DEBUG_CATEGORY_CONFIG, "MediaWindow: invalid saved state bytes=%d error=%s", int(value.size()),
                           qPrintable(error.errorString()));
            else m_mediaWindowState = document.object();
        }
    }
    const QString mode = m_mediaWindowState.value("mode").toString();
    const int savedWidth = m_mediaWindowState.value("width").toInt();
    const int savedHeight = m_mediaWindowState.value("height").toInt();
    const bool saved = savedWidth > 0 && savedHeight > 0 && (mode == "normal" || mode == "maximized" || mode == "fullscreen");
    if (!m_mediaWindowState.isEmpty() && !saved)
        DEBUG_WARN(DEBUG_CATEGORY_CONFIG, "MediaWindow: invalid saved geometry width=%d height=%d mode=%s",
                   savedWidth, savedHeight, qPrintable(mode));
    const QString name = m_mediaWindowState.value("screen").toString();
    const QString serial = m_mediaWindowState.value("serial").toString();
    QScreen* display = nullptr;
    if (saved) {
        for (auto* candidate : QGuiApplication::screens()) {
            if (candidate->name() == name && (serial.isEmpty() || candidate->serialNumber() == serial)) {
                display = candidate; break;
            }
        }
        if (!display && !serial.isEmpty()) {
            for (auto* candidate : QGuiApplication::screens()) {
                if (candidate->serialNumber() == serial) { display = candidate; break; }
            }
        }
    }
    if (!display) {
        display = screen() ? screen() : QGuiApplication::primaryScreen();
        if (saved) DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "MediaWindow: saved monitor unavailable name=%s serial=%s fallback=%s",
                             qPrintable(name), qPrintable(serial), display ? qPrintable(display->name()) : "none");
    }
    if (!display) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "MediaWindow: no physical screen available for restoration"); return; }
    const QRect available = display->availableGeometry();
    if (!available.isValid()) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "MediaWindow: monitor has no available area name=%s", qPrintable(display->name())); return;
    }
    window->winId();
    window->windowHandle()->setScreen(display);
    const QMargins margins = window->windowHandle()->frameMargins();
    const QRect bounds = available.marginsRemoved(margins);
    if (!bounds.isValid()) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "MediaWindow: monitor area cannot fit window frame name=%s frame=%d,%d,%d,%d",
                    qPrintable(display->name()), margins.left(), margins.top(), margins.right(), margins.bottom()); return;
    }
    window->setMinimumSize(window->minimumSize().boundedTo(bounds.size()));
    const QSize size = QSize(saved ? savedWidth : 800, saved ? savedHeight : 600)
        .expandedTo(window->minimumSize()).boundedTo(bounds.size());
    qint64 x = bounds.x() + (bounds.width() - size.width()) / 2;
    qint64 y = bounds.y() + (bounds.height() - size.height()) / 2;
    if (saved) {
        x = qint64(m_mediaWindowState.value("x").toInt()) - m_mediaWindowState.value("screen_x").toInt() + available.x();
        y = qint64(m_mediaWindowState.value("y").toInt()) - m_mediaWindowState.value("screen_y").toInt() + available.y();
    }
    const QRect target(int(qBound(qint64(bounds.left()), x, qint64(bounds.right() - size.width() + 1))),
                       int(qBound(qint64(bounds.top()), y, qint64(bounds.bottom() - size.height() + 1))), size.width(), size.height());
    if (saved && (target.x() != x || target.y() != y || size != QSize(savedWidth, savedHeight)))
        DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "MediaWindow: fitted saved rect into monitor=%s area=%d,%d %dx%d",
                   qPrintable(display->name()), available.x(), available.y(), available.width(), available.height());
    window->setGeometry(target);
    window->setWindowState(!saved || mode == "normal" ? Qt::WindowNoState
        : mode == "fullscreen" ? Qt::WindowFullScreen : Qt::WindowMaximized);
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "MediaWindow: restored %s saved=%d rect=%d,%d %dx%d mode=%s monitor=%s serial=%s",
               qPrintable(window->objectName()), saved, target.x(), target.y(), target.width(), target.height(),
               saved ? qPrintable(mode) : "normal", qPrintable(display->name()), qPrintable(display->serialNumber()));
}

void MainWindow::setupNode() {
    m_node = new UtunNode(this);
    connect(m_node, &UtunNode::error, this, [](const QString& msg) {
        qWarning("UtunNode error: %s", qPrintable(msg));
    });
    connect(m_node, &UtunNode::started, this, [this]() {
        qDebug("UtunNode started");
    });
    connect(m_node, &UtunNode::stopping, this, [this] {
        ++m_objectLinkRevision; m_pendingObjectLink.clear(); m_objectLinkUrl.clear();
        m_autoPlayTimer->stop(); m_autoPlayQueue.clear(); m_autoPlayDownloading.clear(); m_lastAutoPlayId.clear();
        MessageDelegate::stopAllAutoPlay();
        if (m_db) m_db->setDb(nullptr);
        loadGroupContent(QString()); m_accountList->setChannel(QString()); m_currentChannelId.clear();
        DEBUG_INFO(DEBUG_CATEGORY_SYS, "desktop GUI database detached before core teardown");
    });
    connect(m_node, &UtunNode::stopped, this, [this]() {
        qDebug("UtunNode stopped");
        qApp->quit();
    });

    if (!m_cfgDebugFile.isEmpty())
        m_node->setDebugFile(m_cfgDebugFile);
    if (!m_cfgDebugLevel.isEmpty())
        m_node->setDebugLevel(m_cfgDebugLevel);
        if (!m_cfgDebugCategories.isEmpty())
            m_node->setDebugCategories(m_cfgDebugCategories);

    if (!m_cfgDbPath.isEmpty())
        m_node->setDbPath(m_cfgDbPath);

    if (!m_node->start(m_cfgPath))
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "desktop startup failed: %s", qPrintable(m_node->startupError()));
    else
        AnimTimer::instance()->logRegisteredIcons();
}

void MainWindow::setupMessaging() {
    gui_bridge_init();
}

void MainWindow::setupSoundFromConfig() {
    SoundManager* sm = SoundManager::instance();
    if (!sm->init()) return;

    QString soundDir, msgInFile, msgOutFile, connectFile, disconnectFile, callInFile;
    QFile cfgFile(m_cfgPath);
    if (cfgFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        bool inGui = false;
        QTextStream in(&cfgFile);
        while (!in.atEnd()) {
            QString line = in.readLine().trimmed();
            if (line == "[gui]") { inGui = true; continue; }
            if (line.startsWith("[") && line.endsWith("]")) inGui = false;
            if (!inGui) continue;
            if (line.startsWith("sound_dir="))           soundDir = line.mid(10).trimmed();
            else if (line.startsWith("sound_msg_incoming=")) msgInFile = line.mid(19).trimmed();
            else if (line.startsWith("sound_msg_outgoing=")) msgOutFile = line.mid(19).trimmed();
            else if (line.startsWith("sound_connect="))      connectFile = line.mid(14).trimmed();
            else if (line.startsWith("sound_disconnect="))   disconnectFile = line.mid(17).trimmed();
            else if (line.startsWith("sound_call_incoming=")) callInFile = line.mid(20).trimmed();
        }
        cfgFile.close();
    }

    if (soundDir.isEmpty()) soundDir = "sounds";
#ifdef _WIN32
    QDir binDir(QFileInfo(m_cfgPath).absolutePath());
#else
    QDir binDir(QApplication::applicationDirPath());
#endif
    QString baseDir = QDir::isAbsolutePath(soundDir) ? soundDir : binDir.absoluteFilePath(soundDir);
    auto resolvePath = [&baseDir](const QString& f) { return f.isEmpty() ? QString() : QDir::cleanPath(baseDir + "/" + f); };

    if (msgInFile.isEmpty())
        sm->loadSound("msg_incoming", ":/sounds/msg_incoming.mp3");
    else
        sm->loadSound("msg_incoming", resolvePath(msgInFile));

    if (callInFile.isEmpty())
        sm->loadSound("call_incoming", ":/sounds/call_incoming.mp3");
    else
        sm->loadSound("call_incoming", resolvePath(callInFile));

    sm->loadSound("call_connecting", ":/sounds/call_connecting.mp3");
    sm->loadSound("radio_incoming", ":/sounds/radio_incoming.mp3");
    sm->loadSound("call_ringing", ":/sounds/call_ringing.mp3");

    if (!msgOutFile.isEmpty())  sm->loadSound("msg_outgoing", resolvePath(msgOutFile));
    if (!connectFile.isEmpty()) sm->loadSound("connect", resolvePath(connectFile));
    if (!disconnectFile.isEmpty()) sm->loadSound("disconnect", resolvePath(disconnectFile));
    if (m_db && m_db->isOpen()) {
        audio_compressor_set_vad_max_gain(m_db->getUiStateInt("radio_vad_compressor_max_gain_db", audio_compressor_shared_vad_max_gain()));
        audio_compressor_set_shared_settings(m_db->getUiStateInt("compressor_target_level_db", 0),
                                             m_db->getUiStateInt("compressor_gain_only_on_speech", 0));
        sm->setDevices(m_db->getUiState("audio_output_id"), m_db->getUiState("audio_input_id"));
        sm->setEnabled(m_db->getUiStateInt("sound_enabled", 1));
        streamModeSave = [](bool low) { return gui_bridge_post_setting(GUI_SETTING_UI_STATE, "stream_low_latency", low ? "1" : "0") == 0; };
        streamPlaybackModeChanged(m_db->getUiStateInt("stream_low_latency", 0), false);
        sm->setVolume(m_db->getUiStateInt("sound_volume", 80) / 100.0f);
        QString se[] = {"msg_incoming", "msg_outgoing", "connect", "disconnect",
                        "call_incoming", "call_connecting", "call_ringing", "radio_incoming"};
        for (int i = 0; i < 8; i++) {
            const QString& ek = se[i];
            sm->setEvent(ek, m_db->getUiStateInt(QStringLiteral("sound_%1_on").arg(ek), (i == 0 || i >= 4) ? 1 : 0),
                         m_db->getUiStateInt(QStringLiteral("sound_%1_vol").arg(ek), 80) / 100.0f);
        }
    }

    if (m_recorder) {
        m_recorder->init();
        if (m_db && m_db->isOpen()) {
            VoiceEncoder::setPreset(m_db->getUiStateInt("opus_codec_preset", 1));

            int compEnabled = m_db->getUiStateInt("compressor_enabled", 0);
            m_recorder->setCompressorEnabled(compEnabled != 0);
            m_recorder->configureNoiseSuppression(m_db->getUiStateInt("noise_suppression_enabled", 1) != 0,
                                                 m_db->getUiStateInt("noise_suppression_db", SPEEX_NS_DEFAULT_DB));
            {
                audio_compressor_config_t compCfg = {0};
                compCfg.sample_rate = 48000;
                compCfg.channels = 1;
                compCfg.max_gain_db = (float)m_db->getUiStateInt("compressor_max_gain_db", 20);
                compCfg.lookback_ms = m_db->getUiStateInt("compressor_lookback_ms", 200);
                compCfg.lookahead_ms = m_db->getUiStateInt("compressor_lookahead_ms", 100);
                compCfg.rise_rate_per_sec = (float)m_db->getUiStateInt("compressor_rise_rate", 10);
                compCfg.target_level = std::pow(10.0f, m_db->getUiStateInt("compressor_target_level_db", 0) / 20.0f);
                compCfg.use_shared_settings = 1;
                m_recorder->configureCompressor(compCfg);
            }
        }
        DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "AudioRecorder initialized (opusPreset=%d, compressor=%d)",
                   VoiceEncoder::preset(),
                   m_recorder->isCompressorEnabled() ? 1 : 0);
    }
}

void MainWindow::setupBridgeCallbacks() {
    s_mainWindow = this;
    gui_bridge_set_local_invite_cb([](const uint8_t* data, int len) {
        constexpr int header = CHAT_LOCAL_ADDRESS_SIZE + 1;
        struct local_device_address address;
        if (!s_mainWindow || len <= header || header + data[CHAT_LOCAL_ADDRESS_SIZE] >= len ||
            chat_local_address_decode(&address, data)) {
            DEBUG_WARN(DEBUG_CATEGORY_DISCOVERY, "invalid local invitation event bytes=%d", len); return;
        }
        int nameLen = data[CHAT_LOCAL_ADDRESS_SIZE];
        QString name = QString::fromUtf8((const char*)data + header, nameLen);
        QString link = QString::fromUtf8((const char*)data + header + nameLen, len - header - nameLen);
        auto* window = s_mainWindow;
        if (window->findChildren<QMessageBox*>(QStringLiteral("localInvitePrompt")).size() >= 8) {
            DEBUG_WARN(DEBUG_CATEGORY_DISCOVERY, "incoming local invitation prompt limit reached"); return;
        }
        auto* prompt = new QMessageBox(QMessageBox::Question, QCoreApplication::translate("ChatGui", "Invitation"),
            QCoreApplication::translate("ChatGui", "%1 sent an invitation. Connect?").arg(name),
            QMessageBox::Yes | QMessageBox::No, window);
        prompt->setTextFormat(Qt::PlainText); prompt->setAttribute(Qt::WA_DeleteOnClose);
        prompt->setObjectName(QStringLiteral("localInvitePrompt"));
        QObject::connect(prompt, &QMessageBox::finished, window, [window, link](int result) {
            if (result == QMessageBox::Yes) window->onMessageLinkActivated(link);
        });
        prompt->open();
    });
    gui_bridge_set_cpu_load_cb([](int percent) {
        if (!s_mainWindow) return;
        s_mainWindow->m_cpuLoadPending = false;
        s_mainWindow->m_channelList->setCpuLoad(percent);
    });
    gui_bridge_set_member_files_response_cb([](const uint8_t* data, int length) {
        if (s_mainWindow && s_mainWindow->m_fileBrowser) s_mainWindow->m_fileBrowser->onResponse(data, length);
        if (AvatarManager::instance()) AvatarManager::instance()->onResponse(data, length);
    });
    gui_bridge_set_member_files_transfer_cb([](const uint8_t* data, int length) {
        if (s_mainWindow && s_mainWindow->m_fileBrowser) s_mainWindow->m_fileBrowser->onTransfer(data, length);
        if (AvatarManager::instance()) AvatarManager::instance()->onTransfer(data, length);
    });
    gui_bridge_set_avatar_result_cb([](const uint8_t* data, int length) {
        if (AvatarManager::instance()) AvatarManager::instance()->onPublished(data, length);
    });
    gui_bridge_set_group_avatar_result_cb([](const uint8_t* data, int length) {
        if (GroupAvatarManager::instance()) GroupAvatarManager::instance()->onResult(data, length);
    });
    gui_bridge_set_msg_received_cb(onMsgReceivedCallback);
    gui_bridge_set_msg_updated_cb([](const uint8_t* data, int length) {
        if (s_mainWindow && s_mainWindow->m_messageList) s_mainWindow->m_messageList->onMessageUpdated(data, length);
    });
    gui_bridge_set_msg_submitted_cb([](const uint8_t* data, int length) {
        if (s_mainWindow && s_mainWindow->m_messageList) s_mainWindow->m_messageList->onMessageSubmitted(data, length);
        if (s_mainWindow && s_mainWindow->m_kanban) s_mainWindow->m_kanban->onMessageSubmitted(data, length);
    });
    gui_bridge_set_feedback_changed_cb([](const uint8_t* data, int length) {
        if (s_mainWindow && s_mainWindow->m_messageList) s_mainWindow->m_messageList->onFeedbackChanged(data, length);
    });
    gui_bridge_set_attachment_keys_cb([](const uint8_t* data, int length) {
        if (s_mainWindow && s_mainWindow->m_messageList) s_mainWindow->m_messageList->onAttachmentKeysChanged(data, length);
    });
    gui_bridge_set_feedback_result_cb([](const uint8_t* data, int length) {
        if (s_mainWindow && s_mainWindow->m_messageList) s_mainWindow->m_messageList->onFeedbackResult(data, length);
    });
    gui_bridge_set_channel_updated_cb(onChannelUpdatedCallback);
    gui_bridge_set_member_list_cb(onMemberListCallback);
    gui_bridge_set_member_updated_cb(onMemberUpdatedCallback);
    gui_bridge_set_member_removed_cb(onMemberRemovedCallback);
    gui_bridge_set_auto_connect_status_cb(onAutoConnectStatusCallback);
    gui_bridge_set_channel_peers_online_cb(onChannelPeersOnlineCallback);
    gui_bridge_set_db_ready_cb([]() {
        if (!s_mainWindow || !s_mainWindow->m_db) return;
        auto* node = s_mainWindow->m_node;
        auto* database = node ? node->databaseHandle() : nullptr;
        if (!database) { DEBUG_WARN(DEBUG_CATEGORY_SYS, "desktop DB_READY ignored while core unavailable/stopping"); return; }
        s_mainWindow->m_db->setDb(database);
        s_mainWindow->m_db->setDbPath(node->databasePath());
        if (AvatarManager::instance()) AvatarManager::instance()->initialize();
        RadioAudioEngine::instance()->loadSettings(s_mainWindow->m_cfgPath);
        s_mainWindow->m_channelList->loadChannels();
        s_mainWindow->restoreWindowGeometry();
        PttHotkeyManager::instance()->init(s_mainWindow->m_db);   /* глобальные PTT-хоткеи */
        QString lastCid = s_mainWindow->m_db->getUiState("last_channel_id");
        if (!lastCid.isEmpty()) {
            /* восстанавливаем канал только если он реально существует в БД —
               иначе не дёргаем подключение к несуществующей группе */
            bool exists = false;
            const auto chs = s_mainWindow->m_db->getChannels();
            for (const auto& ch : chs) { if (ch.channelId == lastCid) { exists = true; break; } }
            if (exists) {
                s_mainWindow->m_channelList->selectChannel(lastCid);
                s_mainWindow->m_accountList->setChannel(lastCid);
                s_mainWindow->m_currentChannelId = lastCid;
                s_mainWindow->loadGroupContent(lastCid);
            } else {
                saveLastChannelId(QString());
            }
        }
    });
    gui_bridge_set_attachment_downloaded_cb([](const char* ch_id, int ch_id_len, int64_t msg_id) {
        if (!s_mainWindow) return;
        QString chId = QString::fromUtf8(ch_id, ch_id_len);
        s_mainWindow->m_channelList->loadChannels();
        s_mainWindow->m_messageList->onAttachmentDownloaded(chId, msg_id);
        s_mainWindow->onAttachmentDownloadedForAutoPlay(chId, msg_id);
    });
    gui_bridge_set_download_progress_cb([](const char* ch_id, int ch_id_len, int64_t msg_id, int blocks_done, int num_blocks) {
        if (!s_mainWindow) return;
        QString chId = QString::fromUtf8(ch_id, ch_id_len);
        s_mainWindow->m_messageList->updateMessageProgress(chId, msg_id, blocks_done, num_blocks);
    });
    gui_bridge_set_admin_key_received_cb([](const uint8_t* data, int len) {
        if (!s_mainWindow || len < 2) return;
        uint8_t chLen = data[0];
        if (len != 1 + chLen + 32) return;
        QString chId = QString::fromUtf8((const char*)data + 1, chLen);
        auto from = QByteArray((const char*)data + 1 + chLen, 32).toHex();
        s_mainWindow->m_trayIcon->showMessage(
            "vibechat",
            QCoreApplication::translate("ChatGui", "You are now a group co-owner.\nReceived admin key for group %1 from 0x%2")
                .arg(chId, QString::fromLatin1(from)),
            QSystemTrayIcon::Information, 5000);
        if (s_mainWindow->m_currentChannelId == chId)
            s_mainWindow->m_accountList->setChannel(chId);
    });

    /* ── звонки ── */
    gui_bridge_set_call_incoming_cb([](const char* ch_id, int ch_id_len, const uint8_t caller[32], uint64_t call_id) {
        if (!s_mainWindow) return;
        s_mainWindow->onCallIncoming(QString::fromUtf8(ch_id, ch_id_len), QByteArray(reinterpret_cast<const char*>(caller), 32), call_id);
    });
    gui_bridge_set_call_ringing_cb([](uint64_t call_id) {
        if (s_mainWindow) s_mainWindow->onCallRinging(call_id);
    });
    gui_bridge_set_call_phone_busy_cb([](uint64_t call_id) {
        if (!s_mainWindow) return;
        auto* window = s_mainWindow->m_activeCalls.value(call_id, nullptr);
        if (!window || window->state() != CallWindow::Outgoing) return;
        SoundManager::instance()->stopLoop();
        window->setState(CallWindow::WaitDecision);
        window->show(); window->raise();
    });
    gui_bridge_set_call_accepted_cb([](uint64_t call_id, const uint8_t peer[32]) {
        if (s_mainWindow) s_mainWindow->onCallAccepted(call_id, QByteArray(reinterpret_cast<const char*>(peer), 32));
    });
    gui_bridge_set_call_ended_cb([](uint64_t call_id, uint8_t reason) {
        if (s_mainWindow) s_mainWindow->onCallEnded(call_id, reason);
    });
    gui_bridge_set_call_declined_cb([](uint64_t call_id, uint8_t reason) {
        if (s_mainWindow) s_mainWindow->onCallEnded(call_id, reason);
    });
    gui_bridge_set_call_error_cb([](uint64_t call_id, uint8_t err, const char* text, int len) {
        Q_UNUSED(text); Q_UNUSED(len);
        if (s_mainWindow) s_mainWindow->onCallError(call_id, err);
    });
    gui_bridge_set_call_stats_cb([](uint64_t id, int rtt, int buffer, int tempo, int dropped,
                                    int underruns, int minimum, int maximum, int reserve) {
        if (!s_mainWindow) return;
        auto* w = s_mainWindow->m_activeCalls.value(id, nullptr);
        if (w) w->setAudioStats(rtt, buffer, tempo, dropped, underruns, minimum, maximum, reserve);
    });
    gui_bridge_set_call_path_cb([](uint64_t call_id, const char* text, int len) {
        if (s_mainWindow) s_mainWindow->onCallPath(call_id, QString::fromUtf8(text, len));
    });
    gui_bridge_set_call_connection_cb([](uint64_t id, uint8_t type, const uint8_t nextHop[32]) {
        if (!s_mainWindow) return;
        auto* w = s_mainWindow->m_activeCalls.value(id, nullptr);
        if (w && w->state() != CallWindow::Ended) w->setConnectionType(type, QByteArray(reinterpret_cast<const char*>(nextHop), 32));
    });

    /* ── рация: индикация говорящего ── */
    gui_bridge_set_radio_talk_cb([](const uint8_t group[32], const uint8_t source[32], uint8_t on) {
        if (s_mainWindow && s_mainWindow->m_accountList)
            s_mainWindow->m_accountList->model()->onRadioTalk(QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(group), 32).toHex()), QByteArray(reinterpret_cast<const char*>(source), 32), on != 0);
    });

    /* ── DM (p2p чат) ── */
    gui_bridge_set_dm_members_cb([](const uint8_t* data, int len) {
        if (s_mainWindow && s_mainWindow->m_accountList) s_mainWindow->m_accountList->onDmMembers(data, len);
    });
    gui_bridge_set_dm_conv_updated_cb([](const char* conv_id, int conv_id_len, uint64_t generation) {
        if (s_mainWindow)
            s_mainWindow->onDmConvUpdated(QString::fromUtf8(conv_id, conv_id_len), generation);
    });
    gui_bridge_set_dm_delete_result_cb([](const uint8_t* data, int len) {
        if (!s_mainWindow || len != 25) return;
        quint64 request, conv, generation;
        memcpy(&request, data, 8); memcpy(&conv, data + 8, 8); memcpy(&generation, data + 16, 8);
        s_mainWindow->onDmDeleteResult(request, QString::number(conv), generation, data[24] != 0);
    });
    gui_bridge_set_dm_start_result_cb([](const uint8_t* data, int len) {
        if (!s_mainWindow || len != 25) return;
        quint64 request, conv, generation;
        memcpy(&request, data, 8); memcpy(&conv, data + 8, 8); memcpy(&generation, data + 16, 8);
        s_mainWindow->onDmStartResult(request, QString::number(conv), generation, data[24] != 0);
    });
    gui_bridge_set_dm_msg_received_cb([](const char* conv_id, int conv_id_len, const uint8_t author[32]) {
        if (s_mainWindow)
            s_mainWindow->onDmMsgReceived(QString::fromUtf8(conv_id, conv_id_len), QByteArray(reinterpret_cast<const char*>(author), 32));
    });
    gui_bridge_set_network_changed_cb([](int type) {
        if (!s_mainWindow) return;
        if (type == 13) {
            if (s_mainWindow->m_pendingDmStart) s_mainWindow->onDmStartResult(s_mainWindow->m_pendingDmStart, {}, 0, false);
            const auto pending = s_mainWindow->m_dmDeleteRequests;
            for (auto it = pending.cbegin(); it != pending.cend(); ++it)
                s_mainWindow->onDmDeleteResult(it.key(), it.value().first, it.value().second, false);
            s_mainWindow->m_cpuLoadPending = false;
            s_mainWindow->m_channelList->setCpuLoad(0);
            s_mainWindow->m_accountList->setChannel(QString());
            s_mainWindow->m_messageList->clearDmCallRoute();
            s_mainWindow->m_messageList->clearStreams();
            if (s_mainWindow->m_streamPlayer) s_mainWindow->m_streamPlayer->close();
            if (s_mainWindow->m_streamGrid) s_mainWindow->m_streamGrid->close();
            s_mainWindow->m_channelList->clearDmPresence();
        } else {
            if (type == 12) s_mainWindow->m_messageList->watchStreams();
            s_mainWindow->m_channelList->refreshDmPresence();
        }
    });
    gui_bridge_set_dm_call_route_cb([](const uint8_t* data, int len) {
        if (!s_mainWindow) return;
        s_mainWindow->m_messageList->onDmCallRoute(data, len);
        s_mainWindow->m_channelList->onDmCallRoute(data, len);
    });
    gui_bridge_set_dm_messages_cb([](const uint8_t* data, int len) {
        if (s_mainWindow)
            s_mainWindow->m_messageList->onDmMessages(data, len);
    });
    gui_bridge_set_stream_sources_cb([](const uint8_t* data, int len) {
        if (!s_mainWindow) return;
        s_mainWindow->m_messageList->onStreamSources(data, len);
        if (s_mainWindow->m_streamGrid) s_mainWindow->m_streamGrid->onSources(data, len);
    });
    gui_bridge_set_attachment_preparation_cb([](const uint8_t* data, int len) {
        if (s_mainWindow) s_mainWindow->m_messageList->onAttachmentPreparation(data, len);
        if (s_mainWindow && s_mainWindow->m_kanban) s_mainWindow->m_kanban->onAttachmentPreparation(data, len);
    });
}

/* Один publisher обслуживает шапку и свою плитку; окно управления не владеет сеткой и её readers. */
void MainWindow::showStreamPublisher(const QString& group, StreamGridWindow* returnTo) {
    if (AudioCaptureLease::phoneReserved.load()) { DEBUG_WARN(DEBUG_CATEGORY_STREAM, "publisher UI blocked by phone"); return; }
    QElapsedTimer elapsed; elapsed.start();
    if (!m_streamPublisher) {
        m_streamPublisher = new StreamPublishEngine(this);
        connect(m_streamPublisher, &StreamPublishEngine::stateChanged, this, [this](const QString& state) {
            m_messageList->setStreamPublisherGroup(m_streamPublisher->active() ? m_streamPublisher->group() : QString());
            if (m_trayIcon) m_trayIcon->setToolTip(m_streamPublisher->active() ? QStringLiteral("vibechat · ") + state : QStringLiteral("vibechat"));
        });
        connect(m_streamPublisher, &StreamPublishEngine::telemetryChanged, this, [this](const stream_publisher_status& status) {
            m_messageList->setStreamPublisherViewers(m_streamPublisher->group(), status.viewers);
        });
        connect(m_streamPublisher, &StreamPublishEngine::previewVideoChanged, this, [this](const QVariant& frame) {
            if (m_streamGrid) m_streamGrid->setLocalPreview(m_streamPublisher->group(), frame);
        });
    }
    qint64 engineUs = elapsed.nsecsElapsed() / 1000;
    if (m_streamPublishWindow) delete m_streamPublishWindow.data();
    qint64 removedUs = elapsed.nsecsElapsed() / 1000;
    m_streamPublishWindow = new StreamPublishWindow(m_streamPublisher, m_streamPublisher->active() ? m_streamPublisher->group() : group,
        this, m_db ? m_db->getUiState("stream_capture_settings") : QString());
    qint64 constructedUs = elapsed.nsecsElapsed() / 1000;
    connect(m_streamPublishWindow, &StreamPublishWindow::settingsChanged, this,
        [](const QString& settings) { postUiStateKv("stream_capture_settings", settings); });
    connect(m_streamPublishWindow, &StreamPublishWindow::microphoneChanged, this,
        [](const QString& id) { postUiStateKv("audio_input_id", id); });
    if (returnTo) connect(m_streamPublishWindow, &StreamPublishWindow::closed, returnTo, [returnTo] {
        if (!returnTo->isVisible()) return;
        returnTo->raise(); returnTo->activateWindow();
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "desktop publisher returns to multiview group=%s tiles=%d",
            qPrintable(returnTo->group()), returnTo->tileCount());
    });
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "desktop publisher controls opened group=%s return_to_grid=%d",
        qPrintable(group), returnTo != nullptr);
    m_streamPublishWindow->show(); m_streamPublishWindow->raise(); m_streamPublishWindow->activateWindow();
    qint64 shownUs = elapsed.nsecsElapsed() / 1000;
    DEBUG_DEBUG(DEBUG_CATEGORY_STREAM,
        "desktop publisher UI group=%s active=%d return_to_grid=%d engine_us=%lld remove_us=%lld construct_us=%lld show_us=%lld total_us=%lld",
        qPrintable(group), m_streamPublisher->active(), returnTo != nullptr, (long long)engineUs,
        (long long)(removedUs - engineUs), (long long)(constructedUs - removedUs),
        (long long)(shownUs - constructedUs), (long long)shownUs);
}

void MainWindow::loadGroupContent(const QString& channel) {
    QString type = m_db ? m_db->groupType(channel) : QString();
    if (!channel.isEmpty() && !channel.startsWith(QStringLiteral("dm:")) && type != QStringLiteral("chat")) {
        m_messageList->loadChannel(QString());
        m_contentStack->setCurrentWidget(type == QStringLiteral("kanban") ? static_cast<QWidget*>(m_kanban) : m_unknownGroup);
        m_kanban->setGroup(type == QStringLiteral("kanban") ? channel : QString());
    } else {
        m_kanban->setGroup(QString()); m_contentStack->setCurrentWidget(m_messageList); m_messageList->loadChannel(channel);
    }
    applyObjectLink(channel);
}

/* Одно окно записи для всех composer; request уже содержит исходную карточку, текущий выбор её не заменяет. */
void MainWindow::showVideoRecorder(const QString& target, quint64 generation, const QByteArray& reply,
                                 const QString& card, const QString& title, const QByteArray& quote) {
    if (m_videoRecording) { m_videoRecording->show(); m_videoRecording->raise(); m_videoRecording->activateWindow(); return; }
    QString recipient = target;
    if (m_db) for (const auto& channel : m_db->getChannels()) if (channel.channelId == target) recipient = channel.name;
    if (m_db && target.startsWith("dm:")) for (const auto& dm : m_db->getDmConversations())
        if (dm.convId == target.mid(3)) recipient = dm.peerName;
    if (!card.isEmpty()) recipient += QStringLiteral(" / ") + (title.isEmpty() ? card.left(8) : title);
    m_videoRecording = new VideoRecordingWindow(target, recipient, generation, reply,
        m_db ? m_db->getUiState("video_recording_settings") : QString(), card, this, quote);
    auto* window = m_videoRecording.data();
    connect(window, &VideoRecordingWindow::settingsChanged, this,
        [](const QString& settings) { postUiStateKv("video_recording_settings", settings); });
    connect(window, &VideoRecordingWindow::microphoneChanged, this,
        [](const QString& id) { postUiStateKv("audio_input_id", id); });
    connect(m_messageList, &MessageList::videoRecordingSubmitted, window, &VideoRecordingWindow::submissionResult);
    connect(m_messageList, &MessageList::videoRecordingInvalidated, window, &VideoRecordingWindow::invalidate);
    connect(window, &VideoRecordingWindow::submitAttachment, this, [window](const QString& destination, attachment_send_req* req) {
        req->is_dm = destination.startsWith("dm:");
        const QByteArray id = (req->is_dm ? destination.mid(3) : destination).toUtf8();
        snprintf(req->target, sizeof(req->target), "%s", id.constData());
        DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "recording attachment dispatch target=%s card=%s request=%llu",
            req->target,req->card,(unsigned long long)req->request_id);
        if ((req->is_dm && !req->dm_generation) || gui_bridge_submit_attachment(req) < 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "recording attachment submission rejected target=%s card=%s", qPrintable(destination),req->card);
            window->submissionResult(req->request_id, false); attachment_send_discard(req);
        }
    });
    window->show();
}

void MainWindow::setupConnects() {
    connect(m_accountList, &AccountList::clipboardRequested, m_channelList->clipboard(), &ClipboardExchange::sendTo);
    connect(m_accountList, &AccountList::radioCallRequested, this, &MainWindow::onRadioCallRequested);
    gui_bridge_set_radio_call_cb([](const uint8_t* data, int len) {
        if (s_mainWindow) s_mainWindow->onRadioCallEvent(data, len);
    });
    m_messageList->setVideoRecordingEnabled(VideoRecordingEngine::supported());
    m_kanban->setVideoRecordingEnabled(VideoRecordingEngine::supported());
    connect(m_messageList, &MessageList::videoRecordingRequested, this,
        [this](const QString& target, quint64 generation, const QByteArray& reply, const QByteArray& quote) {
            showVideoRecorder(target, generation, reply, {}, {}, quote);
        });
    connect(m_kanban, &KanbanView::videoRecordingRequested, this,
        [this](const QString& group, const QString& card, const QString& title) { showVideoRecorder(group,0,{},card,title); });
    connect(m_messageList, &MessageList::streamPublishRequested, this, [this](const QString& group) { showStreamPublisher(group); });
    connect(m_messageList, &MessageList::streamRequested, this,
        [this](const QString& group, const QByteArray& source, quint64 epoch, const QString& name) {
            if (AudioCaptureLease::phoneReserved.load()) { DEBUG_WARN(DEBUG_CATEGORY_STREAM, "viewer blocked by phone"); return; }
            if (m_streamGrid) delete m_streamGrid.data();
            if (m_streamPlayer) delete m_streamPlayer.data();
            m_streamPlayer = new StreamPlayerWindow(group, source, epoch, name);
            m_streamPlayer->setAttribute(Qt::WA_DeleteOnClose); m_streamPlayer->show();
        });
    connect(m_messageList, &MessageList::allStreamsRequested, this, [this](const QString& group) {
        if (AudioCaptureLease::phoneReserved.load()) { DEBUG_WARN(DEBUG_CATEGORY_STREAM, "multiview blocked by phone"); return; }
        if (m_streamPlayer) delete m_streamPlayer.data();
        if (m_streamGrid && m_streamGrid->group() != group) delete m_streamGrid.data();
        if (!m_streamGrid) {
            m_streamGrid = new StreamGridWindow(group);
            m_streamGrid->setAttribute(Qt::WA_DeleteOnClose);
            connect(m_streamGrid, &StreamGridWindow::publishRequested, this, [this](const QString& group) {
                showStreamPublisher(group, m_streamGrid);
            });
            if (m_streamPublisher) m_streamGrid->setLocalPreview(m_streamPublisher->group(), m_streamPublisher->previewFrame());
        }
        m_streamGrid->show(); m_streamGrid->raise(); m_streamGrid->activateWindow();
    });
    auto updateRadioState = [this] {
        auto* engine = RadioAudioEngine::instance();
        m_accountList->model()->setRadioState(engine->groupId(), engine->isActive(), engine->isTransmitting());
        m_channelList->setActiveRadioChannel(engine->isActive() ? engine->groupId() : QString());
        m_channelList->setRadioPttState(engine->isActive() && engine->captureAvailable(), engine->isTransmitting(), engine->pttLatched());
    };
    auto* radio = RadioAudioEngine::instance();
    connect(radio, &RadioAudioEngine::stateChanged, this, updateRadioState);
    connect(radio, &RadioAudioEngine::pttLatchChanged, this, updateRadioState);
    connect(m_channelList, &ChannelList::radioPttPressed, radio, [radio] {
        if (radio->pttLatched()) radio->setPttLatched(false, "channel-button-pressed");
        else radio->talkBegin();
    });
    connect(m_channelList, &ChannelList::radioPttReleased, radio, &RadioAudioEngine::talkEnd);
    connect(m_channelList, &ChannelList::radioPttLockRequested, radio, [radio] {
        radio->setPttLatched(true, "channel-button-double-click");
    });
    updateRadioState();
    connect(m_channelList, &ChannelList::channelSelected,
            this, [this](const QString& channelId) {
                saveLastChannelId(channelId);
                m_currentChannelId = channelId;
                if (m_pendingObjectLink.isEmpty()) ++m_objectLinkRevision;
                loadGroupContent(channelId);
                if (channelId.startsWith(QStringLiteral("dm:"))) {
                    /* DM: беседа уже создана/обслуживается dm_core */
                    m_accountList->setChannel(channelId);
                    m_channelList->setChannelUnreadCount(channelId, 0);
                    return;
                }
                m_accountList->setChannel(channelId);
                struct chat_create_auto_arg* arg = (struct chat_create_auto_arg*)u_calloc(1, sizeof(*arg));
                if (arg) {
                    snprintf(arg->name, sizeof(arg->name), "%s", channelId.toUtf8().constData());
                    if (gui_bridge_post_bound_fn(chat_core_connect_channel_trampoline, arg, &arg->inst) < 0) u_free(arg);
                } else DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "channel connect request allocation failed channel=%s", qPrintable(channelId));
            });
    connect(m_messageList, &MessageList::shareInviteRequested, this, &MainWindow::showShareInviteDialog);
    connect(m_channelList, &ChannelList::joinRequested,       this, &MainWindow::showJoinDialog);
    connect(m_channelList, &ChannelList::openLinkRequested,   this, &MainWindow::showOpenLinkDialog);
    connect(m_channelList, &ChannelList::createGroupRequested, this, &MainWindow::onCreateGroupRequested);
    connect(m_channelList, &ChannelList::settingsRequested,   this, &MainWindow::showSettings);
    connect(m_channelList, &ChannelList::connMonitorRequested, this, [] { ConnMonitorWindow::showMonitor(); });
    connect(m_channelList, &ChannelList::networkTrafficRequested, this, [] { NetworkTrafficWindow::showMonitor(); });
    connect(m_channelList, &ChannelList::dmDeleteRequested, this, &MainWindow::onDmDeleteRequested);
    connect(m_messageList, &MessageList::channelSettingsRequested, this, &MainWindow::showChannelSettings);
    connect(m_accountList, &AccountList::memberRenamed,
            m_messageList, &MessageList::reloadLocalNicks);
    connect(m_accountList, &AccountList::filesRequested, this, &MainWindow::showMemberFiles);
    connect(m_channelList, &ChannelList::filesRequested, this, [this](const QString& channel) { showMemberFiles(channel); });
    connect(m_messageList, &MessageList::filesRequested, this, [this](const QString& channel) { showMemberFiles(channel); });
    connect(m_messageList, &MessageList::ownFilesRequested, this, [this](const QString& channel) {
        uint8_t ed[32];
        if (channel.size() != 64 || gui_bridge_self_ed(ed)) {
            DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "file browser: own files unavailable channel=%s", qPrintable(channel));
            return;
        }
        showMemberFiles(channel, QByteArray(reinterpret_cast<const char*>(ed), sizeof(ed)));
    });
    connect(m_channelList, &ChannelList::ownFilesDropped, this, [this](const QString& channel, const QStringList& paths) {
        if (!m_fileBrowser) m_fileBrowser = new MemberFileBrowser(m_db, this);
        m_fileBrowser->show(); m_fileBrowser->raise(); m_fileBrowser->activateWindow();
        m_fileBrowser->uploadOwnFiles(channel, paths);
    });
    connect(m_accountList, &AccountList::callRequested, this, &MainWindow::onCallRequested);
    connect(m_messageList, &MessageList::callRequested, this, [this](const QString& ch, const QByteArray& peer, const QString& name) {
        onCallRequested(ch, peer, name);
    });
    connect(m_accountList, &AccountList::pmRequested, this, &MainWindow::onPmRequested);
    connect(m_accountList, &AccountList::inviteRequested, this, &MainWindow::onInviteToPmRequested);
    connect(m_messageList, &MessageList::inviteRequested, this, &MainWindow::onInviteToPmRequested);
    connect(m_messageList, &MessageList::linkActivated, this, &MainWindow::onMessageLinkActivated);
    connect(m_messageList, &MessageList::readPositionChanged,
            this, [this](const QString& channelId, qint64 lastReadMsgId) {
                Q_UNUSED(lastReadMsgId);
                if (m_channelList && m_db)
                    m_channelList->setChannelUnreadCount(channelId,
                        m_db->getUnreadCount(channelId, lastReadMsgId));
            });
    connect(m_messageList, &MessageList::openVideoRequested, this, &MainWindow::openVideo);
    connect(m_messageList, &MessageList::openImageRequested, this, &MainWindow::openImage);
    connect(m_kanban, &KanbanView::openImageRequested, this, &MainWindow::openImage);
    connect(m_kanban, &KanbanView::openVideoRequested, this, &MainWindow::openVideo);
}

/* ── звонки ── */

void MainWindow::showMemberFiles(const QString& channel, const QByteArray& owner) {
    if (channel.startsWith(QStringLiteral("dm:"))) return;
    if (!m_fileBrowser) m_fileBrowser = new MemberFileBrowser(m_db, this);
    m_fileBrowser->setChannel(channel, owner); m_fileBrowser->show(); m_fileBrowser->raise(); m_fileBrowser->activateWindow();
}

/* GUI передаёт мосту значения; ядро и время жизни запроса выбирает мост. */
void MainWindow::onRadioCallRequested(const QString& channel, const QByteArray& peer, bool devices) {
    if (m_radioCallWindow) { m_radioCallWindow->show(); m_radioCallWindow->raise(); return; }
    quint64 id = QRandomGenerator::global()->generate64();
    const auto* key = peer.size() == 32 ? reinterpret_cast<const uint8_t*>(peer.constData()) : nullptr;
    const auto group = QByteArray::fromHex(channel.toLatin1());
    if (group.size() != 32 || group.toHex() != channel.toLatin1() ||
        gui_bridge_radio_call(devices ? GUI_RADIO_CALL_START_DEVICES : GUI_RADIO_CALL_START, id ? id : 1,
            reinterpret_cast<const uint8_t*>(group.constData()), key) < 0)
        DEBUG_WARN(DEBUG_CATEGORY_RADIO, "desktop radio call start was not queued channel=%s peer=%s",
                   qPrintable(channel), peer.toHex().constData());
}

/* Один диалог на незавершённое приглашение; принятая рация живёт своим обычным циклом. */
void MainWindow::onRadioCallEvent(const uint8_t* data, int len) {
    if (!data || len != CHAT_RADIO_CALL_EVENT_SIZE || data[CHAT_RADIO_CALL_STATE] < RADIO_CALL_OUTGOING ||
        data[CHAT_RADIO_CALL_STATE] > RADIO_CALL_SELECTED || data[CHAT_RADIO_CALL_STATE + 1] > RADIO_CALL_POLICY || data[CHAT_RADIO_CALL_STATE + 2] > 1) {
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "desktop invalid radio call event bytes=%d", len); return;
    }
    quint64 id; memcpy(&id, data, 8);
    const QByteArray peer(reinterpret_cast<const char*>(data + CHAT_RADIO_CALL_PEER), 32);
    int state = data[CHAT_RADIO_CALL_STATE], reason = data[CHAT_RADIO_CALL_STATE + 1]; bool incoming = data[CHAT_RADIO_CALL_STATE + 2];
    const QString channel = QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(data + 8), 32).toHex());
    if (state == RADIO_CALL_FINISHED) {
        m_messageList->finishRadioCall(id, reason == RADIO_CALL_ACCEPTED);
        if (m_radioCallId == id) {
            m_radioCallId = 0;
            SoundManager::instance()->stopLoop();
            if (m_radioCallWindow) m_radioCallWindow->close();
            m_radioCallWindow = nullptr;
        }
        if (reason == RADIO_CALL_ACCEPTED) {
            if (m_streamPlayer) { m_streamPlayer->close(); m_streamPlayer = nullptr; }
            if (m_streamGrid) { m_streamGrid->close(); m_streamGrid = nullptr; }
            if (!incoming) {
                m_channelList->selectChannel(channel);
                if (m_currentChannelId != channel || !m_messageList->enableRadio()) reason = RADIO_CALL_AUDIO_ERROR;
            }
            if (reason == RADIO_CALL_ACCEPTED) return;
        }
        if ((incoming && reason != RADIO_CALL_AUDIO_ERROR) || reason == RADIO_CALL_CANCELLED) return;
        const char* text = reason == RADIO_CALL_PHONE_BUSY ? "Subscriber is on a phone call" : (reason == RADIO_CALL_DECLINED || reason == RADIO_CALL_POLICY) ? "Radio call declined" :
            reason == RADIO_CALL_BUSY ? "Subscriber is busy" : reason == RADIO_CALL_NO_ANSWER ? "No answer" :
            reason == RADIO_CALL_AUDIO_ERROR ? "Could not start group radio" : "Subscriber is unavailable";
        auto* notice = new QMessageBox(QMessageBox::Information, QCoreApplication::translate("ChatGui", "Radio call"),
            QCoreApplication::translate("ChatGui", text), QMessageBox::Ok, this);
        notice->setAttribute(Qt::WA_DeleteOnClose); notice->show();
        if (m_trayIcon && (!isVisible() || isMinimized()))
            m_trayIcon->showMessage(QCoreApplication::translate("ChatGui", "Radio call"), QCoreApplication::translate("ChatGui", text));
        return;
    }
    if (state == RADIO_CALL_SELECTED && incoming && m_radioCallId == id) {
        if (m_streamPlayer) { m_streamPlayer->close(); m_streamPlayer = nullptr; }
        if (m_streamGrid) { m_streamGrid->close(); m_streamGrid = nullptr; }
        SoundManager::instance()->stopLoop();
        m_channelList->selectChannel(channel);
        bool ready = m_currentChannelId == channel && m_messageList->enableRadioForCall(id);
        gui_bridge_radio_call(ready ? GUI_RADIO_CALL_ACCEPT : GUI_RADIO_CALL_FAIL, id, 0, 0);
        return;
    }
    if (state == RADIO_CALL_ANSWERING && incoming) return;
    if (state == RADIO_CALL_RINGING) {
        if (m_radioCallId == id && m_radioCallWindow) {
            m_radioCallWindow->setInformativeText(QCoreApplication::translate("ChatGui", "Ringing…"));
            SoundManager::instance()->playLoop("call_ringing");
        }
        return;
    }
    if (state == RADIO_CALL_WAIT_DECISION && incoming) return;
    if (state == RADIO_CALL_WAIT_DECISION && m_radioCallId == id) {
        m_radioCallId = 0;
        SoundManager::instance()->stopLoop();
        if (m_radioCallWindow) m_radioCallWindow->close();
        m_radioCallWindow = nullptr;
    }
    if (m_radioCallId == id) return;
    if (m_radioCallId || !m_activeCalls.isEmpty()) { gui_bridge_radio_call(GUI_RADIO_CALL_REJECT_BUSY, id, 0, 0); return; }
    QString name = m_db->getDisplayName(peer, channel);
    if (name.isEmpty()) name = QString::fromLatin1(peer.toHex());
    QString channelName = channel;
    for (const auto& row : m_db->getChannels()) if (row.channelId == channel) { channelName = row.name; break; }
    auto* box = new QMessageBox(QMessageBox::Question, QCoreApplication::translate("ChatGui", "Radio call"),
        QCoreApplication::translate("ChatGui", incoming ? "Incoming radio call from %1\nGroup: %2" : "Calling %1 by radio\nGroup: %2")
            .arg(name, channelName), QMessageBox::NoButton, this);
    box->setTextFormat(Qt::PlainText);
    box->setAttribute(Qt::WA_DeleteOnClose);
    m_radioCallId = id; m_radioCallWindow = box; m_radioCallAnswered = false;
    if (incoming) {
        auto* accept = box->addButton(QCoreApplication::translate("ChatGui", "Accept"), QMessageBox::AcceptRole);
        auto* warningTimer = new QTimer(box);
        auto updateWarning = [this, box] {
            QStringList warnings;
            if (m_streamPlayer || m_streamGrid) warnings << QCoreApplication::translate("ChatGui", "Accepting will end stream playback and open the radio channel.");
            if (m_streamPublisher && m_streamPublisher->running()) warnings << QCoreApplication::translate("ChatGui", "Your broadcast will continue.");
            box->setInformativeText(warnings.join(QLatin1Char('\n')));
        };
        connect(warningTimer, &QTimer::timeout, box, updateWarning); warningTimer->start(200); updateWarning();
        box->addButton(QCoreApplication::translate("ChatGui", "Decline"), QMessageBox::RejectRole);
        connect(box, &QMessageBox::buttonClicked, this, [this, id, channel, accept](QAbstractButton* button) {
            if (m_radioCallId != id) return;
            m_radioCallAnswered = true;
            if (button != accept) { gui_bridge_radio_call(GUI_RADIO_CALL_DECLINE, id, 0, 0); return; }
            SoundManager::instance()->stopLoop();
            gui_bridge_radio_call(GUI_RADIO_CALL_ANSWER, id, 0, 0);
        });
    } else if (state == RADIO_CALL_WAIT_DECISION) {
        box->setInformativeText(QCoreApplication::translate("ChatGui", "Subscriber is on a phone call"));
        auto* proceed = box->addButton(QCoreApplication::translate("ChatGui", "Call anyway"), QMessageBox::AcceptRole);
        box->addButton(QCoreApplication::translate("ChatGui", "End call"), QMessageBox::RejectRole);
        connect(box, &QMessageBox::buttonClicked, this, [this, id, proceed](QAbstractButton* button) {
            if (m_radioCallId != id || button != proceed) return;
            if (gui_bridge_radio_call(GUI_RADIO_CALL_CONTINUE, id, 0, 0) < 0) return;
            m_radioCallAnswered = true;
            m_radioCallId = 0;
            DEBUG_INFO(DEBUG_CATEGORY_RADIO, "desktop caller overrides phone busy id=%016llx", (unsigned long long)id);
        });
    } else {
        box->addButton(QMessageBox::Cancel);
        box->setInformativeText(QCoreApplication::translate("ChatGui", "Checking subscriber availability…"));
    }
    connect(box, &QDialog::finished, this, [this, id, incoming] {
        if (m_radioCallId == id && !m_radioCallAnswered) {
            m_radioCallAnswered = true;
            SoundManager::instance()->stopLoop();
            gui_bridge_radio_call(incoming ? GUI_RADIO_CALL_DECLINE : GUI_RADIO_CALL_CANCEL, id, 0, 0);
        }
    });
    box->show(); box->raise(); box->activateWindow();
    if (state != RADIO_CALL_WAIT_DECISION) SoundManager::instance()->playLoop(incoming ? "radio_incoming" : "call_connecting");
    if (incoming) {
        if (m_trayIcon && (!isVisible() || isMinimized()))
            m_trayIcon->showMessage(QCoreApplication::translate("ChatGui", "Radio call"), box->text());
        gui_bridge_radio_call(GUI_RADIO_CALL_READY, id, 0, 0);
    }
}

void MainWindow::onCallRequested(const QString& chId, const QByteArray& peer, const QString& name, bool devices) {
    if (AudioCaptureLease::phoneReserved.load()) { DEBUG_WARN(DEBUG_CATEGORY_CALL, "call start blocked by media cleanup"); return; }
    if (chId.startsWith(QStringLiteral("dm:")) && !m_activeCalls.isEmpty()) {
        auto* w = m_activeCalls.constBegin().value();
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "PM call: existing call window id=%llu", (unsigned long long)w->callId());
        w->show(); w->raise(); w->activateWindow();
        return;
    }
    quint64 callId = QRandomGenerator::global()->generate64();
    if (!callId) callId = 1;

    const auto* key = peer.size() == 32 ? reinterpret_cast<const uint8_t*>(peer.constData()) : nullptr;
    if ((devices ? gui_bridge_call_start_devices : gui_bridge_call_start)(chId.toUtf8().constData(), key, callId) < 0) {
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "desktop call start was not queued id=%llu", (unsigned long long)callId); return;
    }

    CallWindow* w = new CallWindow(this);
    w->setCallId(callId);
    w->setPeerName(name);
    w->setState(CallWindow::Outgoing);
    watchCallMedia(w);
    connect(w, &CallWindow::closed, this, &MainWindow::closeCallWindow);
    m_activeCalls.insert(callId, w);
    w->show();

    SoundManager::instance()->playLoop("call_connecting");
}

void MainWindow::onCallIncoming(const QString& chId, const QByteArray& caller, quint64 callId) {
    if (m_activeCalls.contains(callId)) return;
    QString name = m_db->getDisplayName(caller, chId);
    if (name.isEmpty()) name = QString::fromLatin1(caller.toHex());

    SoundManager::instance()->playLoop("call_incoming");
    if (!isVisible() || isMinimized()) {
        m_trayIcon->showMessage("vibechat", QCoreApplication::translate("ChatGui", "Incoming call from %1").arg(name),
                                QSystemTrayIcon::Information, 5000);
    }

    CallWindow* w = new CallWindow(this);
    w->setCallId(callId);
    w->setPeerName(name);
    w->setState(CallWindow::Incoming);
    watchCallMedia(w);
    connect(w, &CallWindow::closed, this, &MainWindow::closeCallWindow);
    m_activeCalls.insert(callId, w);
    w->show();
    w->raise();
    w->activateWindow();
}

void MainWindow::onCallRinging(quint64 callId) {
    /* caller: INVITE доставлен, у пира идёт вызов — фаза 2 (гудки) */
    CallWindow* w = m_activeCalls.value(callId, nullptr);
    if (w && w->state() == CallWindow::Outgoing)
        w->setState(CallWindow::Ringing);
    SoundManager::instance()->playLoop("call_ringing");
}

void MainWindow::watchCallMedia(CallWindow* window) {
    auto* timer = new QTimer(window);
    auto update = [this, window] {
        if (window->state() == CallWindow::Active || window->state() == CallWindow::Preparing || window->state() == CallWindow::Ended) return;
        QStringList warnings;
        if (m_streamPublisher && m_streamPublisher->running())
            warnings << QCoreApplication::translate("ChatGui", "The broadcast will end when the call is accepted.");
        if (m_streamPlayer || m_streamGrid)
            warnings << QCoreApplication::translate("ChatGui", "Stream playback will end when the call is accepted.");
        window->setMediaWarning(warnings.join(QLatin1Char('\n')));
    };
    connect(timer, &QTimer::timeout, window, update); timer->start(200); update();
}

void MainWindow::onCallAccepted(quint64 callId, const QByteArray& peer) {
    if (!m_activeCalls.contains(callId)) { DEBUG_WARN(DEBUG_CATEGORY_CALL, "accepted after local cancellation id=%llu", callId); return; }
    SoundManager::instance()->stopLoop();
    if (!m_phoneMedia) m_phoneMedia = new PhoneMediaDispatcher(this);
    if (auto* window = m_activeCalls.value(callId)) window->setState(CallWindow::Preparing);
    m_phoneMedia->enter(callId, [this] {
        m_radioBeforePhone = RadioAudioEngine::instance()->groupId();
        if (!m_radioBeforePhone.isEmpty()) m_messageList->disableRadioForPhone();
        if (m_streamPublishWindow) { m_streamPublishWindow->disconnect(this); m_streamPublishWindow->close(); }
        if (m_streamPublisher) { m_streamPublisher->setPreview(false); m_streamPublisher->stop(); }
        if (m_recorder) m_recorder->stopForPhone();
        if (m_videoRecording) m_videoRecording->stopForPhone();
        if (m_streamPlayer) m_streamPlayer->stopAsync();
        if (m_streamGrid) m_streamGrid->stopForPhone();
    }, [this] {
        if (m_streamPlayer && !m_streamPlayer->released()) return false;
        if (m_streamGrid && !m_streamGrid->released()) return false;
        if (m_streamPublisher && !m_streamPublisher->captureReleased()) return false;
        if (m_videoRecording && !m_videoRecording->released()) return false;
        if (!RadioAudioEngine::instance()->released() || (m_recorder && !m_recorder->released())) return false;
        if (m_streamPlayer) { m_streamPlayer->deleteLater(); m_streamPlayer = nullptr; }
        if (m_streamGrid) { m_streamGrid->deleteLater(); m_streamGrid = nullptr; }
        return true;
    }, [this, callId, peer] {
        if (!m_activeCalls.contains(callId)) return;
        SoundManager::instance()->stopLoop();
        if (CallAudioEngine::instance()->start(callId) != 0) {
            DEBUG_WARN(DEBUG_CATEGORY_CALL, "desktop call cannot acquire audio; hangup id=%llu", (unsigned long long)callId);
            if (gui_bridge_call_action(GUI_CALL_HANGUP, callId) < 0)
                DEBUG_WARN(DEBUG_CATEGORY_CALL, "desktop failed call hangup was not queued id=%llu", (unsigned long long)callId);
            QMessageBox::information(this, QStringLiteral("vibechat"), QCoreApplication::translate("ChatGui", "The microphone is used by another operation"));
            onCallEnded(callId, CALL_REASON_CONNECT_FAIL);
            return;
        }
        auto* window = m_activeCalls.value(callId);
        const QString name = m_db->getDisplayName(peer);
        window->setPeerName(name.isEmpty() ? QString::fromLatin1(peer.toHex()) : name);
        window->setState(CallWindow::Active);
    }, [this, callId] {
        if (gui_bridge_call_action(GUI_CALL_HANGUP, callId) < 0)
            DEBUG_ERROR(DEBUG_CATEGORY_CALL, "media timeout hangup queue failed id=%llu", (unsigned long long)callId);
        onCallEnded(callId, CALL_REASON_CONNECT_FAIL);
        QMessageBox::information(this, QStringLiteral("vibechat"),
            QCoreApplication::translate("ChatGui", "Could not prepare conversation. Media resources are still being released."));
    });
}

void MainWindow::onCallEnded(quint64 callId, uint8_t reason) {
    const bool phoneBusy = reason == CALL_REASON_PHONE_BUSY && m_activeCalls.contains(callId);
    /* тон завершения (~450мс) → авто-teardown аудио-движка */
    if (CallAudioEngine::instance()->callId() == callId)
        CallAudioEngine::instance()->beginEnd(callId);
    closeCallWindow(callId);
    if (phoneBusy) {
        auto* notice = new QMessageBox(QMessageBox::Information, QStringLiteral("vibechat"),
            QCoreApplication::translate("ChatGui", "Subscriber is on a phone call"), QMessageBox::Ok, this);
        notice->setAttribute(Qt::WA_DeleteOnClose); notice->show();
    }
}

void MainWindow::onCallError(quint64 callId, uint8_t err) {
    /* пир офлайн / нет маршрута — звонок не состоялся */
    DEBUG_WARN(DEBUG_CATEGORY_CALL, "MainWindow: call error id=%016llx err=%u", (unsigned long long)callId, (unsigned)err);
    closeCallWindow(callId);
}

void MainWindow::onCallPath(quint64 callId, const QString& path) {
    CallWindow* w = m_activeCalls.value(callId, nullptr);
    if (w) w->setBgpPath(path);
}

void MainWindow::closeCallWindow(quint64 callId) {
    if (m_phoneMedia && m_phoneMedia->blocked()) {
        if (CallAudioEngine::instance()->callId() == callId) CallAudioEngine::instance()->beginEnd(callId);
        m_phoneMedia->finish(callId, [] { return CallAudioEngine::instance()->released(); }, [this] {
            const QString group = m_radioBeforePhone; m_radioBeforePhone.clear();
            if (group.isEmpty() || !gui_bridge_is_uasync_ready()) return;
            if (!m_db || !m_db->isOpen()) return;
            for (const auto& channel : m_db->getChannels()) if (channel.channelId == group) {
                if (RadioAudioEngine::instance()->start(group) != 0)
                    DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "Radio: restore after phone failed group=%s", qPrintable(group));
                return;
            }
            DEBUG_WARN(DEBUG_CATEGORY_RADIO, "Radio: restore skipped, channel removed group=%s", qPrintable(group));
        });
    }
    SoundManager::instance()->stopLoop();
    CallWindow* w = m_activeCalls.take(callId);
    if (!w) return;
    w->disconnect(this);
    w->deleteLater();
}

/* ── DM (p2p чат) ── */

void MainWindow::onPmRequested(const QString& chId, const QByteArray& peer, const QString& name, bool groups) {
    if (peer.size() != 32) { DEBUG_WARN(DEBUG_CATEGORY_DM, "PM start rejected address bytes=%d", int(peer.size())); return; }
    struct dm_start_req* r = (struct dm_start_req*)u_calloc(1, sizeof(*r));
    if (!r) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "PM start request allocation failed peer=%s", peer.toHex().constData()); return; }
    memcpy(r->peer_ed, peer.constData(), sizeof(r->peer_ed));
    r->mode = groups ? DM_MODE_GROUPS : DM_MODE_DEVICE;
    r->request_id = m_pendingDmStart = ++m_dmStartSequence;
    strncpy(r->source_ch_id, chId.toUtf8().constData(), sizeof(r->source_ch_id) - 1);
    strncpy(r->peer_name, name.toUtf8().constData(), sizeof(r->peer_name) - 1);
    if (gui_bridge_post_bound_fn(dm_start_trampoline, r, &r->inst) < 0) {
        u_free(r); onDmStartResult(m_pendingDmStart, {}, 0, false);
    }
}

void MainWindow::onDmConvUpdated(const QString& convId, quint64 generation) {
    if (!m_channelList || !m_db) return;
    bool visible = false;
    for (const auto& conv : m_db->getDmConversations()) if (conv.convId == convId && conv.generation == generation) visible = true;
    if (!visible) {
        GUI_INFO("PM update ignored: hidden/stale conv=%s generation=%llu", qPrintable(convId), (unsigned long long)generation); return;
    }
    QString key = QStringLiteral("dm:") + convId;
    m_channelList->loadChannels();
    if (m_currentChannelId == key) { loadGroupContent(key); m_accountList->setChannel(key); }
}

void MainWindow::onDmStartResult(quint64 request, const QString& convId, quint64 generation, bool success) {
    if (!request || request != m_pendingDmStart) return;
    m_pendingDmStart = 0;
    if (!success) {
        GUI_ERROR("PM start failed request=%llu", (unsigned long long)request);
        auto* error = new QMessageBox(QMessageBox::Warning, QCoreApplication::translate("ChatGui", "Send message"),
            QCoreApplication::translate("ChatGui", "Could not open the conversation. Please try again."), QMessageBox::Ok, this);
        error->setAttribute(Qt::WA_DeleteOnClose); error->open(); return;
    }
    if (!m_db || !m_channelList) return;
    for (const auto& c : m_db->getDmConversations()) if (c.convId == convId && c.generation == generation) {
        m_channelList->loadChannels(); m_channelList->selectChannel(QStringLiteral("dm:") + convId); return;
    }
    GUI_ERROR("PM start result has no current conversation conv=%s generation=%llu", qPrintable(convId), (unsigned long long)generation);
}

/* Диалог фиксирует поколение строки до вложенного Qt event loop; окончательную проверку выполняет ядро. */
void MainWindow::onDmDeleteRequested(const QString& convId, quint64 generation, const QString& name) {
    for (const auto& pending : m_dmDeleteRequests) if (pending.first == convId) return;
    QMessageBox box(QMessageBox::Warning, QCoreApplication::translate("ChatGui", "Delete conversation"),
        QCoreApplication::translate("ChatGui", "Delete the conversation with %1 and its local files? The other person keeps their history.")
            .arg(name), QMessageBox::NoButton, this);
    auto* remove = box.addButton(QCoreApplication::translate("ChatGui", "Delete"), QMessageBox::DestructiveRole);
    auto* cancel = box.addButton(QCoreApplication::translate("ChatGui", "Cancel"), QMessageBox::RejectRole);
    box.setDefaultButton(cancel); box.setEscapeButton(cancel); box.exec();
    if (box.clickedButton() != remove) return;
    const quint64 request = ++m_dmDeleteSequence;
    m_dmDeleteRequests.insert(request, {convId, generation});
    m_channelList->setDmDeleting(convId, generation, true);
    m_messageList->setDmDeleting(convId, generation, true);
    if (gui_bridge_delete_dm(qPrintable(convId), generation, request)) onDmDeleteResult(request, convId, generation, false);
}

/* Только подтверждённый commit очищает беседу. Старый результат не закрывает PM нового поколения. */
void MainWindow::onDmDeleteResult(quint64 request, const QString& convId, quint64 generation, bool success) {
    auto expected = m_dmDeleteRequests.constFind(request);
    if (expected == m_dmDeleteRequests.cend()) {
        GUI_INFO("PM delete: ignored obsolete result request=%llu", (unsigned long long)request); return;
    }
    if (expected->first != convId || expected->second != generation) {
        GUI_ERROR("PM delete: result target mismatch request=%llu conv=%s generation=%llu", (unsigned long long)request,
                  qPrintable(convId), (unsigned long long)generation); return;
    }
    m_dmDeleteRequests.remove(request);
    GUI_INFO("PM delete completed: request=%llu conv=%s generation=%llu success=%d", (unsigned long long)request,
             qPrintable(convId), (unsigned long long)generation, success);
    if (success && m_messageList->forgetDmHistory(convId, generation)) {
        m_currentChannelId.clear(); saveLastChannelId(QString()); m_accountList->setChannel(QString());
    }
    m_channelList->setDmDeleting(convId, generation, false);
    m_messageList->setDmDeleting(convId, generation, false);
    m_channelList->loadChannels();
    if (!success) {
        GUI_ERROR("PM delete failed: conv=%s generation=%llu request=%llu", qPrintable(convId),
                  (unsigned long long)generation, (unsigned long long)request);
        auto* error = new QMessageBox(QMessageBox::Warning, QCoreApplication::translate("ChatGui", "Delete conversation"),
            QCoreApplication::translate("ChatGui", "Could not delete the conversation. Please try again."), QMessageBox::Ok, this);
        error->setAttribute(Qt::WA_DeleteOnClose); error->open();
    }
}

void MainWindow::onInviteToPmRequested(const QString& source, const QByteArray& peer, const QString& name) {
    if (!m_db || !m_db->isOpen() || !m_node || !m_node->isRunning()) {
        GUI_ERROR("invite PM: node unavailable");
        QMessageBox::warning(this, QCoreApplication::translate("ChatGui",
            "Invitation"), QCoreApplication::translate("ChatGui",
            "The core is not running.")); return;
    }
    if (peer.size() != 32 || QString::fromLatin1(peer.toHex()) == m_node->nodeIdHex()) {
        GUI_ERROR("invite PM: invalid recipient"); return;
    }
    DmInviteDialog dialog(m_db, peer, name, source, this);
    if (dialog.exec() == QDialog::Accepted) {
        m_channelList->loadChannels();
        m_channelList->selectChannel(QStringLiteral("dm:") + dialog.conversationId());
    }
}

void MainWindow::onMessageLinkActivated(const QString& link) {
    ++m_objectLinkRevision; m_pendingObjectLink.clear();
    if (link.startsWith(QStringLiteral("utun://chat/")) || link.startsWith(QStringLiteral("utun://kanban/"))) {
        object_link_target target; char error[128]; const auto bytes = link.toUtf8();
        if (object_link_parse(bytes.constData(), size_t(bytes.size()), &target, error, sizeof(error))) {
            GUI_WARN("object link rejected reason=%s bytes=%d", error, int(bytes.size()));
            QMessageBox::warning(this, QCoreApplication::translate("ChatGui", "Link"), QCoreApplication::translate("ChatGui", "Invalid link.")); return;
        }
        QString group;
        const auto groupKey = QByteArray::fromHex(target.group);
        if (m_db && m_db->isOpen()) {
            for (const auto& row : m_db->getChannels()) if (row.ed25519_pubkey == groupKey) { group = row.channelId; break; }
        }
        if (group.isEmpty() || !m_db->isLocalChannelMember(group)) {
            GUI_WARN("object link group unavailable key=%s", target.group);
            QMessageBox::warning(this, QCoreApplication::translate("ChatGui", "Link"),
                QCoreApplication::translate("ChatGui", "Join this group using a separate invitation first.")); return;
        }
        if (m_db->groupType(group) != (target.kind == OBJECT_LINK_CHAT_MESSAGE ? QStringLiteral("chat") : QStringLiteral("kanban"))) {
            GUI_WARN("object link group type mismatch ch=%s kind=%d", qPrintable(group), target.kind);
            QMessageBox::warning(this, QCoreApplication::translate("ChatGui", "Link"), QCoreApplication::translate("ChatGui", "Invalid link.")); return;
        }
        m_objectLinkUrl = link; m_pendingObjectLink = link;
        GUI_INFO("object link open revision=%llu kind=%d ch=%s", (unsigned long long)m_objectLinkRevision, target.kind, qPrintable(group));
        m_channelList->selectChannel(group);
        if (m_currentChannelId == group) applyObjectLink(group);
        return;
    }
    if (!link.startsWith(QStringLiteral("utun://"))) {
        const QUrl url(link, QUrl::StrictMode);
        if (!url.isValid() || url.host().isEmpty() || (url.scheme() != "http" && url.scheme() != "https")) {
            GUI_ERROR("message link: invalid web address bytes=%d", (int)link.size());
            QMessageBox::warning(this, QCoreApplication::translate("ChatGui",
                "Link"), QCoreApplication::translate("ChatGui",
                "Invalid link.")); return;
        }
        if (!QDesktopServices::openUrl(url)) {
            GUI_ERROR("message link: external open failed scheme=%s host=%s", qPrintable(url.scheme()), qPrintable(url.host()));
            QMessageBox::warning(this, QCoreApplication::translate("ChatGui",
                "Link"), QCoreApplication::translate("ChatGui",
                "Failed to open the link. Check that a browser is installed.")); return;
        }
        GUI_INFO("message link: external open requested scheme=%s host=%s", qPrintable(url.scheme()), qPrintable(url.host()));
        return;
    }
    const auto invite = decodeInviteLink(link);
    if (!invite.error.isEmpty()) {
        GUI_ERROR("invite link: invalid error=%s", qPrintable(invite.error));
        QMessageBox::warning(this, QCoreApplication::translate("ChatGui",
            "Invitation"), QCoreApplication::translate("ChatGui",
            "Invalid link: ") + invite.error); return;
    }
    if (!m_db || !m_db->isOpen() || !m_node || !m_node->isRunning()) {
        GUI_ERROR("invite link: node unavailable");
        QMessageBox::warning(this, QCoreApplication::translate("ChatGui",
            "Invitation"), QCoreApplication::translate("ChatGui",
            "The core is not running.")); return;
    }
    if (invite.kind == INVITE_KIND_SERVER) {
        QStringList ids, labels;
        for (const auto& channel : m_db->getChannels()) {
            if (!m_db->isLocalChannelMember(channel.channelId)) continue;
            ids.append(channel.channelId); labels.append(channel.name + " (" + channel.channelId + ")");
        }
        bool accepted = false;
        const QString selected = QInputDialog::getItem(this, QCoreApplication::translate("ChatGui", "Add server to group"),
            QCoreApplication::translate("ChatGui", "Group:"), labels, qMax(0, ids.indexOf(m_currentChannelId)), false, &accepted);
        int index = labels.indexOf(selected);
        if (!accepted || index < 0) return;
        InviteByLinkDialog dialog(ids[index], m_node, this, link); dialog.exec(); return;
    }
    JoinDialog dialog(m_node, m_db, this, link);
    connect(&dialog, &JoinDialog::joined, this, [this](const QString& groupEd) {
        const auto key = QByteArray::fromHex(groupEd.toLatin1());
        QString channel;
        if (m_db) for (const auto& row : m_db->getChannels()) if (row.ed25519_pubkey == key) { channel = row.channelId; break; }
        if (channel.isEmpty()) { GUI_WARN("joined group not found key=%s", qPrintable(groupEd)); return; }
        reloadChannels(); m_channelList->selectChannel(channel);
    });
    dialog.exec();
}

/* Канал выбран и его виджет подготовлен; имя из URL никогда не участвует в поиске. */
void MainWindow::applyObjectLink(const QString& channel) {
    if (m_pendingObjectLink.isEmpty()) return;
    object_link_target target; const auto bytes = m_pendingObjectLink.toUtf8();
    if (object_link_parse(bytes.constData(), size_t(bytes.size()), &target, nullptr, 0)) {
        GUI_ERROR("object link: pending target became invalid"); m_pendingObjectLink.clear(); return;
    }
    const auto groups = m_db ? m_db->getChannels() : QList<ChannelRow>{};
    const auto groupKey = QByteArray::fromHex(target.group);
    const auto group = std::find_if(groups.cbegin(), groups.cend(), [&](const auto& row) { return row.channelId == channel; });
    if (group == groups.cend() || group->ed25519_pubkey != groupKey) { m_pendingObjectLink.clear(); ++m_objectLinkRevision; return; }
    m_pendingObjectLink.clear();
    if (target.kind == OBJECT_LINK_CHAT_MESSAGE) {
        if (!m_messageList->jumpToMessage(QString::fromLatin1(target.message)))
            objectLinkUnavailable(QCoreApplication::translate("ChatGui", "Object is not available yet."));
    } else m_kanban->openTarget(QString::fromLatin1(target.card), QString::fromLatin1(target.message));
}

void MainWindow::objectLinkUnavailable(const QString& reason) {
    const auto url = m_objectLinkUrl; const auto revision = m_objectLinkRevision;
    GUI_WARN("object link unavailable revision=%llu ch=%s", (unsigned long long)revision, qPrintable(m_currentChannelId));
    auto* dialog = new QMessageBox(QMessageBox::Information, QCoreApplication::translate("ChatGui", "Link"), reason,
        QMessageBox::Retry | QMessageBox::Close, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose); dialog->setTextFormat(Qt::PlainText);
    connect(dialog, &QMessageBox::finished, this, [this, url, revision](int result) {
        if (result == QMessageBox::Retry && revision == m_objectLinkRevision) onMessageLinkActivated(url);
    });
    dialog->open();
}

void MainWindow::showOpenLinkDialog() {
    GUI_DEBUG("open link dialog opened source=channel-footer");
    bool accepted = false;
    const auto link = QInputDialog::getText(this, QCoreApplication::translate("ChatGui", "Open link…"),
        QCoreApplication::translate("ChatGui", "Link:"), QLineEdit::Normal, QApplication::clipboard()->text().trimmed(), &accepted);
    if (accepted && !link.trimmed().isEmpty()) onMessageLinkActivated(link.trimmed());
}

void MainWindow::onDmMsgReceived(const QString& convId, const QByteArray& author) {
    if (!m_db) return;
    bool visible = false;
    for (const auto& conv : m_db->getDmConversations()) if (conv.convId == convId) visible = true;
    if (!visible) { GUI_DEBUG("PM message event ignored: hidden conv=%s", qPrintable(convId)); return; }
    QString key = QStringLiteral("dm:") + convId;
    const bool own = m_node && QString::fromLatin1(author.toHex()) == m_node->nodeIdHex();
    if (!own && !isActiveWindow()) showIncomingMessageNotification(key, m_db->getDisplayName(author));
    if (m_currentChannelId == key) {
        m_messageList->refresh(own);
        return;
    }
    if (own) return;
    if (m_channelList)
        m_channelList->setChannelUnreadCount(key, m_channelList->getChannelUnreadCount(key) + 1);
}

void MainWindow::setupTray() {
    connect(m_kanban, &KanbanView::linkActivated, this, &MainWindow::onMessageLinkActivated);
    connect(m_kanban, &KanbanView::targetUnavailable, this, &MainWindow::objectLinkUnavailable);
    m_trayIcon = new QSystemTrayIcon(this);
    m_trayIcon->setIcon(QIcon(":/icons/app_icon.webp"));
    m_trayIcon->setToolTip("vibechat");

    m_messageNotification = std::make_unique<MessageNotification>();
    connect(m_messageNotification.get(), &MessageNotification::activated, this, [this](const QString& target) {
        showAndActivateWindow(this);
        reloadChannels();
        m_channelList->selectChannel(target);
        if (m_currentChannelId != target) GUI_WARN("notification: target no longer available target=%s", qPrintable(target));
        else GUI_INFO("notification: opened chat target=%s", qPrintable(target));
    });

    m_trayMenu = new QMenu(this);
    QAction *settingsAction = m_trayMenu->addAction(QCoreApplication::translate("ChatGui", "Settings"));
    connect(settingsAction, &QAction::triggered, this, &MainWindow::showSettings);

    m_trayMenu->addSeparator();

    QAction *showAction = m_trayMenu->addAction(QCoreApplication::translate("ChatGui", "Show"));
    connect(showAction, &QAction::triggered, this, [this]() {
        showAndActivateWindow(this);
    });

    QAction *quitAction = m_trayMenu->addAction(QCoreApplication::translate("ChatGui", "Quit"));
    connect(quitAction, &QAction::triggered, this, &MainWindow::requestShutdown);

    m_trayIcon->setContextMenu(m_trayMenu);

    connect(m_trayIcon, &QSystemTrayIcon::activated,
            this, &MainWindow::onTrayActivated);

    m_trayIcon->show();
}

void MainWindow::showIncomingMessageNotification(const QString& target, const QString& author) {
    if (!m_db || !m_db->isOpen() || !m_messageNotification) {
        GUI_WARN("notification: GUI not ready target=%s author=%s", qPrintable(target), qPrintable(author));
        return;
    }
    QString channel;
    if (target.startsWith(QStringLiteral("dm:"))) channel = QCoreApplication::translate("ChatGui", "Direct messages");
    else {
        for (const auto& row : m_db->getChannels()) {
            if (row.channelId == target) { channel = row.name.isEmpty() ? target : row.name; break; }
        }
        if (channel.isEmpty()) {
            GUI_WARN("notification: channel no longer available target=%s", qPrintable(target));
            return;
        }
    }
    m_messageNotification->showMessage(target, author, channel, m_trayIcon->geometry(), screen());
}

void MainWindow::changeEvent(QEvent* event) {
    QMainWindow::changeEvent(event);
    if (event->type() != QEvent::ActivationChange || !isActiveWindow()) return;
    QTimer::singleShot(0, this, [this] {
        auto* input = m_messageList ? m_messageList->focusProxy() : nullptr;
        const bool ready = input && input->isVisible() && input->isEnabled();
        if (QApplication::activeWindow() != this || !isVisible() || isMinimized() ||
            QApplication::activeModalWidget() || QApplication::activePopupWidget() || !ready) {
            GUI_DEBUG("window activation: input focus skipped active=%d visible=%d minimized=%d modal=%d popup=%d input_ready=%d",
                      QApplication::activeWindow() == this, isVisible(), isMinimized(), bool(QApplication::activeModalWidget()),
                      bool(QApplication::activePopupWidget()), ready);
            return;
        }
        m_messageList->setFocus(Qt::ActiveWindowFocusReason);
        auto* focused = QApplication::focusWidget();
        GUI_DEBUG("window activation: message input focused target=%s focus_widget=%s", qPrintable(m_currentChannelId),
                  focused ? focused->metaObject()->className() : "none");
    });
}

void MainWindow::closeEvent(QCloseEvent *event) {
    DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "MainWindow: closeEvent — hiding to tray");
    saveWindowGeometry();
    hide();
    event->ignore();
}

void MainWindow::requestShutdown() {
    if (!m_node || !m_node->isRunning()) {
        DEBUG_INFO(DEBUG_CATEGORY_SYS, "desktop shutdown: core is not running; exiting GUI");
        qApp->exit(1);
        return;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "MainWindow: requestShutdown — saving geometry and stopping node");
    if (m_messageNotification) m_messageNotification->dismiss("shutdown");
    QPointer<QWidget> first = m_imageViewer.data(), last = m_videoPlayer.data();
    if (first && first->isActiveWindow()) std::swap(first, last); // Активный просмотр закрывается последним и задаёт общий снимок.
    if (first) first->close();
    if (last) last->close();
    saveWindowGeometry();
    if (m_messageList) m_messageList->saveChannelState();
    // Записи UI проходят две очереди. Ставим барьер в SQLite после обработки очереди uasync.
    if (m_kanban) m_kanban->finishEditing();
    auto** request = static_cast<UtunNode**>(u_malloc(sizeof(UtunNode*)));
    if (!request) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "desktop shutdown queue allocation failed; stopping directly"); m_node->requestStop(); return;
    }
    *request = m_node;
    if (gui_bridge_post_uasync_owned_fn([](void* arg) {
        auto* node = *static_cast<UtunNode**>(arg); u_free(arg);
        auto* inst = gui_bridge_get_inst();
        if (!inst || !db_async_can_write(inst->db_worker)) { node->requestStop(); return; }
        db_async_submit(inst->db_worker, "desktop-shutdown", DB_ASYNC_READ,
            [](sqlite3*, void*) { return SQLITE_OK; }, node, [](void* owner, const db_async_result* result) {
                if (result->status) DEBUG_ERROR(DEBUG_CATEGORY_SYS, "desktop shutdown database barrier failed status=%d rc=%d: %s",
                    result->status, result->sqlite_code, result->message);
                static_cast<UtunNode*>(owner)->requestStop();
            });
    }, request) < 0) {
        u_free(request); DEBUG_WARN(DEBUG_CATEGORY_SYS, "desktop shutdown queue unavailable; stopping directly"); m_node->requestStop();
    } else DEBUG_INFO(DEBUG_CATEGORY_SYS, "desktop shutdown queued after UI state writes");
}

void MainWindow::onShutdownCheck() {
#ifndef _WIN32
    if (g_signalFlag)
        requestShutdown();
#endif
}

void MainWindow::onTrayActivated(QSystemTrayIcon::ActivationReason reason) {
    if (reason == QSystemTrayIcon::Trigger ||
        reason == QSystemTrayIcon::DoubleClick) {
        showAndActivateWindow(this);
    }
}

void MainWindow::showShareInviteDialog(const QString& channelId) {
    if (!m_db || !m_db->isOpen()) return;

    QPointer<ShareInviteDialog> dialog = new ShareInviteDialog(channelId, m_db, this);
    auto release = qScopeGuard([&] { delete dialog.data(); }); dialog->exec();
}

void MainWindow::showJoinDialog() {
    if (!m_node || !m_node->isRunning()) {
        QMessageBox::warning(this, QCoreApplication::translate("ChatGui", "Error"), QCoreApplication::translate("ChatGui", "Node is not running."));
        return;
    }

    QString autoLink;
    QClipboard* cb = QApplication::clipboard();
    QString clipText = cb->text(QClipboard::Clipboard).trimmed();
    if (clipText.startsWith("utun://") && decodeInviteLink(clipText).error.isEmpty())
        autoLink = clipText;
    else if (cb->supportsSelection()) {
        QString selText = cb->text(QClipboard::Selection).trimmed();
        if (selText.startsWith("utun://") && decodeInviteLink(selText).error.isEmpty())
            autoLink = selText;
    }

    if (!autoLink.isEmpty() && decodeInviteLink(autoLink).kind == INVITE_KIND_SERVER) {
        onMessageLinkActivated(autoLink); return;
    }
    QPointer<JoinDialog> dialog = new JoinDialog(m_node, m_db, this, autoLink);
    auto release = qScopeGuard([&] { delete dialog.data(); }); dialog->exec();
}

void MainWindow::showSettings() {
    QPointer<MainWindow> self(this);
    QPointer<SettingsDialog> dialog = new SettingsDialog(m_cfgPath, m_db, m_node, m_recorder, this);
    auto release = qScopeGuard([&] { delete dialog.data(); });
    const int result = dialog->exec();
    if (self && dialog && result == QDialog::Accepted && dialog->requiresCoreRestart() && m_node && m_node->isRunning()) {
        DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "Restarting desktop core after hide_my_sockets change");
        m_node->stop();
        if (!m_node->start(m_cfgPath)) {
            DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "Desktop privacy restart failed: %s", qPrintable(m_node->startupError()));
            QMessageBox::warning(this, QCoreApplication::translate("ChatGui", "Settings"), m_node->startupError());
        } else DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "Desktop privacy restart completed");
    }
    if (self) reloadChannels();
}

void MainWindow::openVideo(const QString& filePath) {
    if (!m_videoPlayer) {
        m_videoPlayer = new VideoPlayerWindow(nullptr);
        m_videoPlayer->setObjectName(QStringLiteral("chatVideoPlayer"));
        restoreMediaWindowGeometry(m_videoPlayer);
        auto* player = m_videoPlayer.data();
        connect(player, &VideoPlayerWindow::fileOpened, m_messageList, &MessageList::onAttachmentOpened);
        connect(player, &VideoPlayerWindow::closed, this, [this, player] {
            saveMediaWindowGeometry(player);
            m_videoPlayer = nullptr;
            player->deleteLater();
        });
    } else {
        m_videoPlayer->raise();
        m_videoPlayer->activateWindow();
    }
    m_videoPlayer->playFile(filePath);
}

void MainWindow::openImage(const QString& filePath) {
    if (!m_imageViewer) {
        m_imageViewer = new ImageViewerWindow(nullptr);
        m_imageViewer->setObjectName(QStringLiteral("chatImageViewer"));
        restoreMediaWindowGeometry(m_imageViewer);
        auto* viewer = m_imageViewer.data();
        connect(viewer, &ImageViewerWindow::fileOpened, m_messageList, &MessageList::onAttachmentOpened);
        connect(viewer, &ImageViewerWindow::closed, this, [this, viewer] {
            saveMediaWindowGeometry(viewer);
            m_imageViewer = nullptr;
            viewer->deleteLater();
        });
    } else {
        m_imageViewer->raise();
        m_imageViewer->activateWindow();
    }
    m_imageViewer->loadFile(filePath);
    m_imageViewer->show();
}

void MainWindow::onMessageReceived(const char* ch_id, int ch_id_len, const uint8_t author[32], bool initialSync) {
    QString cid = QString::fromUtf8(ch_id, ch_id_len);
    const QByteArray key(reinterpret_cast<const char*>(author), 32);
    const bool own = m_node && QString::fromLatin1(key.toHex()) == m_node->nodeIdHex();
    if (!initialSync && m_db && !own && !isActiveWindow()) showIncomingMessageNotification(cid, m_db->getDisplayName(key));
    if (!initialSync && m_db && !own
        && m_db->getUiStateInt(QStringLiteral("ch_%1_sound_on_message").arg(cid), 1))
        SoundManager::instance()->play("msg_incoming");
    if (m_messageList) {
        if (cid == m_currentChannelId) {
            m_messageList->refresh(!initialSync && m_db && own);
        } else if (m_channelList && !initialSync && m_db && !own) {
            int unread = m_channelList->getChannelUnreadCount(cid) + 1;
            DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "MainWindow::onMessageReceived increment unread ch=%s %d->%d",
                       qPrintable(cid), unread - 1, unread);
            m_channelList->setChannelUnreadCount(cid, unread);
        }
    }
    if (m_db && m_db->getUiStateInt("channel_sort", 0) == 1)
        reloadChannels();

    if (!initialSync && m_db && !own && m_db->isOpen()
        && m_db->getUiStateInt(QStringLiteral("ch_%1_auto_play_voice").arg(cid), 0)) {
        QTimer::singleShot(100, this, [this, cid]() { autoPlayCheckNewMessage(cid); });
    }
}

void MainWindow::onMemberList(const QString& chId, int count, const uint8_t* data, int dataLen) {
    if (AvatarManager::instance()) AvatarManager::instance()->onMembers(chId, count, data, dataLen);
    m_accountList->onMemberList(chId, count, data, dataLen);
}

void MainWindow::onMemberUpdated(const QString& chId, const uint8_t* data) {
    if (AvatarManager::instance()) AvatarManager::instance()->onMembers(chId, 1, data, CHAT_MEMBER_DISPLAY_SIZE, false);
    m_accountList->onMemberUpdated(chId, data);
}

void MainWindow::onMemberRemoved(const QString& chId, const QByteArray& ed) {
    if (AvatarManager::instance()) AvatarManager::instance()->removeMember(chId, ed);
    m_accountList->onMemberRemoved(chId, ed);
}

void MainWindow::onAutoConnectStatus(uint8_t status, uint16_t /*total_tried*/,
                                      uint16_t /*node_count*/, uint16_t /*connected*/) {
    if (m_channelList) {
        m_channelList->setAutoConnectActive(status == 0);
    }
}

void MainWindow::onChannelPeersOnline(const char* ch_id, int ch_id_len, uint16_t online) {
    if (m_channelList) {
        QString cid = QString::fromUtf8(ch_id, ch_id_len);
        m_channelList->setChannelPeersOnline(cid, (int)online);
    }
}

void MainWindow::reloadChannels() {
    if (m_db && m_db->isOpen()) {
        const QString active = RadioAudioEngine::instance()->groupId();
        if (!active.isEmpty()) {
            bool exists = false;
            for (const auto& channel : m_db->getChannels()) if (channel.channelId == active) { exists = true; break; }
            if (!exists) {
                DEBUG_INFO(DEBUG_CATEGORY_RADIO, "Radio: active channel removed group=%s", qPrintable(active));
                RadioAudioEngine::instance()->stop();
            }
        }
    }
    if (!m_currentChannelId.isEmpty() && !m_currentChannelId.startsWith(QStringLiteral("dm:")) && m_db && m_db->isOpen()) {
        QString type = m_db->groupType(m_currentChannelId);
        QWidget* wanted = type == QStringLiteral("kanban") ? static_cast<QWidget*>(m_kanban) :
            type == QStringLiteral("chat") ? static_cast<QWidget*>(m_messageList) : m_unknownGroup;
        if (m_contentStack->currentWidget() != wanted) loadGroupContent(m_currentChannelId);
    }
    if (m_messageList && !m_currentChannelId.isEmpty() && !m_currentChannelId.startsWith(QStringLiteral("dm:")))
        m_messageList->refreshChannelTitle();
    if (m_db && m_db->isOpen() && !m_currentChannelId.isEmpty() && !m_currentChannelId.startsWith(QStringLiteral("dm:"))) {
        bool exists = false;
        for (const auto& channel : m_db->getChannels()) if (channel.channelId == m_currentChannelId) { exists = true; break; }
        if (!exists) {
            DEBUG_INFO(DEBUG_CATEGORY_CHAT, "channel removed from GUI: channel=%s", qPrintable(m_currentChannelId));
            for (int i = m_autoPlayQueue.size() - 1; i >= 0; --i) if (m_autoPlayQueue[i].channelId == m_currentChannelId) {
                m_autoPlayDownloading.remove(qMakePair(m_autoPlayQueue[i].channelId, m_autoPlayQueue[i].msgId));
                m_autoPlayQueue.removeAt(i);
            }
            if (m_pendingChannelSelect == m_currentChannelId) m_pendingChannelSelect.clear();
            loadGroupContent(QString());
            m_accountList->setChannel(QString());
            m_currentChannelId.clear();
            saveLastChannelId(QString());
            if (m_messageNotification) m_messageNotification->dismiss("channel deleted");
        }
    }
    if (m_channelList) {
        m_channelList->loadChannels();
        if (!m_pendingChannelSelect.isEmpty()) {
            m_channelList->selectChannel(m_pendingChannelSelect);
            m_accountList->setChannel(m_pendingChannelSelect);
            loadGroupContent(m_pendingChannelSelect);
            saveLastChannelId(m_pendingChannelSelect);
            m_currentChannelId = m_pendingChannelSelect;
            m_pendingChannelSelect.clear();
        }
    }
}

void MainWindow::onCreateGroupRequested() {
    if (!m_node || !m_node->isRunning()) {
        QMessageBox::warning(this, QCoreApplication::translate("ChatGui", "Error"), QCoreApplication::translate("ChatGui", "Node is not running."));
        return;
    }
    if (!gui_bridge_is_uasync_ready()) {
        QMessageBox::warning(this, QCoreApplication::translate("ChatGui",
            "Error"), QCoreApplication::translate("ChatGui",
            "uTun thread is not ready yet. Please wait and try again."));
        return;
    }

    QPointer<MainWindow> self(this); QPointer<CreateGroupDialog> dialog = new CreateGroupDialog(this);
    auto release = qScopeGuard([&] { delete dialog.data(); });
    const int result = dialog->exec();
    if (!self || !dialog || result != QDialog::Accepted) return;

    QString name = dialog->groupName();
    if (name.isEmpty()) return;

    auto* arg = static_cast<chat_create_auto_arg*>(u_calloc(1, sizeof(chat_create_auto_arg)));
    if (!arg) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "create group request allocation failed"); return; }
    const QByteArray bytes = name.toUtf8();
    if (bytes.size() >= int(sizeof(arg->name))) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "create group name exceeds request capacity: bytes=%d", int(bytes.size())); u_free(arg); return;
    }
    memcpy(arg->name, bytes.constData(), size_t(bytes.size()) + 1);
    snprintf(arg->group_type, sizeof(arg->group_type), "%s", dialog->groupType().toUtf8().constData());
    if (gui_bridge_post_bound_fn(chat_core_create_channel_auto_trampoline, arg, &arg->inst) < 0) { u_free(arg); return; }
    DEBUG_INFO(DEBUG_CATEGORY_CHAT, "MainWindow: create group request posted name=%s", qPrintable(name));
}

void MainWindow::showChannelSettings(const QString& channelId) {
    QPointer<ChannelSettingsDialog> dialog = new ChannelSettingsDialog(channelId, m_db, this);
    auto release = qScopeGuard([&] { delete dialog.data(); });
    connect(dialog, &ChannelSettingsDialog::inviteByLinkRequested, this, [this, dialog](const QString& channel) {
        if (!dialog) return;
        if (!m_node || !m_node->isRunning()) {
            DEBUG_WARN(DEBUG_CATEGORY_CHAT, "channel settings: invite refused because node is not running");
            QMessageBox::warning(dialog, QCoreApplication::translate("ChatGui", "Error"),
                QCoreApplication::translate("ChatGui", "Node is not running.")); return;
        }
        QPointer<InviteByLinkDialog> invite = new InviteByLinkDialog(channel, m_node, dialog);
        auto release = qScopeGuard([&] { delete invite.data(); }); invite->exec();
    });
    dialog->exec();
}

QString MainWindow::autoPlayMediaDir() const {
    if (m_db && m_db->isOpen())
        return m_db->dbPath() + QStringLiteral("/media");
    return QString();
}

/* Для encrypted-вложения MIME и plaintext известны только после проверки/decrypt; неизвестный файл не загружаем как voice. */
QString MainWindow::autoPlayFilePath(const QString& channelId, const MessageRow& message) const {
    if (!m_db) return {};
    if (message.contentType == QLatin1String(ENCRYPTED_ATTACHMENT_TYPE)) {
        const auto snapshot = m_db->getAttachment(channelId, message.messageId);
        if (!snapshot.ready || snapshot.error || snapshot.contentType != "audio/opus") return {};
        struct attachment_info info = {};
        if (attachment_decode((const uint8_t*)snapshot.info.constData(), snapshot.info.size(), &info) || info.kind != ATTACHMENT_VOICE) {
            DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "auto-play: invalid voice metadata channel=%s row=%lld", qPrintable(channelId), (long long)message.id);
            return {};
        }
        return snapshot.path;
    }
    if (message.contentType != "audio/opus") return {};
    const QString file = QJsonDocument::fromJson(message.localAttrs).object().value("fp").toString();
    return file.isEmpty() ? QString() : autoPlayMediaDir() + "/" + channelId + "/" + file;
}

void MainWindow::autoPlayCheckNewMessage(const QString& channelId, qint64 msgId) {
    if (!m_db || !m_db->isOpen() || !m_db->getUiStateInt(QStringLiteral("ch_%1_auto_play_voice").arg(channelId), 0)) return;
    MessageRow message;
    if (msgId > 0) message = m_db->getMessageById(channelId, msgId);
    else {
        const auto messages = m_db->getMessages(channelId, 1);
        if (messages.isEmpty()) return;
        message = messages.first();
    }
    if (!message.id || message.isOutgoing || message.id <= m_lastAutoPlayId.value(channelId)) return;
    const QString path = autoPlayFilePath(channelId, message);
    if (message.contentType != "audio/opus" && path.isEmpty()) return;
    AutoPlayItem item; item.channelId = channelId; item.msgId = message.id;
    if (path.isEmpty() || !QFile::exists(path)) {
        if (gui_bridge_download_attachment(channelId.toUtf8().constData(), message.id) < 0) return;
        item.downloadStarted = true; m_autoPlayDownloading.insert(qMakePair(channelId, message.id));
    }
    m_lastAutoPlayId.insert(channelId, message.id); m_autoPlayQueue.enqueue(item);
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "auto-play: queued channel=%s row=%lld download=%d",
                qPrintable(channelId), (long long)message.id, item.downloadStarted);
    m_autoPlayTimer->start(200); autoPlayTimerTick();
}

void MainWindow::autoPlayTimerTick() {
    if (!m_db || !m_db->isOpen()) {
        m_autoPlayTimer->stop(); m_autoPlayQueue.clear(); m_autoPlayDownloading.clear(); return;
    }
    if (VoicePlayer::isPreparing() || VoicePlayer::isPlaying()) return;
    while (!m_autoPlayQueue.isEmpty()) {
        AutoPlayItem& item = m_autoPlayQueue.head();
        const auto key = qMakePair(item.channelId, item.msgId);
        const MessageRow message = m_db->getMessageById(item.channelId, item.msgId);
        if (!message.id || !m_db->getUiStateInt(QStringLiteral("ch_%1_auto_play_voice").arg(item.channelId), 0)) {
            DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "auto-play: removed unavailable/disabled channel=%s row=%lld",
                        qPrintable(item.channelId), (long long)item.msgId);
            m_autoPlayDownloading.remove(key); m_autoPlayQueue.dequeue(); continue;
        }
        const QString path = autoPlayFilePath(item.channelId, message);
        if (!path.isEmpty() && QFile::exists(path)) {
            DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "auto-play: playing channel=%s row=%lld", qPrintable(item.channelId), (long long)item.msgId);
            m_autoPlayDownloading.remove(key); m_autoPlayQueue.dequeue();
            MessageDelegate::stopAllAutoPlay();
            VoicePlayer::playOpusFile(path, this, [path](float duration, uint64_t playbackId) {
                MessageDelegate::startAutoPlay(path, duration, playbackId);
            });
            return;
        }
        if (!item.downloadStarted) {
            if (gui_bridge_download_attachment(item.channelId.toUtf8().constData(), item.msgId) < 0) {
                m_autoPlayDownloading.remove(key); m_autoPlayQueue.dequeue(); continue;
            }
            item.downloadStarted = true; m_autoPlayDownloading.insert(key);
        }
        return;
    }
    m_autoPlayTimer->stop();
}

void MainWindow::onAttachmentDownloadedForAutoPlay(const QString& chId, int64_t msgId) {
    m_autoPlayDownloading.remove(qMakePair(chId, qint64(msgId)));
    autoPlayCheckNewMessage(chId, msgId);
    if (!m_autoPlayQueue.isEmpty()) { m_autoPlayTimer->start(200); autoPlayTimerTick(); }
}

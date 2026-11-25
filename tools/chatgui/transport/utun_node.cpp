// utun_node.cpp — embedded uTun node running in a dedicated thread
#include "utun_node.h"
#include "../../../src/config_error.h"
#include "../../../src/db_schema.h"
#include "stream_bridge.h"
#include <cinttypes>
#include <utility>
#include <system_error>
#include <QCoreApplication>
#include <QDir>
#include <QThread>
#include "../../lib/socket_compat.h"

// === C headers ===
extern "C" {
#include "utun_instance.h"
#include "etcp_router.h"
#include "etcp_api.h"
#include "etcp.h"
#include "config_parser.h"
#include "config_updater.h"
#include "control_server.h"
#include "secure_channel.h"
#include "../lib/u_async.h"
#include "../lib/ll_queue.h"
#include "../lib/memory_pool.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
    #include "chat/chat_sync.h"
    #include "chat/chat_core.h"
    #include "kanban/kanban.h"
    #include "chat/chat_event.h"
    #include "chat/db_sync.h"
    #include "call/call.h"
#include "call/call_audio.h"
#include "call/call_video_port.h"
#include "radio/radio_audio.h"
    #include "dm/dm_core.h"
    #include "dm/dm_mailbox.h"
    #include "gui_bridge.h"
#include "topo_node_sqlite.h"
#include "topo_node.h"
#include "topo_group.h"
}

UtunNode::UtunNode(QObject* parent) : QObject(parent) {}

UtunNode::~UtunNode() {
    stop();
    if (!m_debugLogPath.isEmpty() && g_debug_config.output_file == m_debugLogPath.constData()) debug_disable_file_output();
}

bool UtunNode::start(const QString& configPath) {
    if (m_running) {
        m_startupError = QStringLiteral("Node is already running");
        DEBUG_WARN(DEBUG_CATEGORY_SYS, "desktop startup rejected: node already running");
        emit error(m_startupError); return false;
    }
    m_configPath = configPath;
    m_startupConfigFailed = false;
    m_startupDatabaseIncompatible = false;
    m_stop = false;
    m_stopping = false;
    { std::lock_guard<std::mutex> guard(m_guiStopMutex); m_guiDetached = false; }
    m_running = true;
    ++m_generation;
    std::promise<QString> startup;
    auto ready = startup.get_future();
    try {
        m_thread = std::thread(&UtunNode::runLoop, this, std::move(startup));
    } catch (const std::system_error& e) {
        m_running = false;
        m_startupError = QStringLiteral("Worker thread creation failed: %1").arg(QString::fromLocal8Bit(e.what()));
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "desktop startup: %s", qPrintable(m_startupError));
        emit error(m_startupError); return false;
    }
    m_startupError = ready.get();
    if (!m_startupError.isEmpty()) {
        if (m_thread.joinable()) m_thread.join();
        finalize();
        emit error(m_startupError);
        return false;
    }
    emit started();
    return true;
}

void UtunNode::setDebugFile(const QString& path) {
    m_debugFile = path;
}

void UtunNode::setDbPath(const QString& path) {
    m_dbPath = path;
    if (!QDir(path).mkpath(".")) DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "desktop database directory creation failed: %s", qPrintable(path));
}

void UtunNode::setDebugLevel(const QString& level) {
    m_debugLevel = level;
}

void UtunNode::setDebugCategories(const QString& categories) {
    m_debugCategories = categories;
}

void UtunNode::requestStop() {
    m_stop = true;
    std::lock_guard<std::mutex> guard(m_uaMutex);
    if (m_ua) uasync_wakeup(m_ua);
}

void UtunNode::stop() {
    if (!m_running) return;
    m_stopping = true;
    detachGuiReaders();
    requestStop();
    if (m_thread.joinable()) m_thread.join();
    finalize();
    m_running = false;
}

/* GUI уже закончил текущие чтения; worker может закрыть общий handle после всех direct-слотов. */
void UtunNode::detachGuiReaders() {
    { std::lock_guard<std::mutex> guard(m_guiStopMutex); if (m_guiDetached) return; }
    emit stopping();
    gui_bridge_set_reader(nullptr);
    if (m_guiDatabase) {
        int rc = sqlite3_close(m_guiDatabase);
        if (rc != SQLITE_OK) {
            DEBUG_ERROR(DEBUG_CATEGORY_SYS, "desktop GUI reader close rc=%d: %s", rc, sqlite3_errmsg(m_guiDatabase));
            sqlite3_close_v2(m_guiDatabase);
        }
        m_guiDatabase = nullptr;
    }
    { std::lock_guard<std::mutex> guard(m_guiStopMutex); m_guiDetached = true; }
    m_guiStopCondition.notify_all();
    DEBUG_DEBUG(DEBUG_CATEGORY_SYS, "desktop GUI readers detached: generation=%llu", (unsigned long long)m_generation);
}

struct sqlite3* UtunNode::databaseHandle() const {
    std::lock_guard<std::mutex> guard(m_snapshotMutex);
    if (m_stopping || !m_running) return nullptr;
    if (m_snapshot.database) return m_snapshot.database; /* Только автономная RAM-конфигурация. */
    if (m_guiDatabase) return m_guiDatabase;
    if (!m_snapshot.databaseReady || m_snapshot.databasePath.isEmpty() || QThread::currentThread() != thread()) return nullptr;
    const QByteArray file = QDir(m_snapshot.databasePath).filePath(QStringLiteral("chats.db")).toUtf8();
    int rc = sqlite3_open_v2(file.constData(), &m_guiDatabase, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (rc == SQLITE_OK) rc = sqlite3_busy_timeout(m_guiDatabase, 1000);
    if (rc == SQLITE_OK) rc = sqlite3_exec(m_guiDatabase, "PRAGMA query_only=ON; PRAGMA foreign_keys=ON", nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "desktop GUI reader open rc=%d file=%s: %s", rc, file.constData(),
            m_guiDatabase ? sqlite3_errmsg(m_guiDatabase) : sqlite3_errstr(rc));
        sqlite3_close_v2(m_guiDatabase); m_guiDatabase = nullptr; return nullptr;
    }
    gui_bridge_set_reader(m_guiDatabase);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "desktop GUI read-only connection opened file=%s", file.constData());
    return m_guiDatabase;
}

QString UtunNode::databasePath() const {
    std::lock_guard<std::mutex> guard(m_snapshotMutex);
    return m_snapshot.databasePath;
}

void UtunNode::finalize() {
    gui_bridge_set_inst(nullptr);
    gui_bridge_set_uasync(nullptr);
    {
        std::lock_guard<std::mutex> guard(m_uaMutex);
        if (m_ua) { uasync_destroy(m_ua, 0); m_ua = nullptr; }
    }
    m_instance = nullptr;
    updateSnapshot(nullptr);
    m_running = false;
    DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "finalize: cleanup done");
}

QString UtunNode::nodeIdHex() const {
    std::lock_guard<std::mutex> guard(m_snapshotMutex);
    return m_snapshot.nodeId;
}

QString UtunNode::pubKeyHex() const {
    std::lock_guard<std::mutex> guard(m_snapshotMutex);
    return m_snapshot.publicKey;
}

int64_t UtunNode::ntpOffsetUs() const {
    std::lock_guard<std::mutex> guard(m_snapshotMutex);
    return m_snapshot.ntpOffsetUs;
}

bool UtunNode::ntpSynced() const {
    std::lock_guard<std::mutex> guard(m_snapshotMutex);
    return m_snapshot.ntpSynced;
}

bool UtunNode::ntpEnabled() const {
    std::lock_guard<std::mutex> guard(m_snapshotMutex);
    return m_snapshot.ntpEnabled;
}

DesktopBleSnapshot UtunNode::bluetoothSnapshot() const {
    std::lock_guard<std::mutex> guard(m_snapshotMutex);
    return m_snapshot.bluetooth;
}

bool UtunNode::setBluetoothFlags(int flags) {
    if (flags < 0 || flags > 7 || !m_running || m_stopping) {
        DEBUG_WARN(DEBUG_CATEGORY_DISCOVERY, "BLE desktop settings rejected flags=%d running=%d stopping=%d",
                   flags, (int)m_running, (int)m_stopping);
        return false;
    }
    m_bluetoothFlags = flags;
    std::lock_guard<std::mutex> guard(m_uaMutex);
    if (m_ua) uasync_wakeup(m_ua);
    return true;
}

ble_transport* UtunNode::bluetoothTransport() const { return m_bluetooth ? m_bluetooth->transport() : nullptr; }

/* Значения копируются в потоке ядра; заимствованный SQLite защищён барьером stopping до уничтожения instance. */
void UtunNode::updateSnapshot(const UTUN_INSTANCE* inst) {
    std::lock_guard<std::mutex> guard(m_snapshotMutex);
    if (!inst) { m_snapshot = {}; return; }
    m_snapshot.database = inst->topo_sqlite_db;
    m_snapshot.databaseReady = inst->database_ready;
    m_snapshot.databasePath = QString::fromUtf8(inst->config->global.db_path);
    if (m_snapshot.nodeId.isEmpty()) {
        m_snapshot.nodeId = QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(inst->my_ed25519_pubkey), 32).toHex());
        m_snapshot.publicKey = QString::fromLatin1(inst->config->global.my_public_key_hex, 64);
    }
    m_snapshot.ntpOffsetUs = inst->ntp.offset_us;
    m_snapshot.ntpSynced = inst->ntp.synced;
    m_snapshot.ntpEnabled = inst->ntp.enabled;
    if (m_bluetooth) m_snapshot.bluetooth = m_bluetooth->snapshot();
}

void UtunNode::runLoop(std::promise<QString> startup) {
    // Повторный init не закрывает прежний FILE; путь прошлого запуска пока ещё жив.
    debug_disable_file_output();
    debug_config_init();
    debug_set_file_rotation(10 * 1024 * 1024, 5);
    debug_enable_function_name(0);

    /* ── debug level from config ── */
    if (!m_debugLevel.isEmpty()) {
        QString lvl = m_debugLevel.toLower();
        debug_level_t dl = DEBUG_LEVEL_INFO;
        if (lvl == "none")  dl = DEBUG_LEVEL_NONE;
        else if (lvl == "error") dl = DEBUG_LEVEL_ERROR;
        else if (lvl == "warn")  dl = DEBUG_LEVEL_WARN;
        else if (lvl == "info")  dl = DEBUG_LEVEL_INFO;
        else if (lvl == "debug") dl = DEBUG_LEVEL_DEBUG;
        else if (lvl == "trace") dl = DEBUG_LEVEL_TRACE;
        g_debug_config.level          = dl;
    } else {
        g_debug_config.level          = DEBUG_LEVEL_INFO;
    }

    /* Системные ошибки и жизненный цикл ядра должны быть видны без специальной настройки. */
    g_debug_config.category_levels[DEBUG_CATEGORY_SYS] = DEBUG_LEVEL_INFO;
    g_debug_config.category_levels[DEBUG_CATEGORY_BGP]    = DEBUG_LEVEL_INFO;

    /* per-category overrides from config: cat=level,cat=level,... */
    if (!m_debugCategories.isEmpty() && debug_parse_config(m_debugCategories.toUtf8().constData()) < 0)
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "desktop debug categories contain invalid settings: %s", qPrintable(m_debugCategories));

    /* file output (shared with GUI via debug_config) */
    if (!m_debugFile.isEmpty()) {
        m_debugLogPath = m_debugFile.toUtf8();
        debug_enable_file_output(m_debugLogPath.constData(), 0);
        debug_enable_console(1);
        g_debug_config.thread_marker = 2;
    }

    utun_instance_set_tun_init_enabled(1);

    struct UASYNC* ua = uasync_create();
    if (!ua) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "desktop startup: uasync_create failed");
        startup.set_value(QStringLiteral("uasync_create failed"));
        return;
    }
    {
        std::lock_guard<std::mutex> guard(m_uaMutex);
        m_ua = ua;
    }

    UTUN_INSTANCE* inst = nullptr;
    // Один откат для любого частичного запуска, до публикации готовности в GUI.
    auto failed = [&](const QString& reason) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "desktop startup rollback: reason=%s instance=%p", qPrintable(reason), (void*)inst);
        if (inst) {
            gui_stream_detach(inst); call_video_port_detach(inst);
            call_audio_destroy(inst); radio_audio_destroy(inst);
            utun_instance_destroy(inst);
        }
        {
            std::lock_guard<std::mutex> guard(m_uaMutex);
            uasync_destroy(ua, 0); m_ua = nullptr;
        }
        startup.set_value(reason);
    };

    /* SQL bootstrap идёт в worker; внешний startup loop продолжает обслуживать uasync. */
    struct utun_config* cfg = parse_config(m_configPath.toUtf8().constData());
    if (!cfg) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "desktop startup: parse_config failed path=%s", qPrintable(m_configPath));
        m_startupConfigFailed = true;
        failed(QString::fromUtf8(config_error_text()));
        return;
    }
    cfg->global.db_sync_enabled = 1;
    cfg->global.client_type = CLIENT_TYPE_DESKTOP;
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "chatgui: device=desktop; supernode role is channel-specific");
    if (!m_dbPath.isEmpty()) {
        const QByteArray directory = m_dbPath.toUtf8();
        if (directory.size() >= int(sizeof(cfg->global.db_path))) {
            DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Database directory too long bytes=%d limit=%zu", int(directory.size()), sizeof(cfg->global.db_path) - 1);
            free_config(cfg);
            failed(QStringLiteral("Database directory is too long:\n%1").arg(m_dbPath)); return;
        }
        memcpy(cfg->global.db_path, directory.constData(), size_t(directory.size()) + 1);
    }

    struct StartupResult { bool ready = false; db_async_result result{}; } database;
    inst = utun_instance_create_from_config_async(ua, cfg, [](void* arg, UTUN_INSTANCE*, const db_async_result* result) {
        auto* state = static_cast<StartupResult*>(arg); state->result = *result; state->ready = true;
    }, &database);
    if (!inst) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "desktop startup: instance creation failed");
        m_startupDatabaseIncompatible = db_schema_is_incompatible();
        free_config(cfg);
        failed(*db_schema_last_error() ? QString::fromUtf8(db_schema_last_error()) : QStringLiteral("utun_instance_create_from_config failed"));
        return;
    }
    while (!database.ready && !m_stop) uasync_poll(ua, 50);
    if (!database.ready || database.result.status) {
        m_startupDatabaseIncompatible = inst->database_incompatible;
        failed(!database.ready ? QStringLiteral("Startup cancelled") : QString::fromUtf8(inst->database_error)); return;
    }

    inst->test_user_ptr = this;

    if (utun_core_start(inst) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "desktop startup: utun_core_start failed");
        failed(QStringLiteral("utun_core_start failed"));
        return;
    }

    DEBUG_INFO(DEBUG_CATEGORY_SYS, "desktop service policy: utun_enabled=%d tun_enabled=%d",
               cfg->global.gui_utun_enabled, cfg->global.tun_enabled);
    if (cfg->global.gui_utun_enabled && utun_service_start(inst) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "desktop startup: UTUN service failed tun_enabled=%d interface=%s",
                    cfg->global.tun_enabled, cfg->global.tun_ifname);
        failed(QStringLiteral("utun_service_start failed; check TUN configuration, driver and permissions"));
        return;
    }

    { int sock_count = 0; for (struct ETCP_SOCKET* s = inst->etcp_sockets; s; s = s->next) sock_count++;
      DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "utun_node: after instance_init, etcp_sockets=%d conns=%d",
                 sock_count, queue_entry_count(inst->connections)); }

    utun_add_nodeinfo_cbk(inst, gui_nodeinfo_cb_impl, nullptr);

    /* Bridge chat events to GUI via gui_bridge */
    chat_event_set_handler(inst, [](struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int len) {
        (void)inst;
        gui_bridge_post(type, data, len);
    });

    StartupResult chat;
    chat_service_start_async(inst, [](void* arg, const db_async_result* result) {
        auto* state = static_cast<StartupResult*>(arg); state->result = *result; state->ready = true;
    }, &chat);
    while (!chat.ready && !m_stop) uasync_poll(ua, 50);
    if (!chat.ready || chat.result.status || kanban_service_start(inst) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "desktop startup: chat_service_start failed");
        failed(QStringLiteral("chat_service_start failed"));
        return;
    }
    if (call_audio_init(inst) != 0) { failed(QStringLiteral("call_audio_init failed")); return; }
    if (call_video_port_attach(inst) != 0) { failed(QStringLiteral("call_video_port_attach failed")); return; }
    if (radio_audio_init(inst) != 0) { failed(QStringLiteral("radio_audio_init failed")); return; }
    if (!gui_stream_attach(inst)) { failed(QStringLiteral("stream bridge initialization failed")); return; }
    chat_core_sync_my_addresses(inst);

    DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "chat_core + chat_sync initialized");
    uasync_mark_running(ua);
    DesktopBle bluetooth(ua, inst->my_keys.public_key, inst);
    m_bluetooth = &bluetooth;
    m_bluetoothFlags = cfg->global.gui_bluetooth_flags;
    bluetooth.apply(m_bluetoothFlags, cfg->global.name);
    updateSnapshot(inst);
    m_instance = inst;
    gui_bridge_set_uasync(ua);
    gui_bridge_set_inst(inst);
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "utun_node: poll started ua=%p running=%d", (void*)ua, uasync_is_running(ua));
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "desktop services ready: core=%d utun=%d chat=%d tun=%d interface=%s",
               inst->core_started, inst->utun_started, inst->chat_started,
               cfg->global.tun_enabled, cfg->global.tun_ifname);
    startup.set_value(QString());

    const uint64_t generation = m_generation;
    while (!m_stop && inst->running) {
        uasync_poll(ua, 100);
        bluetooth.apply(m_bluetoothFlags, cfg->global.name);
        updateSnapshot(inst);
    }
    if (!m_stop) DEBUG_ERROR(DEBUG_CATEGORY_SYS, "desktop core requested stop; releasing services outside callbacks");

    m_stopping = true;
    bluetooth.apply(0, cfg->global.name);
    updateSnapshot(inst);
    m_bluetooth = nullptr;
    gui_bridge_set_inst(nullptr);
    m_instance = nullptr;
    uasync_mark_stopped(ua);
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "utun_node: poll stopped ua=%p running=%d requested=%d",
               (void*)ua, uasync_is_running(ua), (int)m_stop);

    {
        std::unique_lock<std::mutex> guard(m_guiStopMutex);
        if (!m_guiDetached) {
            DEBUG_DEBUG(DEBUG_CATEGORY_SYS, "desktop teardown waiting for GUI readers: generation=%llu", (unsigned long long)generation);
            if (!QMetaObject::invokeMethod(this, [this, generation] {
                if (generation == m_generation) detachGuiReaders();
            }, Qt::QueuedConnection)) DEBUG_ERROR(DEBUG_CATEGORY_SYS, "desktop cannot queue GUI reader detach");
            m_guiStopCondition.wait(guard, [this] { return m_guiDetached; });
        }
    }

    DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "runLoop: destroying instance in worker thread");
    gui_stream_detach(inst); call_video_port_detach(inst);
    call_audio_destroy(inst); radio_audio_destroy(inst);
    utun_instance_destroy(inst);

    DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "runLoop: emitting stopped");
    QMetaObject::invokeMethod(this, [this, generation] {
        if (generation == m_generation) emit stopped();
        else DEBUG_DEBUG(DEBUG_CATEGORY_SYS, "desktop stale stopped ignored: generation=%llu current=%llu",
                         (unsigned long long)generation, (unsigned long long)m_generation);
    });
}

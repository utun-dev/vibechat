#include "gui_bridge.h"
#include "member_edit.h"
#include "db_read.h"
#include "chat/chat_event.h"

#include <QObject>
#include <QMetaObject>
#include <QByteArray>
#include <QCoreApplication>
#include <QPointer>
#include <QHash>
#include <QThread>
#include <cstring>
#include <mutex>
#include <atomic>
#include <new>
#include <exception>
#include <limits>
#include <future>
#include <memory>
#include <chrono>
#include <thread>
#include <openssl/crypto.h>

extern "C" {
#include "../../../lib/u_async.h"
#include "../../../lib/mem.h"
#include "../../../lib/debug_config.h"
#include "../../../lib/db_async.h"
#include "../../../src/utun_instance.h"
#include "../../../src/routing_layer/topo_node.h"
#include "../../../src/routing_layer/topo_group.h"
#include "../../../src/member_files/member_files.h"
#include "../../../src/chat/chat_member.h"
#include "../../../src/chat/chat_core.h"
#include "../../../src/chat/chat_channel.h"
#include "../../../src/chat/chat_clipboard.h"
#include "../../../src/chat/chat_network_stats.h"
#include "../../../src/chat/chat_sync.h"
#include "../../../src/chat/chat_join_port.h"
#include "../../../src/dm/dm_core.h"
#include "../../../src/call/call.h"
#include "../../../src/radio/radio_call.h"
#include "../../../src/media_async/attachment_send.h"
}

/* ── Внутренний объект-приёмник в GUI-потоке ── */

class GuiBridgeReceiver : public QObject {
    Q_OBJECT
public:
    GuiBridgeReceiver(QObject* parent = nullptr) : QObject(parent) {}

    Q_INVOKABLE void processPost(int eventType, QByteArray data);
};

static std::mutex bridge_mutex;
static struct chat_join_port* join_port = nullptr;
static std::atomic<uint64_t> join_sequence{1};

static GuiBridgeReceiver*    g_receiver           = nullptr;
static gui_connect_result_fn g_connect_result_cb  = nullptr;
static gui_msg_received_fn   g_msg_received_cb    = nullptr;
static gui_channel_updated_fn  g_channel_updated_cb  = nullptr;
static gui_auto_connect_status_fn g_auto_connect_status_cb = nullptr;
static gui_channel_peers_online_fn g_channel_peers_online_cb = nullptr;
static gui_db_ready_fn          g_db_ready_cb          = nullptr;
static gui_status_refresh_fn    g_status_refresh_cb    = nullptr;
static gui_cpu_load_fn          g_cpu_load_cb          = nullptr;
static gui_attachment_downloaded_fn g_attachment_downloaded_cb = nullptr;
static gui_download_progress_fn     g_download_progress_cb     = nullptr;
static gui_invite_received_fn       g_invite_received_cb       = nullptr;
static gui_conn_list_fn              g_conn_list_cb            = nullptr;
static gui_conn_metrics_fn           g_conn_metrics_cb         = nullptr;
static gui_conn_metrics_fn           g_network_stats_cb        = nullptr;
static gui_member_detail_fn          g_member_detail_cb        = nullptr;
static gui_member_list_fn            g_member_list_cb          = nullptr;
static gui_member_updated_fn         g_member_updated_cb       = nullptr;
static gui_member_removed_fn         g_member_removed_cb       = nullptr;
static gui_admin_key_received_fn     g_admin_key_received_cb   = nullptr;
static gui_invite_link_ready_fn      g_invite_link_ready_cb    = nullptr;
static gui_invite_candidates_fn      g_invite_candidates_cb    = nullptr;
static gui_local_event_fn g_local_devices_cb = nullptr;
static gui_local_event_fn g_local_invite_cb = nullptr;
static gui_local_event_fn g_local_delivery_cb = nullptr;
static gui_call_incoming_fn          g_call_incoming_cb        = nullptr;
static gui_call_id_fn                g_call_ringing_cb         = nullptr;
static gui_call_id_fn                g_call_phone_busy_cb      = nullptr;
static gui_call_peer_fn              g_call_accepted_cb        = nullptr;
static gui_call_ended_fn             g_call_ended_cb           = nullptr;
static gui_call_ended_fn             g_call_declined_cb        = nullptr;
static gui_call_error_fn             g_call_error_cb           = nullptr;
static gui_call_stats_fn g_call_stats_cb = nullptr;
static gui_call_path_fn              g_call_path_cb            = nullptr;
static gui_call_connection_fn        g_call_connection_cb      = nullptr;
static gui_radio_talk_fn             g_radio_talk_cb           = nullptr;
static gui_dm_msg_received_fn        g_dm_msg_received_cb      = nullptr;
static gui_dm_conv_updated_fn        g_dm_conv_updated_cb      = nullptr;
static gui_dm_messages_fn            g_dm_start_result_cb      = nullptr;
static gui_dm_messages_fn            g_dm_messages_cb          = nullptr;
static gui_dm_messages_fn            g_dm_members_cb           = nullptr;
static gui_dm_messages_fn            g_attachment_preparation_cb = nullptr;
static gui_dm_messages_fn g_dm_call_route_cb = nullptr;
static gui_dm_messages_fn g_dm_invite_result_cb = nullptr;
static gui_dm_messages_fn g_dm_delete_result_cb = nullptr;
static gui_dm_messages_fn g_member_files_response_cb = nullptr;
static gui_dm_messages_fn g_member_files_transfer_cb = nullptr;
static gui_dm_messages_fn g_avatar_result_cb = nullptr;
static gui_dm_messages_fn g_group_avatar_result_cb = nullptr;
static gui_dm_messages_fn g_group_policy_result_cb = nullptr;
static gui_dm_messages_fn g_group_info_result_cb = nullptr;
static gui_dm_messages_fn g_kanban_cb = nullptr;
static gui_dm_messages_fn g_feedback_changed_cb = nullptr;
static gui_dm_messages_fn g_attachment_keys_cb = nullptr;
static gui_dm_messages_fn g_devgroup_result_cb = nullptr;
static gui_dm_messages_fn g_clipboard_received_cb = nullptr;
static gui_dm_messages_fn g_clipboard_result_cb = nullptr;
static gui_dm_messages_fn g_feedback_result_cb = nullptr;
static gui_dm_messages_fn g_msg_submitted_cb = nullptr;
static gui_dm_messages_fn g_msg_updated_cb = nullptr;
static gui_dm_messages_fn g_radio_mute_result_cb = nullptr;
static gui_dm_messages_fn g_radio_call_cb = nullptr;
static gui_dm_messages_fn g_stream_sources_cb = nullptr;
static gui_network_changed_fn g_network_changed_cb = nullptr;
static struct UASYNC*        g_ua                 = nullptr;
static std::atomic<UTUN_INSTANCE*> g_inst{nullptr};
static std::atomic<sqlite3*> g_gui_reader{nullptr}; // Только token GUI-профиля; worker никогда не выполняет на нём SQL.
static uint64_t network_stats_sequence = 0, network_stats_pending = 0; // Под bridge_mutex, включая detach и ответ.

static bool gui_database_current(UTUN_INSTANCE* inst, sqlite3* database) {
    return inst && database && (db_async_can_write(inst->db_worker) ? g_gui_reader.load() == database : inst->topo_sqlite_db == database);
}

struct MemberCompletion {
    QPointer<QObject> context; // Читается только в GUI-потоке.
    std::function<void(int)> callback;
    QMetaObject::Connection destroyed;
};
static QHash<quint64, MemberCompletion> member_completions; // GUI-only, запись живёт до результата/уничтожения context.
static quint64 member_sequence = 0;
struct MemberEditRequest {
    UTUN_INSTANCE* instance;
    sqlite3* database;
    quint64 request;
    size_t nicknameBytes;
    uint8_t ed25519[32];
    char channel[65];
    // За структурой: nickname+NUL, owner JSON+NUL; единый u_malloc-блок.
};

/* uasync не трогает QObject формы; корреляция и проверка lifetime происходят в GUI. */
static void member_edit_result(quint64 request, int result) {
    if (!g_receiver || !QMetaObject::invokeMethod(g_receiver, [request, result] {
        auto it = member_completions.find(request);
        if (it == member_completions.end()) return;
        MemberCompletion completion = std::move(it.value()); member_completions.erase(it);
        QObject::disconnect(completion.destroyed);
        if (completion.context) completion.callback(result);
    }, Qt::QueuedConnection)) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member save: result delivery failed request=%llu",
        (unsigned long long)request);
}

static void cancel_member_edit(void* arg) {
    auto* request = static_cast<MemberEditRequest*>(arg);
    char peer[65];
    DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "member save: cancelled request=%llu ch=%s peer=%s",
        (unsigned long long)request->request, request->channel, sc_public_key_hex(request->ed25519, peer));
    member_edit_result(request->request, -3);
}

static void member_edit_done(void* arg, const db_async_result* result) {
    auto* request = static_cast<MemberEditRequest*>(arg);
    member_edit_result(request->request, result->status ? -1 : 0); u_free(request);
}

static void execute_member_edit(void* arg) {
    auto* request = static_cast<MemberEditRequest*>(arg);
    const char* nickname = reinterpret_cast<const char*>(request + 1);
    int result = -3;
    if (!gui_database_current(request->instance, request->database)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member save: database changed request=%llu", (unsigned long long)request->request);
    } else {
        chat_core_save_member_async(request->instance, request->channel, request->ed25519,
            nickname, nickname + request->nicknameBytes + 1, member_edit_done, request);
        return;
    }
    member_edit_result(request->request, result); u_free(request);
}

/* Зарезервированный post владеет своей записью; owned=true автоматически u_free payload при отмене.
 * cancel, если задан, выполняет уведомление/очистку до этого u_free; при owned=false он сам освобождает payload.
 * Все ссылки списка защищены bridge_mutex. uasync освобождает post после callback/отмены. */
struct BridgePost {
    struct posted_task post;
    BridgePost* previous;
    BridgePost* next;
    void (*function)(void*);
    void* arg;
    void (*cancel)(void*);
    bool owned, linked, cancelled;
};
static_assert(offsetof(BridgePost, post) == 0, "uasync must free the complete bridge post");
static BridgePost* pending_posts = nullptr;
static std::thread::id core_thread;
struct RadioRouteRequest { UTUN_INSTANCE* inst; QByteArray channel; std::promise<uint64_t> result; };

/* Удаление за O(1), без ожидания исполнения функции пользователя под mutex моста. */
static void unlink_post(BridgePost* command) {
    if (command->previous) command->previous->next = command->next;
    else pending_posts = command->next;
    if (command->next) command->next->previous = command->previous;
    command->linked = false;
}

static void execute_post(void* arg) {
    auto* command = static_cast<BridgePost*>(arg);
    {
        std::lock_guard<std::mutex> guard(bridge_mutex);
        if (command->linked) unlink_post(command);
    }
    if (!command->cancelled) command->function(command->arg);
    else {
        if (command->cancel) command->cancel(command->arg);
        if (command->owned) u_free(command->arg);
    }
}

/* Вызывать под bridge_mutex: принятие команды и закрытие ядра образуют одну границу владения. */
static int post_command(void (*function)(void*), void* arg, bool owned, void (*cancel)(void*) = nullptr) {
    if (!function || !g_ua || !g_inst) {
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "GUI command rejected: core unavailable or missing callback"); return -1;
    }
    auto* command = static_cast<BridgePost*>(u_calloc(1, sizeof(BridgePost)));
    if (!command) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "GUI command allocation failed"); return -1; }
    command->function = function; command->arg = arg; command->owned = owned; command->linked = true;
    command->cancel = cancel;
    command->next = pending_posts;
    if (pending_posts) pending_posts->previous = command;
    pending_posts = command;
    command->post.callback = execute_post; command->post.arg = command;
    uasync_post_reserved(g_ua, &command->post);
    return 0;
}

/* Ядро снимается в своём потоке вне poll callbacks, до destroy instance и uasync. */
static void cancel_posts() {
    unsigned cancelled = 0;
    while (pending_posts) {
        auto* command = pending_posts;
        unlink_post(command); command->cancelled = true;
        void* arg = command->arg;
        const bool owned = command->owned;
        void (*cancel)(void*) = command->cancel;
        if (uasync_cancel_post(g_ua, &command->post) == ERR_OK) {
            if (cancel) cancel(arg);
            if (owned) u_free(arg);
            cancelled++;
        } else {
            DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "GUI command already dispatched during detach; deferred disposal");
        }
    }
    if (cancelled) DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "GUI bridge detached: cancelled_commands=%u", cancelled);
}

struct DbReadCompletion {
    QPointer<QObject> context; // Только GUI; worker/отмена моста его не читают.
    GuiDbReadDone callback;
    QMetaObject::Connection destroyed;
};
static QHash<quint64, DbReadCompletion> db_read_completions; // GUI-only, удаляется до пользовательского callback.
static quint64 db_read_sequence = 0; // Не сбрасывается между перезапусками ядра/менеджера.
struct DbReadRequest {
    GuiDbReadWork work; // Замороженные значения, только worker до SQL done.
    QVariant value; // Собственный результат worker, передаётся в GUI после успешного READ.
    UTUN_INSTANCE* instance; // Только uasync: зафиксирован под bridge_mutex, живёт до db_async_destroy.
    sqlite3* expected; // Только uasync: сравнение идентичности старого handle, без SQL на нём.
    quint64 id; // Корреляция с GUI, без QObject-указателей в запросе.
};

static void free_db_read(DbReadRequest* request) { request->~DbReadRequest(); u_free(request); }

/* Уведомление отмены под bridge_mutex только ставит Qt-событие; код клиента выполняется позже в GUI. */
static void db_read_result(DbReadRequest* request, const db_async_result& result) {
    const auto id = request->id;
    QVariant value = result.status == DB_ASYNC_OK ? std::move(request->value) : QVariant();
    if (!g_receiver || !QMetaObject::invokeMethod(g_receiver, [id, result, value = std::move(value)] {
        auto it = db_read_completions.find(id);
        if (it == db_read_completions.end()) return;
        DbReadCompletion completion = std::move(it.value()); db_read_completions.erase(it);
        QObject::disconnect(completion.destroyed);
        if (completion.context) completion.callback(result, value);
    }, Qt::QueuedConnection)) DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "GUI DB result delivery failed request=%llu", (unsigned long long)id);
    free_db_read(request);
}
static void cancel_db_read(void* data) {
    auto* request = static_cast<DbReadRequest*>(data);
    DEBUG_WARN(DEBUG_CATEGORY_CONFIG, "GUI DB read cancelled before SQL request=%llu", (unsigned long long)request->id);
    const db_async_result result{DB_ASYNC_CLOSED, SQLITE_OK, "database request cancelled before execution"};
    db_read_result(request, result);
}
static int db_read_work(sqlite3* db, void* data) {
    auto* request = static_cast<DbReadRequest*>(data);
    try { return request->work(db, &request->value); }
    catch (const std::bad_alloc&) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "GUI DB read allocation failure request=%llu", (unsigned long long)request->id); return SQLITE_NOMEM;
    } catch (const std::exception& error) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "GUI DB read exception request=%llu: %s", (unsigned long long)request->id, error.what());
        return SQLITE_ABORT;
    } catch (...) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "GUI DB read unknown exception request=%llu", (unsigned long long)request->id); return SQLITE_ABORT;
    }
}
static void db_read_done(void* data, const db_async_result* result) {
    auto* request = static_cast<DbReadRequest*>(data);
    if (result->status) DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "GUI DB read failed request=%llu status=%d rc=%d: %s",
        (unsigned long long)request->id, result->status, result->sqlite_code, result->message);
    db_read_result(request, *result);
}
static void execute_db_read(void* data) {
    auto* request = static_cast<DbReadRequest*>(data);
    if (!gui_database_current(request->instance, request->expected) || !request->instance->db_worker) {
        const db_async_result result{DB_ASYNC_CLOSED, SQLITE_OK, "database reader unavailable or database changed"};
        db_read_done(request, &result); return;
    }
    db_async_submit(request->instance->db_worker, "gui-read", DB_ASYNC_READ, db_read_work, request, db_read_done);
}

bool gui_bridge_db_read(sqlite3* expected, QObject* context, GuiDbReadWork work, GuiDbReadDone complete) {
    if (!expected || !context || !work || !complete || !g_receiver || QThread::currentThread() != g_receiver->thread() ||
        context->thread() != g_receiver->thread() || db_read_sequence == std::numeric_limits<quint64>::max()) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG,
            "GUI DB read rejected: db=%d context=%d work=%d callback=%d receiver=%d gui_thread=%d context_thread=%d exhausted=%d",
            expected != nullptr, context != nullptr, bool(work), bool(complete), g_receiver != nullptr,
            g_receiver && QThread::currentThread() == g_receiver->thread(),
            g_receiver && context && context->thread() == g_receiver->thread(), db_read_sequence == std::numeric_limits<quint64>::max());
        return false;
    }
    auto* memory = u_malloc(sizeof(DbReadRequest));
    if (!memory) { DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "GUI DB read request allocation failed"); return false; }
    const quint64 id = ++db_read_sequence;
    auto* request = new(memory) DbReadRequest{std::move(work), {}, nullptr, expected, id};
    DbReadCompletion callback{context, std::move(complete), {}};
    callback.destroyed = QObject::connect(context, &QObject::destroyed, g_receiver, [id] { db_read_completions.remove(id); });
    db_read_completions.insert(id, std::move(callback));
    bool accepted = false;
    {
        std::lock_guard<std::mutex> guard(bridge_mutex);
        if (g_inst && g_ua && uasync_is_running(g_ua) && gui_database_current(g_inst, expected)) {
            request->instance = g_inst;
            accepted = post_command(execute_db_read, request, false, cancel_db_read) == 0;
        }
    }
    if (!accepted) {
        const auto it = db_read_completions.find(id);
        QObject::disconnect(it->destroyed); db_read_completions.erase(it); free_db_read(request);
        DEBUG_WARN(DEBUG_CATEGORY_CONFIG, "GUI DB read queue rejected request=%llu", (unsigned long long)id);
    } else DEBUG_DEBUG(DEBUG_CATEGORY_CONFIG, "GUI DB read queued request=%llu", (unsigned long long)id);
    return accepted;
}

/* Публикация снимка и привязка instance выполняются под тем же mutex, что detach. */
bool gui_bridge_save_member(sqlite3* db, const QString& channel, const QByteArray& ed25519, const QString& nickname,
                            const QByteArray& ownerJson, QObject* context, std::function<void(int)> complete) {
    if (!g_receiver || QThread::currentThread() != g_receiver->thread() || !context || context->thread() != g_receiver->thread() ||
        !db || ed25519.size() != 32 || ed25519 == QByteArray(32, '\0') || !complete || channel.isEmpty() || channel.size() != 64 || channel.contains(QChar(0)) ||
        nickname.contains(QChar(0)) || ownerJson.contains('\0')) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member save: invalid GUI request/context"); return false;
    }
    const QByteArray id = channel.toLatin1(), nick = nickname.toUtf8();
    if (ownerJson.size() > 8192 || static_cast<quint64>(nick.size()) + ownerJson.size() + sizeof(MemberEditRequest) + 2 > UINT32_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member save: snapshot exceeds allocation/JSON limit"); return false;
    }
    if (!chat_channel_valid(id.constData()) || member_sequence == UINT64_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member save: invalid channel or exhausted request ID"); return false;
    }
    std::lock_guard<std::mutex> guard(bridge_mutex);
    if (!g_inst || !g_ua) { DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "member save: core unavailable"); return false; }
    auto* request = static_cast<MemberEditRequest*>(u_malloc(sizeof(MemberEditRequest) + nick.size() + ownerJson.size() + 2));
    if (!request) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member save: snapshot allocation failed"); return false; }
    request->instance = g_inst; request->database = db; request->request = ++member_sequence;
    memcpy(request->ed25519, ed25519.constData(), 32);
    request->nicknameBytes = nick.size(); memcpy(request->channel, id.constData(), id.size() + 1);
    char* data = reinterpret_cast<char*>(request + 1);
    memcpy(data, nick.constData(), nick.size() + 1); memcpy(data + nick.size() + 1, ownerJson.constData(), ownerJson.size() + 1);
    MemberCompletion completion{context, std::move(complete), {}};
    completion.destroyed = QObject::connect(context, &QObject::destroyed, g_receiver,
        [id = member_sequence] { member_completions.remove(id); });
    member_completions.insert(member_sequence, std::move(completion));
    if (post_command(execute_member_edit, request, true, cancel_member_edit) < 0) {
        QObject::disconnect(member_completions.take(member_sequence).destroyed); u_free(request);
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member save: queue rejected"); return false;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "member save: queued request=%llu ch=%s peer=%s",
        (unsigned long long)member_sequence, id.constData(), ed25519.toHex().constData());
    return true;
}

struct TransferAdminRequest { UTUN_INSTANCE* instance; sqlite3* database; uint8_t ed[32]; char channel[65]; };
static void execute_transfer_admin(void* arg) {
    auto* request = static_cast<TransferAdminRequest*>(arg);
    if (gui_database_current(request->instance, request->database))
        chat_core_transfer_admin(request->instance, request->channel, request->ed);
    else DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "admin transfer: database changed");
    u_free(request);
}

bool gui_bridge_transfer_admin(sqlite3* db, const QString& channel, const QByteArray& ed) {
    const QByteArray id = channel.toLatin1();
    if (!db || ed.size() != 32 || !chat_channel_valid(id.constData())) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "admin transfer: invalid request"); return false;
    }
    std::lock_guard<std::mutex> guard(bridge_mutex);
    if (!g_inst || !g_ua) { DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "admin transfer: core unavailable"); return false; }
    auto* request = static_cast<TransferAdminRequest*>(u_calloc(1, sizeof(TransferAdminRequest)));
    if (!request) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "admin transfer: allocation failed"); return false; }
    request->instance = g_inst; request->database = db; memcpy(request->ed, ed.constData(), 32);
    memcpy(request->channel, id.constData(), id.size() + 1);
    if (post_command(execute_transfer_admin, request, true) < 0) { u_free(request); return false; }
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "admin transfer: queued ch=%s ed=%s", id.constData(), ed.toHex().constData());
    return true;
}

/* ── GuiBridgeReceiver implementation ── */

void GuiBridgeReceiver::processPost(int eventType, QByteArray data) {
    const uint8_t* d = (const uint8_t*)data.constData();
    int dlen = data.size();

    if (g_network_changed_cb && (eventType == 4 || eventType == 5 || eventType == 12 || eventType == 13 ||
        eventType == 16 || eventType == 17 || eventType == 24 || eventType == 25 || eventType == 31 || eventType == 33))
        g_network_changed_cb(eventType);
    switch (eventType) {
    case CHAT_EVT_NETWORK_STATS: {
        chat_network_stats_header header{};
        if (dlen < int(sizeof(header))) {
            DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "network stats bridge: truncated header bytes=%d", dlen); break;
        }
        memcpy(&header, d, sizeof(header));
        {
            std::lock_guard<std::mutex> guard(bridge_mutex);
            if (header.request_id != network_stats_pending) break;
            network_stats_pending = 0;
        }
        if (g_network_stats_cb) g_network_stats_cb(d, dlen);
        break;
    }
    case CHAT_EVT_MSG_UPDATED:
        if (dlen < 34 || !d[0] || dlen != 1 + d[0] + 32) {
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "invalid message update bytes=%d", dlen); break;
        }
        if (g_msg_updated_cb) g_msg_updated_cb(d, dlen);
        break;
    case CHAT_EVT_RADIO_CALL:
        if (g_radio_call_cb) g_radio_call_cb(d, dlen);
        break;
    case CHAT_EVT_LOCAL_DEVICES:
        if (g_local_devices_cb) g_local_devices_cb(d, dlen);
        break;
    case CHAT_EVT_LOCAL_INVITE:
        if (g_local_invite_cb) g_local_invite_cb(d, dlen);
        break;
    case CHAT_EVT_LOCAL_DELIVERY:
        if (g_local_delivery_cb) g_local_delivery_cb(d, dlen);
        break;
    case CHAT_EVT_STREAM_SOURCES:
        if (g_stream_sources_cb) g_stream_sources_cb(d, dlen);
        break;
    case CHAT_EVT_CPU_LOAD:
        if (dlen != 2 || d[0] > 1 || d[1] > 100) {
            DEBUG_WARN(DEBUG_CATEGORY_SYS, "invalid CPU load response bytes=%d", dlen);
            if (g_cpu_load_cb) g_cpu_load_cb(0);
        } else if (g_cpu_load_cb) g_cpu_load_cb(d[0] ? d[1] : 0);
        break;
    case CHAT_EVT_MSG_SUBMITTED:
        if (dlen != 9 || d[8] > 2) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "invalid submission result bytes=%d", dlen); break; }
        if (g_msg_submitted_cb) g_msg_submitted_cb(d, dlen);
        break;
    case CHAT_EVT_RADIO_MUTE_RESULT:
        if (g_radio_mute_result_cb) g_radio_mute_result_cb(d, dlen);
        break;
    case CHAT_EVT_FEEDBACK_CHANGED:
        if (g_feedback_changed_cb) g_feedback_changed_cb(d, dlen);
        break;
    case CHAT_EVT_ATTACHMENT_KEYS_CHANGED:
        if (g_attachment_keys_cb) g_attachment_keys_cb(d, dlen);
        break;
    case CHAT_EVT_DEVGROUP_CHANGED:
        if (g_devgroup_result_cb) g_devgroup_result_cb(nullptr, 0);
        break;
    case CHAT_EVT_CLIPBOARD_RECEIVED:
        if (g_clipboard_received_cb) g_clipboard_received_cb(d, dlen);
        else if (dlen > CHAT_CLIPBOARD_EVENT_HEADER) gui_bridge_clipboard_applied(d, 0);
        break;
    case CHAT_EVT_CLIPBOARD_RESULT:
        if (g_clipboard_result_cb) g_clipboard_result_cb(d, dlen);
        break;
    case CHAT_EVT_DEVGROUP_RESULT:
        if (g_devgroup_result_cb) g_devgroup_result_cb(d, dlen);
        break;
    case CHAT_EVT_FEEDBACK_RESULT:
        if (g_feedback_result_cb) g_feedback_result_cb(d, dlen);
        break;
    case CHAT_EVT_GROUP_AVATAR_RESULT:
        if (g_group_avatar_result_cb) g_group_avatar_result_cb(d, dlen);
        break;
    case CHAT_EVT_GROUP_POLICY_RESULT:
        if (g_group_policy_result_cb) g_group_policy_result_cb(d, dlen);
        break;
    case CHAT_EVT_KANBAN_CHANGED:
    case CHAT_EVT_KANBAN_RESULT:
        if (g_kanban_cb) g_kanban_cb(d, dlen);
        break;
    case CHAT_EVT_GROUP_INFO_RESULT:
        if (g_group_info_result_cb) g_group_info_result_cb(d, dlen);
        break;
    case CHAT_EVT_AVATAR_RESULT:
        if (g_avatar_result_cb) g_avatar_result_cb(d, dlen);
        break;
    case CHAT_EVT_MEMBER_FILES_RESPONSE:
        if (g_member_files_response_cb) g_member_files_response_cb(d, dlen);
        break;
    case CHAT_EVT_MEMBER_FILES_TRANSFER:
        if (g_member_files_transfer_cb) g_member_files_transfer_cb(d, dlen);
        break;
    case CHAT_EVT_DM_INVITE_RESULT:
        if (g_dm_invite_result_cb) g_dm_invite_result_cb(d, dlen);
        break;
    case CHAT_EVT_DM_DELETE_RESULT:
        if (dlen != 25 || d[24] > 1) {
            DEBUG_ERROR(DEBUG_CATEGORY_DM, "gui_bridge: invalid DM delete result bytes=%d", dlen); break;
        }
        if (g_dm_delete_result_cb) g_dm_delete_result_cb(d, dlen);
        break;
    case CHAT_EVT_DM_START_RESULT:
        if (dlen != 25 || d[24] > 1) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "gui_bridge: invalid DM start result bytes=%d", dlen); break; }
        if (g_dm_start_result_cb) g_dm_start_result_cb(d, dlen);
        break;
    case CHAT_EVT_DM_CALL_ROUTE:
        if (g_dm_call_route_cb) g_dm_call_route_cb(d, dlen);
        break;
    case GUI_EVT_MSG_RECEIVED:
        if (dlen >= 2) {
            uint8_t chLen = d[0];
            if (chLen == 64 && dlen == 1 + chLen + CHAT_MSG_EVENT_SUFFIX_SIZE && d[dlen - 1] <= 1) {
                const uint8_t* author = d + 1 + chLen;
                const int initialSync = d[dlen - 1];
                QString chId = QString::fromUtf8((const char*)d + 1, chLen);
                char key[65];
                DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "gui_bridge: MSG_RECEIVED ch=%s author=%s initial_sync=%d",
                           qPrintable(chId), sc_public_key_hex(author, key), initialSync);
                if (g_msg_received_cb) g_msg_received_cb((const char*)d + 1, chLen, author, initialSync);
            } else {
                DEBUG_WARN(DEBUG_CATEGORY_CHAT, "gui_bridge: MSG_RECEIVED invalid data length %d", dlen);
            }
        } else {
            DEBUG_WARN(DEBUG_CATEGORY_CHAT, "gui_bridge: MSG_RECEIVED data too short %d", dlen);
        }
        break;
    case GUI_EVT_CONNECT_RESULT:
        if (dlen == CHAT_CONNECT_RESULT_SIZE) {
            uint64_t request; int32_t result;
            const uint8_t* group = d + CHAT_CONNECT_RESULT_GROUP_OFF;
            memcpy(&result, d + CHAT_CONNECT_RESULT_CODE_OFF, 4);
            memcpy(&request, d + CHAT_CONNECT_RESULT_REQUEST_OFF, 8);
            char key[65], groupText[65];
            DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "gui_bridge: CONNECT_RESULT peer=%s group=%s result=%d",
                       sc_public_key_hex(d, key), sc_public_key_hex(group, groupText), result);
            if (g_connect_result_cb) g_connect_result_cb(d, group, result, request);
        } else {
            DEBUG_WARN(DEBUG_CATEGORY_CONNECTION, "gui_bridge: CONNECT_RESULT data too short %d", dlen);
        }
        break;
    case CHAT_EVT_CHANNEL_DELETED:
        if (dlen < 2 || !d[0] || dlen != 1 + d[0]) {
            DEBUG_WARN(DEBUG_CATEGORY_CHAT, "gui_bridge: CHANNEL_DELETED invalid data length %d", dlen);
            break;
        }
        DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "gui_bridge: CHANNEL_DELETED ch=%.*s", int(d[0]), (const char*)d + 1);
        if (g_channel_updated_cb) g_channel_updated_cb((const char*)d + 1, d[0]);
        break;
    case GUI_EVT_CHANNEL_UPDATED:
        if (dlen >= 2) {
            uint8_t chLen = d[0];
            if (dlen >= 1 + chLen) {
                QString chId = QString::fromUtf8((const char*)d + 1, chLen);
                DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "gui_bridge: CHANNEL_UPDATED ch=%s", qPrintable(chId));
                if (g_channel_updated_cb) g_channel_updated_cb((const char*)d + 1, chLen);
            } else DEBUG_WARN(DEBUG_CATEGORY_CHAT, "gui_bridge: truncated CHANNEL_UPDATED bytes=%d channel_bytes=%u", dlen, chLen);
        } else {
            DEBUG_WARN(DEBUG_CATEGORY_CHAT, "gui_bridge: CHANNEL_UPDATED data too short %d", dlen);
        }
        break;
    case GUI_EVT_AUTO_CONNECT_STATUS:
        if (dlen >= 7) {
            uint8_t  status = d[0];
            uint16_t total_tried; memcpy(&total_tried, d + 1, 2);
            uint16_t node_count;  memcpy(&node_count,  d + 3, 2);
            uint16_t connected;   memcpy(&connected,   d + 5, 2);
            const char* st_str = status == 0 ? "in_progress" : status == 1 ? "done" : "stopped";
            if (status != 0) DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "gui_bridge: AUTO_CONNECT %s tried=%u/%u connected=%u", st_str, total_tried, node_count, connected);
            if (g_auto_connect_status_cb) g_auto_connect_status_cb(status, total_tried, node_count, connected);
        } else DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "gui_bridge: truncated AUTO_CONNECT_STATUS bytes=%d", dlen);
        break;
    case GUI_EVT_CHANNEL_PEERS_ONLINE:
        if (dlen >= 3) {
            uint8_t chLen = d[0];
            if (dlen >= 3 + chLen) {
                uint16_t online; memcpy(&online, d + 1 + chLen, 2);
                static char lastChId[256] = {0};
                static uint16_t lastOnline = 0xFFFF;
                if (chLen != (uint8_t)strlen(lastChId) || memcmp(lastChId, d + 1, chLen) != 0 || online != lastOnline) {
                    memcpy(lastChId, d + 1, chLen); lastChId[chLen] = '\0';
                    lastOnline = online;
                    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "gui_bridge: CHANNEL_PEERS_ONLINE ch=%.*s online=%u", chLen, (const char*)d + 1, online);
                }
                if (g_channel_peers_online_cb) g_channel_peers_online_cb((const char*)d + 1, chLen, online);
            } else DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "gui_bridge: truncated CHANNEL_PEERS_ONLINE bytes=%d channel_bytes=%u", dlen, chLen);
        } else DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "gui_bridge: missing CHANNEL_PEERS_ONLINE header bytes=%d", dlen);
        break;
    case GUI_EVT_DB_READY:
        DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "gui_bridge: DB_READY (chat DB tables created)");
        if (g_db_ready_cb) g_db_ready_cb();
        break;
    case GUI_EVT_STATUS_REFRESH:
        if (g_status_refresh_cb) g_status_refresh_cb((const char*)d, dlen);
        break;
    case GUI_EVT_KEYS_GENERATED:
        if (dlen == 64) DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "gui_bridge: local public key generated");
        else DEBUG_WARN(DEBUG_CATEGORY_CONFIG, "gui_bridge: invalid KEYS_GENERATED bytes=%d", dlen);
        break;
    case GUI_EVT_MEMBERS_CHANGED:
    case CHAT_EVT_SERVICE_STARTED:
    case CHAT_EVT_LINK_STATUS_CHANGED:
        // События сети уже переданы network_changed выше; отдельного payload callback нет.
        break;
    case CHAT_EVT_SERVICE_STOPPED:
        if (g_clipboard_result_cb) g_clipboard_result_cb(nullptr, 0);
        break;
    case GUI_EVT_ATTACHMENT_DOWNLOADED:
        if (dlen >= 2) {
            uint8_t chLen = d[0];
            if (dlen >= 1 + chLen + 8) {
                int64_t msgId = 0; memcpy(&msgId, d + 1 + chLen, 8);
                DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "gui_bridge: ATTACHMENT_DOWNLOADED ch=%.*s msgId=%lld", chLen, (const char*)d + 1, (long long)msgId);
                if (g_attachment_downloaded_cb) g_attachment_downloaded_cb((const char*)d + 1, chLen, msgId);
            } else DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "gui_bridge: truncated ATTACHMENT_DOWNLOADED bytes=%d channel_bytes=%u", dlen, chLen);
        } else DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "gui_bridge: missing ATTACHMENT_DOWNLOADED header bytes=%d", dlen);
        break;
    case GUI_EVT_DOWNLOAD_PROGRESS:
        if (dlen >= 2) {
            uint8_t chLen = d[0];
            if (dlen >= 1 + chLen + 16) {
                int64_t msgId = 0; memcpy(&msgId, d + 1 + chLen, 8);
                int32_t bd = 0, nb = 0; memcpy(&bd, d + 1 + chLen + 8, 4); memcpy(&nb, d + 1 + chLen + 12, 4);
                DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "gui_bridge: DOWNLOAD_PROGRESS ch=%.*s msgId=%lld %d/%d",
                            chLen, (const char*)d + 1, (long long)msgId, bd, nb);
                if (g_download_progress_cb) g_download_progress_cb((const char*)d + 1, chLen, msgId, bd, nb);
            } else DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "gui_bridge: truncated DOWNLOAD_PROGRESS bytes=%d channel_bytes=%u", dlen, chLen);
        } else DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "gui_bridge: missing DOWNLOAD_PROGRESS header bytes=%d", dlen);
        break;
    case GUI_EVT_NODEINFO_UPDATE: break; /* network_changed уже уведомлён выше. */
    case 18: /* CHAT_EVT_INVITE_RECEIVED */
        if (g_invite_received_cb) g_invite_received_cb(d, dlen);
        break;
    case GUI_EVT_CONN_LIST:
        if (g_conn_list_cb) g_conn_list_cb(d, dlen);
        break;
    case GUI_EVT_CONN_METRICS:
        if (g_conn_metrics_cb) g_conn_metrics_cb(d, dlen);
        break;
    case GUI_EVT_MEMBER_DETAIL:
        if (g_member_detail_cb) g_member_detail_cb(d, dlen);
        break;
    case GUI_EVT_MEMBER_LIST:
        if (g_member_list_cb) g_member_list_cb(d, dlen);
        break;
    case GUI_EVT_MEMBER_UPDATED:
        if (g_member_updated_cb) g_member_updated_cb(d, dlen);
        break;
    case GUI_EVT_MEMBER_REMOVED:
        if (g_member_removed_cb) g_member_removed_cb(d, dlen);
        break;

    case GUI_EVT_ADMIN_KEY_RECEIVED:
        if (dlen >= 2) {
            uint8_t chLen = d[0];
            if (dlen == 1 + chLen + 32) {
                const auto from = QByteArray((const char*)d + 1 + chLen, 32).toHex();
                DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "gui_bridge: ADMIN_KEY_RECEIVED ch=%.*s from=%s",
                           chLen, (const char*)d + 1, from.constData());
                if (g_admin_key_received_cb) g_admin_key_received_cb(d, dlen);
            } else DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "gui_bridge: truncated ADMIN_KEY_RECEIVED bytes=%d channel_bytes=%u", dlen, chLen);
        } else DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "gui_bridge: missing ADMIN_KEY_RECEIVED header bytes=%d", dlen);
        break;
    case GUI_EVT_INVITE_LINK_READY:
        if (dlen >= 11) {
            uint8_t chLen = d[0];
            if (dlen < 1 + chLen + 10) {
                DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "gui_bridge: truncated INVITE_LINK_READY bytes=%d channel_bytes=%u", dlen, chLen);
                break;
            }
            uint64_t requestId; memcpy(&requestId, d + 1 + chLen, 8);
            uint16_t linkLen; memcpy(&linkLen, d + 1 + chLen + 8, 2);
            if (dlen >= 1 + chLen + 10 + linkLen) {
                QByteArray link((const char*)d + 1 + chLen + 10, linkLen);
                if (g_invite_link_ready_cb) g_invite_link_ready_cb((const char*)d + 1, chLen, requestId, link.constData());
            } else DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "gui_bridge: truncated INVITE_LINK_READY bytes=%d link_bytes=%u", dlen, linkLen);
        } else DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "gui_bridge: missing INVITE_LINK_READY header bytes=%d", dlen);
        break;
    case GUI_EVT_INVITE_CANDIDATES:
        if (dlen >= 2) {
            uint8_t chLen = d[0];
            if (dlen >= 1 + chLen + 34) {
                if (g_invite_candidates_cb) g_invite_candidates_cb((const char*)d + 1, chLen, d + 1 + chLen, dlen - 1 - chLen);
            } else DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "gui_bridge: truncated INVITE_CANDIDATES bytes=%d channel_bytes=%u", dlen, chLen);
        } else DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "gui_bridge: missing INVITE_CANDIDATES header bytes=%d", dlen);
        break;
    case GUI_EVT_DM_MSG_RECEIVED:
        if (dlen >= 2) {
            uint8_t cl = d[0];
            if (cl && dlen == 1 + cl + 32) {
                const uint8_t* author = d + 1 + cl;
                char hex[65];
                DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "gui_bridge: DM_MSG_RECEIVED conv=%.*s author=%s",
                           cl, (const char*)d + 1, sc_public_key_hex(author, hex));
                if (g_dm_msg_received_cb) g_dm_msg_received_cb((const char*)d + 1, cl, author);
            } else DEBUG_WARN(DEBUG_CATEGORY_DM, "gui_bridge: truncated DM_MSG_RECEIVED bytes=%d conversation_bytes=%u", dlen, cl);
        } else DEBUG_WARN(DEBUG_CATEGORY_DM, "gui_bridge: missing DM_MSG_RECEIVED header bytes=%d", dlen);
        break;
    case GUI_EVT_DM_CONV_UPDATED:
        if (dlen >= 10 && d[0] && dlen == 9 + d[0]) {
            uint64_t generation; memcpy(&generation, d + 1 + d[0], 8);
            DEBUG_INFO(DEBUG_CATEGORY_DM, "gui_bridge: DM_CONV_UPDATED conv=%.*s generation=%llu",
                       d[0], (const char*)d + 1, (unsigned long long)generation);
            if (g_dm_conv_updated_cb) g_dm_conv_updated_cb((const char*)d + 1, d[0], generation);
        } else {
            DEBUG_ERROR(DEBUG_CATEGORY_DM, "gui_bridge: invalid DM conversation update bytes=%d", dlen);
        }
        break;
    case GUI_EVT_DM_MESSAGES:
        if (g_dm_messages_cb) g_dm_messages_cb(d, dlen);
        break;
    case CHAT_EVT_DM_MEMBERS:
        if (g_dm_members_cb) g_dm_members_cb(d, dlen);
        break;
    case CHAT_EVT_ATTACHMENT_PREPARATION:
        if (g_attachment_preparation_cb) g_attachment_preparation_cb(d, dlen);
        break;
    case GUI_EVT_CALL_INCOMING:
        if (dlen >= 42 && d[0] && dlen == 41 + d[0]) {
            uint8_t chLen = d[0];
            const uint8_t* caller = d + 1 + chLen;
            uint64_t callId; memcpy(&callId, caller + 32, 8);
            char peer_hex[65];
            DEBUG_INFO(DEBUG_CATEGORY_CALL, "gui_bridge: CALL_INCOMING ch=%.*s caller=%s id=0x%016llx",
                       chLen, (const char*)d + 1, sc_public_key_hex(caller, peer_hex), (unsigned long long)callId);
            if (g_call_incoming_cb) g_call_incoming_cb((const char*)d + 1, chLen, caller, callId);
        } else DEBUG_WARN(DEBUG_CATEGORY_CALL, "gui_bridge: invalid CALL_INCOMING bytes=%d", dlen);
        break;
    case GUI_EVT_CALL_RINGING:
        if (dlen >= 8) {
            uint64_t callId; memcpy(&callId, d, 8);
            if (g_call_ringing_cb) g_call_ringing_cb(callId);
        } else DEBUG_WARN(DEBUG_CATEGORY_CALL, "gui_bridge: truncated CALL_RINGING bytes=%d", dlen);
        break;
    case CHAT_EVT_CALL_PHONE_BUSY:
        if (dlen == 9) {
            uint64_t callId; memcpy(&callId, d, 8);
            if (g_call_phone_busy_cb) g_call_phone_busy_cb(callId);
        } else DEBUG_WARN(DEBUG_CATEGORY_CALL, "gui_bridge: invalid PHONE_BUSY bytes=%d", dlen);
        break;
    case GUI_EVT_CALL_ACCEPTED:
        if (dlen == 41) {
            uint64_t callId; memcpy(&callId, d, 8);
            if (g_call_accepted_cb) g_call_accepted_cb(callId, d + 9);
        } else DEBUG_WARN(DEBUG_CATEGORY_CALL, "gui_bridge: invalid CALL_ACCEPTED bytes=%d", dlen);
        break;
    case GUI_EVT_CALL_ENDED:
        if (dlen >= 9) {
            uint64_t callId; memcpy(&callId, d, 8);
            if (g_call_ended_cb) g_call_ended_cb(callId, d[8]);
        } else DEBUG_WARN(DEBUG_CATEGORY_CALL, "gui_bridge: truncated CALL_ENDED bytes=%d", dlen);
        break;
    case GUI_EVT_CALL_DECLINED:
        if (dlen >= 9) {
            uint64_t callId; memcpy(&callId, d, 8);
            if (g_call_declined_cb) g_call_declined_cb(callId, d[8]);
        } else DEBUG_WARN(DEBUG_CATEGORY_CALL, "gui_bridge: truncated CALL_DECLINED bytes=%d", dlen);
        break;
    case GUI_EVT_CALL_ERROR:
        if (dlen >= 9) {
            uint64_t callId; memcpy(&callId, d, 8);
            uint8_t err = d[8];
            DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "gui_bridge: CALL_ERROR id=0x%016llx err=%u text=%.*s",
                       (unsigned long long)callId, (unsigned)err, dlen - 9, (const char*)d + 9);
            if (g_call_error_cb) g_call_error_cb(callId, err, (const char*)d + 9, dlen - 9);
        } else DEBUG_WARN(DEBUG_CATEGORY_CALL, "gui_bridge: truncated CALL_ERROR bytes=%d", dlen);
        break;
    case GUI_EVT_CALL_PATH:
        if (dlen >= 9) {
            uint64_t callId; memcpy(&callId, d, 8);
            DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "gui_bridge: CALL_PATH id=0x%016llx text=%.*s",
                       (unsigned long long)callId, dlen - 8, (const char*)d + 8);
            if (g_call_path_cb) g_call_path_cb(callId, (const char*)d + 8, dlen - 8);
        } else DEBUG_WARN(DEBUG_CATEGORY_CALL, "gui_bridge: truncated CALL_PATH bytes=%d", dlen);
        break;
    case GUI_EVT_CALL_CONNECTION:
        if (dlen == CHAT_CALL_CONNECTION_SIZE && d[8] <= 3) {
            uint64_t callId; memcpy(&callId, d, 8);
            if (g_call_connection_cb) g_call_connection_cb(callId, d[8], d + 9);
        } else {
            DEBUG_WARN(DEBUG_CATEGORY_CALL, "gui_bridge: invalid CALL_CONNECTION size=%d", dlen);
        }
        break;
    case GUI_EVT_CALL_STATS:
        if (dlen == 24) {
            uint64_t callId; memcpy(&callId, d, 8);
            int values[8];
            for (int i = 0; i < 8; i++) values[i] = (d[8 + i * 2] << 8) | d[9 + i * 2];
            if (g_call_stats_cb) g_call_stats_cb(callId, values[0], values[1], values[2], values[3],
                                                values[4], values[5], values[6], values[7]);
        } else {
            DEBUG_WARN(DEBUG_CATEGORY_CALL, "gui_bridge: invalid CALL_STATS size=%d", dlen);
        }
        break;
    case GUI_EVT_RADIO_TALK:
        if (dlen == 67 && d[66] <= 1) {
            if (g_radio_talk_cb) g_radio_talk_cb(d, d + 32, d[66]);
        } else DEBUG_WARN(DEBUG_CATEGORY_RADIO, "gui_bridge: invalid RADIO_TALK bytes=%d", dlen);
        break;
    default:
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "gui_bridge: unknown event type %d", eventType);
        break;
    }
}

/* Отмена post сообщает результат через Qt, не вызывая код клиента/старого instance под bridge_mutex. */
static void cancel_member_files_request(void* arg) {
    auto* request = static_cast<mf_gui_request*>(arg);
    auto& message = request->message;
    message.kind = MF_RESPONSE; message.status = MF_CANCELLED; message.data_len = 0; message.more = 0;
    uint8_t event[MF_EVENT_RESPONSE_PREFIX + MF_WIRE_MAX];
    memcpy(event, request->group, MF_ADDRESS_SIZE); memcpy(event + MF_ADDRESS_SIZE, request->server, MF_ADDRESS_SIZE);
    int length = mf_encode(&message, event + MF_EVENT_RESPONSE_PREFIX, MF_WIRE_MAX);
    DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "file bridge: request cancelled id=%llu command=%s group=%s server=%s path=%s",
        (unsigned long long)message.request_id, mf_command_text(message.command), QByteArray(reinterpret_cast<const char*>(request->group), 32).toHex().constData(),
        QByteArray(reinterpret_cast<const char*>(request->server), 32).toHex().constData(), message.path);
    if (length > 0) gui_bridge_post(CHAT_EVT_MEMBER_FILES_RESPONSE, event, length + MF_EVENT_RESPONSE_PREFIX);
}

/* До trampoline дисковый I/O ещё не начат: достаточно одного финального события, payload освободит мост. */
static void cancel_member_files_transfer(void* arg) {
    auto* request = static_cast<mf_transfer_request*>(arg);
    uint8_t event[MF_EVENT_TRANSFER_SIZE]{};
    mf_put64(event, request->id); memcpy(event + MF_EVENT_TRANSFER_GROUP, request->group_key, MF_ADDRESS_SIZE); memcpy(event + MF_EVENT_TRANSFER_SERVER, request->server, MF_ADDRESS_SIZE);
    memcpy(event + MF_EVENT_TRANSFER_OWNER, request->owner, MF_ADDRESS_SIZE); event[MF_EVENT_TRANSFER_STATUS] = MF_CANCELLED; event[MF_EVENT_TRANSFER_STATE] = 2;
    DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "file bridge: transfer cancelled id=%llu group=%s server=%s owner=%s upload=%d remote=%s",
        (unsigned long long)request->id, QByteArray(reinterpret_cast<const char*>(request->group_key), 32).toHex().constData(), QByteArray(reinterpret_cast<const char*>(request->server), 32).toHex().constData(),
        QByteArray(reinterpret_cast<const char*>(request->owner), 32).toHex().constData(), request->upload, request->remote);
    gui_bridge_post(CHAT_EVT_MEMBER_FILES_TRANSFER, event, sizeof(event));
}

/* ── C API ── */

extern "C" {

void gui_bridge_init(void) {
    if (!QCoreApplication::instance()) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "GUI bridge requires QCoreApplication"); return; }
    if (g_receiver) { DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "GUI bridge already initialized"); return; }
    g_receiver = new GuiBridgeReceiver(QCoreApplication::instance());
    QObject::connect(g_receiver, &QObject::destroyed, [] { g_receiver = nullptr; });
    DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "GUI bridge initialized in GUI thread");
}

void gui_bridge_set_uasync(struct UASYNC* ua) {
    std::lock_guard<std::mutex> guard(bridge_mutex);
    if (g_inst && ua != g_ua) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "GUI bridge uasync replacement rejected: detach instance first"); return;
    }
    g_ua = ua;
}

void gui_bridge_set_inst(struct UTUN_INSTANCE* inst) {
    struct chat_join_port* replacement = inst ? chat_sync_join_port(inst) : nullptr;
    std::lock_guard<std::mutex> guard(bridge_mutex);
    if (g_inst != inst) { cancel_posts(); network_stats_pending = 0; g_gui_reader = nullptr; }
    chat_join_port_release(join_port); join_port = replacement;
    g_inst = inst; core_thread = inst ? std::this_thread::get_id() : std::thread::id{};
    DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "GUI bridge instance %s: inst=%p ua=%p", inst ? "attached" : "detached", (void*)inst, (void*)g_ua);
}

void gui_bridge_set_reader(sqlite3* reader) {
    g_gui_reader = reader;
}

int gui_bridge_join_link(const char* link, size_t len, uint64_t* request) {
    if (!request) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "GUI join rejected: missing request output"); return CHAT_CONNECT_INVALID; }
    std::lock_guard<std::mutex> guard(bridge_mutex);
    uint64_t id = join_sequence.fetch_add(1);
    int result = chat_join_port_submit(join_port, id, link, len);
    if (!result) *request = id;
    return result;
}

int gui_bridge_offer_link(const char* channel, const char* link, size_t len, uint64_t* request) {
    if (!request) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "GUI offer rejected: missing request output"); return CHAT_CONNECT_INVALID; }
    std::lock_guard<std::mutex> guard(bridge_mutex);
    uint64_t id = join_sequence.fetch_add(1);
    int result = chat_join_port_offer(join_port, id, channel, link, len);
    if (!result) *request = id;
    return result;
}

struct UTUN_INSTANCE* gui_bridge_get_inst(void) {
    return g_inst;
}

void gui_bridge_post(int event_type, const uint8_t* data, int data_len) {
    if (data_len < 0 || (data_len && !data)) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "GUI event rejected: type=%d bytes=%d missing_data=%d", event_type, data_len, !data);
        return;
    }
    if (!g_receiver) { DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "gui_bridge_post: g_receiver=NULL, dropping event %d", event_type); return; }

    QByteArray d;
    if (data_len > 0) d = QByteArray((const char*)data, data_len);

    if (!QMetaObject::invokeMethod(g_receiver, "processPost",
        Qt::QueuedConnection,
        Q_ARG(int, event_type),
        Q_ARG(QByteArray, d)))
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "GUI event queueing failed: type=%d bytes=%d", event_type, data_len);
}

void gui_bridge_post_uasync_fn(void (*fn)(void*), void* arg) {
    std::lock_guard<std::mutex> guard(bridge_mutex);
    post_command(fn, arg, false);
}

int gui_bridge_post_uasync_owned_fn(void (*fn)(void*), void* arg) {
    std::lock_guard<std::mutex> guard(bridge_mutex);
    return post_command(fn, arg, true);
}

/* binding указывает на поле inst самой команды; отдельного чтения указателя из GUI нет. */
int gui_bridge_post_bound_fn(void (*fn)(void*), void* arg, struct UTUN_INSTANCE** binding) {
    std::lock_guard<std::mutex> guard(bridge_mutex);
    if (!fn || !arg || !binding || !g_inst || !g_ua || !uasync_is_running(g_ua)) {
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "GUI core command rejected: fn=%p arg=%p binding=%p inst=%p ua=%p running=%d",
            (void*)fn, arg, (void*)binding, (void*)g_inst.load(), (void*)g_ua, uasync_is_running(g_ua)); return -1;
    }
    *binding = g_inst;
    if (post_command(fn, arg, true) < 0) {
        *binding = nullptr; DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "GUI core command queue failed: fn=%p arg=%p", (void*)fn, arg); return -1;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "GUI core command queued: fn=%p inst=%p ua=%p", (void*)fn, (void*)g_inst.load(), (void*)g_ua);
    return 0;
}

/* Общий путь загрузки для delegate, контекстного меню и auto-play. */
int gui_bridge_download_attachment(const char* channel, int64_t msg_id) {
    const size_t length = channel ? strnlen(channel, sizeof(attachment_dl_req::channel_id)) : 0;
    if (!chat_channel_valid(channel) || msg_id <= 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment download rejected: invalid channel/id length=%zu id=%lld", length, (long long)msg_id);
        return -1;
    }
    std::lock_guard<std::mutex> guard(bridge_mutex);
    if (!g_inst || !g_ua || !uasync_is_running(g_ua)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment download rejected: core unavailable channel=%s id=%lld", channel, (long long)msg_id);
        return -1;
    }
    auto* request = static_cast<attachment_dl_req*>(u_calloc(1, sizeof(attachment_dl_req)));
    if (!request) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment download allocation failed"); return -1; }
    request->inst = g_inst; request->msg_id = msg_id; memcpy(request->channel_id, channel, length + 1);
    if (post_command(chat_core_attachment_download_trampoline, request, true) < 0) {
        u_free(request); DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment download queue failed channel=%s id=%lld", channel, (long long)msg_id);
        return -1;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "attachment download queued channel=%s id=%lld", channel, (long long)msg_id);
    return 0;
}

/* Создание payload и enqueue атомарны относительно detach; GUI не читает заимствованный instance. */
static int bridge_call_start(const char* channel, const uint8_t peer[32], uint64_t call_id, bool devices) {
    const size_t length = channel ? strnlen(channel, sizeof(call_start_arg::ch_id)) : 0;
    const uint8_t zero[32] = {};
    char peer_hex[65];
    if (!length || length == sizeof(call_start_arg::ch_id) || !call_id ||
        (strncmp(channel, "dm:", 3) && (!peer || !memcmp(peer, zero, sizeof(zero))))) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "call bridge: invalid start channel_bytes=%zu peer=%s id=%llu",
            length, sc_public_key_hex(peer, peer_hex), (unsigned long long)call_id); return -1;
    }
    std::lock_guard<std::mutex> guard(bridge_mutex);
    if (!g_inst || !g_ua || !uasync_is_running(g_ua)) {
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "call bridge: start rejected, core unavailable id=%llu", (unsigned long long)call_id); return -1;
    }
    auto* request = static_cast<call_start_arg*>(u_calloc(1, sizeof(call_start_arg)));
    if (!request) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "call bridge: start allocation failed id=%llu", (unsigned long long)call_id); return -1;
    }
    request->inst = g_inst; request->call_id = call_id; request->devices = devices;
    if (peer) memcpy(request->peer, peer, sizeof(request->peer));
    memcpy(request->ch_id, channel, length + 1);
    if (post_command(call_start_trampoline, request, true) < 0) {
        u_free(request); DEBUG_ERROR(DEBUG_CATEGORY_CALL, "call bridge: start queue rejected id=%llu", (unsigned long long)call_id); return -1;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "call bridge: start queued id=%llu channel=%s peer=%s",
        (unsigned long long)call_id, channel, sc_public_key_hex(peer, peer_hex));
    return 0;
}

int gui_bridge_call_start(const char* channel, const uint8_t peer[32], uint64_t id) { return bridge_call_start(channel, peer, id, false); }
int gui_bridge_call_start_devices(const char* channel, const uint8_t peer[32], uint64_t id) { return bridge_call_start(channel, peer, id, true); }

/* Однородный call_id_arg освобождает native trampoline либо очередь при отмене. */
int gui_bridge_call_action(enum gui_call_action action, uint64_t call_id) {
    void (*callback)(void*) = nullptr;
    switch (action) {
    case GUI_CALL_ACCEPT: callback = call_accept_trampoline; break;
    case GUI_CALL_DECLINE: callback = call_decline_trampoline; break;
    case GUI_CALL_HANGUP: callback = call_hangup_trampoline; break;
    case GUI_CALL_CONTINUE: callback = call_continue_trampoline; break;
    }
    if (!callback || !call_id) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "call bridge: invalid action=%d id=%llu", action, (unsigned long long)call_id); return -1;
    }
    std::lock_guard<std::mutex> guard(bridge_mutex);
    if (!g_inst || !g_ua || !uasync_is_running(g_ua)) {
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "call bridge: action rejected, core unavailable action=%d id=%llu",
            action, (unsigned long long)call_id); return -1;
    }
    auto* request = static_cast<call_id_arg*>(u_calloc(1, sizeof(call_id_arg)));
    if (!request) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "call bridge: action allocation failed action=%d id=%llu",
            action, (unsigned long long)call_id); return -1;
    }
    request->inst = g_inst; request->call_id = call_id;
    if (post_command(callback, request, true) < 0) {
        u_free(request); DEBUG_ERROR(DEBUG_CATEGORY_CALL, "call bridge: action queue rejected action=%d id=%llu",
            action, (unsigned long long)call_id); return -1;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "call bridge: action queued action=%d id=%llu", action, (unsigned long long)call_id);
    return 0;
}

/* Значения GUI становятся owned native-запросом одной операцией с проверкой ядра и enqueue. */
uint64_t gui_bridge_radio_route(const char* channel) {
    if (!chat_channel_valid(channel)) {
        DEBUG_WARN(DEBUG_CATEGORY_RADIO, "radio route lookup rejected invalid channel"); return 0;
    }
    std::unique_lock<std::mutex> guard(bridge_mutex);
    if (!g_inst || !g_ua || !uasync_is_running(g_ua)) {
        DEBUG_WARN(DEBUG_CATEGORY_RADIO, "radio route lookup core unavailable ch=%s", channel); return 0;
    }
    if (core_thread == std::this_thread::get_id()) return topo_groups_channel_slot(g_inst.load()->topo_groups, channel);
    auto* request = new (std::nothrow) RadioRouteRequest{g_inst.load(), QByteArray(channel), {}};
    if (!request) { DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "radio route lookup allocation failed"); return 0; }
    auto result = request->result.get_future();
    if (post_command([](void* arg) {
        std::unique_ptr<RadioRouteRequest> work(static_cast<RadioRouteRequest*>(arg));
        work->result.set_value(topo_groups_channel_slot(work->inst->topo_groups, work->channel.constData()));
    }, request, false, [](void* arg) {
        std::unique_ptr<RadioRouteRequest> work(static_cast<RadioRouteRequest*>(arg)); work->result.set_value(0);
    }) < 0) {
        delete request; return 0;
    }
    guard.unlock();
    if (result.wait_for(std::chrono::seconds(2)) == std::future_status::ready) return result.get();
    DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "radio route lookup timed out ch=%s", channel); return 0;
}

int gui_bridge_radio_call(enum gui_radio_call_action action, uint64_t id, const uint8_t group[32], const uint8_t peer[32]) {
    int nativeAction = 0;
    const char* name = nullptr;
    switch (action) {
    case GUI_RADIO_CALL_START_DEVICES: nativeAction = RADIO_CALL_START; name = "start_devices"; break;
    case GUI_RADIO_CALL_ANSWER: nativeAction = RADIO_CALL_ANSWER; name = "answer"; break;
    case GUI_RADIO_CALL_START: nativeAction = RADIO_CALL_START; name = "start"; break;
    case GUI_RADIO_CALL_READY: nativeAction = RADIO_CALL_READY; name = "ready"; break;
    case GUI_RADIO_CALL_ACCEPT: nativeAction = RADIO_CALL_ACCEPT; name = "accept"; break;
    case GUI_RADIO_CALL_DECLINE: nativeAction = RADIO_CALL_DECLINE; name = "decline"; break;
    case GUI_RADIO_CALL_CANCEL: nativeAction = RADIO_CALL_CANCEL; name = "cancel"; break;
    case GUI_RADIO_CALL_FAIL: nativeAction = RADIO_CALL_FAIL; name = "fail"; break;
    case GUI_RADIO_CALL_REJECT_BUSY: nativeAction = RADIO_CALL_REJECT_BUSY; name = "reject_busy"; break;
    case GUI_RADIO_CALL_CONTINUE: nativeAction = RADIO_CALL_CONTINUE; name = "continue"; break;
    }
    const uint8_t zero[32] = {};
    char peer_hex[65];
    if (!name || !id || (nativeAction == RADIO_CALL_START ? !group || !memcmp(group, zero, sizeof(zero)) || !peer || !memcmp(peer, zero, sizeof(zero)) : group || peer)) {
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "radio call bridge: invalid command action=%d id=%llu group=%s peer=%s",
            action, (unsigned long long)id, (group ? QByteArray(reinterpret_cast<const char*>(group), 32).toHex().constData() : "-"), sc_public_key_hex(peer, peer_hex)); return -1;
    }
    std::lock_guard<std::mutex> guard(bridge_mutex);
    if (!g_inst || !g_ua || !uasync_is_running(g_ua)) {
        DEBUG_WARN(DEBUG_CATEGORY_RADIO, "radio call bridge: core unavailable action=%s id=%llu", name, (unsigned long long)id); return -1;
    }
    auto* request = static_cast<radio_call_request*>(u_calloc(1, sizeof(radio_call_request)));
    if (!request) {
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "radio call bridge: request allocation failed action=%s id=%llu", name, (unsigned long long)id);
        return -1;
    }
    request->inst = g_inst; request->id = id; if (group) memcpy(request->group, group, 32); request->action = nativeAction;
    if (peer) memcpy(request->peer, peer, sizeof(request->peer));
    request->devices = action == GUI_RADIO_CALL_START_DEVICES;
    if (post_command(radio_call_trampoline, request, true) < 0) {
        u_free(request);
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "radio call bridge: queue rejected action=%s id=%llu", name, (unsigned long long)id); return -1;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "radio call bridge: queued action=%s id=%llu group=%s peer=%s",
        name, (unsigned long long)id, (group ? QByteArray(reinterpret_cast<const char*>(group), 32).toHex().constData() : "-"), sc_public_key_hex(peer, peer_hex));
    return 0;
}

/* Копия команды и её post принимаются одной операцией с привязкой к живому ядру. */
int gui_bridge_self_ed(uint8_t ed[32]) {
    std::lock_guard<std::mutex> guard(bridge_mutex);
    auto* instance = g_inst.load();
    if (!instance || !ed) {
        DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "file bridge: local identity unavailable");
        return -1;
    }
    memcpy(ed, instance->my_ed25519_pubkey, 32);
    return 0;
}

int gui_bridge_member_files_request(const uint8_t group[32], const uint8_t server[32], const struct mf_message* message) {
    if (!group || mf_zero(group, MF_ADDRESS_SIZE) || !server || mf_zero(server, MF_ADDRESS_SIZE) || !message || !message->request_id || message->command < MF_LIST || message->command > MF_QUOTA ||
        message->data_len > MF_BLOCK_SIZE || strnlen(message->path, sizeof(message->path)) > MF_PATH_MAX ||
        strnlen(message->target, sizeof(message->target)) > MF_PATH_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "file bridge: invalid request group=%s server=%s",
            (group ? QByteArray(reinterpret_cast<const char*>(group), 32).toHex() : QByteArray()).constData(), (server ? QByteArray(reinterpret_cast<const char*>(server), 32).toHex() : QByteArray()).constData()); return -1;
    }
    std::lock_guard<std::mutex> guard(bridge_mutex);
    if (!g_receiver || !g_inst || !g_ua || !uasync_is_running(g_ua)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "file bridge: request rejected, core unavailable id=%llu command=%s",
            (unsigned long long)message->request_id, mf_command_text(message->command)); return -1;
    }
    auto* request = static_cast<mf_gui_request*>(u_calloc(1, sizeof(mf_gui_request)));
    if (!request) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "file bridge: request snapshot allocation failed id=%llu",
            (unsigned long long)message->request_id); return -1;
    }
    request->inst = g_inst; memcpy(request->group, group, MF_ADDRESS_SIZE); memcpy(request->server, server, MF_ADDRESS_SIZE); request->message = *message;
    if (post_command(member_files_gui_request, request, true, cancel_member_files_request) < 0) {
        u_free(request); DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "file bridge: request queue rejected id=%llu",
            (unsigned long long)message->request_id); return -1;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "file bridge: request queued id=%llu command=%s group=%s server=%s owner=%s path=%s",
        (unsigned long long)message->request_id, mf_command_text(message->command), (group ? QByteArray(reinterpret_cast<const char*>(group), 32).toHex() : QByteArray()).constData(),
        (server ? QByteArray(reinterpret_cast<const char*>(server), 32).toHex() : QByteArray()).constData(), (message->owner ? QByteArray(reinterpret_cast<const char*>(message->owner), 32).toHex() : QByteArray()).constData(), message->path);
    return 0;
}

/* Поле inst входа не используется: GUI не получает указатель, способный устареть до enqueue. */
int gui_bridge_member_files_transfer(const struct mf_transfer_request* source) {
    if (!source || !source->id || mf_zero(source->group_key, MF_ADDRESS_SIZE) || mf_zero(source->server, MF_ADDRESS_SIZE) || mf_zero(source->owner, MF_ADDRESS_SIZE) || !mf_path_valid(source->remote, 0) ||
        !source->local[0] || strnlen(source->local, sizeof(source->local)) > MF_PATH_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "file bridge: invalid transfer snapshot"); return -1;
    }
    std::lock_guard<std::mutex> guard(bridge_mutex);
    if (!g_receiver || !g_inst || !g_ua || !uasync_is_running(g_ua)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "file bridge: transfer rejected, core unavailable id=%llu",
            (unsigned long long)source->id); return -1;
    }
    auto* request = static_cast<mf_transfer_request*>(u_calloc(1, sizeof(mf_transfer_request)));
    if (!request) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "file bridge: transfer snapshot allocation failed id=%llu",
            (unsigned long long)source->id); return -1;
    }
    *request = *source; request->inst = g_inst;
    if (post_command(member_files_gui_transfer, request, true, cancel_member_files_transfer) < 0) {
        u_free(request); DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "file bridge: transfer queue rejected id=%llu",
            (unsigned long long)source->id); return -1;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "file bridge: transfer queued id=%llu group=%s server=%s owner=%s upload=%d remote=%s local=%s",
        (unsigned long long)source->id, QByteArray(reinterpret_cast<const char*>(source->group_key), 32).toHex().constData(), (source->server ? QByteArray(reinterpret_cast<const char*>(source->server), 32).toHex() : QByteArray()).constData(),
        (source->owner ? QByteArray(reinterpret_cast<const char*>(source->owner), 32).toHex() : QByteArray()).constData(), source->upload, source->remote, source->local);
    return 0;
}

/* После исполнения отменяет native handle; при detach до исполнения payload просто освобождается. */
int gui_bridge_member_files_cancel(uint64_t id) {
    if (!id) return 0;
    std::lock_guard<std::mutex> guard(bridge_mutex);
    if (!g_inst || !g_ua || !uasync_is_running(g_ua)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "file bridge: cancel rejected id=%llu core_available=%d",
            (unsigned long long)id, g_inst && g_ua && uasync_is_running(g_ua)); return -1;
    }
    auto* request = static_cast<mf_cancel_request*>(u_calloc(1, sizeof(mf_cancel_request)));
    if (!request) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "file bridge: cancel snapshot allocation failed id=%llu", (unsigned long long)id); return -1;
    }
    request->inst = g_inst; request->id = id;
    if (post_command(member_files_gui_cancel, request, true) < 0) {
        u_free(request); DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "file bridge: cancel queue rejected id=%llu", (unsigned long long)id); return -1;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "file bridge: cancel queued id=%llu", (unsigned long long)id);
    return 0;
}

/* Сериализация и публикация образуют одну границу владения с detach ядра. */
int gui_bridge_post_setting(enum gui_setting_target target, const char* key, const char* value) {
    if ((target != GUI_SETTING_UI_STATE && target != GUI_SETTING_CHAT) || !key || !key[0] || !value) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "GUI setting rejected: target=%d missing/invalid key or value", target); return -1;
    }
    const size_t capacity = target == GUI_SETTING_UI_STATE ? sizeof(save_ui_state_arg::data) : sizeof(chat_setting_arg::data);
    size_t keyLength = 0, valueLength = 0;
    while (keyLength < capacity && key[keyLength]) ++keyLength;
    while (valueLength < capacity && value[valueLength]) ++valueLength;
    if (keyLength + valueLength + 2 > capacity) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "GUI setting rejected: target=%d key_bytes=%zu value_bytes=%zu capacity=%zu",
                    target, keyLength, valueLength, capacity);
        return -1;
    }
    std::lock_guard<std::mutex> guard(bridge_mutex);
    if (!g_inst || !g_ua) {
        DEBUG_WARN(DEBUG_CATEGORY_CONFIG, "GUI setting rejected: target=%d key=%s core unavailable", target, key); return -1;
    }
    const size_t bytes = target == GUI_SETTING_UI_STATE ? sizeof(save_ui_state_arg) : sizeof(chat_setting_arg);
    void* request = u_malloc(bytes);
    if (!request) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "GUI setting allocation failed: target=%d key=%s bytes=%zu", target, key, bytes); return -1;
    }
    char* data;
    void (*callback)(void*);
    if (target == GUI_SETTING_UI_STATE) {
        auto* arg = static_cast<save_ui_state_arg*>(request);
        arg->inst = g_inst; data = arg->data; callback = chat_core_save_ui_state_trampoline;
    } else {
        auto* arg = static_cast<chat_setting_arg*>(request);
        arg->inst = g_inst; data = arg->data; callback = chat_core_set_setting_trampoline;
    }
    memcpy(data, key, keyLength + 1); memcpy(data + keyLength + 1, value, valueLength + 1);
    if (post_command(callback, request, true) < 0) {
        u_free(request);
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "GUI setting queue rejected: target=%d key=%s", target, key); return -1;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_CONFIG, "GUI setting queued: target=%d key=%s value_bytes=%zu", target, key, valueLength);
    return 0;
}

/* Смена имени использует тот же owned lifecycle без передачи instance из GUI. */
int gui_bridge_update_name(const char* name) {
    if (!name) { DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "GUI name rejected: missing name"); return -1; }
    size_t length = 0;
    while (length < sizeof(update_my_name_arg::name) && name[length]) ++length;
    if (length == sizeof(update_my_name_arg::name)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "GUI name rejected: name exceeds request capacity"); return -1;
    }
    std::lock_guard<std::mutex> guard(bridge_mutex);
    if (!g_inst || !g_ua) { DEBUG_WARN(DEBUG_CATEGORY_CONFIG, "GUI name rejected: core unavailable"); return -1; }
    auto* request = static_cast<update_my_name_arg*>(u_malloc(sizeof(update_my_name_arg)));
    if (!request) { DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "GUI name allocation failed"); return -1; }
    request->inst = g_inst; memcpy(request->name, name, length + 1);
    if (post_command(chat_core_update_my_name_trampoline, request, true) < 0) {
        u_free(request); DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "GUI name queue rejected"); return -1;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_CONFIG, "GUI name queued: bytes=%zu", length);
    return 0;
}

/* Общий codec composer: ID little-endian и success=0, без вызова GUI под mutex. */
static void post_submission_failure(uint64_t id) {
    if (!id) return;
    uint8_t result[9] = {0};
    for (unsigned i = 0; i < 8; ++i) result[i] = (uint8_t)(id >> (8 * i));
    gui_bridge_post(CHAT_EVT_MSG_SUBMITTED, result, sizeof(result));
}

/* Отмена выполняется в uasync, но не вызывает обработчик ядра под bridge_mutex; результат копируется в Qt-очередь. */
static void cancel_message_submit(void* arg) {
    auto* request = static_cast<chat_msg_submit*>(arg);
    DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "composer submission cancelled: request=%llu", (unsigned long long)request->request_id);
    post_submission_failure(request->request_id);
}

int gui_bridge_submit_message(struct chat_msg_submit* req) {
    std::lock_guard<std::mutex> guard(bridge_mutex);
    if (!req || !g_inst || !g_ua || !uasync_is_running(g_ua)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "composer submission: core unavailable"); return -1;
    }
    req->inst = g_inst;
    return post_command(chat_core_submit_trampoline, req, true, cancel_message_submit);
}

/* PM composer получает отказ также при detach до исполнения native trampoline. */
static void cancel_dm_submit(void* arg) {
    auto* request = static_cast<dm_send_req*>(arg);
    DEBUG_WARN(DEBUG_CATEGORY_DM, "PM submission cancelled: request=%llu", (unsigned long long)request->request_id);
    post_submission_failure(request->request_id);
}

int gui_bridge_submit_dm_message(struct dm_send_req* req) {
    std::lock_guard<std::mutex> guard(bridge_mutex);
    if (!req || !g_inst || !g_ua || !uasync_is_running(g_ua)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "PM submission: core/request unavailable"); return -1;
    }
    req->inst = g_inst;
    if (post_command(dm_send_trampoline, req, true, cancel_dm_submit) < 0) {
        req->inst = nullptr; DEBUG_ERROR(DEBUG_CATEGORY_DM, "PM submission: queue allocation failed"); return -1;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_DM, "PM text queued: conversation=%s request=%llu", req->conv_id, (unsigned long long)req->request_id);
    return 0;
}

/* Отмена до native trampoline не требует живого media owner; callback освобождает весь запрос сам. */
static void cancel_attachment_submit(void* arg) {
    auto* request = static_cast<attachment_send_req*>(arg);
    post_submission_failure(request->request_id);
    attachment_send_discard(request);
}

int gui_bridge_submit_attachment(struct attachment_send_req* req) {
    std::lock_guard<std::mutex> guard(bridge_mutex);
    if (!req || !g_inst || !g_ua || !uasync_is_running(g_ua)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment submission: core/request unavailable"); return -1;
    }
    req->inst = g_inst;
    if (post_command(attachment_send_trampoline, req, false, cancel_attachment_submit) < 0) {
        req->inst = nullptr; DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment submission: queue allocation failed"); return -1;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "attachment queued: target=%s request=%llu", req->target, (unsigned long long)req->request_id);
    return 0;
}

int gui_bridge_is_uasync_ready(void) {
    std::lock_guard<std::mutex> guard(bridge_mutex);
    return g_ua && g_inst ? 1 : 0;
}

void gui_bridge_set_connect_result_cb(gui_connect_result_fn cb) {
    g_connect_result_cb = cb;
}

void gui_bridge_set_msg_received_cb(gui_msg_received_fn cb) {
    g_msg_received_cb = cb;
}

void gui_bridge_set_channel_updated_cb(gui_channel_updated_fn cb) {
    g_channel_updated_cb = cb;
}


void gui_bridge_set_auto_connect_status_cb(gui_auto_connect_status_fn cb) {
    g_auto_connect_status_cb = cb;
}

void gui_bridge_set_channel_peers_online_cb(gui_channel_peers_online_fn cb) {
    g_channel_peers_online_cb = cb;
}

void gui_bridge_set_db_ready_cb(gui_db_ready_fn cb) {
    g_db_ready_cb = cb;
}

void gui_bridge_set_status_refresh_cb(gui_status_refresh_fn cb) {
    g_status_refresh_cb = cb;
}

void gui_bridge_set_cpu_load_cb(gui_cpu_load_fn cb) {
    g_cpu_load_cb = cb;
}

int gui_bridge_request_cpu_load(void) {
    std::lock_guard<std::mutex> guard(bridge_mutex);
    if (!g_ua || !g_inst) return 0;
    return post_command(chat_core_collect_cpu_load_trampoline, g_inst, false) == 0;
}

void gui_bridge_set_attachment_downloaded_cb(gui_attachment_downloaded_fn cb) {
    g_attachment_downloaded_cb = cb;
}

void gui_bridge_set_download_progress_cb(gui_download_progress_fn cb) {
    g_download_progress_cb = cb;
}


void gui_bridge_set_invite_received_cb(gui_invite_received_fn cb) {
    g_invite_received_cb = cb;
}

void gui_bridge_set_conn_list_cb(gui_conn_list_fn cb) {
    g_conn_list_cb = cb;
}

void gui_bridge_set_conn_metrics_cb(gui_conn_metrics_fn cb) {
    g_conn_metrics_cb = cb;
}

void gui_bridge_set_network_stats_cb(gui_conn_metrics_fn cb) {
    g_network_stats_cb = cb;
}

/* Очередь не накапливает снимки при занятом uasync или медленном GUI. */
int gui_bridge_request_network_stats(uint64_t* request_id) {
    if (!request_id) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "network stats bridge: missing request output"); return -1; }
    std::lock_guard<std::mutex> guard(bridge_mutex);
    if (!g_receiver || !g_inst || !g_ua || !uasync_is_running(g_ua)) return 0;
    if (network_stats_pending) return 2;
    auto* request = static_cast<chat_network_stats_request*>(u_calloc(1, sizeof(chat_network_stats_request)));
    if (!request) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "network stats bridge: request allocation failed"); return -1; }
    request->inst = g_inst; request->request_id = ++network_stats_sequence;
    if (post_command(chat_core_collect_network_stats_trampoline, request, true) < 0) { u_free(request); return -1; }
    network_stats_pending = *request_id = request->request_id;
    DEBUG_TRACE(DEBUG_CATEGORY_GENERAL, "network stats bridge: queued request=%llu", (unsigned long long)*request_id);
    return 1;
}

void gui_bridge_set_member_detail_cb(gui_member_detail_fn cb) {
    g_member_detail_cb = cb;
}

void gui_bridge_set_member_list_cb(gui_member_list_fn cb) {
    g_member_list_cb = cb;
}

void gui_bridge_set_member_updated_cb(gui_member_updated_fn cb) {
    g_member_updated_cb = cb;
}

void gui_bridge_set_member_removed_cb(gui_member_removed_fn cb) {
    g_member_removed_cb = cb;
}


void gui_bridge_set_admin_key_received_cb(gui_admin_key_received_fn cb) {
    g_admin_key_received_cb = cb;
}

void gui_bridge_set_invite_link_ready_cb(gui_invite_link_ready_fn cb) {
    g_invite_link_ready_cb = cb;
}

void gui_bridge_set_invite_candidates_cb(gui_invite_candidates_fn cb) {
    g_invite_candidates_cb = cb;
}

void gui_bridge_set_local_devices_cb(gui_local_event_fn cb) { g_local_devices_cb = cb; }
void gui_bridge_set_local_invite_cb(gui_local_event_fn cb) { g_local_invite_cb = cb; }
void gui_bridge_set_local_delivery_cb(gui_local_event_fn cb) { g_local_delivery_cb = cb; }

void gui_bridge_set_call_incoming_cb(gui_call_incoming_fn cb) {
    g_call_incoming_cb = cb;
}

void gui_bridge_set_call_ringing_cb(gui_call_id_fn cb) {
    g_call_ringing_cb = cb;
}
void gui_bridge_set_call_phone_busy_cb(gui_call_id_fn cb) { g_call_phone_busy_cb = cb; }

void gui_bridge_set_call_accepted_cb(gui_call_peer_fn cb) {
    g_call_accepted_cb = cb;
}

void gui_bridge_set_call_ended_cb(gui_call_ended_fn cb) {
    g_call_ended_cb = cb;
}

void gui_bridge_set_call_declined_cb(gui_call_ended_fn cb) {
    g_call_declined_cb = cb;
}

void gui_bridge_set_call_error_cb(gui_call_error_fn cb) {
    g_call_error_cb = cb;
}

void gui_bridge_set_call_path_cb(gui_call_path_fn cb) {
    g_call_path_cb = cb;
}

void gui_bridge_set_call_connection_cb(gui_call_connection_fn cb) {
    g_call_connection_cb = cb;
}

void gui_bridge_set_radio_talk_cb(gui_radio_talk_fn cb) {
    g_radio_talk_cb = cb;
}

void gui_bridge_set_dm_msg_received_cb(gui_dm_msg_received_fn cb) {
    g_dm_msg_received_cb = cb;
}

void gui_bridge_set_network_changed_cb(gui_network_changed_fn cb) { g_network_changed_cb = cb; }
void gui_bridge_set_stream_sources_cb(gui_dm_messages_fn cb) { g_stream_sources_cb = cb; }
void gui_bridge_set_dm_call_route_cb(gui_dm_messages_fn cb) { g_dm_call_route_cb = cb; }
void gui_bridge_set_dm_invite_result_cb(gui_dm_messages_fn cb) { g_dm_invite_result_cb = cb; }
void gui_bridge_set_dm_delete_result_cb(gui_dm_messages_fn cb) { g_dm_delete_result_cb = cb; }
void gui_bridge_set_dm_start_result_cb(gui_dm_messages_fn cb) { g_dm_start_result_cb = cb; }
void gui_bridge_set_member_files_response_cb(gui_dm_messages_fn cb) { g_member_files_response_cb = cb; }
void gui_bridge_set_member_files_transfer_cb(gui_dm_messages_fn cb) { g_member_files_transfer_cb = cb; }
void gui_bridge_set_avatar_result_cb(gui_dm_messages_fn cb) { g_avatar_result_cb = cb; }
void gui_bridge_set_group_avatar_result_cb(gui_dm_messages_fn cb) { g_group_avatar_result_cb = cb; }
void gui_bridge_set_group_policy_result_cb(gui_dm_messages_fn cb) { g_group_policy_result_cb = cb; }
void gui_bridge_set_group_info_result_cb(gui_dm_messages_fn cb) { g_group_info_result_cb = cb; }
void gui_bridge_set_feedback_changed_cb(gui_dm_messages_fn cb) { g_feedback_changed_cb = cb; }
void gui_bridge_set_attachment_keys_cb(gui_dm_messages_fn cb) { g_attachment_keys_cb = cb; }
void gui_bridge_set_feedback_result_cb(gui_dm_messages_fn cb) { g_feedback_result_cb = cb; }
void gui_bridge_set_msg_submitted_cb(gui_dm_messages_fn cb) { g_msg_submitted_cb = cb; }
void gui_bridge_set_msg_updated_cb(gui_dm_messages_fn cb) { g_msg_updated_cb = cb; }
void gui_bridge_set_radio_mute_result_cb(gui_dm_messages_fn cb) { g_radio_mute_result_cb = cb; }
void gui_bridge_set_radio_call_cb(gui_dm_messages_fn cb) { g_radio_call_cb = cb; }

/* Резервируем post вместе с запросом: UI получает отказ, если поставить удаление в очередь невозможно. */
int gui_bridge_delete_dm(const char* conv_id, uint64_t generation, uint64_t request_id) {
    std::lock_guard<std::mutex> guard(bridge_mutex);
    if (!g_inst || !g_ua || !uasync_is_running(g_ua) || !conv_id || !conv_id[0] || strlen(conv_id) >= 64 ||
        strspn(conv_id, "0123456789") != strlen(conv_id) || !generation || !request_id) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM delete: invalid request or stopped core generation=%llu request=%llu",
                    (unsigned long long)generation, (unsigned long long)request_id); return -1;
    }
    auto* req = static_cast<dm_delete_req*>(u_calloc(1, sizeof(dm_delete_req)));
    if (!req) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM delete: request allocation failed"); return -1;
    }
    req->inst = g_inst; req->generation = generation; req->request_id = request_id; strcpy(req->conv_id, conv_id);
    if (post_command(dm_delete_trampoline, req, true) < 0) { u_free(req); return -1; }
    DEBUG_INFO(DEBUG_CATEGORY_DM, "DM delete queued: conv=%s generation=%llu request=%llu", conv_id,
               (unsigned long long)generation, (unsigned long long)request_id);
    return 0;
}

int gui_bridge_toggle_member_radio_mute(const char* channel_id, const uint8_t ed[32]) {
    std::lock_guard<std::mutex> guard(bridge_mutex);
    const int running = g_inst && g_ua ? uasync_is_running(g_ua) : 0;
    const bool valid_channel = chat_channel_valid(channel_id);
    if (!g_inst || !running || !valid_channel || !ed) {
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "radio: GUI mute rejected inst=%p ua=%p running=%d valid_channel=%d member=%s",
                    (void*)g_inst.load(), (void*)g_ua, running, valid_channel, QByteArray(reinterpret_cast<const char*>(ed), ed ? 32 : 0).toHex().constData()); return -1;
    }
    auto* req = static_cast<chat_member_radio_mute_req*>(u_calloc(1, sizeof(chat_member_radio_mute_req)));
    if (!req) {
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "radio: GUI mute request allocation failed"); return -1;
    }
    req->inst = g_inst; memcpy(req->ed, ed, sizeof(req->ed)); strcpy(req->ch_id, channel_id);
    if (post_command(chat_core_toggle_member_radio_mute_trampoline, req, true) < 0) { u_free(req); return -1; }
    DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "radio: GUI mute queued ch=%s member=%s", channel_id, QByteArray(reinterpret_cast<const char*>(ed), ed ? 32 : 0).toHex().constData());
    return 0;
}

void gui_bridge_set_dm_conv_updated_cb(gui_dm_conv_updated_fn cb) {
    g_dm_conv_updated_cb = cb;
}

void gui_bridge_set_dm_messages_cb(gui_dm_messages_fn cb) {
    g_dm_messages_cb = cb;
}

void gui_bridge_set_dm_members_cb(gui_dm_messages_fn cb) {
    g_dm_members_cb = cb;
}

void gui_bridge_set_attachment_preparation_cb(gui_dm_messages_fn cb) {
    g_attachment_preparation_cb = cb;
}

} /* extern "C" */

/* ── uTun nodeinfo callback (called from uasync thread) ── */

extern "C" void gui_nodeinfo_cb_impl(struct TOPO_GROUP* group, struct TOPO_GROUP_NODE* nq, void* arg) {
    (void)group; (void)arg;
    if (nq) gui_bridge_post(GUI_EVT_NODEINFO_UPDATE, nullptr, 0);
}

#include "gui_bridge_impl.moc"

void gui_bridge_set_call_stats_cb(gui_call_stats_fn cb) { g_call_stats_cb = cb; }

void gui_bridge_set_devgroup_result_cb(gui_dm_messages_fn cb) { g_devgroup_result_cb = cb; }

void gui_bridge_set_clipboard_received_cb(gui_dm_messages_fn cb) { g_clipboard_received_cb = cb; }
void gui_bridge_set_clipboard_result_cb(gui_dm_messages_fn cb) { g_clipboard_result_cb = cb; }

static void cancel_clipboard(void* arg) {
    auto* req = static_cast<chat_clipboard_request*>(arg);
    OPENSSL_cleanse(req + 1, req->size);
    DEBUG_WARN(DEBUG_CATEGORY_CHAT, "clipboard command cancelled: request=%llu action=%d", (unsigned long long)req->request, req->action);
}

int gui_bridge_send_clipboard(uint64_t request, int type, const uint8_t* data, size_t size, const uint8_t group[32], const uint8_t peer[32]) {
    if (!request || !data || !size || (type != CHAT_CLIPBOARD_TEXT && type != CHAT_CLIPBOARD_PNG) ||
        size > (type == CHAT_CLIPBOARD_TEXT ? CHAT_CLIPBOARD_TEXT_MAX : CHAT_CLIPBOARD_IMAGE_MAX)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "clipboard bridge invalid send bytes=%zu type=%d", size, type); return -1;
    }
    std::lock_guard<std::mutex> guard(bridge_mutex);
    if (!g_inst || !g_ua) { DEBUG_WARN(DEBUG_CATEGORY_CHAT, "clipboard bridge core unavailable"); return -1; }
    auto* req = static_cast<chat_clipboard_request*>(u_calloc(1, sizeof(chat_clipboard_request) + size));
    if (!req) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "clipboard bridge allocation failed"); return -1; }
    req->inst = g_inst; req->request = request; req->type = type; req->size = uint32_t(size); memcpy(req + 1, data, size);
    if (group) memcpy(req->group, group, 32);
    if (peer) memcpy(req->peer_ed, peer, 32);
    if (!post_command(chat_clipboard_trampoline, req, true, cancel_clipboard)) return 0;
    OPENSSL_cleanse(req + 1, size); u_free(req); return -1;
}

int gui_bridge_clipboard_applied(const uint8_t* header, int success) {
    if (!header) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "clipboard bridge missing receive header"); return -1; }
    std::lock_guard<std::mutex> guard(bridge_mutex);
    if (!g_inst || !g_ua) { DEBUG_WARN(DEBUG_CATEGORY_CHAT, "clipboard apply core unavailable"); return -1; }
    auto* req = static_cast<chat_clipboard_request*>(u_calloc(1, sizeof(chat_clipboard_request)));
    if (!req) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "clipboard apply allocation failed"); return -1; }
    req->inst = g_inst; req->action = 1; req->success = success; memcpy(req->id, header, 16);
    memcpy(req->group, header + CHAT_CLIPBOARD_EVENT_GROUP_OFF, 32);
    memcpy(req->peer_ed, header + CHAT_CLIPBOARD_EVENT_PEER_ED_OFF, 32);
    if (!post_command(chat_clipboard_trampoline, req, true)) return 0;
    u_free(req); return -1;
}

int gui_bridge_offer_clipboard_files(uint64_t request, const chat_clipboard_source* sources, size_t count,
    const uint8_t* manifest, size_t size, const uint8_t group[32], const uint8_t peer[32]) {
    if (!request || (count && !sources) || count > CHAT_CLIPBOARD_SOURCES_MAX || !manifest || !size || size > CHAT_CLIPBOARD_TEXT_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "clipboard bridge invalid file offer count=%zu bytes=%zu", count, size); return -1;
    }
    std::lock_guard<std::mutex> guard(bridge_mutex);
    if (!g_inst || !g_ua) { DEBUG_WARN(DEBUG_CATEGORY_CHAT, "clipboard file offer core unavailable"); return -1; }
    size_t entries = count * sizeof(*sources);
    auto* req = static_cast<chat_clipboard_request*>(u_calloc(1, sizeof(chat_clipboard_request) + entries + size));
    if (req) { if (group) memcpy(req->group, group, 32); if (peer) memcpy(req->peer_ed, peer, 32); }
    if (!req) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "clipboard file offer allocation failed"); return -1; }
    req->inst = g_inst; req->action = 2; req->request = request; req->source_count = uint32_t(count); req->size = uint32_t(size);
    if (entries) memcpy(req + 1, sources, entries);
    memcpy(reinterpret_cast<uint8_t*>(req + 1) + entries, manifest, size);
    if (!post_command(chat_clipboard_trampoline, req, true)) return 0;
    u_free(req); return -1;
}
static void cancel_clipboard_pull(void* arg) {
    auto* req = static_cast<chat_clipboard_pull_request*>(arg);
    DEBUG_WARN(DEBUG_CATEGORY_CHAT, "clipboard file pull command cancelled"); req->done(req->arg, -2);
}
int gui_bridge_fetch_clipboard_file(const uint8_t group[32], const uint8_t peer[32], const uint8_t object[16], uint64_t size,
    const char* destination, void (*done)(void*, int), void* arg) {
    if (!group || !peer || !object || !done || !destination || strlen(destination) >= sizeof(chat_clipboard_pull_request::destination)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "clipboard bridge invalid file pull"); return -1;
    }
    std::lock_guard<std::mutex> guard(bridge_mutex);
    if (!g_inst || !g_ua) { DEBUG_WARN(DEBUG_CATEGORY_CHAT, "clipboard file pull core unavailable"); return -1; }
    auto* req = static_cast<chat_clipboard_pull_request*>(u_calloc(1, sizeof(chat_clipboard_pull_request)));
    if (!req) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "clipboard file pull allocation failed"); return -1; }
    req->inst = g_inst; memcpy(req->group, group, 32); memcpy(req->peer_ed, peer, 32); req->size = size; memcpy(req->object, object, 16);
    memcpy(req->destination, destination, strlen(destination) + 1); req->done = done; req->arg = arg;
    if (!post_command(chat_clipboard_pull_trampoline, req, true, cancel_clipboard_pull)) return 0;
    u_free(req); return -1;
}

void gui_bridge_set_kanban_cb(gui_dm_messages_fn cb) { g_kanban_cb = cb; }

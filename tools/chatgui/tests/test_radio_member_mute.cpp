/* Настоящее ядро, SQLite и Opus: локальное отключение источника, события и восстановление после restart чата. */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <QTemporaryDir>

extern "C" {
#include "utun_instance.h"
#include "config_parser.h"
#include "chat/chat_core.h"
#include "chat/chat_member.h"
#include "chat/chat_event.h"
#include "routing_layer/topo_group.h"
#include "routing_layer/topo_node_sqlite.h"
#include "radio/radio.h"
#include "radio/radio_audio.h"
#include "u_async.h"
#include "debug_config.h"
#include "opus_codec.h"
#include "mem.h"
#include "db_async.h"
#include "silero_vad.h"
}

#ifdef UTUN_TEST_WRAP_VAD
/* Детерминированная речь; автомат VAD, кодер и признак занятого эфира остаются настоящими. */
extern "C" silero_vad_t* __wrap_silero_vad_create_default() { return reinterpret_cast<silero_vad_t*>(1); }
extern "C" void __wrap_silero_vad_destroy(silero_vad_t*) {}
extern "C" void __wrap_silero_vad_reset(silero_vad_t*) {}
extern "C" int __wrap_silero_vad_process(silero_vad_t*, const float*, float* probability) { *probability = 1.0f; return 0; }
#endif

#define REQUIRE(condition) do { if (!(condition)) { std::fprintf(stderr, "radio mute line %d: %s\n", __LINE__, #condition); std::abort(); } } while (0)
static constexpr uint64_t SOURCE_A = 42, SOURCE_B = 43;
static const uint8_t ed_a[32] = {1}, ed_b[32] = {2}, unknown_ed[32] = {3};
static int updates, results, successes;
static uint8_t last_flags;
static std::string watched_channel;

static void event(UTUN_INSTANCE*, int type, const uint8_t* data, int len) {
    if (type == CHAT_EVT_MEMBER_UPDATED && len == data[0] + 2 + CHAT_MEMBER_DISPLAY_SIZE && data[0] == watched_channel.size() &&
        !std::memcmp(data + 1, watched_channel.data(), watched_channel.size())) {
        chat_member_display m{}; REQUIRE(chat_core_deserialize_member(data + data[0] + 2, &m) == 0);
        if (!memcmp(m.ed25519, ed_a, 32)) { ++updates; last_flags = m.flags; }
    }
    if (type == CHAT_EVT_RADIO_MUTE_RESULT) { REQUIRE(len == CHAT_RADIO_MUTE_RESULT_SIZE); ++results; successes += data[64] != 0; }
}

static void database_idle(UTUN_INSTANCE* inst) {
    uint64_t deadline = get_time_us() + 5000000; unsigned quiet = 0;
    while (quiet < 2 && get_time_us() < deadline) {
        uasync_poll(inst->ua, 1); quiet = db_async_pending(inst->db_worker) ? 0 : quiet + 1;
    }
    REQUIRE(quiet == 2);
}

static void activate(UTUN_INSTANCE* inst, uint64_t group) {
    radio_set_active(inst, group, 1); database_idle(inst); REQUIRE(radio_is_active(inst, group));
}

static TOPO_GROUP* first_channel(UTUN_INSTANCE* inst) {
    for (ll_entry* e = inst->topo_groups->group_list->head; e; e = e->next) {
        auto* group = reinterpret_cast<TOPO_GROUP*>(e);
        if (group->group_type == TOPO_GROUP_TYPE_CHAT) return group;
    }
    return nullptr;
}

static void frames(UTUN_INSTANCE* inst, uint64_t group, uint64_t source, uint16_t first_seq = 0) {
    auto* enc = opus_codec_encoder_create(48000, 1); REQUIRE(enc);
    int16_t pcm[960];
    for (int i = 0; i < 960; ++i) pcm[i] = 7000;
    for (uint16_t i = 0; i < 8; ++i) {
        uint8_t packet[256]; int bytes = opus_codec_encode(enc, pcm, 960, packet, sizeof(packet)); REQUIRE(bytes > 0);
        radio_audio_on_frame(inst, group, source == SOURCE_A ? ed_a : ed_b, 1, first_seq + i, 0, packet, bytes, nullptr);
    }
    opus_codec_encoder_destroy(enc);
}

static std::vector<int16_t> pull(uint64_t group) {
    std::vector<int16_t> pcm(16 * 1920);
    for (int i = 0; i < 16; ++i) REQUIRE(radio_audio_pull_pcm(group, pcm.data() + i * 1920, 1920) == 1920);
    return pcm;
}

static bool audible(const std::vector<int16_t>& pcm) {
    for (int16_t sample : pcm) if (sample > 1000 || sample < -1000) return true;
    return false;
}

static bool flag(UTUN_INSTANCE* inst, const std::string& channel, uint64_t source) {
    uint8_t member[CHAT_MEMBER_DISPLAY_SIZE];
    REQUIRE(chat_core_get_single_member(inst, channel.c_str(), source == SOURCE_A ? ed_a : ed_b, member) == 0);
    uint8_t* members = nullptr; int count = 0;
    REQUIRE(chat_core_get_member_list(inst, channel.c_str(), &members, &count) == 0);
    bool found = false;
    for (int i = 0; i < count; ++i) {
        chat_member_display m{}; REQUIRE(chat_core_deserialize_member(members + i * CHAT_MEMBER_DISPLAY_SIZE, &m) == 0);
        if (!memcmp(m.ed25519, source == SOURCE_A ? ed_a : ed_b, 32)) { REQUIRE(m.flags == member[CHAT_MEMBER_FLAGS_OFF]); found = true; }
    }
    u_free(members); REQUIRE(found);
    return member[CHAT_MEMBER_FLAGS_OFF] & CHAT_MEMBER_FLAG_RADIO_MUTED;
}

int main() {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); debug_set_category_level_by_name("radio", "debug");
    UASYNC* ua = uasync_create(); REQUIRE(ua);
    QTemporaryDir directory; REQUIRE(directory.isValid());
    std::string config =
        "[global]\nmy_private_key=704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f\n"
        "my_public_key=7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f\n"
        "db_path=" + directory.path().toStdString() + "\n[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n";
    UTUN_INSTANCE* inst = utun_instance_create_from_str(ua, config.c_str());
    REQUIRE(inst); inst->config->global.db_sync_enabled = 0;
    REQUIRE(utun_core_start(inst) == 0 && chat_service_start(inst) == 0);
    chat_core_create_channel_auto(inst, "mute test");
    TOPO_GROUP* group = first_channel(inst); REQUIRE(group);
    uint64_t gid = group->group_id; std::string channel = group->channel_id;
    watched_channel = channel;
    uint8_t key[32] = {1};
    REQUIRE(topo_node_sqlite_member_placeholder_put(inst->topo_sqlite_db, channel.c_str(), SOURCE_A, key, key) == 0);
    REQUIRE(topo_node_sqlite_member_placeholder_put(inst->topo_sqlite_db, channel.c_str(), SOURCE_B, ed_b, ed_b) == 0);
    chat_event_set_handler(inst, event);
    REQUIRE(radio_audio_init(inst) == 0); activate(inst, gid);
    REQUIRE(radio_audio_start(gid, 0) == 0);
    frames(inst, gid, SOURCE_B); auto reference = pull(gid); REQUIRE(audible(reference));
    radio_audio_stop(); REQUIRE(radio_audio_start(gid, 0) == 0);

    /* Отключение посреди передачи удаляет уже накопленный A, сохраняя точный PCM источника B. */
    frames(inst, gid, SOURCE_A); frames(inst, gid, SOURCE_B);
    REQUIRE(chat_core_toggle_member_radio_mute(inst, channel.c_str(), ed_a) == 0);
    chat_core_create_channel_auto(inst, "independent mute");
    TOPO_GROUP* other = nullptr;
    for (ll_entry* e = inst->topo_groups->group_list->head; e; e = e->next) {
        auto* candidate = reinterpret_cast<TOPO_GROUP*>(e);
        if (candidate->group_id != gid && candidate->group_type == TOPO_GROUP_TYPE_CHAT) other = candidate;
    }
    REQUIRE(other);
    REQUIRE(topo_node_sqlite_member_placeholder_put(inst->topo_sqlite_db, other->channel_id, SOURCE_A, key, key) == 0);
    activate(inst, other->group_id);
    REQUIRE(!flag(inst, other->channel_id, SOURCE_A) && !radio_member_muted(inst, other->group_id, ed_a));
    REQUIRE(flag(inst, channel, SOURCE_A) && radio_member_muted(inst, gid, ed_a));
    REQUIRE(last_flags & CHAT_MEMBER_FLAG_RADIO_MUTED);
    REQUIRE(pull(gid) == reference);
    frames(inst, gid, SOURCE_A, 8); REQUIRE(!audible(pull(gid)));
    REQUIRE(chat_core_toggle_member_radio_mute(inst, channel.c_str(), ed_a) == 0);
    frames(inst, gid, SOURCE_A, 16); REQUIRE(audible(pull(gid)) && radio_audio_output_needed());
    REQUIRE(!flag(inst, channel, SOURCE_A) && !radio_member_muted(inst, gid, ed_a));

    /* Два подряд нажатия обрабатываются по актуальному состоянию, каждое получает результат. */
    database_idle(inst); int before_updates = updates;
    for (int i = 0; i < 2; ++i) {
        auto* req = static_cast<chat_member_radio_mute_req*>(u_calloc(1, sizeof(chat_member_radio_mute_req))); REQUIRE(req);
        req->inst = inst; std::memcpy(req->ed, ed_a, 32); std::strcpy(req->ch_id, channel.c_str());
        chat_core_toggle_member_radio_mute_trampoline(req);
    }
    database_idle(inst);
    REQUIRE(results == 2 && successes == 2 && updates > before_updates && !flag(inst, channel, SOURCE_A));
    REQUIRE(!(last_flags & CHAT_MEMBER_FLAG_RADIO_MUTED)); before_updates = updates;
    REQUIRE(chat_core_toggle_member_radio_mute(inst, channel.c_str(), inst->my_ed25519_pubkey) < 0);
    REQUIRE(chat_core_toggle_member_radio_mute(inst, channel.c_str(), unknown_ed) < 0);
    REQUIRE(radio_set_member_muted(inst, gid, ed_a, 2) < 0);
    REQUIRE(sqlite3_exec(inst->topo_sqlite_db, "PRAGMA query_only=ON", nullptr, nullptr, nullptr) == SQLITE_OK);
    auto* rejected = static_cast<chat_member_radio_mute_req*>(u_calloc(1, sizeof(chat_member_radio_mute_req))); REQUIRE(rejected);
    rejected->inst = inst; std::memcpy(rejected->ed, ed_a, 32); std::strcpy(rejected->ch_id, channel.c_str());
    chat_core_toggle_member_radio_mute_trampoline(rejected);
    database_idle(inst); REQUIRE(results == 3 && successes == 2 && updates == before_updates);
    REQUIRE(!flag(inst, channel, SOURCE_A) && !radio_member_muted(inst, gid, ed_a));
    REQUIRE(sqlite3_exec(inst->topo_sqlite_db, "PRAGMA query_only=OFF", nullptr, nullptr, nullptr) == SQLITE_OK);

    /* Локальные поля сохраняются при замене подписанного блока участника. */
    REQUIRE(chat_core_toggle_member_radio_mute(inst, channel.c_str(), ed_a) == 0);
    uint8_t signature[64] = {1};
    REQUIRE(topo_node_sqlite_member_block_put(inst->topo_sqlite_db, channel.c_str(), SOURCE_A, signature, 1, signature, 2,
        key, key, "{}", 0, signature) == 0);
    /* Возвращаем placeholder: тест restart не требует поддельной криптоподписи. */
    std::string sql = "UPDATE \"peers_" + channel + "\" SET source=1 WHERE node_id=42";
    REQUIRE(sqlite3_exec(inst->topo_sqlite_db, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
    REQUIRE(flag(inst, channel, SOURCE_A));
    radio_audio_stop(); radio_audio_destroy(inst); chat_service_stop(inst);
    REQUIRE(chat_service_start(inst) == 0);
    REQUIRE(flag(inst, channel, SOURCE_A));
    activate(inst, gid); REQUIRE(radio_member_muted(inst, gid, ed_a));
    REQUIRE(radio_audio_init(inst) == 0 && radio_audio_start(gid, 0) == 0);
    frames(inst, gid, SOURCE_A); REQUIRE(!audible(pull(gid)) && !radio_audio_output_needed());
    frames(inst, gid, SOURCE_B); REQUIRE(audible(pull(gid)) && radio_audio_output_needed());
#ifdef UTUN_TEST_WRAP_VAD
    radio_audio_stop(); REQUIRE(radio_audio_start(gid, 0) == 0 && radio_audio_capture_start(gid, 1, 1) == 0);
    REQUIRE(radio_audio_auto_mode());
    int16_t speech[1920]; for (int16_t& sample : speech) sample = 7000;
    frames(inst, gid, SOURCE_A); REQUIRE(!audible(pull(gid)));
    for (int i = 0; i < 6; ++i) REQUIRE(radio_audio_feed_pcm(gid, speech, 1920) == 0);
    REQUIRE(!radio_audio_transmitting()); /* Заглушённый голос удерживает авто-PTT. */
    radio_audio_stop(); REQUIRE(radio_audio_start(gid, 0) == 0 && radio_audio_capture_start(gid, 1, 1) == 0);
    for (int i = 0; i < 6; ++i) REQUIRE(radio_audio_feed_pcm(gid, speech, 1920) == 0);
    REQUIRE(radio_audio_transmitting()); /* При свободном эфире та же речь запускает передачу. */
#endif
    radio_audio_stop(); radio_audio_destroy(inst);
    utun_instance_destroy(inst);
    inst = utun_instance_create_from_str(ua, config.c_str()); REQUIRE(inst); inst->config->global.db_sync_enabled = 0;
    REQUIRE(utun_core_start(inst) == 0 && chat_service_start(inst) == 0 && flag(inst, channel, SOURCE_A));
    activate(inst, gid); REQUIRE(radio_member_muted(inst, gid, ed_a));
    REQUIRE(radio_audio_init(inst) == 0 && radio_audio_start(gid, 0) == 0);
    frames(inst, gid, SOURCE_A); REQUIRE(!audible(pull(gid)));
    frames(inst, gid, SOURCE_B); REQUIRE(audible(pull(gid)));
    radio_audio_stop(); radio_audio_destroy(inst);
    sql = "UPDATE \"peers_" + channel + "\" SET local_nick='local alias' WHERE node_id=42";
    REQUIRE(sqlite3_exec(inst->topo_sqlite_db, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
    sql = "ALTER TABLE \"peers_" + channel + "\" DROP COLUMN radio_muted";
    REQUIRE(sqlite3_exec(inst->topo_sqlite_db, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
    /* Старый формат отвергается без миграции: проверяем сохранность схемы и локального имени. */
    auto read_text = [](sqlite3* db, const std::string& query) {
        sqlite3_stmt* stmt = nullptr;
        REQUIRE(sqlite3_prepare_v2(db, query.c_str(), -1, &stmt, nullptr) == SQLITE_OK);
        REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
        const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        REQUIRE(text);
        std::string result(text);
        REQUIRE(sqlite3_finalize(stmt) == SQLITE_OK);
        return result;
    };
    const std::string schema_query = "SELECT sql FROM sqlite_schema WHERE name='peers_" + channel + "'";
    const std::string old_schema = read_text(inst->topo_sqlite_db, schema_query);
    utun_instance_destroy(inst);
    inst = utun_instance_create_from_str(ua, config.c_str()); REQUIRE(!inst);
    sqlite3* unchanged = nullptr;
    REQUIRE(sqlite3_open_v2(directory.filePath("chats.db").toUtf8().constData(), &unchanged, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
    REQUIRE(read_text(unchanged, schema_query) == old_schema);
    REQUIRE(read_text(unchanged, "SELECT local_nick FROM \"peers_" + channel + "\" WHERE node_id=42") == "local alias");
    REQUIRE(sqlite3_close(unchanged) == SQLITE_OK);
    uasync_destroy(ua, 0);
    std::puts("PASS: audio mute/unmute, other source PCM, events, DB errors, channel isolation, VAD busy, member update, restart, schema rejection");
    return 0;
}

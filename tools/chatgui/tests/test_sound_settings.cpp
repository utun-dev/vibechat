/* Настоящие SoundSettingsPage/bridge/SQLite: выполнение, отказ и отмена сохранения без утечек. */
#include "../src/soundsettingspage.h"
#include "../src/sound_manager.h"
#include "../db/db_manager.h"
#include "../transport/gui_bridge.h"
#include <QtTest>
#include <QCheckBox>
#include <QSlider>
#include <QPushButton>
#include <QToolTip>
#include <QScopeGuard>
#include <cstring>
extern "C" {
#include "utun_instance.h"
#include "chat/chat_core.h"
#include "mem.h"
#include "debug_config.h"
}

#ifdef UTUN_TEST_SETTINGS_ALLOC_FAILURES
static int failAllocation;
static const char* allocationFile;
static bool rejectAllocation(const char* where) {
    if (!failAllocation || !strstr(where, allocationFile) || --failAllocation) return false;
    DEBUG_WARN(DEBUG_CATEGORY_CONFIG, "[INJECT] sound settings allocation failure at %s", where);
    return true;
}
extern "C" void* __real_u_malloc_impl(uint32_t, const char*);
extern "C" void* __wrap_u_malloc_impl(uint32_t bytes, const char* where) {
    return rejectAllocation(where) ? nullptr : __real_u_malloc_impl(bytes, where);
}
extern "C" void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
extern "C" void* __wrap_u_calloc_impl(uint32_t count, uint32_t bytes, const char* where) {
    return rejectAllocation(where) ? nullptr : __real_u_calloc_impl(count, bytes, where);
}
#endif

struct SoundSettingsCore {
    UASYNC* ua = uasync_create();
    UTUN_INSTANCE* inst = nullptr;
    DbManager db;
    ~SoundSettingsCore() {
        gui_bridge_set_inst(nullptr); gui_bridge_set_uasync(nullptr);
        if (ua) uasync_mark_stopped(ua);
        db.setDb(nullptr);
        if (inst) utun_instance_destroy(inst);
        if (ua) uasync_destroy(ua, 0);
    }
    bool start() {
        if (!ua) return false;
        const char* config = "[global]\n"
            "my_private_key=704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f\n"
            "my_public_key=7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f\n"
            "[chatserver]\ngroup_autoconnect=0\nmember_files_enabled=0\nstorage_autoload=0\n"
            "local_invites_enabled=0\nlocal_broadcasts_enabled=0\n[ntp]\nenabled=no\n";
        inst = utun_instance_create_from_str(ua, config);
        if (!inst || utun_core_start(inst) || chat_service_start(inst)) return false;
        db.setDb(inst->topo_sqlite_db);
        gui_bridge_set_uasync(ua); gui_bridge_set_inst(inst);
        uasync_poll(ua, 0);
        return true;
    }
};

// uasync пополняет кеш таймеров при poll. Свободные блоки принадлежат пулу, а не запросам.
static size_t liveAllocations() {
    return u_get_allocated_count() - memory_pool_get_total_free_blocks();
}

static const char* events[] = {"msg_incoming", "msg_outgoing", "connect", "disconnect",
                               "call_incoming", "call_connecting", "call_ringing", "radio_incoming"};

static QMap<QString, QString> expectedSettings(int revision) {
    QMap<QString, QString> values{{"sound_enabled", QString::number(revision % 2)},
                                 {"sound_volume", QString::number(37 + revision)}};
    for (int i = 0; i < 8; ++i) {
        values.insert(QString("sound_%1_on").arg(events[i]), QString::number((i + revision) % 2));
        values.insert(QString("sound_%1_vol").arg(events[i]), QString::number((i * 17 + revision) % 101));
    }
    return values;
}

class SoundSettingsTest : public QObject {
    Q_OBJECT
    void editPage(SoundSettingsPage& page, const QMap<QString, QString>& values) {
        const auto checks = page.findChildren<QCheckBox*>(); QCOMPARE(checks.size(), 9);
        const auto sliders = page.findChildren<QSlider*>(); QCOMPARE(sliders.size(), 9);
        checks[0]->setChecked(values["sound_enabled"].toInt()); sliders[0]->setValue(values["sound_volume"].toInt());
        for (int i = 0; i < 8; ++i) {
            checks[i + 1]->setChecked(values[QString("sound_%1_on").arg(events[i])].toInt());
            sliders[i + 1]->setValue(values[QString("sound_%1_vol").arg(events[i])].toInt());
        }
    }
    void checkLocalSettings(const QMap<QString, QString>& values) {
        auto* manager = SoundManager::instance();
        QCOMPARE(manager->isEnabled(), bool(values["sound_enabled"].toInt()));
        QCOMPARE(manager->volume(), values["sound_volume"].toInt() / 100.0f);
        for (const auto* event : events) {
            const auto settings = manager->eventSettings(event);
            QCOMPARE(settings.enabled, bool(values[QString("sound_%1_on").arg(event)].toInt()));
            QCOMPARE(settings.volume, values[QString("sound_%1_vol").arg(event)].toInt() / 100.0f);
        }
    }
private slots:
    void previewDoesNotApplyDraft() {
        auto* manager = SoundManager::instance();
        const auto enabled = manager->isEnabled(); const float volume = manager->volume();
        const auto event = manager->eventSettings("msg_incoming");
        SoundSettingsPage page(nullptr);
        const auto checks = page.findChildren<QCheckBox*>(); const auto sliders = page.findChildren<QSlider*>();
        checks[0]->setChecked(!enabled); sliders[0]->setValue(37); sliders[1]->setValue(23);
        auto* preview = page.findChild<QPushButton*>("previewSound0"); QVERIFY(preview); preview->click();
        QCOMPARE(manager->isEnabled(), enabled); QCOMPARE(manager->volume(), volume);
        QCOMPARE(manager->eventSettings("msg_incoming").enabled, event.enabled);
        QCOMPARE(manager->eventSettings("msg_incoming").volume, event.volume);
        QToolTip::hideText();
    }

    void initTestCase() {
        debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
        debug_set_category_level(DEBUG_CATEGORY_CONFIG, DEBUG_LEVEL_DEBUG);
        debug_set_category_level(DEBUG_CATEGORY_GENERAL, DEBUG_LEVEL_DEBUG);
        debug_set_category_level(DEBUG_CATEGORY_CHAT, DEBUG_LEVEL_DEBUG);
        gui_bridge_init(); SoundManager::instance(); // Настройки не требуют открытия звукового устройства.
    }
    void unavailableCoreReleasesRequests() {
        SoundSettingsPage page(nullptr);
        const auto baseline = liveAllocations();
        for (int revision : {1, 2}) {
            const auto values = expectedSettings(revision); editPage(page, values);
            page.applyAndSave(); checkLocalSettings(values);
            QCOMPARE(liveAllocations(), baseline);
        }
    }
    void detachingCoreReleasesPendingRequests() {
        SoundSettingsCore core; QVERIFY(core.start());
        SoundSettingsPage page(&core.db);
        const auto values = expectedSettings(1); editPage(page, values);
        const auto baseline = liveAllocations();
        page.applyAndSave(); checkLocalSettings(values);
        QVERIFY(core.ua->posted_tasks_head);
        for (auto it = values.cbegin(); it != values.cend(); ++it) QVERIFY(core.db.getUiState(it.key()).isEmpty());
        gui_bridge_set_inst(nullptr);
        QCOMPARE(liveAllocations(), baseline);
        uasync_poll(core.ua, 0);
        for (auto it = values.cbegin(); it != values.cend(); ++it) QVERIFY(core.db.getUiState(it.key()).isEmpty());
    }
    void savesSnapshotAfterPageDestruction() {
        SoundSettingsCore core; QVERIFY(core.start());
        const auto baseline = liveAllocations();
        for (int revision : {1, 2}) {
            const auto values = expectedSettings(revision);
            {
                SoundSettingsPage page(&core.db); editPage(page, values);
                page.applyAndSave(); checkLocalSettings(values);
            }
            // Очередь хранит снимок значений независимо от времени жизни формы.
            QVERIFY(core.ua->posted_tasks_head);
            uasync_poll(core.ua, 0);
            for (auto it = values.cbegin(); it != values.cend(); ++it) QCOMPARE(core.db.getUiState(it.key()), it.value());
            QCOMPARE(liveAllocations(), baseline);
        }
    }
#ifdef UTUN_TEST_SETTINGS_ALLOC_FAILURES
    void allocationFailure_data() {
        QTest::addColumn<QByteArray>("source"); QTest::addColumn<int>("position"); QTest::addColumn<bool>("cancel");
        for (const auto* source : {"soundsettingspage.cpp", "gui_bridge_impl.cpp"})
            for (int position : {1, 9, 18})
                for (bool cancel : {false, true})
                    QTest::newRow(qPrintable(QString("%1-%2-%3").arg(source).arg(position).arg(cancel ? "cancel" : "execute")))
                        << QByteArray(source) << position << cancel;
    }
    void allocationFailure() {
        QFETCH(QByteArray, source); QFETCH(int, position); QFETCH(bool, cancel);
        SoundSettingsCore core; QVERIFY(core.start());
        SoundSettingsPage page(&core.db);
        const auto values = expectedSettings(1); editPage(page, values);
        const auto baseline = liveAllocations();
        allocationFile = source.constData(); failAllocation = position;
        auto reset = qScopeGuard([] { failAllocation = 0; allocationFile = nullptr; });
        page.applyAndSave(); QCOMPARE(failAllocation, 0); checkLocalSettings(values);
        if (cancel) gui_bridge_set_inst(nullptr);
        else uasync_poll(core.ua, 0);
        QCOMPARE(liveAllocations(), baseline);
        int saved = 0;
        for (auto it = values.cbegin(); it != values.cend(); ++it) {
            const auto stored = core.db.getUiState(it.key());
            if (stored.isEmpty()) continue;
            QCOMPARE(stored, it.value()); ++saved;
        }
        QCOMPARE(saved, cancel ? 0 : 17);
    }
#endif
};

QTEST_MAIN(SoundSettingsTest)
#include "test_sound_settings.moc"

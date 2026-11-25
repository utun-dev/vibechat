/* Реальные страницы/диалог/MainWindow/bridge/SQLite: запросы переживают форму, освобождаются при отказе/отмене/OOM. */
#include "../src/storagesettingspage.h"
#include "../src/audiodevicesettingspage.h"
#include "../src/settingsdialog.h"
#include "../src/desktop_integration.h"
#include "../src/networksettingspage.h"
#include "../src/gui_theme.h"
#include "../src/channellist.h"
#include "../src/channeldelegate.h"
#include "../src/messagelist.h"
#include "../src/messagedelegate.h"
#include "../src/chatview.h"
#include "../src/emojitabbar.h"
#include "../src/message_notification.h"
#include "../src/bluetoothsettingspage.h"
#include "../src/mainwindow.h"
#include "../src/imageviewer_window.h"
#include "../src/videoplayer_window.h"
#include "../src/radio_audio_engine.h"
#include "../src/radiosettingspage.h"
#include "../src/voicemessageencoder.h"
#include "../db/db_manager.h"
#include "../transport/gui_bridge.h"
#include "../transport/node_config.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QMessageBox>
#include <QCheckBox>
#include <QSpinBox>
#include <QSettings>
#include <QSplitter>
#include <QComboBox>
#include <QListWidget>
#include <QTextEdit>
#include <QDir>
#include <QTranslator>
#include <QToolButton>
#include <QStackedWidget>
#include <QScrollArea>
#include <QScopeGuard>
#include <QPushButton>
#include <QJsonDocument>
#include <map>
#include <memory>
#include <cstring>
extern "C" {
#include "utun_instance.h"
#include "chat/chat_core.h"
#include "chat/chat_setting.h"
#include "mem.h"
#include "debug_config.h"
}

#ifdef UTUN_TEST_SETTINGS_ALLOC_FAILURES
#include "miniaudio.h"

static bool watching, failMalloc;
static int failAllocation;
static std::map<void*, const char*> outstanding;
static bool commandSource(const char* where) {
    return strstr(where, "/gui_bridge_impl.cpp:") || strstr(where, "/settingsdialog.cpp:") ||
           strstr(where, "/storagesettingspage.cpp:") || strstr(where, "/audiodevicesettingspage.cpp:") ||
           strstr(where, "/mainwindow.cpp:");
}
static bool rejectAllocation(const char* where, bool mallocCall) {
    if (!failAllocation || mallocCall != failMalloc || !strstr(where, "/gui_bridge_impl.cpp:") || --failAllocation) return false;
    DEBUG_WARN(DEBUG_CATEGORY_CONFIG, "[INJECT] settings allocation failure malloc=%d at %s", mallocCall, where);
    return true;
}
extern "C" void* __real_u_malloc_impl(uint32_t, const char*);
extern "C" void* __wrap_u_malloc_impl(uint32_t bytes, const char* where) {
    if (rejectAllocation(where, true)) return nullptr;
    void* ptr = __real_u_malloc_impl(bytes, where);
    if (ptr && watching && commandSource(where)) outstanding.emplace(ptr, where);
    return ptr;
}
extern "C" void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
extern "C" void* __wrap_u_calloc_impl(uint32_t count, uint32_t bytes, const char* where) {
    if (rejectAllocation(where, false)) return nullptr;
    void* ptr = __real_u_calloc_impl(count, bytes, where);
    if (ptr && watching && commandSource(where)) outstanding.emplace(ptr, where);
    return ptr;
}
extern "C" void __real_u_free_impl(void*, const char*);
extern "C" void __wrap_u_free_impl(void* ptr, const char* where) {
    if (watching) outstanding.erase(ptr);
    __real_u_free_impl(ptr, where);
}
// Настоящий mixer работает через null backend: тест закрытия окна не зависит от аппаратных аудиоустройств.
extern "C" ma_result __real_ma_context_init(const ma_backend*, ma_uint32, const ma_context_config*, ma_context*);
extern "C" ma_result __wrap_ma_context_init(const ma_backend*, ma_uint32, const ma_context_config* config, ma_context* context) {
    const ma_backend backend = ma_backend_null;
    return __real_ma_context_init(&backend, 1, config, context);
}
#endif

struct SettingsCore {
    QTemporaryDir directory;
    UASYNC* ua = uasync_create();
    UTUN_INSTANCE* inst = nullptr;
    DbManager db;
    QString path() const { return directory.filePath("utun.cfg"); }
    ~SettingsCore() {
        gui_bridge_set_inst(nullptr); gui_bridge_set_uasync(nullptr);
        if (ua) uasync_mark_stopped(ua);
        db.setDb(nullptr);
        if (inst) utun_instance_destroy(inst);
        if (ua) uasync_destroy(ua, 0);
    }
    bool start() {
        if (!ua || !directory.isValid()) return false;
        const QByteArray text = "[global]\n"
            "my_private_key=704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f\n"
            "my_public_key=7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f\n"
            "my_node_name=Before\n[chatserver]\ngroup_autoconnect=0\nmember_files_enabled=0\nstorage_autoload=0\n";
        QFile file(path());
        if (!file.open(QIODevice::WriteOnly) || file.write(text) != text.size()) return false;
        file.close();
        inst = utun_instance_create_from_str(ua, text.constData());
        if (!inst || utun_core_start(inst) || chat_service_start(inst)) return false;
        db.setDb(inst->topo_sqlite_db);
        gui_bridge_set_uasync(ua); gui_bridge_set_inst(inst);
        RadioAudioEngine::instance()->loadSettings(path());
        uasync_poll(ua, 0); QCoreApplication::processEvents();
        return RadioAudioEngine::instance()->settingsReady();
    }
};

class SettingsSaveTest : public QObject {
    Q_OBJECT
    QTemporaryDir m_desktopConfig;
    enum Page { Storage, Audio, Dialog };
    enum WindowOutcome { Unavailable, Cancelled, Executed, Destroyed };
    std::unique_ptr<QWidget> createPage(SettingsCore& core, int kind) {
        std::unique_ptr<QWidget> result;
        if (kind == Storage) {
            auto* page = new StorageSettingsPage(&core.db);
            page->setConfigPath(core.path()); page->loadFromDb(); result.reset(page);
        } else if (kind == Audio) {
            auto* page = new AudioDeviceSettingsPage(&core.db, nullptr);
            page->setConfigPath(core.path()); page->loadFromDb(); result.reset(page);
        } else {
            auto* page = new SettingsDialog(core.path(), &core.db, nullptr, nullptr);
            for (auto* edit : page->findChildren<QLineEdit*>()) if (edit->text() == "Before") edit->setText("After");
            page->findChild<QSpinBox*>("streamSubblocks")->setValue(12);
            page->findChild<QSpinBox*>("streamRedundant")->setValue(3);
            page->findChild<QSpinBox*>("streamStallTimeout")->setValue(750);
            page->findChild<QSpinBox*>("streamMaxLiveLag")->setValue(12000);
            page->findChild<QSpinBox*>("stream_regular_limit")->setValue(256);
            page->findChild<QSpinBox*>("stream_temporary_limit")->setValue(96);
            page->findChild<QSpinBox*>("stream_background_limit")->setValue(24);
            page->findChild<QSpinBox*>("stream_peer_temporary_limit")->setValue(20);
            page->findChild<QSpinBox*>("stream_peer_limit")->setValue(80);
            page->findChild<QSpinBox*>("stream_control_limit")->setValue(256);
            page->findChild<QSpinBox*>("stream_extra_receive_limit")->setValue(8);
            page->findChild<QCheckBox*>("radioPttDoublePress")->setChecked(false);
            page->findChild<QSpinBox*>("callVideoMinimumBitrate")->setValue(700);
            page->findChild<QComboBox*>("radioOpusBitrate")->setCurrentIndex(2);
            result.reset(page);
        }
        auto* storage = kind == Storage ? static_cast<StorageSettingsPage*>(result.get()) : result->findChild<StorageSettingsPage*>();
        if (storage) {
            storage->findChild<QCheckBox*>()->setChecked(true);
            const auto spins = storage->findChildren<QSpinBox*>();
            spins[0]->setValue(17); spins[1]->setValue(3);
        }
        auto* audio = kind == Audio ? static_cast<AudioDeviceSettingsPage*>(result.get()) : result->findChild<AudioDeviceSettingsPage*>();
        if (audio) {
            audio->findChild<QCheckBox*>("compressorGainOnlyOnSpeech")->setChecked(true);
            audio->findChild<QSpinBox*>("compressorVadMaxGain")->setValue(27);
            audio->findChild<QCheckBox*>("noiseSuppressionEnabled")->setChecked(false);
            audio->findChild<QSpinBox*>("noiseSuppressionDb")->setValue(-30);
            const auto combos = audio->findChildren<QComboBox*>();
            combos[2]->setCurrentIndex(combos[2]->findData(2));
            const QMap<QString, int> compressor{{"compressorMaxGain", 0}, {"compressorLookback", 350},
                                                {"compressorLookahead", 220}, {"compressorTargetLevel", -9}};
            for (auto it = compressor.cbegin(); it != compressor.cend(); ++it) {
                auto* slider = audio->findChild<QSlider*>(it.key());
                if (!slider) { QTest::qFail(qPrintable(it.key()), __FILE__, __LINE__); return result; }
                slider->setValue(it.value());
            }
        }
        return result;
    }
    void apply(QWidget* page, int kind) {
        if (kind == Storage) static_cast<StorageSettingsPage*>(page)->applyAndSave();
        else if (kind == Audio) static_cast<AudioDeviceSettingsPage*>(page)->applyAndSave();
        else QVERIFY(QMetaObject::invokeMethod(page, "onSave", Qt::DirectConnection));
    }
    void checkSaved(SettingsCore& core, int kind) {
        if (kind != Audio) {
            QCOMPARE(core.db.getUiStateInt("storage_autoload", -1), 1);
            QCOMPARE(core.db.getUiStateInt("storage_autoload_maxsize_mb", -1), 17);
            QCOMPARE(core.db.getUiStateInt("storage_maxsize_gb", -1), 3);
            QCOMPARE(chat_setting_get_int(core.inst, "storage_autoload", -1), 1);
            QSettings config(core.path(), QSettings::IniFormat);
            QCOMPARE(config.value("chatserver/storage_unit_size").toString(), QString("17M"));
            QCOMPARE(config.value("chatserver/storage_total_size").toString(), QString("3G"));
        }
        if (kind != Storage) {
            QCOMPARE(core.db.getUiStateInt("opus_codec_preset", -1), 2);
            QCOMPARE(chat_setting_get_int(core.inst, "opus_codec_preset", -1), 2);
            QVERIFY(!core.db.getUiState("call_video_bitrate_range_kbps").isEmpty());
            QCOMPARE(core.db.getUiState("call_video_bitrate_range_kbps"), QString::fromUtf8(chat_setting_get(core.inst,
                                                                                                       "call_video_bitrate_range_kbps")));
            QCOMPARE(VoiceEncoder::preset(), 2);
            QCOMPARE(core.db.getUiStateInt("compressor_max_gain_db", -1), 0);
            QCOMPARE(chat_setting_get_int(core.inst, "compressor_max_gain_db", -1), 0);
            AudioDeviceSettingsPage restored(&core.db, nullptr); restored.loadFromDb();
            QCOMPARE(core.db.getUiStateInt("noise_suppression_enabled", -1), 0);
            QCOMPARE(core.db.getUiStateInt("noise_suppression_db", 0), -30);
            QCOMPARE(chat_setting_get_int(core.inst, "noise_suppression_enabled", -1), 0);
            QCOMPARE(chat_setting_get_int(core.inst, "noise_suppression_db", 0), -30);
            QVERIFY(!restored.findChild<QCheckBox*>("noiseSuppressionEnabled")->isChecked());
            QCOMPARE(restored.findChild<QSpinBox*>("noiseSuppressionDb")->value(), -30);
            QVERIFY(!restored.findChild<QSpinBox*>("noiseSuppressionDb")->isEnabled());
            QSettings noiseConfig(core.path(), QSettings::IniFormat);
            QCOMPARE(core.db.getUiStateInt("compressor_gain_only_on_speech", -1), 1);
            QCOMPARE(core.db.getUiStateInt("radio_vad_compressor_max_gain_db", -1), 27);
            QCOMPARE(restored.findChild<QSpinBox*>("compressorVadMaxGain")->value(), 27);
            QVERIFY(restored.findChild<QCheckBox*>("compressorGainOnlyOnSpeech")->isChecked());
            QVERIFY(restored.findChild<QCheckBox*>("compressorGainOnlyOnSpeech")->isEnabled());
            QVERIFY(restored.findChild<QSlider*>("compressorTargetLevel")->isEnabled());
            QCOMPARE(noiseConfig.value("chatserver/noise_suppression_enabled").toInt(), 0);
            QCOMPARE(noiseConfig.value("chatserver/noise_suppression_db").toInt(), -30);
            const QMap<QString, QPair<const char*, int>> compressor{
                {"compressorLookback", {"compressor_lookback_ms", 350}},
                {"compressorLookahead", {"compressor_lookahead_ms", 220}},
                {"compressorTargetLevel", {"compressor_target_level_db", -9}}};
            for (auto it = compressor.cbegin(); it != compressor.cend(); ++it) {
                QCOMPARE(core.db.getUiStateInt(it.value().first, -100), it.value().second);
                auto* slider = restored.findChild<QSlider*>(it.key()); QVERIFY(slider);
                QCOMPARE(slider->value(), it.value().second);
            }
        }
        if (kind == Dialog) {
            QCOMPARE(core.db.getUiState("call_video_bitrate_range_kbps"), QString("700:4000"));
            QCOMPARE(core.db.getUiStateInt("radio_opus_bitrate", -1), 64);
            QCOMPARE(chat_setting_get_int(core.inst, "radio_opus_bitrate", -1), 64);
            QCOMPARE(QString::fromUtf8(core.inst->name), QString("After"));
            QCOMPARE(chat_setting_get_int(core.inst, "stream_subblocks", -1), 12);
            QCOMPARE(chat_setting_get_int(core.inst, "stream_rs_redundant", -1), 3);
            QCOMPARE(chat_setting_get_int(core.inst, "stream_stall_timeout_ms", -1), 750);
            QCOMPARE(chat_setting_get_int(core.inst, "stream_max_live_lag_ms", -1), 12000);
            for (const auto& limit : {QPair<const char*, int>{"stream_regular_limit", 256}, {"stream_temporary_limit", 96},
                {"stream_background_limit", 24}, {"stream_peer_temporary_limit", 20}, {"stream_peer_limit", 80},
                {"stream_control_limit", 256}, {"stream_extra_receive_limit", 8}}) {
                QCOMPARE(chat_setting_get_int(core.inst, limit.first, -1), limit.second);
                QSettings config(core.path(), QSettings::IniFormat);
                QCOMPARE(config.value(QStringLiteral("chatserver/") + limit.first).toInt(), limit.second);
            }
            QCOMPARE(core.db.getUiStateInt("join_policy", -1), 1);
            QCOMPARE(core.db.getUiStateInt("group_autoconnect", -1), 1);
            QCOMPARE(core.db.getUiState("radio_ptt_double_press"), QString("0"));
            QCOMPARE(core.db.getUiState("radio_ptt_hotkey"), QString());
            RadioSettingsPage restored(&core.db);
            QVERIFY(!restored.doublePressEnabled());
        }
    }
private slots:
    void streamRequiredPartsLimitIsAtomicAndAllowsParityAbove192() {
        SettingsCore core; QVERIFY(core.start());
        SettingsDialog page(core.path(), &core.db, nullptr, nullptr);
        auto* total = page.findChild<QSpinBox*>("streamSubblocks");
        auto* parity = page.findChild<QSpinBox*>("streamRedundant");
        QVERIFY(total && parity);
        QFile before(core.path()); QVERIFY(before.open(QIODevice::ReadOnly)); const auto original = before.readAll(); before.close();
        const int oldTotal = chat_setting_get_int(core.inst, "stream_subblocks", -1);
        for (int n : {193, 255}) {
            total->setValue(n); parity->setValue(n - 193);
            QVERIFY(QMetaObject::invokeMethod(&page, "onSave", Qt::DirectConnection));
            uasync_poll(core.ua, 0); QCoreApplication::processEvents();
            QFile after(core.path()); QVERIFY(after.open(QIODevice::ReadOnly)); QCOMPARE(after.readAll(), original);
            QCOMPARE(chat_setting_get_int(core.inst, "stream_subblocks", -1), oldTotal);
        }
        parity->setValue(63);
        QVERIFY(QMetaObject::invokeMethod(&page, "onSave", Qt::DirectConnection));
        uasync_poll(core.ua, 0); QCoreApplication::processEvents();
        QSettings config(core.path(), QSettings::IniFormat);
        QCOMPARE(config.value("chatserver/stream_subblocks").toInt(), 255);
        QCOMPARE(config.value("chatserver/stream_rs_redundant").toInt(), 63);
        QCOMPARE(chat_setting_get_int(core.inst, "stream_subblocks", -1), 255);
        QCOMPARE(chat_setting_get_int(core.inst, "stream_rs_redundant", -1), 63);
        page.findChild<QSpinBox*>("stream_regular_limit")->setValue(256); parity->setValue(0);
        QVERIFY(QMetaObject::invokeMethod(&page, "onSave", Qt::DirectConnection));
        uasync_poll(core.ua, 0); QCoreApplication::processEvents();
        QCOMPARE(chat_setting_get_int(core.inst, "stream_regular_limit", -1), 256);
        QCOMPARE(chat_setting_get_int(core.inst, "stream_rs_redundant", -1), 0);
    }
    void hideMySocketsPersistsWithoutOverwritingServerType() {
        SettingsCore core; QVERIFY(core.start());
        NetworkSettingsPage page;
        page.loadFromConfig(core.path());
        auto* hidden = page.findChild<QCheckBox*>("hideMySockets"); QVERIFY(hidden);
        QVERIFY(!hidden->isChecked()); QVERIFY(!page.hideMySocketsChanged());
        hidden->setChecked(true); QVERIFY(page.hideMySocketsChanged());
        QVERIFY(page.saveToConfig(core.path()));
        NetworkSettingsPage restored; restored.loadFromConfig(core.path());
        QVERIFY(restored.findChild<QCheckBox*>("hideMySockets")->isChecked());
        QVERIFY(!restored.hideMySocketsChanged());
        hidden->setChecked(false); QVERIFY(!page.hideMySocketsChanged());
        QVERIFY(page.saveToConfig(core.path()));
        restored.loadFromConfig(core.path());
        QVERIFY(!restored.findChild<QCheckBox*>("hideMySockets")->isChecked());
    }

    void noiseSuppressionDefaultsAndRuntimeWithoutConfig() {
        SettingsCore core; QVERIFY(core.start());
        AudioDeviceSettingsPage page(&core.db, nullptr); page.loadFromDb();
        auto* enabled = page.findChild<QCheckBox*>("noiseSuppressionEnabled");
        auto* level = page.findChild<QSpinBox*>("noiseSuppressionDb");
        QVERIFY(enabled && level && enabled->isChecked() && level->isEnabled());
        QCOMPARE(level->value(), -20);
        QCOMPARE(chat_setting_get_int(core.inst, "noise_suppression_enabled", 0), 1);
        QCOMPARE(chat_setting_get_int(core.inst, "noise_suppression_db", 0), -20);
        enabled->setChecked(false); level->setValue(-35); page.applyAndSave();
        uasync_poll(core.ua, 0);
        QCOMPARE(chat_setting_get_int(core.inst, "noise_suppression_enabled", -1), 0);
        QCOMPARE(chat_setting_get_int(core.inst, "noise_suppression_db", 0), -35);
        AudioDeviceSettingsPage restored(&core.db, nullptr); restored.loadFromDb();
        QVERIFY(!restored.findChild<QCheckBox*>("noiseSuppressionEnabled")->isChecked());
        QCOMPARE(restored.findChild<QSpinBox*>("noiseSuppressionDb")->value(), -35);
    }
    void mediaGeometryCachedBeforeDatabaseWrite() {
        SettingsCore core; QVERIFY(core.start());
        MainWindow window(nullptr, nullptr, QString());
        const QString photo = core.directory.filePath("photo.png");
        QImage image(32, 24, QImage::Format_RGB32); image.fill(Qt::green); QVERIFY(image.save(photo));
        auto findWindow = [](const QString& name) -> QWidget* {
            for (auto* widget : QApplication::topLevelWidgets())
                if (widget->objectName() == name && widget->isVisible()) return widget;
            return nullptr;
        };
        QVERIFY(QMetaObject::invokeMethod(&window, "openImage", Q_ARG(QString, photo)));
        auto* viewer = findWindow("chatImageViewer"); QVERIFY(viewer && !viewer->isFullScreen());
        viewer->setGeometry(90, 70, 540, 360); QCoreApplication::processEvents();
        const QRect expected = viewer->normalGeometry();
        QVERIFY(viewer->close());
        QVERIFY(core.db.getUiState("media_window_geometry").isEmpty()); // uasync ещё не исполнил запись.
        QVERIFY(QMetaObject::invokeMethod(&window, "openVideo", Q_ARG(QString, core.directory.filePath("missing.mp4"))));
        auto* player = findWindow("chatVideoPlayer"); QVERIFY(player);
        QCOMPARE(player->normalGeometry(), expected);
        QVERIFY(QMetaObject::invokeMethod(player, "onEngineOpened", Q_ARG(int, 3840), Q_ARG(int, 2160),
                                         Q_ARG(qint64, 60000), Q_ARG(bool, false)));
        QCOMPARE(player->normalGeometry(), expected); QVERIFY(!player->isFullScreen());
        QVERIFY(player->close());
        QVERIFY(QMetaObject::invokeMethod(&window, "openImage", Q_ARG(QString, photo)));
        viewer = findWindow("chatImageViewer"); QVERIFY(viewer);
        QCOMPARE(viewer->normalGeometry(), expected);
        uasync_poll(core.ua, 0);
        const auto saved = QJsonDocument::fromJson(core.db.getUiState("media_window_geometry").toUtf8()).object();
        QCOMPARE(saved.value("x").toInt(), expected.x()); QCOMPARE(saved.value("y").toInt(), expected.y());
        QCOMPARE(saved.value("width").toInt(), expected.width()); QCOMPARE(saved.value("height").toInt(), expected.height());
        QVERIFY(viewer->close()); uasync_poll(core.ua, 0);
    }

    void bluetoothSettingsPersistAllPermissions() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString path = directory.filePath("bluetooth.cfg");
        BluetoothSettingsPage page(path, nullptr);
        auto* enabled = page.findChild<QCheckBox*>("bluetoothEnabled");
        auto* advertise = page.findChild<QCheckBox*>("bluetoothAdvertise");
        auto* incoming = page.findChild<QCheckBox*>("bluetoothIncoming");
        QVERIFY(enabled && advertise && incoming);
        QVERIFY(!enabled->isChecked()); QVERIFY(!advertise->isEnabled());
        enabled->setChecked(true); advertise->setChecked(true); incoming->setChecked(true);
        QVERIFY(page.save());
        NodeConfig config(path); QVERIFY(config.load()); QCOMPARE(config.bluetoothFlags(), 7);
        BluetoothSettingsPage restored(path, nullptr);
        QVERIFY(restored.findChild<QCheckBox*>("bluetoothEnabled")->isChecked());
        QVERIFY(restored.findChild<QCheckBox*>("bluetoothIncoming")->isChecked());
        enabled->setChecked(false); QVERIFY(page.save());
        QVERIFY(config.load()); QCOMPARE(config.bluetoothFlags(), 6);
        BluetoothSettingsPage disabled(path, nullptr);
        QVERIFY(!disabled.findChild<QCheckBox*>("bluetoothEnabled")->isChecked());
    }
    void settingsNavigationRetainsDraft() {
        SettingsCore core; QVERIFY(core.start());
        SettingsDialog settings(core.path(), &core.db, nullptr, nullptr); settings.show();
        auto* navigation = settings.findChild<QListWidget*>("settingsNavigation"); QVERIFY(navigation);
        auto* theme = settings.findChild<QComboBox*>("themeCombo"); QVERIFY(theme);
        auto* bitrate = settings.findChild<QSpinBox*>("callVideoMinimumBitrate"); QVERIFY(bitrate);
        auto* radio = settings.findChild<QComboBox*>("radioOpusBitrate"); QVERIFY(radio);
        theme->setCurrentIndex(theme->findData("light")); bitrate->setValue(700); radio->setCurrentIndex(radio->findData(96));
        QStringList visited;
        for (int row = 0; row < navigation->count(); ++row) {
            auto* item = navigation->item(row);
            if (!(item->flags() & Qt::ItemIsSelectable)) continue;
            navigation->setCurrentRow(row); QCoreApplication::processEvents();
            const QString key = item->data(Qt::UserRole + 1).toString(); visited.append(key);
            auto* page = settings.findChild<QWidget*>("settings-" + key); QVERIFY(page && page->isVisible());
            QCOMPARE(theme->currentData().toString(), QString("light")); QCOMPARE(bitrate->value(), 700);
            QCOMPARE(radio->currentData().toInt(), 96);
        }
        QCOMPARE(visited, QStringList({"profile", "interface", "sounds", "audio", "calls", "radio", "broadcast",
                                       "network", "bluetooth", "storage", "status", "database"}));
        QVERIFY(settings.findChild<QWidget*>("settings-interface")->isAncestorOf(theme));
        QVERIFY(settings.findChild<QWidget*>("settings-calls")->isAncestorOf(bitrate));
        QVERIFY(settings.findChild<QWidget*>("settings-radio")->isAncestorOf(radio));
        settings.reject();
        NodeConfig restored(core.path()); QVERIFY(restored.load()); QCOMPARE(restored.theme(), QString("dark"));
        QCOMPARE(core.db.getUiState("call_video_bitrate_range_kbps"), QString());
    }

    void themeSaveCancelAndLiveWidgets() {
        SettingsCore core; QVERIFY(core.start());
        const auto previous = QApplication::palette();
        const auto previousStyle = qApp->styleSheet();
        auto restore = qScopeGuard([&] { QApplication::setPalette(previous); qApp->setStyleSheet(previousStyle); });
        applyGuiTheme("dark");
        MainWindow preview(nullptr, nullptr, core.path());
        auto* channels = preview.findChild<ChannelList*>(); QVERIFY(channels);
        auto* messages = preview.findChild<MessageList*>(); QVERIFY(messages);
        auto* channelView = channels->findChild<QListView*>(); QVERIFY(channelView);
        auto* channelModel = qobject_cast<QStandardItemModel*>(channelView->model()); QVERIFY(channelModel);
        for (int i = 0; i < 2; ++i) {
            auto* item = new QStandardItem;
            item->setData(i == 0 ? "Selected channel" : "General", ChannelNameRole);
            item->setData("Alice: Latest message preview", ChannelLastMessageRole);
            item->setData(QString::number(i + 1), ChannelChannelIdRole);
            item->setData(i + 1, ChannelPeersOnlineRole); channelModel->appendRow(item);
        }
        channelView->setCurrentIndex(channelModel->index(0, 0));
        auto* view = messages->findChild<ChatView*>(); QVERIFY(view);
        auto* model = qobject_cast<QStandardItemModel*>(view->model()); QVERIFY(model);
        for (int i = 0; i < 4; ++i) {
            auto* item = new QStandardItem;
            item->setData(i % 2 ? "Me" : "Alice", MsgAuthorRole);
            item->setData(i == 2 ? "Code example:\n```cpp\nint main() { return 0; }\n```" :
                i == 3 ? "A link: https://example.com and **bold text**" : "Чёрный текст на белом фоне. Hello!", MsgTextRole);
            item->setData("12:34", MsgTimeRole); item->setData(i % 2, MsgIsOutgoingRole);
            item->setData(i + 1, MsgIdRole);
            if (i == 1) {
                item->setData(true, MsgHasQuoteRole); item->setData("Alice", MsgQuoteAuthorRole);
                item->setData("Quoted message", MsgQuoteTextRole);
            }
            model->appendRow(item);
        }
        preview.resize(1100, 650); preview.show();
        auto* gear = channels->findChild<QPushButton*>("settingsButton"); QVERIFY(gear);
        QCoreApplication::processEvents();
        const QImage darkIcon = gear->icon().pixmap(24, 24).toImage();
        {
            SettingsDialog cancelled(core.path(), &core.db, nullptr, nullptr);
            auto* combo = cancelled.findChild<QComboBox*>("themeCombo"); QVERIFY(combo);
            combo->setCurrentIndex(combo->findData("light")); cancelled.reject();
        }
        NodeConfig config(core.path()); QVERIFY(config.load()); QCOMPARE(config.theme(), QString("dark"));
        QCOMPARE(QApplication::palette().window().color(), guiThemePalette("dark").window().color());
        auto* composer = messages->findChild<QTextEdit*>(); QVERIFY(composer);
        composer->setPlainText("Message input / Поле ввода");
        MessageNotification notification;
        const QString screenshots = qEnvironmentVariable("UTUN_THEME_SCREENSHOTS");
        if (!screenshots.isEmpty()) QVERIFY(QDir().mkpath(screenshots));
        for (const QString& theme : {QString("light"), QString("dark")}) {
            SettingsDialog settings(core.path(), &core.db, nullptr, nullptr);
            auto* combo = settings.findChild<QComboBox*>("themeCombo"); QVERIFY(combo);
            combo->setCurrentIndex(combo->findData(theme));
            QVERIFY(QMetaObject::invokeMethod(&settings, "onSave", Qt::DirectConnection));
            QCOMPARE(settings.result(), int(QDialog::Accepted));
            QVERIFY(config.load()); QCOMPARE(config.theme(), theme);
            uasync_poll(core.ua, 0); QCoreApplication::processEvents();
            QCOMPARE(view->palette().text().color(), guiThemePalette(theme).text().color());
            auto* editor = messages->findChild<QTextEdit*>(); QVERIFY(editor);
            QCOMPARE(editor->palette().base().color(), guiThemePalette(theme).base().color());
            QCOMPARE(editor->palette().text().color(), guiThemePalette(theme).text().color());
            if (theme == "light") QVERIFY(darkIcon != gear->icon().pixmap(24, 24).toImage());
            if (!screenshots.isEmpty()) {
                QVERIFY(preview.grab().save(screenshots + "/chat-" + theme + ".png"));
                notification.showMessage("1", "Alice", "General", QRect());
                QCoreApplication::processEvents();
                QVERIFY(notification.grab().save(screenshots + "/notification-" + theme + ".png"));
                notification.dismiss("theme-test");
                for (const QString& language : {QString("en"), QString("ru")}) {
                    QTranslator translator; QVERIFY(translator.load(":/translations/chatgui_" + language + ".qm"));
                    QVERIFY(qApp->installTranslator(&translator));
                    auto remove = qScopeGuard([&] { qApp->removeTranslator(&translator); });
                    SettingsDialog pages(core.path(), &core.db, nullptr, nullptr); pages.show();
                    auto* categories = pages.findChild<QListWidget*>("settingsNavigation"); QVERIFY(categories);
                    const QString prefix = screenshots + "/settings-" + theme + (language == "en" ? "" : "-ru") + "-";
                    for (int row = 0; row < categories->count(); ++row) {
                        const auto* item = categories->item(row);
                        if (!(item->flags() & Qt::ItemIsSelectable)) continue;
                        const QString key = item->data(Qt::UserRole + 1).toString();
                        categories->setCurrentRow(row); QCoreApplication::processEvents();
                        QVERIFY(pages.grab().save(prefix + key + ".png"));
                        auto* page = pages.findChild<QWidget*>("settings-" + key); QVERIFY(page);
                        for (auto* button : page->findChildren<QToolButton*>())
                            if (button->property("settingsRole").toString() == "details") button->setChecked(true);
                        QCoreApplication::processEvents();
                        QVERIFY(pages.grab().save(prefix + key + "-expanded.png"));
                        for (auto* button : page->findChildren<QToolButton*>())
                            if (button->property("settingsRole").toString() == "details") button->setChecked(false);
                        pages.resize(760, 460); QCoreApplication::processEvents();
                        QVERIFY(pages.grab().save(prefix + key + "-small.png"));
                        if (key == "network") {
                            const auto* network = pages.findChild<NetworkSettingsPage*>(); QVERIFY(network);
                            for (auto* edit : network->findChildren<QLineEdit*>())
                                QVERIFY2(edit->height() >= edit->fontMetrics().height() + 2,
                                    qPrintable(QString("Network field clipped: height=%1 text=%2")
                                        .arg(edit->height()).arg(edit->fontMetrics().height())));
                        }
                        pages.resize(960, 680); QCoreApplication::processEvents();
                    }
                }
            }
        }
    }

    void initTestCase() {
#ifdef Q_OS_LINUX
        QVERIFY(m_desktopConfig.isValid());
        qputenv("XDG_CONFIG_HOME", m_desktopConfig.path().toUtf8());
        qputenv("XDG_CONFIG_DIRS", m_desktopConfig.filePath("system-config").toUtf8());
#endif
        debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); gui_bridge_init();
        debug_set_category_level(DEBUG_CATEGORY_CONFIG, DEBUG_LEVEL_DEBUG);
        debug_set_category_level(DEBUG_CATEGORY_GENERAL, DEBUG_LEVEL_DEBUG);
        auto* warnings = new QTimer(this);
        connect(warnings, &QTimer::timeout, [] {
            if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) box->accept();
        });
        warnings->start(10);
    }
#ifdef Q_OS_LINUX
    void linuxAutostartSettings() {
        SettingsCore core; QVERIFY(core.start());
        const QString path = m_desktopConfig.filePath("autostart/org.utun.vibechat.desktop");
        QVERIFY(!desktopAutostartEnabled());
        {
            SettingsDialog cancelled(core.path(), &core.db, nullptr, nullptr);
            auto* check = cancelled.findChild<QCheckBox*>("linuxStartAtLogin"); QVERIFY(check);
            QVERIFY(!check->isChecked()); check->setChecked(true); cancelled.reject();
        }
        QVERIFY(!QFile::exists(path));
        {
            SettingsDialog settings(core.path(), &core.db, nullptr, nullptr);
            auto* check = settings.findChild<QCheckBox*>("linuxStartAtLogin"); QVERIFY(check);
            check->setChecked(true);
            QVERIFY(QMetaObject::invokeMethod(&settings, "onSave", Qt::DirectConnection));
            QCOMPARE(settings.result(), int(QDialog::Accepted));
        }
        QVERIFY(desktopAutostartEnabled());
        {
            SettingsDialog settings(core.path(), &core.db, nullptr, nullptr);
            auto* check = settings.findChild<QCheckBox*>("linuxStartAtLogin"); QVERIFY(check);
            QVERIFY(check->isChecked()); check->setChecked(false);
            QVERIFY(QMetaObject::invokeMethod(&settings, "onSave", Qt::DirectConnection));
            QCOMPARE(settings.result(), int(QDialog::Accepted));
        }
        QVERIFY(!desktopAutostartEnabled()); QVERIFY(!QFile::exists(path));
        QVERIFY(setDesktopAutostartEnabled(true, QCoreApplication::applicationFilePath()));
        {
            QFile entry(path); QVERIFY(entry.open(QIODevice::ReadOnly));
            QByteArray content = entry.readAll(); entry.close(); content.replace("Hidden=false", "Hidden=true");
            QVERIFY(entry.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QCOMPARE(entry.write(content), qint64(content.size()));
        }
        {
            SettingsDialog settings(core.path(), &core.db, nullptr, nullptr);
            auto* check = settings.findChild<QCheckBox*>("linuxStartAtLogin"); QVERIFY(check);
            QVERIFY(!check->isChecked());
            QVERIFY(QMetaObject::invokeMethod(&settings, "onSave", Qt::DirectConnection));
            QCOMPARE(settings.result(), int(QDialog::Accepted));
        }
        // Неизменённая опция не перезаписывает отключение, сделанное через рабочий стол.
        QVERIFY(!desktopAutostartEnabled()); QVERIFY(QFile::remove(path));
        QVERIFY(QDir().rmdir(m_desktopConfig.filePath("autostart")));
        QFile obstacle(m_desktopConfig.filePath("autostart")); QVERIFY(obstacle.open(QIODevice::WriteOnly)); obstacle.close();
        {
            SettingsDialog settings(core.path(), &core.db, nullptr, nullptr);
            settings.findChild<QCheckBox*>("linuxStartAtLogin")->setChecked(true);
            QVERIFY(QMetaObject::invokeMethod(&settings, "onSave", Qt::DirectConnection));
            QCOMPARE(settings.result(), int(QDialog::Rejected));
        }
        QVERIFY(!desktopAutostartEnabled()); QVERIFY(obstacle.remove());
    }
#endif
    void init() {
#ifdef UTUN_TEST_SETTINGS_ALLOC_FAILURES
        watching = false; outstanding.clear(); failAllocation = 0;
#endif
    }
    void cleanup() {
#ifdef UTUN_TEST_SETTINGS_ALLOC_FAILURES
        watching = false; failAllocation = 0; outstanding.clear();
#endif
    }
    void lifecycle_data() {
        QTest::addColumn<int>("kind"); QTest::addColumn<int>("outcome");
        for (int kind : {Storage, Audio, Dialog})
            for (int outcome = 0; outcome < 3; ++outcome)
                QTest::newRow(qPrintable(QString("%1-%2").arg(kind).arg(outcome))) << kind << outcome;
    }
    void lifecycle() {
        QFETCH(int, kind); QFETCH(int, outcome);
        SettingsCore core; QVERIFY(core.start());
        auto page = createPage(core, kind);
        if (!outcome) gui_bridge_set_inst(nullptr); // Отказ при недоступном ядре.
        const auto baseline = u_get_allocated_count();
#ifdef UTUN_TEST_SETTINGS_ALLOC_FAILURES
        watching = true;
#endif
        apply(page.get(), kind);
        page.reset(); // Снимок запросов не должен зависеть от формы.
        if (outcome == 2) { uasync_poll(core.ua, 0); checkSaved(core, kind); }
        else {
            gui_bridge_set_inst(nullptr);
            QCOMPARE(core.db.getUiStateInt("storage_autoload", -1), -1);
            QCOMPARE(core.db.getUiStateInt("opus_codec_preset", -1), -1);
            QCOMPARE(u_get_allocated_count(), baseline);
        }
#ifdef UTUN_TEST_SETTINGS_ALLOC_FAILURES
        QCOMPARE(outstanding.size(), size_t(0));
#endif
    }
    void windowGeometry_data() {
        QTest::addColumn<int>("outcome"); QTest::addColumn<bool>("payload"); QTest::addColumn<int>("position");
        QTest::newRow("unavailable") << int(Unavailable) << false << 0;
        QTest::newRow("cancelled") << int(Cancelled) << false << 0;
        QTest::newRow("executed") << int(Executed) << false << 0;
        QTest::newRow("window-destroyed-before-execution") << int(Destroyed) << false << 0;
#ifdef UTUN_TEST_SETTINGS_ALLOC_FAILURES
        for (bool payload : {false, true})
            for (int position : {1, 2, 3})
                for (int outcome : {int(Cancelled), int(Executed)})
                    QTest::newRow(qPrintable(QString("oom-%1-%2-%3").arg(payload ? "payload" : "queue").arg(position).arg(outcome)))
                        << outcome << payload << position;
#endif
    }
    void windowGeometry() {
        QFETCH(int, outcome); QFETCH(bool, payload); QFETCH(int, position);
        SettingsCore core; QVERIFY(core.start());
        // Ядро принадлежит fixture, nullptr db отключает создание фонового UtunNode в настоящем окне.
        auto window = std::make_unique<MainWindow>(nullptr, nullptr, QString());
        window->show(); QCoreApplication::processEvents();
        // Окно запрашивает статус «Моих устройств»; завершаем инициализацию до учёта команд сохранения/OOM.
        int startupTasks = 0;
        for (auto* task = core.ua->posted_tasks_head; task; task = task->next) ++startupTasks;
        DEBUG_DEBUG(DEBUG_CATEGORY_CONFIG, "Window geometry setup: processing startup tasks=%d", startupTasks);
        uasync_poll(core.ua, 0); QCoreApplication::processEvents();
        auto* splitter = qobject_cast<QSplitter*>(window->centralWidget());
        QVERIFY(splitter); QCOMPARE(splitter->sizes().size(), 3);
        const QRect geometry = window->normalGeometry();
        QVERIFY(!geometry.isEmpty());
        const auto sizes = splitter->sizes();
        const QStringList keys{"window_geometry", "window_maximized", "window_splitter"};
        const QStringList values{
            QStringLiteral("%1,%2,%3,%4").arg(geometry.width()).arg(geometry.height()).arg(geometry.x()).arg(geometry.y()),
            window->isMaximized() ? QStringLiteral("1") : QStringLiteral("0"),
            QStringLiteral("%1,%2,%3").arg(sizes[0]).arg(sizes[1]).arg(sizes[2])};
        if (outcome == Unavailable) gui_bridge_set_inst(nullptr);
        QVERIFY(!core.ua->posted_tasks_head);
        const auto baseline = u_get_allocated_count();
#ifdef UTUN_TEST_SETTINGS_ALLOC_FAILURES
        watching = true; failMalloc = payload; failAllocation = position;
#endif
        QVERIFY(!window->close()); // closeEvent сохраняет настройки и скрывает окно в tray.
        QVERIFY(!window->isVisible());
        int queued = 0;
        for (auto* task = core.ua->posted_tasks_head; task; task = task->next) ++queued;
        QCOMPARE(queued, outcome == Unavailable ? 0 : position ? 2 : 3);
        window->resize(1100, 700); // В очереди должен остаться снимок исходной геометрии.
        if (outcome == Destroyed) window.reset();
        if (outcome == Executed || outcome == Destroyed) uasync_poll(core.ua, 0);
        else gui_bridge_set_inst(nullptr);
        QVERIFY(!core.ua->posted_tasks_head);
        for (int i = 0; i < keys.size(); ++i) {
            const bool saved = (outcome == Executed || outcome == Destroyed) && i + 1 != position;
            QCOMPARE(core.db.getUiState(keys[i]), saved ? values[i] : QString());
        }
        // poll может расширить пул таймеров uasync; владение командами проверяется отдельно по местам выделения.
        if (outcome == Unavailable || outcome == Cancelled) QCOMPARE(u_get_allocated_count(), baseline);
#ifdef UTUN_TEST_SETTINGS_ALLOC_FAILURES
        QCOMPARE(failAllocation, 0);
        for (const auto& item : outstanding)
            DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "Unfreed UI setting command ptr=%p source=%s", item.first, item.second);
        QCOMPARE(outstanding.size(), size_t(0));
#endif
        DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "Window geometry verified: outcome=%d failed_setting=%d payload=%d queued=%d",
                   outcome, position, payload, queued);
    }
    void pairLimits_data() {
        QTest::addColumn<int>("target"); QTest::addColumn<QByteArray>("key"); QTest::addColumn<QByteArray>("value");
        QTest::addColumn<bool>("accepted");
        for (int target : {int(GUI_SETTING_UI_STATE), int(GUI_SETTING_CHAT)}) {
            const int capacity = target == GUI_SETTING_UI_STATE ? sizeof(save_ui_state_arg::data) : sizeof(chat_setting_arg::data);
            const QByteArray prefix = QByteArray::number(target) + '-';
            QTest::newRow((prefix + "null-key").constData()) << target << QByteArray() << QByteArray("v") << false;
            QTest::newRow((prefix + "empty-key").constData()) << target << QByteArray("") << QByteArray("v") << false;
            QTest::newRow((prefix + "null-value").constData()) << target << QByteArray("k") << QByteArray() << false;
            QTest::newRow((prefix + "empty-value").constData()) << target << QByteArray("k") << QByteArray("") << true;
            QTest::newRow((prefix + "exact-fit").constData()) << target << QByteArray("k") << QByteArray(capacity - 3, 'v') << true;
            QTest::newRow((prefix + "long-value").constData()) << target << QByteArray("k") << QByteArray(capacity - 2, 'v') << false;
            QTest::newRow((prefix + "long-key").constData()) << target << QByteArray(capacity, 'k') << QByteArray("") << false;
        }
    }
    void pairLimits() {
        QFETCH(int, target); QFETCH(QByteArray, key); QFETCH(QByteArray, value); QFETCH(bool, accepted);
        SettingsCore core; QVERIFY(core.start());
        const auto baseline = u_get_allocated_count();
        QCOMPARE(gui_bridge_post_setting(static_cast<gui_setting_target>(target), key.isNull() ? nullptr : key.constData(),
                                         value.isNull() ? nullptr : value.constData()), accepted ? 0 : -1);
        gui_bridge_set_inst(nullptr);
        QCOMPARE(u_get_allocated_count(), baseline);
    }
    void nameLimits_data() {
        QTest::addColumn<QByteArray>("name"); QTest::addColumn<bool>("accepted");
        QTest::newRow("null") << QByteArray() << false;
        QTest::newRow("empty") << QByteArray("") << true;
        QTest::newRow("exact-fit") << QByteArray(sizeof(update_my_name_arg::name) - 1, 'n') << true;
        QTest::newRow("too-long") << QByteArray(sizeof(update_my_name_arg::name), 'n') << false;
    }
    void nameLimits() {
        QFETCH(QByteArray, name); QFETCH(bool, accepted);
        SettingsCore core; QVERIFY(core.start());
        const auto baseline = u_get_allocated_count();
        QCOMPARE(gui_bridge_update_name(name.isNull() ? nullptr : name.constData()), accepted ? 0 : -1);
        gui_bridge_set_inst(nullptr);
        QCOMPARE(u_get_allocated_count(), baseline);
    }
    void copiesInputsBeforeReturning() {
        SettingsCore core; QVERIFY(core.start());
#ifdef UTUN_TEST_SETTINGS_ALLOC_FAILURES
        watching = true;
#endif
        QByteArray key("snapshot"), value("before"), name("After"), setting("storage_autoload"), enabled("1");
        QCOMPARE(gui_bridge_post_setting(GUI_SETTING_UI_STATE, key.constData(), value.constData()), 0);
        QCOMPARE(gui_bridge_post_setting(GUI_SETTING_CHAT, setting.constData(), enabled.constData()), 0);
        QCOMPARE(gui_bridge_update_name(name.constData()), 0);
        key.fill('x'); value.fill('x'); name.fill('x'); setting.fill('x'); enabled.fill('0');
        uasync_poll(core.ua, 0);
        QCOMPARE(core.db.getUiState("snapshot"), QString("before"));
        QCOMPARE(chat_setting_get_int(core.inst, "storage_autoload", -1), 1);
        QCOMPARE(QString::fromUtf8(core.inst->name), QString("After"));
#ifdef UTUN_TEST_SETTINGS_ALLOC_FAILURES
        QCOMPARE(outstanding.size(), size_t(0));
#endif
    }
#ifdef UTUN_TEST_SETTINGS_ALLOC_FAILURES
    void allocationFailure_data() {
        QTest::addColumn<int>("kind"); QTest::addColumn<bool>("payload"); QTest::addColumn<int>("position");
        QTest::addColumn<bool>("cancel");
        for (int kind : {Storage, Audio, Dialog}) {
            for (bool payload : {false, true}) {
                const int count = kind == Storage ? 4 : kind == Audio ? 25 : payload ? 41 : 59;
                QList<int> positions{1, 2, count};
                if (count / 2 > 2) positions.append(count / 2);
                for (int position : positions)
                    for (bool cancel : {false, true})
                        QTest::newRow(qPrintable(QString("%1-%2-%3-%4").arg(kind).arg(payload).arg(position).arg(cancel)))
                            << kind << payload << position << cancel;
            }
        }
    }
    void allocationFailure() {
        QFETCH(int, kind); QFETCH(bool, payload); QFETCH(int, position); QFETCH(bool, cancel);
        SettingsCore core; QVERIFY(core.start());
        auto page = createPage(core, kind);
        watching = true; failMalloc = payload; failAllocation = position;
        apply(page.get(), kind);
        QCOMPARE(failAllocation, 0);
        if (cancel) gui_bridge_set_inst(nullptr);
        else uasync_poll(core.ua, 0);
        QCOMPARE(outstanding.size(), size_t(0));
    }
    void nameAllocationFailure_data() {
        QTest::addColumn<bool>("payload");
        QTest::newRow("payload") << true; QTest::newRow("queue") << false;
    }
    void nameAllocationFailure() {
        QFETCH(bool, payload);
        SettingsCore core; QVERIFY(core.start());
        watching = true; failMalloc = payload; failAllocation = 1;
        QCOMPARE(gui_bridge_update_name("After"), -1);
        QCOMPARE(failAllocation, 0);
        QCOMPARE(outstanding.size(), size_t(0));
        QVERIFY(!core.ua->posted_tasks_head);
    }
#endif
};

QTEST_MAIN(SettingsSaveTest)
#include "test_settings_save.moc"

/* Настоящие Qt-кнопки, null audio backend и общий radio_audio; подменяем только вероятность VAD и сетевой выход. */
#include "../src/radiopanel.h"
#include "../src/radiosettingspage.h"
#include "../src/ptt_hotkey_manager.h"
#include <QCheckBox>
#include <QProcess>
#include <QSpinBox>
#include "../src/radio_audio_engine.h"
#include "../src/sound_manager.h"
#include "../src/memberlistmodel.h"
#include <QApplication>
#include <QPushButton>
#include <QLabel>
#include <QMenuBar>
#include <QMenu>
#include <QTemporaryDir>
#include <QFile>
#include <QElapsedTimer>
#include <QTest>
#include <QSignalSpy>
#include <algorithm>
#include <atomic>
extern "C" {
#include "utun_instance.h"
#include "radio/radio_audio.h"
#include "radio/radio.h"
#include "chat/chat_setting.h"
#include "chat/chat_member.h"
#include "config_parser.h"
#include "u_async.h"
#include "debug_config.h"
#include "silero_vad.h"
#include "mem.h"
}
#include <X11/Xlib.h>
#include <X11/keysym.h>

#define REQUIRE(condition) do { if (!(condition)) qFatal("radio panel line %d: %s", __LINE__, #condition); } while (0)
static UTUN_INSTANCE instance{};
static std::atomic<float> probability{0};
static bool detectorAvailable = true;
static bool failCapture;
static int begins, ends, packets, inputs, outputs;

static QString channelKey(int n) { return QStringLiteral("%1").arg(n, 2, 16, QLatin1Char('0')).repeated(32); }
static QList<QPair<uint64_t, int>> subscriptions;
extern "C" uint64_t gui_bridge_radio_route(const char* channel) {
    for (int n = 7; n <= 10; n++) if (channelKey(n) == QString::fromLatin1(channel)) return n;
    return 0;
}
extern "C" UTUN_INSTANCE* gui_bridge_get_inst() { return &instance; }
extern "C" int gui_bridge_self_ed(uint8_t ed[32]) { memcpy(ed, instance.my_ed25519_pubkey, 32); return 0; }
extern "C" int gui_bridge_is_uasync_ready() { return instance.ua != nullptr; }
extern "C" void gui_bridge_post_uasync_fn(void (*fn)(void*), void* arg) {
    if (fn == radio_set_active_trampoline) {
        auto* request = static_cast<radio_set_active_arg*>(arg);
        subscriptions.append({request->group_id, request->on}); u_free(request); return;
    }
    // Стенд проверяет аудио-движок и собственную строку, без чат-сервиса и его БД.
    if (fn == chat_core_request_member_list_trampoline || fn == chat_core_request_member_rtt_trampoline) { u_free(arg); return; }
    uasync_post(instance.ua, fn, arg);
}
extern "C" int gui_bridge_post_uasync_owned_fn(void (*fn)(void*), void* arg) {
    if (!instance.ua) return -1;
    gui_bridge_post_uasync_fn(fn, arg);
    return 0;
}
extern "C" int gui_bridge_post_bound_fn(void (*fn)(void*), void* arg, UTUN_INSTANCE** binding) {
    if (!instance.ua) return -1;
    *binding = &instance;
    return gui_bridge_post_uasync_owned_fn(fn, arg);
}
extern "C" int __wrap_radio_talk_begin(UTUN_INSTANCE*, uint64_t) { ++begins; return 0; }
extern "C" int __wrap_radio_talk_end(UTUN_INSTANCE*, uint64_t) { ++ends; return 0; }
extern "C" int __wrap_radio_talk_send(UTUN_INSTANCE*, uint64_t, const uint8_t*, int) { ++packets; return 0; }
extern "C" silero_vad_t* __wrap_silero_vad_create_default() { return detectorAvailable ? reinterpret_cast<silero_vad_t*>(1) : nullptr; }
extern "C" void __wrap_silero_vad_destroy(silero_vad_t*) {}
extern "C" void __wrap_silero_vad_reset(silero_vad_t*) {}
extern "C" int __wrap_silero_vad_process(silero_vad_t*, const float*, float* result) { *result = probability.load(); return 0; }
extern "C" ma_result __real_ma_context_init(const ma_backend*, ma_uint32, const ma_context_config*, ma_context*);
extern "C" ma_result __real_ma_device_start(ma_device*);
extern "C" ma_result __wrap_ma_context_init(const ma_backend*, ma_uint32, const ma_context_config*, ma_context* context) {
    ma_backend backend = ma_backend_null;
    return __real_ma_context_init(&backend, 1, nullptr, context);
}
extern "C" ma_result __wrap_ma_device_start(ma_device* device) {
    if (device->type == ma_device_type_capture) ++inputs; else ++outputs;
    if (failCapture && device->type == ma_device_type_capture) return MA_ERROR;
    return __real_ma_device_start(device);
}

static void pump(int milliseconds) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < milliseconds) {
        uasync_poll(instance.ua, 10);
        QApplication::processEvents();
    }
}

template<class Predicate> static void await(Predicate ready) {
    QElapsedTimer timer;
    timer.start();
    while (!ready() && timer.elapsed() < 2500) pump(10);
    REQUIRE(ready());
}

/* Единственный TX-writer после остановки устройств: реальный PCM/DSP/FSM, подменён только ответ модели. */
static void test_combined_capture() {
    const auto saved = instance.chat_settings;
    auto feed = [](int value, int channels, int frames) {
        int16_t pcm[1920];
        std::fill_n(pcm, 960 * channels, int16_t(value));
        for (int i = 0; i < frames; ++i) REQUIRE(radio_audio_feed_pcm(99, pcm, 960 * channels) == 0);
        uasync_poll(instance.ua, 0);
    };
    REQUIRE(chat_setting_set(&instance, "radio_level_on_dbfs", "-35") == 0);
    REQUIRE(chat_setting_set(&instance, "radio_level_off_dbfs", "-45") == 0);
    REQUIRE(chat_setting_set(&instance, "radio_auto_hangover_ms", "50") == 0);
    REQUIRE(chat_setting_set(&instance, "radio_vad_threshold", "65") == 0);
    detectorAvailable = true;
    for (int channels : {1, 2}) for (int compressor : {0, 1}) {
        REQUIRE(chat_setting_set(&instance, "radio_compressor_enabled", compressor ? "1" : "0") == 0);
        REQUIRE(radio_audio_start(99, 0) == 0);
        REQUIRE(radio_audio_capture_start(99, channels, RADIO_AUTO_VAD_LEVEL) == 0);
        REQUIRE(radio_audio_auto_mode() == RADIO_AUTO_VAD_LEVEL);
        probability = 0.1f; feed(2000, channels, 10);
        REQUIRE(!radio_audio_transmitting());
        probability = 0.9f; feed(50, channels, 10);
        REQUIRE(!radio_audio_transmitting()); // Компрессор VAD не должен обходить порог исходного уровня.
        REQUIRE(radio_audio_get_input_status().vad_valid);
        feed(2000, channels, 10);
        REQUIRE(radio_audio_transmitting());
        feed(50, channels, 10);
        pump(60); feed(50, channels, 4);
        REQUIRE(!radio_audio_transmitting());
        radio_audio_talk_begin(99); feed(0, channels, 2);
        REQUIRE(radio_audio_transmitting()); // Ручной PTT сохраняет приоритет над обоими условиями.
        radio_audio_talk_end(99); REQUIRE(!radio_audio_transmitting());
        radio_audio_stop(); uasync_poll(instance.ua, 0);
    }
    detectorAvailable = false;
    REQUIRE(radio_audio_start(99, 0) == 0 && radio_audio_capture_start(99, 1, RADIO_AUTO_VAD_LEVEL) == 0);
    REQUIRE(radio_audio_auto_mode() == RADIO_AUTO_OFF);
    probability = 0.9f; feed(4000, 1, 10);
    REQUIRE(!radio_audio_transmitting()); // Отказ VAD не превращает AND в активацию только по уровню.
    radio_audio_stop(); uasync_poll(instance.ua, 0);
    detectorAvailable = true; probability = 0; instance.chat_settings = saved;
    qInfo("radio panel: combined VAD AND same-window raw level, mono/stereo, AGC on/off, hangover and manual override PASS");
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_WARN);
    debug_set_category_level(DEBUG_CATEGORY_RADIO, DEBUG_LEVEL_INFO);
    instance.ua = uasync_create();
    REQUIRE(instance.ua && radio_audio_init(&instance) == 0);
    REQUIRE(chat_setting_set(&instance, "radio_auto_enabled", "0") == 0);
    auto* manager = SoundManager::instance();
    REQUIRE(manager->init());
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    QString configPath = directory.filePath("radio.cfg");
    QFile config(configPath);
    REQUIRE(config.open(QIODevice::WriteOnly));
    REQUIRE(config.write("[chatserver]\nradio_auto_enabled=0\nradio_opus_bitrate=32\n") > 0);
    config.close();
    auto* engine = RadioAudioEngine::instance();
    MemberListModel members;
    members.setChannel(channelKey(7), false);
    chat_member_display self{}; self.ed25519[31] = 1; self.is_self = 1;
    uint8_t member[CHAT_MEMBER_DISPLAY_SIZE]; chat_core_serialize_member(&self, member);
    members.onMemberList(channelKey(7), 1, member, sizeof(member));
    QObject::connect(engine, &RadioAudioEngine::stateChanged, &members, [&] {
        members.setRadioState(engine->groupId(), engine->isActive(), engine->isTransmitting());
    });
    auto selfSpeaking = [&] { return members.index(0, 0).data(MemberListModel::RadioSpeakingRole).toBool(); };
    engine->loadSettings(configPath);
    await([&]() { return engine->settingsReady() && !engine->settingsBusy(); });
    REQUIRE(!engine->autoEnabled() && engine->setting("radio_vad_compressor_max_gain_db") == 10);
    RadioPanel panel;
    panel.setMembers(&members);
    panel.resize(580, 50);
    panel.show();
    auto* vad = panel.findChild<QPushButton*>("radioAutoButton");
    auto* ptt = panel.findChild<QPushButton*>("radioPttButton");
    auto* status = panel.findChild<QLabel*>("radioStatus");
    auto* devices = panel.findChild<QMenuBar*>("radioDeviceMenus");
    auto* input = panel.findChild<QAction*>("radioInputButton");
    auto* output = panel.findChild<QAction*>("radioOutputButton");
    REQUIRE(vad && ptt && status && !vad->isEnabled());
    REQUIRE(devices && input && output && input->menu() && output->menu());
    REQUIRE(!input->icon().isNull() && !output->icon().isNull());
    REQUIRE(engine->start(channelKey(7)) == 0);
    await([&]() { return engine->captureAvailable() && ptt->isEnabled(); });
    REQUIRE(!vad->isChecked() && vad->isEnabled());
    QTest::mouseClick(devices, Qt::LeftButton, Qt::NoModifier, devices->actionGeometry(input).center());
    await([&] { return input->menu()->isVisible(); });
    qInfo("radio navigation: input opened");
    QTest::keyClick(input->menu(), Qt::Key_Right); await([&] { return output->menu()->isVisible(); });
    qInfo("radio navigation: Right opened output");
    REQUIRE(!input->menu()->isVisible());
    QTest::keyClick(output->menu(), Qt::Key_Left); await([&] { return input->menu()->isVisible(); });
    qInfo("radio navigation: Left opened input");
    REQUIRE(!output->menu()->isVisible());
    qInfo("radio navigation: hover output input_visible=%d output_visible=%d bar_width=%d", input->menu()->isVisible(),
        output->menu()->isVisible(), devices->width());
    const QPoint hoverPoint = devices->actionGeometry(output).center();
    QMouseEvent hover(QEvent::MouseMove, hoverPoint, devices->mapToGlobal(hoverPoint), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(devices, &hover); await([&] { return output->menu()->isVisible(); });
    REQUIRE(!input->menu()->isVisible());
    QTest::keyClick(output->menu(), Qt::Key_Escape);
    REQUIRE(!input->menu()->isVisible() && !output->menu()->isVisible());
    qInfo("radio panel: native menu navigation by arrows, hover and Escape PASS");
    REQUIRE(status->text().isEmpty() && status->toolTip().isEmpty());
    const auto silentOutput = output->icon().pixmap(24).toImage();
    chat_member_display guest{}; guest.ed25519[31] = 2; guest.online = 1; snprintf(guest.name, sizeof(guest.name), "<b>Alice");
    chat_core_serialize_member(&guest, member); members.onMemberUpdated(channelKey(7), member);
    guest.ed25519[31] = 3; guest.flags = CHAT_MEMBER_FLAG_RADIO_MUTED; snprintf(guest.name, sizeof(guest.name), "Bob&");
    chat_core_serialize_member(&guest, member); members.onMemberUpdated(channelKey(7), member);
    members.onRadioTalk(channelKey(7), QByteArray(31, '\0') + char(2), true); members.onRadioTalk(channelKey(7), QByteArray(31, '\0') + char(3), true);
    REQUIRE(status->text().contains("&lt;b&gt;Alice") && status->text().contains("Bob&amp;"));
    REQUIRE(status->text().contains("#4CAF50") && status->text().contains("#EF5350"));
    REQUIRE(!status->text().contains("<b>Alice") && !status->toolTip().contains("<b>Alice"));
    const auto speakingOutput = output->icon().pixmap(24).toImage(); REQUIRE(speakingOutput != silentOutput);
    bool green = false;
    for (int y = 0; y < speakingOutput.height(); ++y) for (int x = 0; x < speakingOutput.width(); ++x) {
        const QColor color = speakingOutput.pixelColor(x, y);
        green = green || (color.alpha() > 200 && color.green() > color.red() + 50);
    }
    REQUIRE(green && devices->actionGeometry(input).right() < devices->actionGeometry(output).left());
    REQUIRE(devices->geometry().right() < status->geometry().left());
    REQUIRE(panel.grab().save("/tmp/desktop-radio-speakers.png"));
    members.onRadioTalk(channelKey(7), QByteArray(31, '\0') + char(2), false); REQUIRE(!status->text().contains("Alice") && status->text().contains("Bob&amp;"));
    REQUIRE(output->icon().pixmap(24).toImage() == speakingOutput); // Динамик остаётся активным при речи приглушённого участника.
    guest.flags = 0; chat_core_serialize_member(&guest, member); members.onMemberUpdated(channelKey(7), member);
    REQUIRE(!status->text().contains("#EF5350") && status->text().contains("Bob&amp;"));
    members.onMemberRemoved(channelKey(7), QByteArray(31, '\0') + char(3)); members.onMemberRemoved(channelKey(7), QByteArray(31, '\0') + char(2));
    REQUIRE(status->text().isEmpty() && output->icon().pixmap(24).toImage() == silentOutput);
    qInfo("radio panel: icon device menus, multiple speakers, mute colors, rename-safe HTML and stop/remove PASS");
    REQUIRE(vad->geometry().right() < ptt->geometry().left());
    int oldOutputs = outputs, oldInputs = inputs;
    vad->click();
    REQUIRE(engine->settingsBusy() && !vad->isEnabled());
    await([&]() { return !engine->settingsBusy() && engine->autoAvailable(); });
    REQUIRE(vad->isChecked() && outputs == oldOutputs && inputs == oldInputs + 1);
    // Удержание открывает редактор без переключения автоматики и перезапуска захвата.
    oldInputs = inputs;
    QTest::mousePress(vad, Qt::LeftButton); pump(550);
    auto* popup = panel.findChild<QMenu*>("radioActivationPopup");
    REQUIRE(popup && popup->isVisible() && engine->autoEnabled() && inputs == oldInputs);
    QTest::mouseRelease(vad, Qt::LeftButton);
    REQUIRE(popup->isVisible() && engine->autoEnabled() && !engine->settingsBusy());
    auto* quickVad = popup->findChild<QPushButton*>("radioModeVad");
    auto* quickLevel = popup->findChild<QPushButton*>("radioModeLevel");
    auto* quickThreshold = popup->findChild<QSpinBox*>("radioVadThreshold");
    auto* quickOn = popup->findChild<QSpinBox*>("radioLevelOn");
    auto* quickOff = popup->findChild<QSpinBox*>("radioLevelOff");
    auto* vadMeter = popup->findChild<QWidget*>("radioVadMeter");
    auto* levelMeter = popup->findChild<QWidget*>("radioLevelMeter");
    auto* quickProbability = popup->findChild<QLabel*>("radioVadText");
    auto* quickGain = popup->findChild<QLabel*>("radioVadGainText");
    REQUIRE(quickVad && quickLevel && quickThreshold && quickOn && quickOff && vadMeter && levelMeter && quickProbability && quickGain);
    REQUIRE(!popup->findChild<QPushButton*>("radioActivationApply"));
    quickLevel->click(); await([&] { return !engine->settingsBusy() && engine->autoMode() == 2; });
    REQUIRE(quickOn->isEnabled() && quickThreshold->isEnabled());
    quickVad->click(); quickThreshold->setValue(60);
    await([&] { return !engine->settingsBusy() && engine->autoMode() == 1 && engine->setting("radio_vad_threshold") == 60; });
    REQUIRE(engine->autoEnabled());
    auto markerPoint = [](QWidget* meter, int value, bool vad) {
        return QPoint(1 + qRound(float(value + (vad ? 0 : 90)) / (vad ? 100 : 90) * (meter->width() - 2)), 8);
    };
    auto moveMarker = [&](QWidget* meter, int value, bool vad) {
        const QPoint point = markerPoint(meter, value, vad);
        QMouseEvent move(QEvent::MouseMove, point, meter->mapToGlobal(point), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(meter, &move);
    };
    quickThreshold->setValue(61); QTest::qWait(150); // Ядро ещё не опрашивается: сохранение намеренно задержано.
    REQUIRE(engine->settingsBusy() && quickThreshold->isEnabled());
    QTest::mousePress(vadMeter, Qt::LeftButton, Qt::NoModifier, markerPoint(vadMeter, 61, true));
    moveMarker(vadMeter, 75, true); REQUIRE(qAbs(quickThreshold->value() - 75) <= 1);
    QTest::mouseRelease(vadMeter, Qt::LeftButton, Qt::NoModifier, markerPoint(vadMeter, 75, true));
    const int draggedVad = quickThreshold->value();
    await([&] { return !engine->settingsBusy() && engine->setting("radio_vad_threshold") == draggedVad; });
    REQUIRE(popup->isVisible());
    await([&] { return engine->inputLevelValid() && engine->vadValid(); });
    oldInputs = inputs; oldOutputs = outputs;
    int unavailableDuringDrag = 0;
    auto levelUpdates = QObject::connect(engine, &RadioAudioEngine::inputLevelChanged, &panel, [&] {
        if (!engine->inputLevelValid() || !engine->vadValid()) ++unavailableDuringDrag;
    });
    QTest::mousePress(vadMeter, Qt::LeftButton, Qt::NoModifier, markerPoint(vadMeter, draggedVad, true));
    for (int threshold : {70, 65}) {
        moveMarker(vadMeter, threshold, true); pump(200);
        REQUIRE(qAbs(quickThreshold->value() - threshold) <= 1 && engine->setting("radio_vad_threshold") == draggedVad);
        REQUIRE(!engine->settingsBusy() && inputs == oldInputs && outputs == oldOutputs && unavailableDuringDrag == 0);
    }
    QObject::disconnect(levelUpdates);
    QTest::mouseRelease(vadMeter, Qt::LeftButton, Qt::NoModifier, markerPoint(vadMeter, 65, true));
    await([&] { return !engine->settingsBusy() && engine->setting("radio_vad_threshold") == quickThreshold->value(); });
    REQUIRE(inputs == oldInputs + 1 && outputs == oldOutputs);
    pump(200); REQUIRE(inputs == oldInputs + 1);
    qInfo("radio panel: slow VAD drag keeps microphone/VAD measurements valid, one capture restart on release PASS");
    oldInputs = inputs;
    QTest::mousePress(levelMeter, Qt::LeftButton, Qt::NoModifier, markerPoint(levelMeter, quickOn->value(), false));
    moveMarker(levelMeter, -25, false); REQUIRE(qAbs(quickOn->value() + 25) <= 1);
    moveMarker(levelMeter, -90, false); REQUIRE(quickOn->value() == quickOff->value() + 1);
    pump(200); REQUIRE(inputs == oldInputs && engine->inputLevelValid() && engine->setting("radio_level_on_dbfs") == -35);
    QTest::mouseRelease(levelMeter, Qt::LeftButton, Qt::NoModifier, markerPoint(levelMeter, -25, false));
    QTest::mousePress(levelMeter, Qt::LeftButton, Qt::NoModifier, markerPoint(levelMeter, quickOff->value(), false));
    moveMarker(levelMeter, -60, false); REQUIRE(qAbs(quickOff->value() + 60) <= 1);
    moveMarker(levelMeter, 0, false); REQUIRE(quickOff->value() == quickOn->value() - 1);
    QTest::mouseRelease(levelMeter, Qt::LeftButton, Qt::NoModifier, markerPoint(levelMeter, -60, false));
    await([&] { return !engine->settingsBusy() && engine->setting("radio_level_on_dbfs") == quickOn->value() &&
        engine->setting("radio_level_off_dbfs") == quickOff->value(); });
    REQUIRE(inputs == oldInputs + 1);
    quickOn->setValue(-35); quickOff->setValue(-45); quickThreshold->setValue(60);
    await([&] { return !engine->settingsBusy() && engine->setting("radio_vad_threshold") == 60 && engine->vadValid(); });
    probability = 0.25f;
    await([&] { return quickProbability->text().contains("25.0%") && quickGain->text().contains("dB"); });
    REQUIRE(engine->vadCompressorEnabled() && engine->vadGain() > 0);
    REQUIRE(popup->grab().save("/tmp/desktop-radio-activation-popup.png"));
    quickThreshold->setValue(70);
    QTest::keyClick(popup, Qt::Key_Escape); REQUIRE(!popup->isVisible());
    await([&] { return !engine->settingsBusy() && engine->setting("radio_vad_threshold") == 70; });
    QMetaObject::invokeMethod(vad, "customContextMenuRequested", Q_ARG(QPoint, QPoint()));
    REQUIRE(popup->isVisible() && quickThreshold->value() == 70);
    popup->hide(); probability = 0;
    REQUIRE(config.open(QIODevice::ReadOnly));
    REQUIRE(config.readAll().contains("radio_vad_threshold=70")); config.close();
    qInfo("radio panel: immediate settings, mouse drag both meters, pending edits during save, close persistence and live values PASS");
    pump(750); // Полное кольцо prefetch до начала речи.
    int previousPackets = packets;
    probability = 0.9f;
    await([&]() { return engine->isTransmitting() && begins == 1 && packets >= previousPackets + 30; });
    REQUIRE(!engine->isTalking() && ptt->property("transmitting").toBool() && begins == 1);
    REQUIRE(selfSpeaking()); // VAD включает собственный динамик без нажатой PTT.
    REQUIRE(panel.grab().save("/tmp/desktop-radio-vad-meter.png"));
    REQUIRE(packets >= previousPackets + 30);
    int beforeManual = begins;
    QTest::mousePress(ptt, Qt::LeftButton);
    REQUIRE(engine->isTalking());
    engine->setHotkeyTalking(true);
    QTest::mouseRelease(ptt, Qt::LeftButton);
    REQUIRE(engine->isTalking());
    engine->setHotkeyTalking(false);
    pump(100);
    REQUIRE(!engine->isTalking() && engine->isTransmitting() && begins == beforeManual);
    probability = 0;
    await([&]() { return !engine->isTransmitting() && ends == 1; });
    REQUIRE(!ptt->property("transmitting").toBool() && ends == 1);
    REQUIRE(!selfSpeaking());
    qInfo("radio panel: automatic TX, 600ms prefetch, actual UI state and manual/hotkey handover PASS");

    engine->setHotkeyTalking(true);
    await([&]() { return engine->isTransmitting(); });
    REQUIRE(selfSpeaking());
    oldOutputs = outputs; oldInputs = inputs;
    vad->click();
    await([&]() { return !engine->settingsBusy() && !engine->autoEnabled() && engine->isTransmitting(); });
    REQUIRE(engine->isTalking() && !engine->autoAvailable() && outputs == oldOutputs && inputs == oldInputs + 1);
    engine->setHotkeyTalking(false);
    await([&]() { return !engine->isTransmitting(); });
    REQUIRE(!selfSpeaking());
    // Реальная последовательность double-click: первый click, второе нажатие DoubleClick, release.
    auto latchPtt = [&]() {
        QTest::mouseClick(ptt, Qt::LeftButton);
        QTest::mouseDClick(ptt, Qt::LeftButton);
        QTest::mouseRelease(ptt, Qt::LeftButton);
        REQUIRE(engine->isTalking() && engine->pttLatched());
        REQUIRE(ptt->text().contains(QStringLiteral("\U0001F512")));
        await([&]() { return engine->isTransmitting(); });
    };
    latchPtt();
    engine->setHotkeyTalking(true);
    engine->setHotkeyTalking(false);
    REQUIRE(engine->isTalking()); // Отпускание хоткея не снимает фиксацию кнопки.
    QTest::mousePress(ptt, Qt::LeftButton);
    REQUIRE(!engine->isTalking()); // Следующий click отпускает уже на нажатии.
    QTest::mouseRelease(ptt, Qt::LeftButton);
    await([&]() { return !engine->isTransmitting(); });
    latchPtt();
    engine->setHotkeyTalking(true);
    QTest::mouseClick(ptt, Qt::LeftButton);
    REQUIRE(!engine->isTalking() && !engine->pttLatched()); // Общий unlock прекращает TX даже при ещё зажатом хоткее.
    REQUIRE(ptt->text() == "PTT");
    engine->setHotkeyTalking(false);
    await([&]() { return !engine->isTransmitting(); });
    latchPtt();
    panel.hide(); pump(50);
    REQUIRE(engine->groupId() == channelKey(7) && engine->isTalking() && engine->pttLatched());
    REQUIRE(engine->start(channelKey(7)) == 0 && engine->isTalking() && engine->pttLatched());
    panel.show();
    subscriptions.clear();
    REQUIRE(engine->start(channelKey(8)) == 0);
    REQUIRE(subscriptions.size() == 2 && subscriptions[0] == qMakePair(uint64_t(7), 0) &&
        subscriptions[1] == qMakePair(uint64_t(8), 1));
    await([&]() { return engine->captureAvailable(); });
    REQUIRE(!engine->isTalking() && !engine->isTransmitting());
    REQUIRE(engine->start(channelKey(7)) == 0);
    await([&]() { return engine->captureAvailable(); });
    QTest::mousePress(ptt, Qt::LeftButton);
    REQUIRE(engine->isTalking()); // После смены канала снова работает обычное удержание.
    QTest::mouseRelease(ptt, Qt::LeftButton);
    REQUIRE(!engine->isTalking());
    qInfo("radio panel: shared latch, lock indicator, click release, held hotkey cancellation and channel reset PASS");
    if (app.arguments().contains("--ptt-hotkey")) {
        auto* display = XOpenDisplay(nullptr);
        REQUIRE(display);
        QString code = QString::number(XKeysymToKeycode(display, XK_F8));
        XCloseDisplay(display);
        auto* hotkey = PttHotkeyManager::instance();
        hotkey->init(nullptr);
        hotkey->setHotkey(code);
        REQUIRE(hotkey->hotkey() == code);
        auto key = [&](bool pressed) {
            REQUIRE(QProcess::execute("xdotool", {pressed ? "keydown" : "keyup", "F8"}) == 0);
            pump(30);
        };
        auto lock = [&]() {
            hotkey->setHotkey(code); // Новая пара не связана с предыдущим сценарием.
            key(true); REQUIRE(engine->isTalking());
            key(false); REQUIRE(!engine->isTalking());
            key(true); key(false); REQUIRE(engine->isTalking() && engine->pttLatched());
            REQUIRE(ptt->text().contains(QStringLiteral("\U0001F512")));
            REQUIRE(ptt->toolTip() == "PTT locked. Click to release.");
        };
        key(true); REQUIRE(engine->isTalking());
        key(true); key(false); REQUIRE(!engine->isTalking()); // Повтор keydown не фиксирует.
        lock();
        engine->talkBegin(); engine->talkEnd(); REQUIRE(engine->isTalking());
        key(true); REQUIRE(!engine->isTalking()); // Нажатие снимает фиксацию сразу.
        pump(40); REQUIRE(!engine->isTalking());
        key(false); REQUIRE(!engine->isTalking());
        REQUIRE(!engine->pttLatched() && ptt->text() == "PTT");
        lock();
        {
            RadioPanel anotherPanel; // Новая панель сразу видит существующий общий замок.
            REQUIRE(anotherPanel.findChild<QPushButton*>("radioPttButton")->text().contains(QStringLiteral("\U0001F512")));
        }
        REQUIRE(engine->isTalking() && engine->pttLatched());
        QTest::mousePress(ptt, Qt::LeftButton);
        REQUIRE(!engine->isTalking() && !engine->pttLatched() && ptt->text() == "PTT");
        pump(50); REQUIRE(!engine->isTalking());
        QTest::mouseRelease(ptt, Qt::LeftButton);
        key(true); REQUIRE(engine->isTalking() && !engine->pttLatched()); // Unlock сбрасывает историю клавиатурной пары.
        key(false); REQUIRE(!engine->isTalking());
        hotkey->setHotkey(code);
        key(true); key(false); key(true);
        REQUIRE(engine->isTalking() && engine->pttLatched());
        QTest::mouseClick(ptt, Qt::LeftButton); // Снимаем замок до отпускания второй клавиатурной кнопки.
        pump(50); REQUIRE(!engine->isTalking() && !engine->pttLatched() && ptt->text() == "PTT");
        key(false); REQUIRE(!engine->isTalking());
        lock();
        hotkey->setDoublePressEnabled(false);
        REQUIRE(!engine->isTalking());
        key(true); key(false); key(true); key(false);
        REQUIRE(!engine->isTalking()); // Галочка выключена: только удержание.
        latchPtt();
        REQUIRE(engine->isTalking()); // Кнопочная фиксация доступна независимо от галочки.
        key(true); REQUIRE(!engine->isTalking() && !engine->pttLatched() && ptt->text() == "PTT");
        pump(50); REQUIRE(!engine->isTalking()); // Хоткей снимает мышиный замок даже при выключенной галочке.
        key(false); REQUIRE(!engine->isTalking());
        latchPtt();
        hotkey->setDoublePressEnabled(true);
        latchPtt();
        key(true); REQUIRE(!engine->isTalking() && !engine->pttLatched());
        key(false); REQUIRE(!engine->isTalking());
        lock();
        engine->stop();
        REQUIRE(!engine->isTalking());
        REQUIRE(engine->start(channelKey(7)) == 0);
        await([&] { return engine->captureAvailable(); });
        pump(40); REQUIRE(!engine->isTalking()); // Фиксация не переживает stop/start.
        key(true); REQUIRE(engine->isTalking());
        engine->stop();
        REQUIRE(engine->start(channelKey(7)) == 0);
        await([&] { return engine->captureAvailable(); });
        pump(40); REQUIRE(!engine->isTalking()); // Удержание при stop требует нового нажатия.
        key(false); key(true); REQUIRE(engine->isTalking());
        key(false); REQUIRE(!engine->isTalking());
        hotkey->setHotkey(QString());
        qInfo("radio hotkey: X11 polling, shared lock, mouse/key unlock, lock indicator, checkbox and restart PASS");
    }
    REQUIRE(engine->applySettings({{"radio_vad_threshold", 65}, {"radio_auto_hangover_ms", 400},
                                  {"radio_vad_compressor_max_gain_db", 30}, {"radio_compressor_max_gain_db", 6}}));
    await([&]() { return !engine->settingsBusy(); });
    REQUIRE(engine->setting("radio_vad_compressor_max_gain_db") == 30 && engine->setting("radio_compressor_max_gain_db") == 6);
    REQUIRE(!engine->applySettings({{"radio_vad_compressor_max_gain_db", 61}}));
    REQUIRE(engine->setting("radio_vad_compressor_max_gain_db") == 30);

    detectorAvailable = false;
    vad->click();
    await([&]() { return !engine->settingsBusy(); });
    REQUIRE(vad->isChecked() && engine->captureAvailable() && !engine->autoAvailable() && status->text() == "Automatic transmission unavailable");
    QTest::mousePress(ptt, Qt::LeftButton);
    await([&]() { return engine->isTransmitting(); });
    REQUIRE(selfSpeaking());
    QTest::mouseRelease(ptt, Qt::LeftButton);
    await([&]() { return !engine->isTransmitting(); });
    REQUIRE(!selfSpeaking());
    qInfo("radio panel: capture-only restart preserves output/manual owner; VAD failure leaves manual PTT working PASS");

    RadioSettingsPage settings(nullptr);
    settings.resize(620, 780); settings.show();
    REQUIRE(!settings.findChild<RadioActivationSettings*>() && !settings.findChild<QSpinBox*>("radioVadThreshold") &&
        !settings.findChild<QSpinBox*>("radioLevelOn") && !settings.findChild<QSpinBox*>("radioHangover"));
    auto* doublePress = settings.findChild<QCheckBox*>("radioPttDoublePress");
    auto* radioAec = settings.findChild<QCheckBox*>("radioAecEnabled");
    REQUIRE(radioAec && radioAec->isChecked());
    REQUIRE(settings.findChildren<QPushButton*>("radioPttShortcut").size() == 1);
    REQUIRE(doublePress && doublePress->isChecked() && settings.doublePressEnabled() && settings.hotkey().isEmpty());
    doublePress->setChecked(false);
    radioAec->setChecked(false);
    REQUIRE(settings.apply() && !settings.doublePressEnabled());
    await([&] { return !engine->settingsBusy(); });
    REQUIRE(chat_setting_get_int(&instance, "radio_aec_enabled", -1) == 0 && chat_setting_get_int(&instance, "aec_enabled", 1) == 1);
    REQUIRE(config.open(QIODevice::ReadOnly));
    REQUIRE(config.readAll().contains("radio_aec_enabled=0")); config.close();
    latchPtt();
    QTest::mouseClick(ptt, Qt::LeftButton);
    REQUIRE(!engine->isTalking()); // Применённая галочка не меняет двойной клик кнопки.
    doublePress->setChecked(true);
    radioAec->setChecked(true);
    REQUIRE(settings.apply() && settings.doublePressEnabled());
    await([&] { return !engine->settingsBusy(); });
    auto* burst = settings.findChild<QSpinBox*>("radioBurstFrames");
    REQUIRE(burst && burst->minimum() == 1 && burst->maximum() == 10 && burst->value() == 4);
    oldInputs = inputs; oldOutputs = outputs;
    burst->setValue(10); REQUIRE(settings.apply());
    await([&]() { return !engine->settingsBusy(); });
    REQUIRE(engine->setting("radio_burst_frames") == 10 && inputs == oldInputs && outputs == oldOutputs);
    RadioActivationSettings activation;
    activation.resize(420, 640); activation.show();
    auto* mode = activation.findChild<QPushButton*>("radioModeLevel");
    auto* on = activation.findChild<QSpinBox*>("radioLevelOn");
    auto* off = activation.findChild<QSpinBox*>("radioLevelOff");
    auto* levelText = activation.findChild<QLabel*>("radioLevelText");
    REQUIRE(mode && on && off && levelText);
    mode->click(); on->setValue(-50); off->setValue(-40);
    pump(200);
    REQUIRE(!engine->settingsBusy() && engine->autoMode() == 1 &&
        !activation.findChild<QLabel*>("radioActivationError")->text().isEmpty());
    off->setValue(-60);
    await([&]() { return !engine->settingsBusy() && engine->autoMode() == 2 && engine->autoAvailable() && engine->inputLevelValid(); });
    REQUIRE(engine->autoMode() == 2 && !vad->icon().isNull() && vad->text().isEmpty() && radio_audio_auto_mode() == RADIO_AUTO_LEVEL);
    REQUIRE(activation.findChild<QSpinBox*>("radioVadThreshold")->isEnabled() && on->isEnabled());
    REQUIRE(!engine->isTransmitting() && status->text() == "Waiting for signal");
    REQUIRE(levelText->text().contains("dBFS") && engine->inputLevel() == -120);
    // Silero недоступен; уровень работает и сохраняется вместе с парой порогов.
    REQUIRE(config.open(QIODevice::ReadOnly));
    QByteArray levelConfig = config.readAll(); config.close();
    REQUIRE(levelConfig.contains("radio_auto_mode=2") && levelConfig.contains("radio_level_on_dbfs=-50") &&
            levelConfig.contains("radio_level_off_dbfs=-60"));
    auto* parsedLevel = parse_config(configPath.toUtf8().constData());
    REQUIRE(parsedLevel && chat_setting_get_int_state(&parsedLevel->global.chat_settings, "radio_auto_mode", 0) == 2 &&
            chat_setting_get_int_state(&parsedLevel->global.chat_settings, "radio_level_on_dbfs", 0) == -50 &&
            chat_setting_get_int_state(&parsedLevel->global.chat_settings, "radio_level_off_dbfs", 0) == -60);
    free_config(parsedLevel);
    REQUIRE(activation.grab().save("/tmp/desktop-radio-level-settings.png"));
    REQUIRE(settings.apply()); await([&] { return !engine->settingsBusy(); });
    REQUIRE(engine->autoMode() == 2 && engine->setting("radio_level_on_dbfs") == -50 && engine->setting("radio_level_off_dbfs") == -60);
    auto* combinedMode = activation.findChild<QPushButton*>("radioModeVadLevel");
    REQUIRE(combinedMode); combinedMode->click();
    await([&] { return !engine->settingsBusy() && engine->autoMode() == RADIO_AUTO_VAD_LEVEL; });
    REQUIRE(engine->autoMode() == RADIO_AUTO_VAD_LEVEL && !engine->autoAvailable() && radio_audio_auto_mode() == RADIO_AUTO_OFF);
    mode->click();
    await([&] { return !engine->settingsBusy() && engine->autoMode() == RADIO_AUTO_LEVEL && engine->autoAvailable(); });
    vad->click();
    await([&]() { return !engine->settingsBusy() && !engine->autoEnabled() && engine->inputLevelValid(); });
    REQUIRE(!engine->autoAvailable() && levelText->text().contains("dBFS"));
    engine->stop();
    REQUIRE(!engine->inputLevelValid() && levelText->text().contains("Enable radio"));
    settings.hide(); activation.hide();
    REQUIRE(engine->applySettings({{"radio_auto_enabled", 1}, {"radio_auto_mode", 1}}));
    await([&]() { return !engine->settingsBusy(); });
    qInfo("radio panel: level mode without Silero, live meter, inactive meter, threshold pair validation and saved settings PASS");

    engine->stop();
    REQUIRE(!engine->isTransmitting() && !engine->captureAvailable() && !vad->isEnabled());
    REQUIRE(!selfSpeaking());
    REQUIRE(config.open(QIODevice::ReadOnly));
    QByteArray saved = config.readAll();
    config.close();
    REQUIRE(saved.contains("radio_auto_enabled=1") && saved.contains("radio_vad_threshold=65") &&
            saved.contains("radio_auto_hangover_ms=400") && saved.contains("radio_vad_compressor_max_gain_db=30") &&
            saved.contains("radio_opus_bitrate=32") && saved.contains("radio_burst_frames=10"));
    // Как при рестарте приложения: runtime-настройки заново читаются парсером из сохранённого INI.
    auto* parsed = parse_config(configPath.toUtf8().constData());
    REQUIRE(parsed);
    instance.chat_settings = parsed->global.chat_settings;
    free_config(parsed);
    engine->loadSettings(configPath);
    await([&]() { return !engine->settingsBusy(); });
    REQUIRE(engine->autoEnabled() && engine->setting("radio_vad_threshold") == 65 && engine->setting("radio_burst_frames") == 10);
    detectorAvailable = true;
    REQUIRE(engine->start(channelKey(8)) == 0);
    await([&]() { return engine->autoAvailable(); });
    REQUIRE(engine->groupId() == channelKey(8) && vad->isChecked());
    engine->stop();
    failCapture = true;
    REQUIRE(engine->start(channelKey(9)) == 0);
    REQUIRE(!engine->captureAvailable() && vad->isEnabled() && !ptt->isEnabled());
    vad->click();
    await([&]() { return !engine->settingsBusy() && !engine->autoEnabled(); });
    REQUIRE(!engine->captureAvailable());
    failCapture = false;
    await([&]() { return engine->captureAvailable(); });
    vad->click();
    await([&]() { return !engine->settingsBusy() && engine->autoAvailable(); });
    REQUIRE(vad->isChecked() && ptt->isEnabled());
    qInfo("radio panel: unavailable microphone permits VAD settings; capture recovery restores controls PASS");
    engine->stop();
    engine->loadSettings(directory.filePath("missing.cfg"));
    await([&]() { return !engine->settingsBusy(); });
    REQUIRE(!engine->applySettings({{"radio_auto_enabled", 0}}) && engine->autoEnabled() && !engine->settingsError().isEmpty());
    for (auto* menu : {input->menu(), output->menu()}) {
        REQUIRE(QMetaObject::invokeMethod(menu, "aboutToShow", Qt::DirectConnection));
        REQUIRE(!menu->actions().isEmpty());
    }
    engine->loadSettings(configPath);
    await([&]() { return !engine->settingsBusy(); });
    REQUIRE(engine->start(channelKey(10)) == 0 && engine->autoAvailable());
    pump(100);
    REQUIRE(panel.grab().save("/tmp/desktop-radio-panel.png"));
    engine->stop();
    manager->shutdown();
    test_combined_capture();
    radio_audio_destroy(&instance);
    uasync_destroy(instance.ua, 0);
    instance.ua = nullptr;
    size_t allocated = u_get_allocated_count();
    instance.ua = uasync_create();
    REQUIRE(instance.ua);
    engine->loadSettings(configPath);
    REQUIRE(engine->settingsBusy() && instance.ua->posted_tasks_head);
    uasync_destroy(instance.ua, 0);
    instance.ua = nullptr;
    REQUIRE(u_get_allocated_count() == allocated);
    qInfo("radio panel: pending settings request freed on core shutdown PASS");

    // Повторная загрузка после отмены старой задачи должна поставить запрос в новое ядро.
    debug_set_category_level(DEBUG_CATEGORY_RADIO, DEBUG_LEVEL_DEBUG);
    instance.ua = uasync_create();
    REQUIRE(instance.ua && chat_setting_set(&instance, "radio_vad_threshold", "75") == 0);
    engine->loadSettings(configPath);
    REQUIRE(engine->settingsBusy() && !engine->settingsReady() && instance.ua->posted_tasks_head);
    await([&] { return engine->settingsReady() && !engine->settingsBusy(); });
    REQUIRE(engine->setting("radio_vad_threshold") == 75);
    uasync_destroy(instance.ua, 0); instance.ua = nullptr;
    REQUIRE(u_get_allocated_count() == allocated);
    qInfo("radio panel: settings reload after cancelled core request PASS");

    // Старый uasync уже закончил чтение, но GUI completion ждёт доставки при запуске нового ядра.
    instance.ua = uasync_create();
    REQUIRE(instance.ua && chat_setting_set(&instance, "radio_vad_threshold", "20") == 0);
    engine->loadSettings(configPath);
    uasync_poll(instance.ua, 0);
    REQUIRE(engine->settingsBusy() && !instance.ua->posted_tasks_head);
    uasync_destroy(instance.ua, 0); instance.ua = nullptr;
    instance.ua = uasync_create();
    REQUIRE(instance.ua && chat_setting_set(&instance, "radio_vad_threshold", "80") == 0);
    engine->loadSettings(configPath);
    REQUIRE(engine->settingsBusy() && !engine->settingsReady() && instance.ua->posted_tasks_head);
    QSignalSpy staleSettings(engine, &RadioAudioEngine::stateChanged);
    QCoreApplication::sendPostedEvents(engine, QEvent::MetaCall);
    REQUIRE(engine->settingsBusy() && !engine->settingsReady() && engine->setting("radio_vad_threshold") == 75);
    REQUIRE(staleSettings.isEmpty() && instance.ua->posted_tasks_head);
    await([&] { return engine->settingsReady() && !engine->settingsBusy(); });
    REQUIRE(engine->setting("radio_vad_threshold") == 80 && staleSettings.size() == 1);
    uasync_destroy(instance.ua, 0); instance.ua = nullptr;
    REQUIRE(u_get_allocated_count() == allocated);
    qInfo("radio panel: stale GUI settings completion cannot unlock or overwrite restarted core PASS");

    // Перечитывание из stateChanged сохраняет порядок: принятая запись должна предшествовать новому чтению.
    instance.ua = uasync_create();
    REQUIRE(instance.ua && chat_setting_set(&instance, "radio_vad_threshold", "85") == 0);
    engine->loadSettings(configPath);
    await([&] { return engine->settingsReady() && !engine->settingsBusy(); });
    REQUIRE(engine->setting("radio_vad_threshold") == 85);
    bool reloaded = false;
    auto reload = QObject::connect(engine, &RadioAudioEngine::stateChanged, engine, [&] {
        if (reloaded || !engine->settingsBusy()) return;
        reloaded = true;
        engine->loadSettings(configPath);
    });
    REQUIRE(engine->applySettings({{"radio_vad_threshold", 55}}) && reloaded);
    QObject::disconnect(reload);
    QSignalSpy appliedSettings(engine, &RadioAudioEngine::stateChanged);
    await([&] { return engine->settingsReady() && !engine->settingsBusy(); });
    REQUIRE(engine->setting("radio_vad_threshold") == 55 && appliedSettings.size() == 1);
    uasync_destroy(instance.ua, 0); instance.ua = nullptr;
    REQUIRE(u_get_allocated_count() == allocated);
    qInfo("radio panel: settings reload from stateChanged preserves accepted apply order PASS");
    qInfo("radio panel: saved settings, channel change, device menus, range validation and config failure PASS");
    return 0;
}

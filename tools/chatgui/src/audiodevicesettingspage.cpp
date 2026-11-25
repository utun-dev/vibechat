#include <QCoreApplication>
#include "audiodevicesettingspage.h"
#include "settings_layout.h"
#include "audio_device.h"
#include <QHideEvent>
#include "sound_manager.h"
#include "audiorecorder.h"
#include "voicemessageencoder.h"

extern "C" {
#include "../../lib/audio_compressor.h"
}
#include "../../lib/debug_config.h"
#include "../db/db_manager.h"
#include "../transport/gui_bridge.h"
#include "../transport/config_updater.h"
extern "C" {
#include "call/call_video_rate.h"
}
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QFormLayout>
#include <QFrame>
#include <QDebug>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QMessageBox>
#include <cmath>

AudioDeviceSettingsPage::AudioDeviceSettingsPage(DbManager* db, AudioRecorder* recorder, QWidget* parent)
    : QWidget(parent), m_db(db), m_recorder(recorder)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 12, 0, 0);

    layout->setSpacing(14);
    settingsHeading(layout, QCoreApplication::translate("ChatGui", "Devices"));
    auto* formLayout = new QFormLayout();
    formLayout->setContentsMargins(0, 0, 0, 0);
    formLayout->setSpacing(10);

    auto* outLabel = new QLabel(QCoreApplication::translate("ChatGui", "Playback device:"), this);
    m_outputCombo = new QComboBox(this);
    m_outputCombo->setStyleSheet(
        "QComboBox { border: 1px solid palette(mid); border-radius: 4px;"
        " padding: 6px 10px; font-size: 13px; background: palette(base); }");
    formLayout->addRow(outLabel, m_outputCombo);

    auto* inLabel = new QLabel(QCoreApplication::translate("ChatGui", "Recording device:"), this);
    m_inputCombo = new QComboBox(this);
    m_inputCombo->setStyleSheet(m_outputCombo->styleSheet());
    formLayout->addRow(inLabel, m_inputCombo);

    layout->addLayout(formLayout);

    /* --- Microphone test --- */
    settingsHeading(layout, QCoreApplication::translate("ChatGui", "Device checks"));
    auto* micRow = new QHBoxLayout();
    micRow->setSpacing(8);
    m_micLevelBar = new QProgressBar(this);
    m_micLevelBar->setRange(0, 100);
    m_micLevelBar->setValue(0);
    m_micLevelBar->setTextVisible(false);
    m_micLevelBar->setFixedHeight(16);
    micRow->addWidget(m_micLevelBar, 1);

    m_testMicBtn = new QPushButton(QCoreApplication::translate("ChatGui", "Test Microphone"), this);
    m_testMicBtn->setCheckable(true);
    m_testMicBtn->setStyleSheet(
        "QPushButton { border: 1px solid palette(mid); border-radius: 4px;"
        " padding: 4px 12px; font-size: 12px; background: palette(button); }"
        "QPushButton:checked { background: #c0392b; color: white; }");
    connect(m_testMicBtn, &QPushButton::toggled, this, &AudioDeviceSettingsPage::onTestMicToggled);
    micRow->addWidget(m_testMicBtn);
    layout->addLayout(micRow);

    /* --- Speaker test --- */
    m_testSpeakerBtn = new QPushButton(QCoreApplication::translate("ChatGui", "Test Speaker (440Hz)"), this);
    m_testSpeakerBtn->setStyleSheet(
        "QPushButton { border: 1px solid palette(mid); border-radius: 4px;"
        " padding: 6px 16px; font-size: 13px; background: palette(button); }"
        "QPushButton:hover { background: palette(light); }");
    connect(m_testSpeakerBtn, &QPushButton::clicked, this, &AudioDeviceSettingsPage::onTestSpeaker);
    layout->addWidget(m_testSpeakerBtn, 0, Qt::AlignLeft);

    settingsHeading(layout, QCoreApplication::translate("ChatGui", "Voice processing"));
    buildCompressorUI(layout);
    /* --- Codec quality --- */
    auto* codecLabel = new QLabel(QCoreApplication::translate("ChatGui", "Voice codec quality"), this);
    codecLabel->setStyleSheet("font-size: 13px; font-weight: bold;");
    layout->addWidget(codecLabel);

    m_codecPresetCombo = new QComboBox(this);
    m_codecPresetCombo->addItem(QCoreApplication::translate("ChatGui", "Low (16 kbps, fast)"), 0);
    m_codecPresetCombo->addItem(QCoreApplication::translate("ChatGui", "Standard (32 kbps)"), 1);
    m_codecPresetCombo->addItem(QCoreApplication::translate("ChatGui", "High (64 kbps, best)"), 2);
    m_codecPresetCombo->setStyleSheet(m_outputCombo->styleSheet());
    layout->addWidget(m_codecPresetCombo);

    layout->addStretch();

    m_radioCodecPage = new QWidget(this);
    m_radioCodecPage->setObjectName("radioQualitySettings"); m_radioCodecPage->hide();
    auto* radioLayout = new QVBoxLayout(m_radioCodecPage);
    radioLayout->setContentsMargins(0, 0, 0, 0); radioLayout->setSpacing(12);
    /* --- Radio (PTT) Opus --- */
    auto* radioLabel = new QLabel(QCoreApplication::translate("ChatGui", "Radio (PTT) — Opus"), this);
    radioLabel->setStyleSheet("font-size: 13px; font-weight: bold;");
    radioLayout->addWidget(radioLabel);

    auto* radioForm = new QFormLayout();
    radioForm->setContentsMargins(0, 0, 0, 0);
    radioForm->setSpacing(10);

    m_radioOpusModeCombo = new QComboBox(this);
    m_radioOpusModeCombo->setObjectName("radioOpusMode");
    m_radioOpusModeCombo->addItem(QCoreApplication::translate("ChatGui", "Voice (VOIP)"), 0);
    m_radioOpusModeCombo->addItem(QCoreApplication::translate("ChatGui", "Music (AUDIO)"), 1);
    m_radioOpusModeCombo->setStyleSheet(m_outputCombo->styleSheet());
    radioForm->addRow(new QLabel(QCoreApplication::translate("ChatGui", "Mode:"), this), m_radioOpusModeCombo);

    m_radioOpusBitrateCombo = new QComboBox(this);
    m_radioOpusBitrateCombo->setObjectName("radioOpusBitrate");
    for (int br : {16, 32, 64, 96, 128})
        m_radioOpusBitrateCombo->addItem(QString::number(br) + QCoreApplication::translate("ChatGui", " kbps"), br);
    m_radioOpusBitrateCombo->setStyleSheet(m_outputCombo->styleSheet());
    radioForm->addRow(new QLabel(QCoreApplication::translate("ChatGui", "Bitrate:"), this), m_radioOpusBitrateCombo);

    radioLayout->addLayout(radioForm);

    m_radioCaptureStereoCb = new QCheckBox(QCoreApplication::translate("ChatGui", "Capture stereo (playback is always stereo)"), this);
    m_radioCaptureStereoCb->setStyleSheet("font-size: 12px;");
    radioLayout->addWidget(m_radioCaptureStereoCb);


    m_callPage = new QWidget(this);
    m_callPage->setObjectName("callMediaSettings"); m_callPage->hide();
    auto* callLayout = new QVBoxLayout(m_callPage);
    callLayout->setContentsMargins(0, 0, 0, 0); callLayout->setSpacing(12);
    settingsHeading(callLayout, QCoreApplication::translate("ChatGui", "Call audio"));
    m_aecEnabledCb = new QCheckBox(QCoreApplication::translate("ChatGui", "Enable echo cancellation (AEC) in calls"), m_callPage);
    callLayout->addWidget(m_aecEnabledCb);
    settingsHeading(callLayout, QCoreApplication::translate("ChatGui", "Video quality"));
    auto* callForm = new QFormLayout();
    m_jitterReserveSpin = new QDoubleSpinBox(this);
    m_jitterReserveSpin->setRange(0.5, 2.0);
    m_jitterReserveSpin->setDecimals(1);
    m_jitterReserveSpin->setSingleStep(0.1);
    m_jitterReserveSpin->setValue(1.0);
    m_jitterReserveSpin->setSuffix(QCoreApplication::translate("ChatGui", " s"));
    m_jitterReserveSpin->setToolTip(QCoreApplication::translate("ChatGui", "Maximum adaptive jitter reserve for calls. Applies to the next call."));

    m_videoMinimumSpin = new QSpinBox(this);
    m_videoMaximumSpin = new QSpinBox(this);
    m_videoMinimumSpin->setObjectName("callVideoMinimumBitrate"); m_videoMaximumSpin->setObjectName("callVideoMaximumBitrate");
    for (auto* spin : {m_videoMinimumSpin, m_videoMaximumSpin}) {
        spin->setRange(CALL_VIDEO_RATE_MIN_BPS / 1000, CALL_VIDEO_RATE_MAX_BPS / 1000);
        spin->setSingleStep(100);
        spin->setSuffix(QCoreApplication::translate("ChatGui", " kbit/s"));
    }
    m_videoMinimumSpin->setValue(CALL_VIDEO_RATE_MIN_BPS / 1000);
    m_videoMaximumSpin->setValue(CALL_VIDEO_RATE_MAX_BPS / 1000);
    connect(m_videoMinimumSpin, QOverload<int>::of(&QSpinBox::valueChanged), m_videoMaximumSpin, &QSpinBox::setMinimum);
    connect(m_videoMaximumSpin, QOverload<int>::of(&QSpinBox::valueChanged), m_videoMinimumSpin, &QSpinBox::setMaximum);
    callForm->addRow(QCoreApplication::translate("ChatGui", "Call video: minimum bitrate"), m_videoMinimumSpin);
    callForm->addRow(QCoreApplication::translate("ChatGui", "Call video: maximum bitrate"), m_videoMaximumSpin);
    callLayout->addLayout(callForm);
    auto* videoHint = new QLabel(QCoreApplication::translate("ChatGui",
        "Video bitrate adapts to the connection within this range. Default: 300–4000 kbit/s."), this);
    videoHint->setWordWrap(true);
    callLayout->addWidget(videoHint);
    auto* delayDetails = settingsDetails(callLayout, QCoreApplication::translate("ChatGui", "Connection buffering"));
    auto* delayForm = new QFormLayout;
    delayForm->addRow(QCoreApplication::translate("ChatGui", "Call: maximum jitter reserve"), m_jitterReserveSpin);
    delayDetails->addLayout(delayForm);

    connect(SoundManager::instance(), &SoundManager::contextAboutToReset, this, &AudioDeviceSettingsPage::stopSpeakerTest);
    m_speakerTimer = new QTimer(this); m_speakerTimer->setSingleShot(true);
    connect(m_speakerTimer, &QTimer::timeout, this, &AudioDeviceSettingsPage::stopSpeakerTest);

    m_levelTimer = new QTimer(this);
    m_levelTimer->setInterval(25);
    connect(m_levelTimer, &QTimer::timeout, this, &AudioDeviceSettingsPage::onLevelTimer);
}

AudioDeviceSettingsPage::~AudioDeviceSettingsPage() {
    stopMicTest();
    stopSpeakerTest();
}

void AudioDeviceSettingsPage::loadFromDb() {
    auto playbackDevs = SoundManager::enumeratePlaybackDevices();
    auto captureDevs = SoundManager::enumerateCaptureDevices();

    /* Output devices */
    m_outputCombo->clear();
    m_outputCombo->addItem(QCoreApplication::translate("ChatGui", "Default (auto)"), QString());
    for (int i = 0; i < playbackDevs.size(); i++) {
        QString label = playbackDevs[i].name;
        if (playbackDevs[i].isDefault) label += QCoreApplication::translate("ChatGui", " (default)");
        m_outputCombo->addItem(label, playbackDevs[i].id);
    }

    /* Input devices */
    m_inputCombo->clear();
    m_inputCombo->addItem(QCoreApplication::translate("ChatGui", "Default (auto)"), QString());
    for (int i = 0; i < captureDevs.size(); i++) {
        QString label = captureDevs[i].name;
        if (captureDevs[i].isDefault) label += QCoreApplication::translate("ChatGui", " (default)");
        m_inputCombo->addItem(label, captureDevs[i].id);
    }

    if (m_db) {
        m_savedOutputId = m_db->getUiState("audio_output_id");
        m_savedInputId  = m_db->getUiState("audio_input_id");
        m_savedCodecPreset = m_db->getUiStateInt("opus_codec_preset", 1);
        m_savedRadioOpusMode = m_db->getUiStateInt("radio_opus_mode", 0);
        m_savedRadioOpusBitrate = m_db->getUiStateInt("radio_opus_bitrate", 32);
        m_savedRadioCaptureStereo = m_db->getUiStateInt("radio_capture_stereo", 0);
        m_savedCompressorEnabled = m_db->getUiStateInt("compressor_enabled", 0);
        m_savedCompressorMaxGainDb = m_db->getUiStateInt("compressor_max_gain_db", 20);
        m_savedCompressorLookbackMs = m_db->getUiStateInt("compressor_lookback_ms", 200);
        m_savedCompressorLookaheadMs = m_db->getUiStateInt("compressor_lookahead_ms", 100);
        m_savedCompressorRiseRate = m_db->getUiStateInt("compressor_rise_rate", 10);
        m_savedCompressorTargetLevelDb = m_db->getUiStateInt("compressor_target_level_db", 0);
        m_savedCompressorSpeechOnly = m_db->getUiStateInt("compressor_gain_only_on_speech", 0);
        m_savedCompressorVadGain = m_db->getUiStateInt("radio_vad_compressor_max_gain_db", audio_compressor_shared_vad_max_gain());
        m_jitterReserveSpin->setValue(m_db->getUiStateInt("call_jitter_max_reserve_ms", 1000) / 1000.0);
        QByteArray range = m_db->getUiState("call_video_bitrate_range_kbps").toUtf8();
        uint32_t minimum = CALL_VIDEO_RATE_MIN_BPS, maximum = CALL_VIDEO_RATE_MAX_BPS;
        if (!range.isEmpty()) call_video_rate_parse_range(range.constData(), &minimum, &maximum);
        m_videoMinimumSpin->setValue(CALL_VIDEO_RATE_MIN_BPS / 1000);
        m_videoMaximumSpin->setValue(maximum / 1000);
        m_videoMinimumSpin->setValue(minimum / 1000);
        m_savedAecEnabled = m_db->getUiStateInt("aec_enabled", 1);
        m_savedNoiseSuppressionEnabled = m_db->getUiStateInt("noise_suppression_enabled", 1);
        m_savedNoiseSuppressionDb = m_db->getUiStateInt("noise_suppression_db", SPEEX_NS_DEFAULT_DB);
    }

    /* Select saved indices */
    for (int i = 0; i < m_outputCombo->count(); i++) {
        if (m_outputCombo->itemData(i).toString() == m_savedOutputId)
            { m_outputCombo->setCurrentIndex(i); break; }
    }
    for (int i = 0; i < m_inputCombo->count(); i++) {
        if (m_inputCombo->itemData(i).toString() == m_savedInputId)
            { m_inputCombo->setCurrentIndex(i); break; }
    }
    for (int i = 0; i < m_codecPresetCombo->count(); i++) {
        if (m_codecPresetCombo->itemData(i).toInt() == m_savedCodecPreset)
            { m_codecPresetCombo->setCurrentIndex(i); break; }
    }
    for (int i = 0; i < m_radioOpusModeCombo->count(); i++) {
        if (m_radioOpusModeCombo->itemData(i).toInt() == m_savedRadioOpusMode)
            { m_radioOpusModeCombo->setCurrentIndex(i); break; }
    }
    for (int i = 0; i < m_radioOpusBitrateCombo->count(); i++) {
        if (m_radioOpusBitrateCombo->itemData(i).toInt() == m_savedRadioOpusBitrate)
            { m_radioOpusBitrateCombo->setCurrentIndex(i); break; }
    }
    m_radioCaptureStereoCb->setChecked(m_savedRadioCaptureStereo != 0);

    m_compressorEnabledCb->setChecked(m_savedCompressorEnabled != 0);
    m_compressorMaxGainSlider->setValue(m_savedCompressorMaxGainDb);
    m_compressorLookbackSlider->setValue(m_savedCompressorLookbackMs);
    m_compressorLookaheadSlider->setValue(m_savedCompressorLookaheadMs);
    m_compressorRiseRateSlider->setValue(m_savedCompressorRiseRate);
    m_compressorTargetLevelSlider->setValue(m_savedCompressorTargetLevelDb);
    m_compressorSpeechOnlyCb->setChecked(m_savedCompressorSpeechOnly != 0);
    m_compressorVadGain->setValue(m_savedCompressorVadGain);
    m_aecEnabledCb->setChecked(m_savedAecEnabled != 0);
    m_noiseSuppressionEnabledCb->setChecked(m_savedNoiseSuppressionEnabled != 0);
    m_noiseSuppressionSpin->setValue(m_savedNoiseSuppressionDb);
    updateCompressorLabels();
}

void AudioDeviceSettingsPage::applyAndSave() {
    const char* rangeKey = "call_video_bitrate_range_kbps";
    QString range = QStringLiteral("%1:%2").arg(m_videoMinimumSpin->value()).arg(m_videoMaximumSpin->value());
    if (!m_configPath.isEmpty() && !config_update_option(m_configPath, "chatserver", rangeKey, range)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video bitrate range config save failed path=%s", qPrintable(m_configPath));
        QMessageBox::warning(this, QCoreApplication::translate("ChatGui", "Settings"),
            QCoreApplication::translate("ChatGui", "Could not save the call video bitrate range."));
        return;
    }
    gui_bridge_post_setting(GUI_SETTING_CHAT, rangeKey, range.toUtf8().constData());
    QString outputId = m_outputCombo->currentData().toString();
    QString inputId = m_inputCombo->currentData().toString();
    int codecPreset = m_codecPresetCombo->currentData().toInt();

    const QPair<const char*, QString> textSettings[] = {{"audio_output_id", outputId}, {"audio_input_id", inputId}, {rangeKey, range}};
    for (const auto& setting : textSettings)
        gui_bridge_post_setting(GUI_SETTING_UI_STATE, setting.first, setting.second.toUtf8().constData());
    SoundManager::instance()->setDevices(outputId, inputId);
    VoiceEncoder::setPreset(codecPreset);

    int radioOpusMode = m_radioOpusModeCombo->currentData().toInt();
    int radioOpusBitrate = m_radioOpusBitrateCombo->currentData().toInt();
    int radioCaptureStereo = m_radioCaptureStereoCb->isChecked() ? 1 : 0;

    int compEnabled = m_compressorEnabledCb->isChecked() ? 1 : 0;
    int compMaxGain = m_compressorMaxGainSlider->value();
    int compLookback = m_compressorLookbackSlider->value();
    int compLookahead = m_compressorLookaheadSlider->value();
    int compRiseRate = m_compressorRiseRateSlider->value();
    int compTargetLevel = m_compressorTargetLevelSlider->value();
    int compSpeechOnly = m_compressorSpeechOnlyCb->isChecked() ? 1 : 0;
    int vadGain = m_compressorVadGain->value();
    audio_compressor_set_vad_max_gain(vadGain);
    audio_compressor_set_shared_settings(compTargetLevel, compSpeechOnly);

    int jitterReserveMs = (int)std::lround(m_jitterReserveSpin->value() * 1000);
    int aecEnabled = m_aecEnabledCb->isChecked() ? 1 : 0;
    int nsEnabled = m_noiseSuppressionEnabledCb->isChecked() ? 1 : 0;
    int nsDb = m_noiseSuppressionSpin->value();
    const QPair<const char*, int> numericSettings[] = {
        {"opus_codec_preset", codecPreset}, {"radio_opus_mode", radioOpusMode}, {"radio_opus_bitrate", radioOpusBitrate},
        {"radio_capture_stereo", radioCaptureStereo}, {"compressor_enabled", compEnabled}, {"compressor_max_gain_db", compMaxGain},
        {"compressor_rise_rate", compRiseRate}, {"call_jitter_max_reserve_ms", jitterReserveMs}, {"aec_enabled", aecEnabled},
        {"noise_suppression_enabled", nsEnabled}, {"noise_suppression_db", nsDb}, {"radio_vad_compressor_max_gain_db", vadGain}
    };
    for (const auto& setting : numericSettings) {
        const QByteArray value = QByteArray::number(setting.second);
        gui_bridge_post_setting(GUI_SETTING_UI_STATE, setting.first, value.constData());
        if (!m_configPath.isEmpty() && !config_update_option(m_configPath, "chatserver", setting.first, QString::fromLatin1(value))) {
            DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "Audio settings: config save failed key=%s", setting.first); continue;
        }
        gui_bridge_post_setting(GUI_SETTING_CHAT, setting.first, value.constData());
    }

    // Параметры рекордера и общая цель/режим роста хранятся в UI state, вне chat_setting ядра.
    const QPair<const char*, int> recorderSettings[] = {
        {"compressor_lookback_ms", compLookback}, {"compressor_lookahead_ms", compLookahead},
        {"compressor_target_level_db", compTargetLevel}, {"compressor_gain_only_on_speech", compSpeechOnly}
    };
    for (const auto& setting : recorderSettings)
        gui_bridge_post_setting(GUI_SETTING_UI_STATE, setting.first, QByteArray::number(setting.second).constData());

    m_savedOutputId = outputId;
    m_savedInputId = inputId;
    m_savedCodecPreset = codecPreset;
    m_savedRadioOpusMode = radioOpusMode;
    m_savedRadioOpusBitrate = radioOpusBitrate;
    m_savedRadioCaptureStereo = radioCaptureStereo;
    m_savedCompressorEnabled = compEnabled;
    m_savedCompressorMaxGainDb = compMaxGain;
    m_savedCompressorLookbackMs = compLookback;
    m_savedCompressorLookaheadMs = compLookahead;
    m_savedCompressorRiseRate = compRiseRate;
    m_savedCompressorTargetLevelDb = compTargetLevel;
    m_savedCompressorSpeechOnly = compSpeechOnly;
    m_savedCompressorVadGain = vadGain;
    m_savedAecEnabled = aecEnabled;
    m_savedNoiseSuppressionEnabled = nsEnabled;
    m_savedNoiseSuppressionDb = nsDb;
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "Audio settings: noise suppression enabled=%d suppress=%ddB", nsEnabled, nsDb);
    DEBUG_DEBUG(DEBUG_CATEGORY_CONFIG,
        "requested output_default=%d input_default=%d codec=%d radioOpus=%d/%dkbps stereo=%d "
        "compressor=%d gain=%ddB back=%dms ahead=%dms rise=%.1fx target=%ddBFS aec=%d",
               outputId.isEmpty(), inputId.isEmpty(), codecPreset, radioOpusMode, radioOpusBitrate, radioCaptureStereo,
               compEnabled, compMaxGain, compLookback, compLookahead, (float)compRiseRate, compTargetLevel, aecEnabled);

    if (m_recorder) {
        m_recorder->configureNoiseSuppression(nsEnabled != 0, nsDb);
        m_recorder->setCompressorEnabled(compEnabled != 0);
        {
            audio_compressor_config_t compCfg = {0};
            compCfg.sample_rate = 48000;
            compCfg.channels = 1;
            compCfg.max_gain_db = (float)compMaxGain;
            compCfg.lookback_ms = compLookback;
            compCfg.lookahead_ms = compLookahead;
            compCfg.rise_rate_per_sec = (float)compRiseRate;
            compCfg.target_level = std::pow(10.0f, compTargetLevel / 20.0f);
            compCfg.use_shared_settings = 1;
            m_recorder->configureCompressor(compCfg);
        }
    }
}

void AudioDeviceSettingsPage::onTestMicToggled() {
    if (m_testMicBtn->isChecked()) {
        m_testRecorder = new AudioRecorder(this);
        m_testRecorder->init();
        QString id = m_inputCombo->currentData().toString();
        m_testRecorder->setCaptureDevice(id);
        m_testRecorder->startRecording(true);
        m_micTestActive = true;
        m_levelTimer->start();
        m_testMicBtn->setText(QCoreApplication::translate("ChatGui", "Stop Test"));
        m_micLevelBar->setValue(0);
    } else {
        stopMicTest();
        m_testMicBtn->setText(QCoreApplication::translate("ChatGui", "Test Microphone"));
    }
}

void AudioDeviceSettingsPage::stopMicTest() {
    m_levelTimer->stop();
    if (m_testRecorder) {
        m_testRecorder->stopRecording();
        m_testRecorder->shutdown();
        delete m_testRecorder;
        m_testRecorder = nullptr;
    }
    m_micTestActive = false;
    m_micLevelBar->setValue(0);
    m_testMicBtn->setChecked(false);
}

void AudioDeviceSettingsPage::onLevelTimer() {
    if (!m_micTestActive || !m_testRecorder || !m_testRecorder->isRecording()) {
        stopMicTest();
        return;
    }
    float peak = m_testRecorder->peakLevel();
    /* Linear to dB: 20*log10(peak), map -60..0 dB to 0..100 */
    float dbVal = peak > 0.0001f ? 20.0f * std::log10(peak) : -70.0f;
    int pct = (int)((dbVal + 70.0f) / 70.0f * 100.0f);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    m_micLevelBar->setValue(pct);
    const char* color = pct < 60 ? "#27ae60" : pct < 85 ? "#f1c40f" : "#e74c3c";
    m_micLevelBar->setStyleSheet(QString(
        "QProgressBar { border: 1px solid palette(mid); border-radius: 4px; background: palette(base); }"
        "QProgressBar::chunk { background: %1; border-radius: 3px; }").arg(color));
}

void AudioDeviceSettingsPage::stopSpeakerTest() {
    if (m_speakerTimer) m_speakerTimer->stop();
    m_speakerTest.reset();
    m_testSpeakerBtn->setText(QCoreApplication::translate("ChatGui", "Test Speaker (440Hz)"));
}

void AudioDeviceSettingsPage::hideEvent(QHideEvent* event) {
    stopMicTest(); stopSpeakerTest();
    QWidget::hideEvent(event);
}

void AudioDeviceSettingsPage::onTestSpeaker() {
    if (m_speakerTest) { stopSpeakerTest(); return; }
    auto* context = SoundManager::instance()->context();
    const QString output = m_outputCombo->currentData().toString();
    ma_device_id id;
    if (!context || (!output.isEmpty() && !SoundManager::deviceId(output, &id))) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "Speaker test: selected device unavailable id=%s context=%p", qPrintable(output), (void*)context);
        QMessageBox::warning(this, QCoreApplication::translate("ChatGui", "Speaker test"),
            QCoreApplication::translate("ChatGui", "The selected playback device is unavailable."));
        return;
    }
    auto config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32; config.playback.channels = 2; config.sampleRate = 48000;
    config.playback.pDeviceID = output.isEmpty() ? nullptr : &id;
    m_speakerTest = std::make_unique<AudioDevice>();
    auto tone = [frame = unsigned(0)](const void*, void* out, unsigned count) mutable {
        auto* pcm = static_cast<float*>(out);
        for (unsigned i = 0; i < count; ++i, ++frame) {
            const float fade = qBound(0.0f, qMin(float(frame), 12000.0f - frame) / 240.0f, 1.0f);
            const float sample = frame < 12000 ? .15f * fade * std::sin(2.0 * M_PI * 440.0 * frame / 48000.0) : 0;
            pcm[2 * i] = sample; pcm[2 * i + 1] = sample;
        }
    };
    if (!m_speakerTest->init(context, config, "settings-speaker-test", 0, tone) || !m_speakerTest->start()) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "Speaker test: failed to start selected output id=%s", qPrintable(output));
        stopSpeakerTest();
        QMessageBox::warning(this, QCoreApplication::translate("ChatGui", "Speaker test"),
            QCoreApplication::translate("ChatGui", "The selected playback device is unavailable."));
        return;
    }
    m_testSpeakerBtn->setText(QCoreApplication::translate("ChatGui", "Stop Test"));
    m_speakerTimer->start(300);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "Speaker test: selected output id=%s duration=250ms tone=440Hz", qPrintable(output));
}


void AudioDeviceSettingsPage::buildCompressorUI(QVBoxLayout* layout) {
    auto* sharedLayout = layout;
    m_noiseSuppressionEnabledCb = new QCheckBox(QCoreApplication::translate("ChatGui", "Microphone noise suppression (Speex)"), this);
    m_noiseSuppressionEnabledCb->setObjectName("noiseSuppressionEnabled");
    m_noiseSuppressionEnabledCb->setChecked(true);
    m_noiseSuppressionEnabledCb->setToolTip(QCoreApplication::translate("ChatGui",
        "Applies to calls, radio and voice messages. Turning off removes the filter and its delay. Voice message changes apply to the next recording."));
    layout->addWidget(m_noiseSuppressionEnabledCb);
    m_noiseSuppressionSpin = new QSpinBox(this);
    m_noiseSuppressionSpin->setObjectName("noiseSuppressionDb");
    m_noiseSuppressionSpin->setRange(SPEEX_NS_MIN_DB, SPEEX_NS_MAX_DB);
    m_noiseSuppressionSpin->setValue(SPEEX_NS_DEFAULT_DB);
    m_noiseSuppressionSpin->setSuffix(QCoreApplication::translate("ChatGui", " dB"));
    m_noiseSuppressionSpin->setToolTip(QCoreApplication::translate("ChatGui", "More negative values suppress noise more strongly. Default: -20 dB."));
    connect(m_noiseSuppressionEnabledCb, &QCheckBox::toggled, m_noiseSuppressionSpin, &QSpinBox::setEnabled);
    auto* nsForm = new QFormLayout();
    nsForm->addRow(QCoreApplication::translate("ChatGui", "Noise suppression:"), m_noiseSuppressionSpin);
    layout->addLayout(nsForm);

    m_compressorEnabledCb = new QCheckBox(QCoreApplication::translate("ChatGui", "Enable compressor (leveler/normalizer)"), this);
    m_compressorEnabledCb->setStyleSheet("font-size: 12px;");
    layout->addWidget(m_compressorEnabledCb);

    layout = settingsDetails(layout, QCoreApplication::translate("ChatGui", "Compressor details"));
    layout->parentWidget()->setEnabled(m_compressorEnabledCb->isChecked());
    connect(m_compressorEnabledCb, &QCheckBox::toggled, layout->parentWidget(), &QWidget::setEnabled);

    auto makeSliderRow = [&](const QString& name, QSlider*& slider, QLabel*& lbl,
                             int min, int max, int step) {
        auto* row = new QHBoxLayout();
        row->setSpacing(6);
        auto* textLabel = new QLabel(name, this);
        textLabel->setMinimumWidth(180);
        textLabel->setStyleSheet("font-size: 12px;");
        row->addWidget(textLabel);
        slider = new QSlider(Qt::Horizontal, this);
        slider->setRange(min, max);
        slider->setSingleStep(step);
        slider->setStyleSheet(
            "QSlider::groove:horizontal { height: 4px; background: palette(mid); border-radius: 2px; }"
            "QSlider::handle:horizontal { width: 12px; margin: -5px 0; background: palette(highlight); border-radius: 6px; }");
        row->addWidget(slider, 1);
        lbl = new QLabel(this);
        lbl->setFixedWidth(60);
        lbl->setStyleSheet("font-size: 12px;");
        lbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        row->addWidget(lbl);
        layout->addLayout(row);
    };

    makeSliderRow(QCoreApplication::translate("ChatGui", "Max gain:"), m_compressorMaxGainSlider, m_compressorMaxGainLabel,
                   0, 50, 1);
    m_compressorMaxGainSlider->setObjectName("compressorMaxGain");
    makeSliderRow(QCoreApplication::translate("ChatGui", "Lookback time:"), m_compressorLookbackSlider, m_compressorLookbackLabel,
                   50, 500, 10);
    m_compressorLookbackSlider->setObjectName("compressorLookback");
    makeSliderRow(QCoreApplication::translate("ChatGui", "Lookahead time:"), m_compressorLookaheadSlider, m_compressorLookaheadLabel,
                   20, 500, 10);
    m_compressorLookaheadSlider->setObjectName("compressorLookahead");
    makeSliderRow(QCoreApplication::translate("ChatGui", "Rise rate (x/sec):"), m_compressorRiseRateSlider, m_compressorRiseRateLabel,
                   2, 10, 1);
    layout = settingsDetails(sharedLayout, QCoreApplication::translate("ChatGui", "Shared compressor settings"));
    auto* sharedHint = new QLabel(QCoreApplication::translate("ChatGui",
        "Target level and speech-only gain growth apply to calls, radio, broadcasts and voice messages."), this);
    sharedHint->setWordWrap(true); layout->addWidget(sharedHint);
    makeSliderRow(QCoreApplication::translate("ChatGui", "Target level:"), m_compressorTargetLevelSlider, m_compressorTargetLevelLabel,
                   -40, 0, 1);
    m_compressorTargetLevelSlider->setObjectName("compressorTargetLevel");
    m_compressorSpeechOnlyCb = new QCheckBox(QCoreApplication::translate("ChatGui", "Increase gain only during speech"), this);
    m_compressorSpeechOnlyCb->setObjectName("compressorGainOnlyOnSpeech");
    m_compressorSpeechOnlyCb->setToolTip(QCoreApplication::translate("ChatGui",
        "VAD only controls gain growth. Silence is still transmitted and excessive gain can always decrease."));
    layout->addWidget(m_compressorSpeechOnlyCb);
    m_compressorVadGain = new QSpinBox(this); m_compressorVadGain->setObjectName("compressorVadMaxGain");
    m_compressorVadGain->setRange(1, 60); m_compressorVadGain->setSuffix(QCoreApplication::translate("ChatGui", " dB"));
    auto* vadForm = new QFormLayout();
    vadForm->addRow(QCoreApplication::translate("ChatGui", "VAD max gain:"), m_compressorVadGain); layout->addLayout(vadForm);
    m_compressorVadGain->setToolTip(QCoreApplication::translate("ChatGui",
        "Separate compressor before VAD: independent gain and history, no VAD gating. Applies to all speech detectors."));

    auto connectSlider = [this](QSlider* slider) {
        connect(slider, &QSlider::valueChanged, this, &AudioDeviceSettingsPage::updateCompressorLabels);
    };
    connectSlider(m_compressorMaxGainSlider);
    connectSlider(m_compressorLookbackSlider);
    connectSlider(m_compressorLookaheadSlider);
    connectSlider(m_compressorRiseRateSlider);
    connectSlider(m_compressorTargetLevelSlider);
    updateCompressorLabels();
}

void AudioDeviceSettingsPage::updateCompressorLabels() {
    if (m_compressorMaxGainLabel)
        m_compressorMaxGainLabel->setText(QString::number(m_compressorMaxGainSlider->value()) + QCoreApplication::translate("ChatGui", " dB"));
    if (m_compressorRiseRateLabel) {
        int v = m_compressorRiseRateSlider->value();
        m_compressorRiseRateLabel->setText(QString::number(v) + "x");
    }
    m_compressorLookbackLabel->setText(QString::number(m_compressorLookbackSlider->value()) + QCoreApplication::translate("ChatGui", " ms"));
    m_compressorLookaheadLabel->setText(QString::number(m_compressorLookaheadSlider->value()) + QCoreApplication::translate("ChatGui", " ms"));
    m_compressorTargetLevelLabel->setText(QString::number(m_compressorTargetLevelSlider->value()) + " dBFS");
}

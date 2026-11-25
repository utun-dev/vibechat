#include <QCoreApplication>
#include "radiosettingspage.h"
#include "ptt_hotkey_manager.h"
#include "radio_audio_engine.h"
#include "../db/db_manager.h"
#include "../../lib/debug_config.h"

#include <QApplication>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QHideEvent>
#include <QShowEvent>
#include <QFormLayout>
#include <QSpinBox>
#include <QCheckBox>
#include <QButtonGroup>
#include <QPainter>
#include <algorithm>
#include <QKeyEvent>
#include <QMouseEvent>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

/* Измеритель с перетаскиваемыми порогами; QSpinBox остаётся единственным источником их значений. */
class RadioLevelMeter : public QWidget {
public:
    explicit RadioLevelMeter(QWidget* parent) : QWidget(parent) { setMinimumSize(200, 34); setCursor(Qt::SizeHorCursor); }
    QSpinBox *start = nullptr, *stop = nullptr;
    float level = -120;
    bool valid = false, vad = false;
protected:
    bool event(QEvent* event) override {
        if (event->type() == QEvent::Hide || event->type() == QEvent::UngrabMouse) m_dragged = nullptr;
        return QWidget::event(event);
    }
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        QRect bar(1, 1, width() - 2, 16);
        painter.fillRect(bar, palette().color(QPalette::Mid));
        if (valid) painter.fillRect(QRect(bar.left(), bar.top(), x(level) - bar.left(), bar.height()), QColor("#27ae60"));
        painter.setPen(palette().color(QPalette::Text));
        painter.drawText(QRect(0, 18, width(), 16), Qt::AlignLeft, vad ? QStringLiteral("0%") : QStringLiteral("−90 dBFS"));
        painter.drawText(QRect(0, 18, width(), 16), Qt::AlignRight, vad ? QStringLiteral("100%") : QStringLiteral("0"));
        for (auto* spin : {stop, start}) {
            if (!spin) continue;
            const QColor color = spin == start ? QColor("#e67e22") : QColor("#3390ec");
            const int position = x(spin->value());
            painter.setPen(QPen(color, 2)); painter.drawLine(position, 0, position, 18);
            painter.setBrush(color); painter.drawPolygon(QPolygon({QPoint(position, 12), QPoint(position - 4, 18), QPoint(position + 4, 18)}));
        }
    }
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton || event->pos().y() > 18 || !start) { event->ignore(); return; }
        m_dragged = stop && qAbs(event->pos().x() - x(stop->value())) < qAbs(event->pos().x() - x(start->value())) ? stop : start;
        DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "Radio settings: drag marker vad=%d start=%d value=%d", vad, m_dragged == start, m_dragged->value());
        moveMarker(event->pos().x()); event->accept();
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if (m_dragged && (event->buttons() & Qt::LeftButton)) { moveMarker(event->pos().x()); event->accept(); }
        else event->ignore();
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton || !m_dragged) { event->ignore(); return; }
        moveMarker(event->pos().x());
        DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "Radio settings: marker released vad=%d start=%d value=%d", vad, m_dragged == start, m_dragged->value());
        m_dragged = nullptr; event->accept();
    }
private:
    int x(float value) const { return 1 + qRound(std::clamp(vad ? value / 100 : (value + 90) / 90, 0.0f, 1.0f) * (width() - 2)); }
    void moveMarker(int position) {
        int value = qRound(float(position - 1) / (width() - 2) * (vad ? 100 : 90)) - (vad ? 0 : 90);
        int minimum = m_dragged->minimum(), maximum = m_dragged->maximum();
        if (stop) {
            if (m_dragged == start) minimum = std::clamp(stop->value() + 1, minimum, maximum);
            else maximum = std::clamp(start->value() - 1, minimum, maximum);
        }
        m_dragged->setValue(std::clamp(value, minimum, maximum));
    }
    QSpinBox* m_dragged = nullptr; // Маркер, выбранный при нажатии; не меняется при пересечении мышью соседнего порога.
};

RadioActivationSettings::RadioActivationSettings(QWidget* parent) : QWidget(parent) {
    auto* form = new QFormLayout(this);
    form->setContentsMargins(0, 0, 0, 8);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    auto add = [&](const QString& label, const char* name, int min, int max, const QString& suffix) {
        auto* spin = new QSpinBox(this);
        spin->setObjectName(QLatin1String(name)); spin->setRange(min, max); spin->setSuffix(suffix);
        auto* text = new QLabel(label, this); text->setWordWrap(true);
        form->addRow(text, spin);
        connect(spin, QOverload<int>::of(&QSpinBox::valueChanged), this, &RadioActivationSettings::settingsEdited);
        return spin;
    };
    auto label = [&](const char* name) {
        auto* text = new QLabel(this); text->setObjectName(QLatin1String(name)); text->setWordWrap(true);
        form->addRow(text); return text;
    };
    auto* modes = new QWidget(this);
    auto* row = new QHBoxLayout(modes); row->setContentsMargins(0, 0, 0, 0); row->setSpacing(0);
    m_mode = new QButtonGroup(this);
    for (int mode : {1, 2, 3}) {
        QString title = mode == 1 ? QStringLiteral("VAD") : mode == 2 ? QCoreApplication::translate("ChatGui", "By signal level") :
            QCoreApplication::translate("ChatGui", "VAD + level");
        auto* button = new QPushButton(title, modes);
        button->setObjectName(mode == 1 ? QStringLiteral("radioModeVad") : mode == 2 ?
            QStringLiteral("radioModeLevel") : QStringLiteral("radioModeVadLevel"));
        button->setCheckable(true); button->setMinimumHeight(36);
        button->setStyleSheet("QPushButton:checked { background: palette(highlight); color: palette(highlighted-text); }");
        m_mode->addButton(button, mode); row->addWidget(button, 1);
        connect(button, &QPushButton::clicked, this, [this, mode]() {
            DEBUG_INFO(DEBUG_CATEGORY_RADIO, "Radio settings: select mode=%d (automatic apply)", mode);
            settingsEdited();
        });
    }
    m_mode->button(1)->setChecked(true);
    form->addRow(QCoreApplication::translate("ChatGui", "Automatic transmission:"), modes);
    m_levelText = label("radioLevelText");
    m_levelMeter = new RadioLevelMeter(this); m_levelMeter->setObjectName(QStringLiteral("radioLevelMeter"));
    form->addRow(m_levelMeter);
    m_levelOn = add(QCoreApplication::translate("ChatGui", "Start threshold (orange):"), "radioLevelOn", -90, 0, QStringLiteral(" dBFS"));
    m_levelOff = add(QCoreApplication::translate("ChatGui", "Stop threshold (blue):"), "radioLevelOff", -90, 0, QStringLiteral(" dBFS"));
    m_levelMeter->start = m_levelOn; m_levelMeter->stop = m_levelOff;
    m_levelMeter->setToolTip(QCoreApplication::translate("ChatGui", "Drag the orange and blue markers to adjust the start and stop thresholds."));
    m_vadText = label("radioVadText");
    m_vadMeter = new RadioLevelMeter(this); m_vadMeter->vad = true; m_vadMeter->setObjectName(QStringLiteral("radioVadMeter"));
    form->addRow(m_vadMeter);
    m_threshold = add(QCoreApplication::translate("ChatGui", "VAD speech threshold:"), "radioVadThreshold", 10, 95, QStringLiteral(" %"));
    m_vadMeter->start = m_threshold;
    m_vadMeter->setToolTip(QCoreApplication::translate("ChatGui", "Drag the orange marker to adjust the speech probability threshold."));
    m_threshold->setToolTip(QCoreApplication::translate("ChatGui",
        "Speech probability threshold: lower is more sensitive. This is not a volume threshold."));
    m_gainText = label("radioVadGainText");
    m_hangover = add(QCoreApplication::translate("ChatGui", "Silence before ending transmission:"), "radioHangover", 50, 2000,
                    QCoreApplication::translate("ChatGui", " ms"));
    m_error = label("radioActivationError"); m_error->setStyleSheet(QStringLiteral("color: palette(bright-text);"));
    m_applyTimer.setSingleShot(true); m_applyTimer.setInterval(100);
    m_levelMeter->installEventFilter(this); m_vadMeter->installEventFilter(this);
    connect(&m_applyTimer, &QTimer::timeout, this, &RadioActivationSettings::applyPending);
    auto* engine = RadioAudioEngine::instance();
    connect(engine, &RadioAudioEngine::stateChanged, this, [this, engine]() {
        setEnabled(engine->settingsReady());
        if (!engine->settingsReady()) { m_loaded = false; m_pending = false; m_applyTimer.stop(); }
        if (!m_loaded && engine->settingsReady()) loadSettings();
        else updateControls();
        if (m_pending && !m_dragging && !engine->settingsBusy() && !m_applyTimer.isActive()) m_applyTimer.start(100);
    });
    connect(engine, &RadioAudioEngine::inputLevelChanged, this, &RadioActivationSettings::updateMeters);
    loadSettings();
}

QMap<QString, int> RadioActivationSettings::values() const {
    return {{"radio_auto_mode", m_mode->checkedId()}, {"radio_level_on_dbfs", m_levelOn->value()},
        {"radio_level_off_dbfs", m_levelOff->value()}, {"radio_vad_threshold", m_threshold->value()},
        {"radio_auto_hangover_ms", m_hangover->value()}};
}

bool RadioActivationSettings::eventFilter(QObject* object, QEvent* event) {
    if (object == m_levelMeter || object == m_vadMeter) {
        auto* mouse = dynamic_cast<QMouseEvent*>(event);
        if (event->type() == QEvent::MouseButtonPress && mouse->button() == Qt::LeftButton && mouse->pos().y() <= 18) {
            m_dragging = true; m_applyTimer.stop();
            DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "Radio settings: suspend apply while dragging meter=%s", qPrintable(object->objectName()));
        } else if (m_dragging && ((event->type() == QEvent::MouseButtonRelease && mouse->button() == Qt::LeftButton) ||
            event->type() == QEvent::Hide || event->type() == QEvent::UngrabMouse)) {
            m_dragging = false; m_applyTimer.start(0);
            DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "Radio settings: apply final marker after drag meter=%s pending=%d event=%d",
                qPrintable(object->objectName()), m_pending, int(event->type()));
        }
    }
    return QWidget::eventFilter(object, event);
}

void RadioActivationSettings::settingsEdited() {
    if (!m_loaded) return;
    m_pending = true; updateControls();
    if (!m_dragging && !m_applyTimer.isActive()) m_applyTimer.start(100);
}

void RadioActivationSettings::applyPending() {
    auto* engine = RadioAudioEngine::instance();
    if (!m_pending || m_dragging || !engine->settingsReady() || engine->settingsBusy()) return;
    if (m_levelOff->value() >= m_levelOn->value()) {
        m_pending = false;
        DEBUG_WARN(DEBUG_CATEGORY_RADIO, "Radio settings: automatic apply deferred, invalid thresholds on=%d off=%d",
            m_levelOn->value(), m_levelOff->value());
        return;
    }
    QMap<QString, int> changes;
    const auto draft = values();
    for (auto it = draft.cbegin(); it != draft.cend(); ++it) if (engine->setting(it.key()) != it.value()) changes.insert(it.key(), it.value());
    m_pending = false;
    if (changes.isEmpty()) return;
    DEBUG_INFO(DEBUG_CATEGORY_RADIO, "Radio settings: automatic apply mode=%d on=%d off=%d vad=%d gain=%d hangover=%d",
               m_mode->checkedId(), m_levelOn->value(), m_levelOff->value(), m_threshold->value(),
               engine->setting("radio_vad_compressor_max_gain_db"), m_hangover->value());
    engine->applySettings(changes); // Движок показывает и логирует ошибку; повторяем только после новой правки пользователя.
}

void RadioActivationSettings::loadSettings() {
    auto* engine = RadioAudioEngine::instance();
    setEnabled(engine->settingsReady());
    if (!engine->settingsReady() || (m_loaded && (m_pending || engine->settingsBusy()))) return;
    m_loaded = false;
    m_mode->button(engine->autoMode())->setChecked(true);
    m_levelOn->setValue(engine->setting(QStringLiteral("radio_level_on_dbfs")));
    m_levelOff->setValue(engine->setting(QStringLiteral("radio_level_off_dbfs")));
    m_threshold->setValue(engine->setting(QStringLiteral("radio_vad_threshold")));
    m_hangover->setValue(engine->setting(QStringLiteral("radio_auto_hangover_ms")));
    m_loaded = true;
    updateControls();
}

void RadioActivationSettings::updateControls() {
    if (!m_loaded) return;
    QString error = m_levelOff->value() >= m_levelOn->value()
        ? QCoreApplication::translate("ChatGui", "The stop threshold must be lower than the start threshold")
        : RadioAudioEngine::instance()->settingsError();
    m_error->setText(error); m_error->setVisible(!error.isEmpty());
    updateMeters();
}

void RadioActivationSettings::updateMeters() {
    auto* engine = RadioAudioEngine::instance();
    m_levelMeter->valid = engine->inputLevelValid(); m_levelMeter->level = engine->inputLevel(); m_levelMeter->update();
    QString text;
    if (!engine->isActive()) text = QCoreApplication::translate("ChatGui", "Enable radio to measure the microphone level");
    else if (!engine->inputLevelValid()) text = QCoreApplication::translate("ChatGui", "Microphone level unavailable");
    else text = QCoreApplication::translate("ChatGui", "Current level: %1 dBFS").arg(engine->inputLevel(), 0, 'f', 1);
    m_levelText->setText(text); m_levelMeter->setAccessibleName(text);
    m_vadMeter->valid = engine->vadValid(); m_vadMeter->level = engine->vadProbability() * 100; m_vadMeter->update();
    if (engine->vadValid()) text = QCoreApplication::translate("ChatGui", "VAD speech probability: %1%").arg(engine->vadProbability() * 100, 0, 'f', 1);
    else if (!engine->autoEnabled() || (engine->autoMode() != 1 && engine->autoMode() != 3))
        text = QCoreApplication::translate("ChatGui", "Enable automatic transmission in VAD mode to see speech probability");
    else if (engine->autoBlocked()) text = QCoreApplication::translate("ChatGui", "VAD measurement paused: channel busy");
    else text = QCoreApplication::translate("ChatGui", "VAD speech probability unavailable");
    m_vadText->setText(text); m_vadMeter->setAccessibleName(text);
    if (!engine->vadValid()) text = QCoreApplication::translate("ChatGui", "Current VAD compressor gain: —");
    else if (!engine->vadCompressorEnabled()) text = QCoreApplication::translate("ChatGui", "VAD compressor off (0 dB)");
    else text = QCoreApplication::translate("ChatGui", "Current VAD compressor gain: %1 dB")
        .arg((engine->vadGain() >= 0 ? QStringLiteral("+") : QString()) + QString::number(engine->vadGain(), 'f', 1));
    m_gainText->setText(text);
}

RadioSettingsPage::RadioSettingsPage(DbManager* db, QWidget* parent) : QWidget(parent), m_db(db) {
    auto* layout = new QVBoxLayout(this);
    auto* title = new QLabel(QCoreApplication::translate("ChatGui", "Radio (PTT)"), this);
    title->setStyleSheet("font-weight: bold; font-size: 13px;");
    layout->addWidget(title);
    m_audioSettings = new QWidget(this);
    auto* audio = new QFormLayout(m_audioSettings); audio->setContentsMargins(0, 0, 0, 8);
    m_aec = new QCheckBox(QCoreApplication::translate("ChatGui", "Enable echo cancellation (AEC) in radio"), m_audioSettings);
    m_aec->setObjectName("radioAecEnabled"); audio->addRow(m_aec);
    m_compressor = new QCheckBox(QCoreApplication::translate("ChatGui", "Transmission and VAD compressor"), m_audioSettings);
    audio->addRow(m_compressor);
    m_txGain = new QSpinBox(m_audioSettings); m_txGain->setObjectName("radioTxGain"); m_txGain->setRange(1, 60);
    m_txGain->setSuffix(QCoreApplication::translate("ChatGui", " dB"));
    audio->addRow(QCoreApplication::translate("ChatGui", "Transmission max gain:"), m_txGain);
    m_burstFrames = new QSpinBox(m_audioSettings); m_burstFrames->setObjectName("radioBurstFrames"); m_burstFrames->setRange(1, 10);
    m_burstFrames->setToolTip(QCoreApplication::translate("ChatGui",
        "Applies to the next transmission. Each frame is 20 ms; larger batches reduce overhead but add delay. The final partial batch is sent immediately."));
    audio->addRow(QCoreApplication::translate("ChatGui", "Burst size (Opus frames):"), m_burstFrames);
    layout->addWidget(m_audioSettings);
    m_audioError = new QLabel(this); m_audioError->setWordWrap(true); m_audioError->setStyleSheet("color: palette(bright-text);");
    layout->addWidget(m_audioError);
    auto* engine = RadioAudioEngine::instance();
    connect(engine, &RadioAudioEngine::stateChanged, this, [this, engine] {
        m_audioSettings->setEnabled(engine->settingsReady() && !engine->settingsBusy());
        if (!engine->settingsReady()) m_audioLoaded = false;
        if (!m_audioLoaded && engine->settingsReady()) loadAudioSettings();
        m_audioError->setText(engine->settingsError()); m_audioError->setVisible(!engine->settingsError().isEmpty());
    });
    m_doublePress = new QCheckBox(QCoreApplication::translate("ChatGui", "Lock on double press"), this);
    m_doublePress->setObjectName(QStringLiteral("radioPttDoublePress"));
    m_doublePress->setToolTip(QCoreApplication::translate("ChatGui",
        "Applies to the shortcut only. The PTT button always supports double-click locking."));
    layout->addWidget(m_doublePress);
    if (PttHotkeyManager::usesPortal()) {
        auto* hint = new QLabel(QCoreApplication::translate("ChatGui",
            "The global shortcut works from any application. Hold it to talk. When double-press locking is enabled, "
            "press twice to lock transmission, then press again to release. Assign a regular key with Ctrl, Shift, Alt or "
            "Win in the system dialog. Changes are saved by the system immediately. Keyboard "
            "transmission is disabled during setup."), this);
        hint->setWordWrap(true);
        layout->addWidget(hint);
        m_button = new QPushButton(QCoreApplication::translate("ChatGui", "Set PTT shortcut…"), this);
        m_button->setObjectName(QStringLiteral("radioPttShortcut"));
        layout->addWidget(m_button);
        m_label = new QLabel(this);
        m_label->setWordWrap(true); m_label->setTextFormat(Qt::PlainText);
        layout->addWidget(m_label);
        m_portalStatus = new QLabel(this);
        m_portalStatus->setWordWrap(true);
        m_portalStatus->setTextFormat(Qt::PlainText);
        m_portalStatus->setStyleSheet("color: palette(placeholder-text); font-size: 11px;");
        layout->addWidget(m_portalStatus);
        connect(m_button, &QPushButton::clicked, this, [this]() { PttHotkeyManager::instance()->configurePortal(this); });
        connect(PttHotkeyManager::instance(), &PttHotkeyManager::portalChanged, this, &RadioSettingsPage::updatePortalLabels);
        loadFromDb();
        layout->addStretch(1);
        return;
    }
    auto* hint = new QLabel(
        QCoreApplication::translate("ChatGui",
            "Hold a shortcut to transmit from any application, even when ChatGUI is not focused. "
            "When double-press locking is enabled, press twice to lock transmission, then press again to release. "
            "Group radio must be enabled. To record a shortcut, hold the desired keys together and "
            "release them. Left and right Ctrl, Shift, Alt and Win are distinct. Fn is available only "
            "if the keyboard reports it to the OS."), this);
    hint->setWordWrap(true);
    hint->setStyleSheet("color: palette(placeholder-text); font-size: 11px;");
    layout->addWidget(hint);
    if (!PttHotkeyManager::isSupported()) {
        auto* warn = new QLabel(QCoreApplication::translate("ChatGui",
            "Global shortcuts are available on Windows and Linux/X11. Shortcut recording is "
            "unavailable on this platform."), this);
        warn->setWordWrap(true);
        warn->setStyleSheet("color: palette(bright-text); font-size: 12px;");
        layout->addWidget(warn);
    }
    auto* row = new QHBoxLayout();
    m_button = new QPushButton(QCoreApplication::translate("ChatGui", "Set PTT shortcut…"), this);
    m_button->setObjectName(QStringLiteral("radioPttShortcut"));
    m_button->setEnabled(PttHotkeyManager::isSupported());
    auto* clear = new QPushButton(QCoreApplication::translate("ChatGui", "Clear"), this);
    clear->setObjectName(QStringLiteral("radioPttClear"));
    row->addWidget(m_button); row->addWidget(clear); row->addStretch();
    layout->addLayout(row);
    m_label = new QLabel(this);
    m_label->setWordWrap(true);
    m_label->setTextFormat(Qt::PlainText);
    m_label->setStyleSheet("font-size: 13px; padding-bottom: 12px;");
    layout->addWidget(m_label);
    connect(m_button, &QPushButton::clicked, this, [this]() {
        if (m_capturing) finishCapture(false); else startCapture();
    });
    connect(clear, &QPushButton::clicked, this, [this]() {
        finishCapture(false);
        m_chord.clear(); updateLabel();
        DEBUG_INFO(DEBUG_CATEGORY_RADIO, "PTT: shortcut draft cleared");
    });
    connect(PttHotkeyManager::instance(), &PttHotkeyManager::keysChanged, this, [this](const PttKeyChord& held) {
        if (!m_capturing) return;
        if (m_capture.observe(held)) finishCapture(true);
        else if (!held.isEmpty())
            m_label->setText(PttHotkeyManager::instance()->describe(m_capture.candidate));
    });
    layout->addStretch(1);
    loadFromDb();
}

RadioSettingsPage::~RadioSettingsPage() {
    finishCapture(false);
    if (m_portalStatus && isVisible()) PttHotkeyManager::instance()->inhibitPortal(false);
}

void RadioSettingsPage::startCapture() {
    finishCapture(false);
    DEBUG_INFO(DEBUG_CATEGORY_RADIO, "PTT: shortcut recording requested");
    if (!PttHotkeyManager::instance()->beginCapture()) {
        m_label->setText(QCoreApplication::translate("ChatGui", "Failed to start key recording. See the radio log for details."));
        return;
    }
    m_capturing = true;
    m_capture.candidate.clear();
    m_button->setText(QCoreApplication::translate("ChatGui", "Cancel recording"));
    m_label->setText(QCoreApplication::translate("ChatGui", "Press one or more keys together…"));
    qApp->installEventFilter(this);
}

void RadioSettingsPage::finishCapture(bool save) {
    if (!m_capturing) return;
    if (save) {
        m_chord = m_capture.candidate;
        DEBUG_INFO(DEBUG_CATEGORY_RADIO, "PTT: captured shortcut=[%s] codes=[%s]",
                   qPrintable(PttHotkeyManager::instance()->describe(m_capture.candidate)), qPrintable(pttEncodeChord(m_capture.candidate)));
    } else {
        DEBUG_INFO(DEBUG_CATEGORY_RADIO, "PTT: recording cancelled candidate=[%s]",
                   qPrintable(pttEncodeChord(m_capture.candidate)));
    }
    m_capturing = false;
    qApp->removeEventFilter(this);
    PttHotkeyManager::instance()->endCapture();
    m_button->setText(QCoreApplication::translate("ChatGui", "Set PTT shortcut…"));
    updateLabel();
}

bool RadioSettingsPage::eventFilter(QObject* object, QEvent* event) {
    if (m_capturing) {
        if (event->type() == QEvent::ShortcutOverride) { event->accept(); return true; }
        if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease) {
            auto* key = static_cast<QKeyEvent*>(event);
            // Только ручная запись: сравниваем доставку событий окну с независимым опросом ОС.
            if (!key->isAutoRepeat()) {
                DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "PTT: capture window event=%s qt=0x%x native_vk=%u scan=%u modifiers=0x%x",
                            event->type() == QEvent::KeyPress ? "press" : "release",
                            unsigned(key->key()), unsigned(key->nativeVirtualKey()), unsigned(key->nativeScanCode()),
                            unsigned(key->nativeModifiers()));
#ifdef Q_OS_WIN
                int vk = int(key->nativeVirtualKey());
                if (vk > 0 && vk < 256) {
                    unsigned asyncState = static_cast<unsigned short>(GetAsyncKeyState(vk));
                    unsigned queueState = static_cast<unsigned short>(GetKeyState(vk));
                    DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "PTT: capture Windows vk=%d async=0x%04x queue=0x%04x remote_session=%d",
                                vk, asyncState, queueState, GetSystemMetrics(SM_REMOTESESSION));
                }
#endif
            }
            return true;
        }
    }
    return QWidget::eventFilter(object, event);
}

void RadioSettingsPage::showEvent(QShowEvent* event) {
    if (m_portalStatus) PttHotkeyManager::instance()->inhibitPortal(true);
    QWidget::showEvent(event);
}

void RadioSettingsPage::hideEvent(QHideEvent* event) {
    if (m_portalStatus) PttHotkeyManager::instance()->inhibitPortal(false);
    finishCapture(false);
    QWidget::hideEvent(event);
}

void RadioSettingsPage::updateLabel() {
    m_label->setText(PttHotkeyManager::instance()->describe(m_chord));
}

void RadioSettingsPage::loadFromDb() {
    loadAudioSettings();
    m_doublePress->setChecked(!m_db || !m_db->isOpen() || m_db->getUiState("radio_ptt_double_press") != "0");
    if (m_portalStatus) { updatePortalLabels(); return; }
    finishCapture(false);
    m_chord = m_db && m_db->isOpen() ? pttDecodeChord(m_db->getUiState("radio_ptt_hotkey")) : PttKeyChord();
    updateLabel();
}

QString RadioSettingsPage::hotkey() const { return pttEncodeChord(m_chord); }
bool RadioSettingsPage::doublePressEnabled() const { return m_doublePress->isChecked(); }

void RadioSettingsPage::loadAudioSettings() {
    auto* engine = RadioAudioEngine::instance();
    m_audioSettings->setEnabled(engine->settingsReady() && !engine->settingsBusy());
    if (!engine->settingsReady()) return;
    m_burstFrames->setValue(engine->setting(QStringLiteral("radio_burst_frames")));
    m_compressor->setChecked(engine->setting(QStringLiteral("radio_compressor_enabled")) != 0);
    m_aec->setChecked(engine->setting(QStringLiteral("radio_aec_enabled")) != 0);
    m_txGain->setValue(engine->setting(QStringLiteral("radio_compressor_max_gain_db")));
    m_audioError->setText(engine->settingsError()); m_audioError->setVisible(!engine->settingsError().isEmpty());
    m_audioLoaded = true;
}

bool RadioSettingsPage::apply() {
    DEBUG_INFO(DEBUG_CATEGORY_RADIO, "Radio settings: apply transmission AEC=%d compressor=%d gain=%d burst=%d",
        m_aec->isChecked(), m_compressor->isChecked(), m_txGain->value(), m_burstFrames->value());
    if (!RadioAudioEngine::instance()->applySettings({{"radio_aec_enabled", m_aec->isChecked() ? 1 : 0},
        {"radio_compressor_enabled", m_compressor->isChecked() ? 1 : 0},
        {"radio_compressor_max_gain_db", m_txGain->value()}, {"radio_burst_frames", m_burstFrames->value()}})) return false;
    PttHotkeyManager::instance()->setDoublePressEnabled(doublePressEnabled());
    if (m_portalStatus) return true;
    finishCapture(false);
    PttHotkeyManager::instance()->setHotkey(hotkey());
    return true;
}

void RadioSettingsPage::updatePortalLabels() {
    auto* manager = PttHotkeyManager::instance();
    QString text = manager->portalDescription();
    m_label->setText(QCoreApplication::translate("ChatGui", "PTT shortcut: %1")
        .arg(text.isEmpty() ? QCoreApplication::translate("ChatGui", "Not set") : text));
    m_portalStatus->setText(manager->portalStatus());
    m_button->setEnabled(!manager->portalBusy());
}

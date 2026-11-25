#include <QCoreApplication>
#include "radiopanel.h"
#include "radio_audio_engine.h"
#include "radiosettingspage.h"
#include "sound_manager.h"
#include "memberlistmodel.h"
#include "outline_icon.h"
#include "capture_controls.h"
#include "../transport/gui_bridge.h"
extern "C" {
#include "chat/chat_core.h"
#include "chat/chat_member.h"
#include "../../lib/debug_config.h"
#include "../../lib/mem.h"
}
#include <QHBoxLayout>
#include <QMenuBar>
#include <QPushButton>
#include <QLabel>
#include <QMenu>
#include <QActionGroup>
#include <QSignalBlocker>
#include <QStyle>
#include <QMouseEvent>
#include <QWidgetAction>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QPainter>
#include <QScreen>
#include <QGuiApplication>
#include <cstring>
#include <algorithm>

static QIcon outputIcon(const QColor& color, bool speaking) {
    QPainterPath path; path.moveTo(3, 9); path.lineTo(7, 9); path.lineTo(12, 4);
    path.lineTo(12, 20); path.lineTo(7, 15); path.lineTo(3, 15); path.closeSubpath();
    if (speaking) { path.arcMoveTo(QRectF(10, 4, 12, 16), -60); path.arcTo(QRectF(10, 4, 12, 16), -60, 120); }
    QIcon icon;
    for (int scale : {1, 2, 3, 4}) {
        QPixmap pixmap(24 * scale, 24 * scale); pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap); painter.setRenderHint(QPainter::Antialiasing); painter.scale(scale, scale);
        painter.setPen(QPen(color, speaking ? 2.6 : 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawPath(path); painter.end(); icon.addPixmap(pixmap);
    }
    return icon;
}

/* Полоса показывает текущую вероятность речи, не установленный порог срабатывания. */
class RadioAutoButton final : public QPushButton {
public:
    explicit RadioAutoButton(QWidget* parent) : QPushButton(QStringLiteral("VAD"), parent) {}
protected:
    void paintEvent(QPaintEvent* event) override {
        QPushButton::paintEvent(event);
        auto* engine = RadioAudioEngine::instance();
        if (engine->autoMode() != 1 && engine->autoMode() != 3) return;
        QPainter painter(this);
        QRectF track(6, height() - 7, width() - 12, 3);
        painter.fillRect(track, isChecked() ? QColor(0, 0, 0, 65) : palette().color(QPalette::Mid));
        if (!engine->vadValid()) return;
        track.setWidth(track.width() * std::clamp(engine->vadProbability(), 0.0f, 1.0f));
        painter.fillRect(track, QColor("#4cdb88"));
    }
};

RadioPanel::RadioPanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("radioPanel"));
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(10, 6, 10, 6);
    row->setSpacing(8);
    m_devices = new QMenuBar(this); m_devices->setObjectName("radioDeviceMenus");
    m_devices->setNativeMenuBar(false); m_devices->setMouseTracking(true);
    m_devices->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    auto* menuStyle = new CaptureControls::MenuStyle(34, QSize(24, 24)); menuStyle->setParent(this); m_devices->setStyle(menuStyle);
    m_input = m_devices->addMenu(QCoreApplication::translate("ChatGui", "Microphone"));
    m_output = m_devices->addMenu(QCoreApplication::translate("ChatGui", "Audio output"));
    m_input->setObjectName("radioInputMenu"); m_output->setObjectName("radioOutputMenu");
    m_input->menuAction()->setObjectName("radioInputButton"); m_output->menuAction()->setObjectName("radioOutputButton");
    for (auto* menu : {m_input, m_output}) {
        auto* group = new QActionGroup(menu); group->setExclusive(true);
        connect(menu, &QMenu::aboutToShow, this, [this, menu]() { populateDevices(menu, menu == m_input); });
    }
    row->addWidget(m_devices);
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("radioStatus"));
    m_status->setTextFormat(Qt::RichText);
    m_status->setMinimumWidth(0);
    m_status->setWordWrap(true);
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    row->addWidget(m_status, 1);
    m_auto = new RadioAutoButton(this);
    m_auto->setObjectName(QStringLiteral("radioAutoButton"));
    m_auto->setAccessibleName(QCoreApplication::translate("ChatGui", "Automatic voice transmission"));
    m_auto->setCheckable(true);
    m_auto->setMinimumWidth(72);
    m_auto->setFixedHeight(34);
    m_auto->installEventFilter(this);
    m_auto->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_auto, &QWidget::customContextMenuRequested, this, [this]() { showActivationSettings(); });
    m_autoHold.setSingleShot(true); m_autoHold.setInterval(500);
    connect(&m_autoHold, &QTimer::timeout, this, [this]() {
        if (!m_auto->isDown() || !m_auto->isEnabled()) return;
        m_autoLongPressed = true; m_auto->setDown(false);
        showActivationSettings();
    });
    m_auto->setStyleSheet("QPushButton:checked { background: palette(highlight); color: palette(highlighted-text); }");
    row->addWidget(m_auto);
    m_ptt = new QPushButton(QStringLiteral("PTT"), this);
    m_ptt->setObjectName(QStringLiteral("radioPttButton"));
    m_ptt->setAccessibleName(QCoreApplication::translate("ChatGui", "Hold to talk"));
    m_ptt->installEventFilter(this);
    m_ptt->setFixedSize(64, 34);
    m_ptt->setStyleSheet("QPushButton[transmitting=\"true\"] { background: #226d38; color: white; }");
    row->addWidget(m_ptt);
    auto* engine = RadioAudioEngine::instance();
    connect(m_ptt, &QPushButton::pressed, this, [engine]() {
        if (engine->pttLatched()) engine->setPttLatched(false, "button-pressed");
        else engine->talkBegin();
    });
    connect(m_ptt, &QPushButton::released, engine, &RadioAudioEngine::talkEnd);
    connect(m_auto, &QPushButton::clicked, this, [this](bool checked) {
        if (m_autoLongPressed) { updateState(); return; }
        RadioAudioEngine::instance()->applySettings({{QStringLiteral("radio_auto_enabled"), checked ? 1 : 0}});
        updateState();
    });
    connect(engine, &RadioAudioEngine::stateChanged, this, &RadioPanel::updateState);
    connect(engine, &RadioAudioEngine::pttLatchChanged, this, &RadioPanel::updateState);
    connect(engine, &RadioAudioEngine::inputLevelChanged, m_auto, [this]() { m_auto->update(); });
    DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "RadioPanel: VAD button meter uses live speech probability (0..100%%), refresh <=10Hz");
    connect(SoundManager::instance(), &SoundManager::devicesChanged, this, &RadioPanel::updateDevices);
    updateState();
    updateDevices();
    updateDeviceIcons();
    DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "RadioPanel: shared device menubar, native hover/arrows, speaker wave/bold green and participant colors");
}

void RadioPanel::setMembers(MemberListModel* members) {
    if (m_members == members) return;
    if (m_members) disconnect(m_members, nullptr, this, nullptr);
    m_members = members;
    if (members) {
        connect(members, &QAbstractItemModel::dataChanged, this, &RadioPanel::updateSpeakers);
        connect(members, &QAbstractItemModel::modelReset, this, &RadioPanel::updateSpeakers);
        connect(members, &QAbstractItemModel::rowsInserted, this, &RadioPanel::updateSpeakers);
        connect(members, &QAbstractItemModel::rowsRemoved, this, &RadioPanel::updateSpeakers);
        connect(members, &QObject::destroyed, this, [this] { m_members.clear(); updateSpeakers(); });
    }
    updateSpeakers();
}

RadioPanel::~RadioPanel() {
    if (m_ptt->isDown()) {
        DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "RadioPanel: release held button on panel destruction");
        RadioAudioEngine::instance()->talkEnd();
    }
}

/* Второе нажатие Qt приходит как DoubleClick: не отдаём его обычному переключателю pressed. */
bool RadioPanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_auto) {
        if (event->type() == QEvent::MouseButtonPress && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
            m_autoLongPressed = false; m_autoHold.start();
        } else if (event->type() == QEvent::MouseButtonRelease && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
            m_autoHold.stop();
            if (m_autoLongPressed) { m_autoLongPressed = false; m_auto->setDown(false); return true; }
        } else if (event->type() == QEvent::Leave || event->type() == QEvent::Hide || event->type() == QEvent::WindowDeactivate) {
            m_autoHold.stop();
        } else if (event->type() == QEvent::KeyPress) m_autoLongPressed = false;
    }
    if (watched == m_ptt && event->type() == QEvent::MouseButtonDblClick && m_ptt->isEnabled()) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton && m_ptt->rect().contains(mouse->pos())) {
            RadioAudioEngine::instance()->setPttLatched(true, "button-double-click");
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void RadioPanel::showActivationSettings() {
    if (!m_auto->isEnabled()) return;
    m_autoHold.stop(); m_auto->setDown(false);
    if (!m_activationMenu) {
        m_activationMenu = new QMenu(this);
        m_activationMenu->setObjectName(QStringLiteral("radioActivationPopup"));
        auto* action = new QWidgetAction(m_activationMenu);
        auto* contents = new QWidget(m_activationMenu);
        auto* layout = new QVBoxLayout(contents);
        auto* scroll = new QScrollArea(contents);
        scroll->setFrameShape(QFrame::NoFrame); scroll->setWidgetResizable(true);
        m_activationSettings = new RadioActivationSettings;
        scroll->setWidget(m_activationSettings);
        int height = QGuiApplication::primaryScreen()->availableGeometry().height();
        int contentHeight = m_activationSettings->heightForWidth(420);
        if (contentHeight < 0) contentHeight = m_activationSettings->sizeHint().height();
        scroll->setFixedSize(420, std::min(contentHeight + 24, std::min(480, std::max(160, height - 120))));
        layout->addWidget(scroll);
        action->setDefaultWidget(contents); m_activationMenu->addAction(action);
        connect(m_activationMenu, &QMenu::aboutToHide, this, [this]() {
            m_autoLongPressed = false;
            DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "RadioPanel: close activation quick settings");
        });
    }
    m_activationSettings->loadSettings();
    DEBUG_INFO(DEBUG_CATEGORY_RADIO, "RadioPanel: open activation quick settings mode=%d automatic_apply=1",
        RadioAudioEngine::instance()->autoMode());
    m_activationMenu->popup(m_auto->mapToGlobal(QPoint(0, m_auto->height())));
}

void RadioPanel::updateState() {
    auto* engine = RadioAudioEngine::instance();
    QSignalBlocker blocker(m_auto);
    m_auto->setText(engine->autoMode() == 2 ? QString() : engine->autoMode() == 3 ? QStringLiteral("VAD+") : QStringLiteral("VAD"));
    QIcon levelIcon;
    if (engine->autoMode() == 2) {
        QPixmap pixmap(24, 24); pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setPen(QPen(engine->autoEnabled() ? Qt::white : palette().color(QPalette::ButtonText), 3, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(5, 10, 5, 19); painter.drawLine(12, 4, 12, 19); painter.drawLine(19, 7, 19, 19);
        painter.end(); levelIcon = QIcon(pixmap);
    }
    m_auto->setIcon(levelIcon); m_auto->setIconSize(QSize(24, 24));
    m_auto->setChecked(engine->autoEnabled());
    m_auto->setEnabled(engine->isActive() && engine->settingsReady() && !engine->settingsBusy());
    if (!engine->isActive() && m_activationMenu) m_activationMenu->hide();
    m_ptt->setEnabled(engine->isActive() && engine->captureAvailable());
    m_ptt->setText(engine->pttLatched() ? QStringLiteral("PTT \U0001F512") : QStringLiteral("PTT"));
    m_ptt->setToolTip(engine->pttLatched() ? QCoreApplication::translate("ChatGui", "PTT locked. Click to release.")
        : QCoreApplication::translate("ChatGui", "Hold to talk, or double-click to lock PTT. Click again to release. Global shortcuts also work."));
    m_ptt->setAccessibleName(engine->pttLatched() ? QCoreApplication::translate("ChatGui", "PTT locked. Click to release.")
                                        : QCoreApplication::translate("ChatGui", "Hold to talk"));
    bool transmitting = engine->isTransmitting();
    if (m_ptt->property("transmitting").toBool() != transmitting || !m_ptt->property("transmitting").isValid()) {
        m_ptt->setProperty("transmitting", transmitting);
        m_ptt->style()->unpolish(m_ptt);
        m_ptt->style()->polish(m_ptt);
        m_ptt->update();
    }
    QString state;
    if (!engine->captureError().isEmpty()) state = engine->captureError();
    else if (!engine->settingsError().isEmpty()) state = QCoreApplication::translate("ChatGui", "Settings error");
    else if (engine->settingsBusy()) state = QCoreApplication::translate("ChatGui", "Applying settings…");
    else if (!engine->captureAvailable()) state = QCoreApplication::translate("ChatGui", "Microphone unavailable");
    else if (engine->autoEnabled() && !engine->autoAvailable()) state = QCoreApplication::translate("ChatGui", "Automatic transmission unavailable");
    else if (transmitting) state = QString();
    else if (engine->autoEnabled() && engine->autoBlocked()) state = QCoreApplication::translate("ChatGui", "Channel busy");
    else if (!engine->autoEnabled()) state = QString();
    else state = engine->autoMode() == 2 ? QCoreApplication::translate("ChatGui", "Waiting for signal")
                                       : QCoreApplication::translate("ChatGui", "VAD listening");
    m_state = state; updateSpeakers();
    const QString toggle = engine->autoEnabled() ? QCoreApplication::translate("ChatGui", "Disable automatic voice transmission")
                                                : QCoreApplication::translate("ChatGui", "Enable automatic voice transmission");
    const QString mode = engine->autoMode() == 2 ? QCoreApplication::translate("ChatGui", "By signal level") :
        engine->autoMode() == 3 ? QCoreApplication::translate("ChatGui", "VAD + level") : QStringLiteral("VAD");
    const QString hint = QCoreApplication::translate("ChatGui", "Hold or right-click to adjust activation mode and thresholds");
    m_auto->setToolTip(mode + "\n" + toggle + "\n" + hint);
    m_auto->setAccessibleName(mode + ". " + toggle); m_auto->setAccessibleDescription(hint);
    m_auto->update();
}

void RadioPanel::updateSpeakers() {
    auto* engine = RadioAudioEngine::instance();
    QStringList names, styled; bool speaking = engine->isTransmitting(); int muted = 0;
    if (m_members && engine->isActive()) for (int row = 0; row < m_members->rowCount(); ++row) {
        const auto index = m_members->index(row, 0); const int flags = index.data(Qt::UserRole + 3).toInt();
        if (!index.data(MemberListModel::RadioSpeakingRole).toBool() || (flags & CHAT_MEMBER_FLAG_DELETED)) continue;
        speaking = true;
        const bool silent = flags & CHAT_MEMBER_FLAG_RADIO_MUTED; muted += silent;
        const QString name = index.data(Qt::DisplayRole).toString(); names.append(name);
        styled.append(QStringLiteral("<span style=\"color:%1; font-weight:600\">%2</span>")
            .arg(silent ? QStringLiteral("#EF5350") : QStringLiteral("#4CAF50"), name.toHtmlEscaped()));
    }
    QString text = styled.join(QStringLiteral(", ")), hint = names.join(QStringLiteral(", "));
    if (!m_state.isEmpty()) {
        if (!text.isEmpty()) { text += QStringLiteral(" · "); hint += QStringLiteral(" · "); }
        text += m_state.toHtmlEscaped(); hint += engine->settingsError().isEmpty() ? m_state : engine->settingsError();
    }
    if (m_status->text() != text || m_status->accessibleName() != hint || m_speaking != speaking) {
        DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "RadioPanel: speaker indicator active=%d names=%d muted=%d status=%d",
            speaking, int(names.size()), muted, !m_state.isEmpty());
        m_status->setText(text);
        m_status->setToolTip(hint.isEmpty() ? QString() :
            QStringLiteral("<qt>%1</qt>").arg(hint.toHtmlEscaped().replace('\n', QStringLiteral("<br>"))));
        m_status->setAccessibleName(hint);
    }
    if (m_speaking != speaking) { m_speaking = speaking; updateDeviceIcons(); }
}

void RadioPanel::updateDeviceIcons() {
    const QColor color = palette().color(QPalette::ButtonText);
    m_input->menuAction()->setIcon(outlineIcon(microphoneOutline(), color, 24));
    m_output->menuAction()->setIcon(outputIcon(m_speaking ? QColor("#4CAF50") : color, m_speaking));
}

void RadioPanel::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange) updateDeviceIcons();
}

/* Список перечитывается при открытии меню: подключённые устройства видны без перезапуска. */
void RadioPanel::populateDevices(QMenu* menu, bool capture) {
    menu->clear();
    auto* group = menu->findChild<QActionGroup*>();
    QString selected = capture ? SoundManager::instance()->captureDevice() : SoundManager::instance()->playbackDevice();
    auto add = [&](const QString& name, const QString& id) {
        auto* action = menu->addAction(name);
        action->setCheckable(true);
        action->setChecked(id == selected);
        group->addAction(action);
        connect(action, &QAction::triggered, this, [this, id, capture]() { selectDevice(id, capture); });
    };
    add(QCoreApplication::translate("ChatGui", "Default (auto)"), QString());
    auto devices = capture ? SoundManager::enumerateCaptureDevices() : SoundManager::enumeratePlaybackDevices();
    for (const auto& device : devices) add(device.name, device.id);
}

void RadioPanel::selectDevice(const QString& id, bool capture) {
    auto* inst = gui_bridge_get_inst();
    if (!inst || !gui_bridge_is_uasync_ready()) {
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "RadioPanel: cannot save device: core unavailable");
        m_state = QCoreApplication::translate("ChatGui", "Core unavailable"); updateSpeakers();
        return;
    }
    const char* key = capture ? "audio_input_id" : "audio_output_id";
    QByteArray value = id.toUtf8();
    auto* request = static_cast<save_ui_state_arg*>(u_calloc(1, sizeof(save_ui_state_arg)));
    if (!request) {
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "RadioPanel: device settings allocation failed");
        m_state = QCoreApplication::translate("ChatGui", "Failed to select device"); updateSpeakers();
        return;
    }
    size_t keyLen = strlen(key), valueLen = value.size();
    if (keyLen + valueLen + 2 > sizeof(request->data)) {
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "RadioPanel: device ID too long bytes=%zu", valueLen);
        u_free(request);
        m_state = QCoreApplication::translate("ChatGui", "Failed to select device"); updateSpeakers();
        return;
    }
    request->inst = inst;
    memcpy(request->data, key, keyLen + 1);
    memcpy(request->data + keyLen + 1, value.constData(), valueLen + 1);
    gui_bridge_post_uasync_fn(chat_core_save_ui_state_trampoline, request);
    auto* manager = SoundManager::instance();
    DEBUG_INFO(DEBUG_CATEGORY_RADIO, "RadioPanel: selected %s default=%d", capture ? "input" : "output", id.isEmpty());
    manager->setDevices(capture ? manager->playbackDevice() : id, capture ? id : manager->captureDevice());
}

void RadioPanel::updateDevices() {
    auto* manager = SoundManager::instance();
    for (auto* menu : {m_input, m_output}) {
        bool capture = menu == m_input;
        QString id = capture ? manager->captureDevice() : manager->playbackDevice();
        QString name = QCoreApplication::translate("ChatGui", "Default (auto)");
        if (!id.isEmpty()) {
            name = QCoreApplication::translate("ChatGui", "Selected device unavailable; using an available device");
            auto devices = capture ? manager->enumerateCaptureDevices() : manager->enumeratePlaybackDevices();
            for (const auto& device : devices) if (device.id == id) { name = device.name; break; }
        }
        const QString title = QCoreApplication::translate("ChatGui", capture ? "Microphone" : "Audio output");
        const QString description = title + ": " + name;
        menu->menuAction()->setToolTip(description); menu->menuAction()->setText(description); menu->setAccessibleName(description);
    }
}

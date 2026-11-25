#include <QCoreApplication>
#include "ptt_hotkey_manager.h"
#include "radio_audio_engine.h"
#include "../db/db_manager.h"
#include "../../lib/debug_config.h"

#include <QApplication>
#include <QWidget>
#ifdef UTUN_PTT_PORTAL
#include "ptt_portal.h"
#endif
#ifdef Q_OS_WIN
#include <qt_windows.h>
#elif defined(Q_OS_LINUX)
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#endif

PttHotkeyManager* PttHotkeyManager::instance() {
    static auto* manager = new PttHotkeyManager();
    return manager;
}

PttHotkeyManager::PttHotkeyManager() : QObject(qApp) {
    m_clock.start();
    connect(RadioAudioEngine::instance(), &RadioAudioEngine::pttLatchChanged, this, [this]() { m_activation.reset(); });
    connect(RadioAudioEngine::instance(), &RadioAudioEngine::stateChanged, this, [this]() {
        auto* engine = RadioAudioEngine::instance();
        if (!engine->isActive() || !engine->captureAvailable())
            resetHotkey(engine->isActive() ? "microphone-unavailable" : "radio-stopped");
    });
#ifdef Q_OS_LINUX
    if (isSupported() && !usesPortal()) m_display = XOpenDisplay(nullptr);
    if (isSupported() && !usesPortal() && !m_display) DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "PTT: cannot open X11 display");
#endif
#ifdef UTUN_PTT_PORTAL
    if (usesPortal()) {
        m_portal = new PttPortal(this);
        connect(m_portal, &PttPortal::talkingChanged, this, &PttHotkeyManager::updatePressed);
        connect(m_portal, &PttPortal::resetRequested, this, [this](const QString& reason) {
            resetHotkey(qPrintable(reason));
        });
        connect(m_portal, &PttPortal::descriptionsChanged, this, &PttHotkeyManager::portalChanged);
        connect(m_portal, &PttPortal::statusChanged, this, &PttHotkeyManager::portalChanged);
    }
#endif
    m_timer.setInterval(10);
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, &PttHotkeyManager::pollKeys);
}

PttHotkeyManager::~PttHotkeyManager() {
    m_timer.stop();
#ifdef Q_OS_LINUX
    if (m_display) XCloseDisplay(static_cast<Display*>(m_display));
#endif
}

bool PttHotkeyManager::isSupported() {
#ifdef Q_OS_WIN
    return true;
#elif defined(Q_OS_LINUX)
    return usesPortal() || QGuiApplication::platformName() == "xcb";
#else
    return false;
#endif
}

void PttHotkeyManager::init(DbManager* db) {
    m_db = db;
    reload();
#ifdef UTUN_PTT_PORTAL
    if (m_portal) { m_portal->initialize(); return; }
#endif
    if (isSupported()) {
#ifdef Q_OS_LINUX
        if (!m_display) return;
#endif
        m_timer.start();
        DEBUG_INFO(DEBUG_CATEGORY_RADIO, "PTT: global keyboard polling enabled interval=10ms");
    } else DEBUG_WARN(DEBUG_CATEGORY_RADIO, "PTT: global keyboard unavailable platform=%s", qPrintable(QGuiApplication::platformName()));
}

QString PttHotkeyManager::hotkey() const { return pttEncodeChord(m_chord); }

void PttHotkeyManager::setHotkey(const QString& value) {
    if (usesPortal()) return;
    resetHotkey("shortcut-configured");
    m_chord = pttDecodeChord(value);
    m_activation.pressed = pttChordHeld(m_chord, m_held);
    if (!value.isEmpty() && m_chord.isEmpty()) DEBUG_WARN(DEBUG_CATEGORY_RADIO, "PTT: invalid key chord '%s'", qPrintable(value));
    DEBUG_INFO(DEBUG_CATEGORY_RADIO, "PTT: configured shortcut=[%s] codes=[%s]", qPrintable(describe(m_chord)), qPrintable(hotkey()));
}

void PttHotkeyManager::reload() {
    setDoublePressEnabled(!m_db || !m_db->isOpen() || m_db->getUiState("radio_ptt_double_press") != "0");
    setHotkey(m_db && m_db->isOpen() ? m_db->getUiState("radio_ptt_hotkey") : QString());
}

void PttHotkeyManager::setDoublePressEnabled(bool enabled) {
    if (m_doublePressEnabled == enabled) return;
    resetHotkey("double-press-setting-changed");
    m_doublePressEnabled = enabled;
    DEBUG_INFO(DEBUG_CATEGORY_RADIO, "PTT: shortcut double-press locking enabled=%d", enabled);
}

/* Одинаковое поведение физического сочетания и Wayland-сигналов, без реакции на автоповтор. */
void PttHotkeyManager::updatePressed(bool pressed) {
    auto* engine = RadioAudioEngine::instance();
    if (m_capturing || !engine->isActive() || !engine->captureAvailable()) {
        resetHotkey(m_capturing ? "recording" : engine->isActive() ? "microphone-unavailable" : "radio-stopped");
        m_activation.pressed = pressed;
        return;
    }
    if (m_activation.pressed == pressed) return;
    bool doublePress = m_activation.observe(pressed, m_clock.elapsed(), m_doublePressEnabled ? QApplication::doubleClickInterval() : 0);
    DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "PTT: shortcut pressed=%d double_press=%d latched=%d group=%s",
                pressed, doublePress, engine->pttLatched(), qPrintable(engine->groupId()));
    if (pressed && engine->pttLatched()) engine->setPttLatched(false, "shortcut-pressed");
    else if (doublePress) engine->setPttLatched(true, "shortcut-double-press");
    else engine->setHotkeyTalking(pressed);
}

/* Сброс не превращает ещё удерживаемую клавишу в новое нажатие после восстановления рации. */
void PttHotkeyManager::resetHotkey(const char* reason) {
    if (m_activation.lastPress >= 0)
        DEBUG_INFO(DEBUG_CATEGORY_RADIO, "PTT: shortcut reset reason=%s pressed=%d double_press_pending=%d",
                   reason, m_activation.pressed, m_activation.lastPress >= 0);
    m_activation.reset();
    auto* engine = RadioAudioEngine::instance();
    engine->setPttLatched(false, reason);
    engine->setHotkeyTalking(false);
}

bool PttHotkeyManager::readKeys(PttKeyChord& held) const {
#ifdef Q_OS_WIN
    for (int key = 8; key < 256; key++) {
        // Общие VK_SHIFT/CONTROL/MENU дублируют левые/правые — исключаем их.
        if (key == VK_SHIFT || key == VK_CONTROL || key == VK_MENU) continue;
        if (GetAsyncKeyState(key) & 0x8000) held.insert(key);
    }
    return true;
#elif defined(Q_OS_LINUX)
    if (!m_display) return false;
    char keys[32];
    if (!XQueryKeymap(static_cast<Display*>(m_display), keys)) return false;
    for (int key = 8; key < 256; key++) if ((unsigned char)keys[key / 8] & (1u << (key % 8))) held.insert(key);
    return true;
#else
    return false;
#endif
}

void PttHotkeyManager::pollKeys() {
    PttKeyChord held;
    if (!readKeys(held)) {
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "PTT: keyboard query failed; transmission disabled");
        m_timer.stop();
        resetHotkey("keyboard-query-failed");
        return;
    }
    if (m_capturing && !m_captureArmed && held.isEmpty()) {
        m_captureArmed = true;
        DEBUG_INFO(DEBUG_CATEGORY_RADIO, "PTT: capture armed after all keys released");
    }
    bool changed = held != m_held;
    m_held = held;
    if (m_capturing && changed) {
        DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "PTT: capture poll armed=%d held=[%s] codes=[%s] application_state=%d",
                    m_captureArmed, qPrintable(describe(held)), qPrintable(pttEncodeChord(held)), int(QGuiApplication::applicationState()));
    }
    updatePressed(pttChordHeld(m_chord, held));
    if (m_capturing && m_captureArmed && changed) emit keysChanged(held);
}

bool PttHotkeyManager::beginCapture() {
    if (!m_timer.isActive() || m_capturing) {
        DEBUG_WARN(DEBUG_CATEGORY_RADIO, "PTT: capture unavailable polling=%d capturing=%d", m_timer.isActive(), m_capturing);
        return false;
    }
    m_capturing = true;
    PttKeyChord held;
    if (!readKeys(held)) {
        m_capturing = false;
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "PTT: cannot read keys when starting capture");
        return false;
    }
    m_captureArmed = held.isEmpty(); // ждём отпускания клавиши, которой нажали кнопку
    resetHotkey("recording-started");
    DEBUG_INFO(DEBUG_CATEGORY_RADIO, "PTT: capture started armed=%d held=[%s] codes=[%s] platform=%s interval=%dms",
               m_captureArmed, qPrintable(describe(held)), qPrintable(pttEncodeChord(held)),
               qPrintable(QGuiApplication::platformName()), m_timer.interval());
    if (!m_captureArmed) {
        DEBUG_INFO(DEBUG_CATEGORY_RADIO, "PTT: capture waiting for ALL keys to be released; keys pressed meanwhile are ignored");
    }
    return true;
}

void PttHotkeyManager::endCapture() {
    if (!m_capturing) return;
    m_capturing = false;
    DEBUG_INFO(DEBUG_CATEGORY_RADIO, "PTT: capture finished armed=%d polling=%d last_held=[%s] codes=[%s]",
               m_captureArmed, m_timer.isActive(), qPrintable(describe(m_held)), qPrintable(pttEncodeChord(m_held)));
}

QString PttHotkeyManager::keyName(int key) const {
#ifdef Q_OS_WIN
    switch (key) {
    case VK_LSHIFT: return "ShiftL"; case VK_RSHIFT: return "ShiftR";
    case VK_LCONTROL: return "CtrlL"; case VK_RCONTROL: return "CtrlR";
    case VK_LMENU: return "AltL"; case VK_RMENU: return "AltR";
    case VK_LWIN: return "WinL"; case VK_RWIN: return "WinR";
    }
    UINT scan = MapVirtualKeyW(key, MAPVK_VK_TO_VSC_EX);
    LONG param = LONG((scan & 0xff) << 16);
    if (scan & 0xff00) param |= 1 << 24;
    wchar_t name[128];
    if (GetKeyNameTextW(param, name, 128)) return QString::fromWCharArray(name);
#elif defined(Q_OS_LINUX)
    if (m_display) {
        KeySym sym = XkbKeycodeToKeysym(static_cast<Display*>(m_display), key, 0, 0);
        switch (sym) {
        case XK_Shift_L: return "ShiftL"; case XK_Shift_R: return "ShiftR";
        case XK_Control_L: return "CtrlL"; case XK_Control_R: return "CtrlR";
        case XK_Alt_L: return "AltL"; case XK_Alt_R: return "AltR";
        case XK_Super_L: return "WinL"; case XK_Super_R: return "WinR";
        case XK_space: return "Space";
        }
        const char* name = XKeysymToString(sym);
        if (name) return QString::fromLatin1(name);
    }
#endif
    return QCoreApplication::translate("ChatGui", "Key %1").arg(key);
}

QString PttHotkeyManager::describe(const PttKeyChord& chord) const {
    auto keys = chord.values();
    std::sort(keys.begin(), keys.end());
    QStringList names;
    for (int key : keys) names << keyName(key);
    return names.isEmpty() ? QCoreApplication::translate("ChatGui", "Not set") : names.join(" + ");
}

bool PttHotkeyManager::usesPortal() {
#ifdef UTUN_PTT_PORTAL
    return qEnvironmentVariable("XDG_SESSION_TYPE") == "wayland" || QGuiApplication::platformName().startsWith("wayland");
#else
    return false;
#endif
}
void PttHotkeyManager::configurePortal(QWidget* parentWindow) {
#ifdef UTUN_PTT_PORTAL
    if (!m_portal) return;
    QString id;
    if (parentWindow && QGuiApplication::platformName() == "xcb")
        id = "x11:" + QString::number(qulonglong(parentWindow->window()->winId()), 16);
    m_portal->configure(id);
#else
    Q_UNUSED(parentWindow);
#endif
}
QString PttHotkeyManager::portalDescription() const {
#ifdef UTUN_PTT_PORTAL
    if (m_portal) return m_portal->description();
#endif
    return {};
}
QString PttHotkeyManager::portalStatus() const {
#ifdef UTUN_PTT_PORTAL
    if (m_portal) return m_portal->status();
#endif
    return {};
}
bool PttHotkeyManager::portalBusy() const {
#ifdef UTUN_PTT_PORTAL
    if (m_portal) return m_portal->busy();
#endif
    return false;
}
void PttHotkeyManager::inhibitPortal(bool inhibited) {
#ifdef UTUN_PTT_PORTAL
    if (m_portal) m_portal->setInhibited(inhibited);
#else
    Q_UNUSED(inhibited);
#endif
}

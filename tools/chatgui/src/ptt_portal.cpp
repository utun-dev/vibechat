#include <QCoreApplication>
#include "ptt_portal.h"
#include "../../lib/debug_config.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDBusVariant>
#include <QUuid>

static const QString kService = "org.freedesktop.portal.Desktop";
static const QString kPath = "/org/freedesktop/portal/desktop";
static const QString kInterface = "org.freedesktop.portal.GlobalShortcuts";
static const QString kRequestInterface = "org.freedesktop.portal.Request";
static const QString kSessionInterface = "org.freedesktop.portal.Session";

QDBusArgument& operator<<(QDBusArgument& arg, const PttPortalShortcut& shortcut) {
    arg.beginStructure(); arg << shortcut.id << shortcut.properties; arg.endStructure(); return arg;
}
const QDBusArgument& operator>>(const QDBusArgument& arg, PttPortalShortcut& shortcut) {
    arg.beginStructure(); arg >> shortcut.id >> shortcut.properties; arg.endStructure(); return arg;
}

PttPortal::PttPortal(QObject* parent, const QString& sessionToken) : QObject(parent), m_sessionToken(sessionToken) {
    qDBusRegisterMetaType<PttPortalShortcut>();
    qDBusRegisterMetaType<PttPortalShortcuts>();
    m_requestTimer.setSingleShot(true);
    connect(&m_requestTimer, &QTimer::timeout, this, [this]() { fail("request timed out"); });
    auto* watcher = new QDBusServiceWatcher(kService, QDBusConnection::sessionBus(), QDBusServiceWatcher::WatchForUnregistration, this);
    connect(watcher, &QDBusServiceWatcher::serviceUnregistered, this, [this]() { fail("portal service disappeared"); });
    auto bus = QDBusConnection::sessionBus();
    bool ok = bus.connect(kService, kPath, kInterface, "Activated", this,
                          SLOT(activated(QDBusObjectPath,QString,qulonglong,QVariantMap)));
    ok &= bus.connect(kService, kPath, kInterface, "Deactivated", this,
                       SLOT(deactivated(QDBusObjectPath,QString,qulonglong,QVariantMap)));
    ok &= bus.connect(kService, kPath, kInterface, "ShortcutsChanged", this,
                       SLOT(shortcutsChanged(QDBusObjectPath,PttPortalShortcuts)));
    if (!ok) fail("cannot subscribe to portal signals");
}

PttPortal::~PttPortal() { closeSession(); }

bool PttPortal::busy() const {
    return m_state == State::Checking || m_state == State::Creating || m_state == State::Listing || m_state == State::Binding;
}
void PttPortal::setStatus(const QString& text) { m_status = text; emit statusChanged(); }

void PttPortal::initialize() {
    if (busy() || m_state == State::Ready || m_state == State::Unbound) return;
    m_state = State::Checking;
    setStatus(QCoreApplication::translate("ChatGui", "Connecting global shortcuts…"));
    auto message = QDBusMessage::createMethodCall(kService, kPath, "org.freedesktop.DBus.Properties", "Get");
    message.setArguments({kInterface, "version"});
    auto* watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
    uint epoch = m_epoch;
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, epoch]() {
        QDBusPendingReply<QDBusVariant> reply = *watcher;
        watcher->deleteLater();
        if (epoch != m_epoch || m_state != State::Checking) return;
        if (reply.isError()) { fail(reply.error().message()); return; }
        uint version = reply.value().variant().toUInt();
        if (version < 2) { fail(QString("GlobalShortcuts version %1; version 2 required").arg(version)); return; }
        DEBUG_INFO(DEBUG_CATEGORY_RADIO, "PTT portal: connected version=%u", version);
        m_state = State::Creating;
        // Стабильный session token сохраняет идентичность обычного приложения в KDE без sandbox app_id.
        request("CreateSession", {}, {{"session_handle_token", m_sessionToken}});
    });
}

void PttPortal::request(const QString& method, const QVariantList& arguments, QVariantMap options) {
    auto bus = QDBusConnection::sessionBus();
    QString sender = bus.baseService().mid(1).replace('.', '_');
    QString token = "utun_ptt_" + QUuid::createUuid().toString(QUuid::Id128);
    m_request = kPath + "/request/" + sender + "/" + token;
    if (!bus.connect(kService, m_request, kRequestInterface, "Response", this, SLOT(response(uint,QVariantMap,QDBusMessage)))) {
        fail("cannot subscribe to request response"); return;
    }
    options.insert("handle_token", token);
    auto message = QDBusMessage::createMethodCall(kService, kPath, kInterface, method);
    QVariantList args = arguments; args << options; message.setArguments(args);
    QString expected = m_request;
    DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "PTT portal: %s request=%s session=%s", qPrintable(method), qPrintable(expected), qPrintable(m_session));
    // Binding может ждать действий пользователя; остальные операции ограничены 30 секундами.
    if (m_state != State::Binding) m_requestTimer.start(30000);
    auto* watcher = new QDBusPendingCallWatcher(bus.asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, expected]() {
        QDBusPendingReply<QDBusObjectPath> reply = *watcher;
        watcher->deleteLater();
        if (m_request != expected) return; // Response уже обработан или сессия закрыта
        if (reply.isError()) { fail(reply.error().message()); return; }
        if (reply.value().path() != expected) fail("portal returned unexpected request path");
    });
}

void PttPortal::disconnectRequest() {
    m_requestTimer.stop();
    if (m_request.isEmpty()) return;
    QDBusConnection::sessionBus().disconnect(kService, m_request, kRequestInterface, "Response", this, SLOT(response(uint,QVariantMap,QDBusMessage)));
    m_request.clear();
}

void PttPortal::response(uint code, const QVariantMap& results, const QDBusMessage& message) {
    if (m_request.isEmpty() || message.path() != m_request) return;
    State state = m_state;
    disconnectRequest();
    DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "PTT portal: response state=%d code=%u", int(state), code);
    if (code != 0) {
        fail(QString("request %1: response=%2%3").arg(int(state)).arg(code).arg(code == 1 ? " (cancelled)" : ""));
        if (code == 1) setStatus(QCoreApplication::translate("ChatGui", "Assignment cancelled. Click the button to try again."));
        return;
    }
    if (state == State::Creating) {
        m_session = results.value("session_handle").toString();
        if (!m_session.startsWith(kPath + "/session/")) { fail("invalid session handle"); return; }
        if (!QDBusConnection::sessionBus().connect(kService, m_session, kSessionInterface, "Closed", this, SLOT(sessionClosed()))) {
            fail("cannot subscribe to session closure"); return;
        }
        m_state = State::Listing;
        request("ListShortcuts", {QVariant::fromValue(QDBusObjectPath(m_session))}, {});
    } else if (state == State::Listing) {
        auto shortcuts = qdbus_cast<PttPortalShortcuts>(results.value("shortcuts"));
        updateDescriptions(shortcuts);
        bool known = false;
        for (const auto& shortcut : shortcuts) if (isPttId(shortcut.id)) known = true;
        m_configureAfterBind = known && m_configureRequested;
        m_state = State::Unbound;
        if (known || m_configureRequested) bind();
        else setStatus(QCoreApplication::translate("ChatGui", "Assign a shortcut in system settings."));
    } else if (state == State::Binding) {
        m_state = State::Ready;
        updateDescriptions(qdbus_cast<PttPortalShortcuts>(results.value("shortcuts")));
        setStatus(QCoreApplication::translate("ChatGui", "Global shortcuts connected."));
        DEBUG_INFO(DEBUG_CATEGORY_RADIO, "PTT portal: bindings ready session=%s", qPrintable(m_session));
        // Первое BindShortcuts само открывает окно назначения, повторное Configure — отдельный вызов.
        m_configureRequested = false;
        if (m_configureAfterBind) { m_configureAfterBind = false; openConfiguration(); }
    } else fail("unexpected request response");
}

bool PttPortal::isPttId(const QString& id) { return id == "ptt"; }

void PttPortal::bind() {
    PttPortalShortcuts shortcuts{{"ptt", {{"description", QCoreApplication::translate("ChatGui", "Radio: transmit")}}}};
    m_state = State::Binding;
    setStatus(QCoreApplication::translate("ChatGui", "Assigning global shortcuts…"));
    request("BindShortcuts", {QVariant::fromValue(QDBusObjectPath(m_session)), QVariant::fromValue(shortcuts), m_parentWindow}, {});
}

void PttPortal::configure(const QString& parentWindow) {
    m_parentWindow = parentWindow;
    if (m_state == State::Ready) { openConfiguration(); return; }
    m_configureRequested = true;
    if (m_state == State::Unbound) bind();
    else initialize();
}

void PttPortal::openConfiguration() {
    auto message = QDBusMessage::createMethodCall(kService, kPath, kInterface, "ConfigureShortcuts");
    message.setArguments({QVariant::fromValue(QDBusObjectPath(m_session)), m_parentWindow, QVariantMap{}});
    auto* watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
    uint epoch = m_epoch;
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, epoch]() {
        QDBusPendingReply<> reply = *watcher;
        watcher->deleteLater();
        if (epoch != m_epoch) return;
        if (reply.isError()) { fail(reply.error().message()); return; }
        DEBUG_INFO(DEBUG_CATEGORY_RADIO, "PTT portal: system configuration opened");
    });
}

void PttPortal::updateDescriptions(const PttPortalShortcuts& shortcuts) {
    m_description.clear();
    for (const auto& shortcut : shortcuts) {
        if (!isPttId(shortcut.id)) continue;
        m_description = shortcut.properties.value("trigger_description").toString();
        DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "PTT portal: %s trigger=%s", qPrintable(shortcut.id), qPrintable(m_description));
    }
    emit descriptionsChanged();
}

void PttPortal::activated(const QDBusObjectPath& session, const QString& id, qulonglong, const QVariantMap&) {
    if (session.path() != m_session || m_state != State::Ready || !isPttId(id) || m_inhibited) return;
    m_pressed = true; updateTalking();
}
void PttPortal::deactivated(const QDBusObjectPath& session, const QString& id, qulonglong, const QVariantMap&) {
    if (session.path() != m_session || !isPttId(id)) return;
    m_pressed = false; updateTalking();
}
void PttPortal::shortcutsChanged(const QDBusObjectPath& session, const PttPortalShortcuts& shortcuts) {
    if (session.path() != m_session) return;
    m_pressed = false; updateTalking();
    emit resetRequested("portal-shortcut-changed");
    updateDescriptions(shortcuts);
}
void PttPortal::setInhibited(bool inhibited) {
    m_inhibited = inhibited;
    if (inhibited) {
        m_pressed = false; updateTalking();
        emit resetRequested("portal-inhibited");
    }
}
void PttPortal::updateTalking() {
    bool talking = !m_inhibited && m_pressed;
    if (m_talking == talking) return;
    m_talking = talking;
    DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "PTT portal: shortcut pressed=%d inhibited=%d", m_pressed, m_inhibited);
    emit talkingChanged(talking);
}

void PttPortal::closeSession() {
    m_epoch++;
    auto bus = QDBusConnection::sessionBus();
    if (!m_request.isEmpty()) {
        auto close = QDBusMessage::createMethodCall(kService, m_request, kRequestInterface, "Close");
        close.setAutoStartService(false);
        bus.asyncCall(close);
        disconnectRequest();
    }
    if (!m_session.isEmpty()) {
        bus.disconnect(kService, m_session, kSessionInterface, "Closed", this, SLOT(sessionClosed()));
        auto close = QDBusMessage::createMethodCall(kService, m_session, kSessionInterface, "Close");
        close.setAutoStartService(false);
        bus.asyncCall(close);
        m_session.clear();
    }
}
void PttPortal::fail(const QString& detail) {
    DEBUG_WARN(DEBUG_CATEGORY_RADIO, "PTT portal: %s", qPrintable(detail));
    closeSession();
    m_pressed = false; updateTalking();
    emit resetRequested("portal-failed");
    m_configureRequested = m_configureAfterBind = false;
    m_state = State::Failed;
    m_description.clear(); emit descriptionsChanged();
    setStatus(QCoreApplication::translate("ChatGui", "Global shortcuts unavailable. Try assigning again; see the radio log for details."));
}
void PttPortal::sessionClosed() { fail("portal session closed"); }

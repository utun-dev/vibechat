#include "single_instance.h"
#include "debug_config.h"
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QLocalSocket>
#include <QTimer>
#include <QWidget>
#include <QRegularExpression>
#include <utility>
#include "../../../src/chat/object_link.h"
#ifdef _WIN32
#include <windows.h>
#endif

static const QByteArray activationRequest("ACTIVATE\n");
static const QByteArray activationReply("OK\n");

// Общий путь для повторного запуска, трея и уведомлений; showNormal потерял бы maximized.
void showAndActivateWindow(QWidget* window) {
    if (!window) { DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "GUI activation: window is unavailable"); return; }
    QWidget* modal = QApplication::activeModalWidget();
    if (window->isMinimized()) window->setWindowState(window->windowState() & ~Qt::WindowMinimized);
    window->show();
    QWidget* target = modal ? modal : window;
    if (target != window) {
        if (target->isMinimized()) target->setWindowState(target->windowState() & ~Qt::WindowMinimized);
        target->show();
    }
    target->raise(); target->activateWindow();
#ifdef _WIN32
    if (QApplication::platformName() == QStringLiteral("windows") && !SetForegroundWindow(reinterpret_cast<HWND>(target->winId())))
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "GUI activation: Windows refused foreground pid=%lld", QCoreApplication::applicationPid());
#endif
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "GUI activation: visible=%d minimized=%d maximized=%d modal=%d active=%d",
        window->isVisible(), window->isMinimized(), window->isMaximized(), target->isModal(), target->isActiveWindow());
}

// Канонический путь объединяет обращения через symlink; домашний каталог разделяет пользователей.
SingleInstance::SingleInstance(const QString& profileDirectory, QObject* parent)
    : QObject(parent), m_lock(QDir(profileDirectory).filePath(QStringLiteral("vibechat.lock"))) {
    QString path = QFileInfo(profileDirectory).canonicalFilePath();
    if (path.isEmpty()) { m_error = QStringLiteral("Profile directory does not exist: %1").arg(profileDirectory); return; }
    QString identity = QDir::homePath() + '\n' + path;
#ifdef _WIN32
    identity = identity.toCaseFolded();
#endif
    m_name = QStringLiteral("vibechat-") + QString::fromLatin1(
        QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha256).toHex().left(32));
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    connect(&m_server, &QLocalServer::newConnection, this, &SingleInstance::acceptConnections);
}

SingleInstance::~SingleInstance() {
    if (m_lock.isLocked()) {
        DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "GUI instance: stopping pid=%lld server=%s",
            QCoreApplication::applicationPid(), qPrintable(m_name));
        m_server.close();
    }
}

SingleInstance::Result SingleInstance::fail(const QString& error) {
    m_error = error;
    DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "GUI instance: %s", qPrintable(error));
    return Error;
}

// Только владелец блокировки может убирать оставшийся после аварии адрес локального канала.
SingleInstance::Result SingleInstance::start(const QString& url) {
    if (m_started) return fail(QStringLiteral("Instance startup requested twice"));
    m_started = true;
    if (!url.isEmpty() && (!url.startsWith(QStringLiteral("utun://")) || url.toUtf8().size() > OBJECT_LINK_MAX ||
        url.contains(QRegularExpression(QStringLiteral("[\\x00-\\x20\\x7f]"))))) return fail(QStringLiteral("Invalid startup URL"));
    m_pendingUrl = url; m_pending = !url.isEmpty();
    if (!m_error.isEmpty()) return fail(m_error);
    if (!m_lock.tryLock()) {
        if (m_lock.error() != QLockFile::LockFailedError)
            return fail(QStringLiteral("Cannot lock profile %1: error=%2").arg(m_lock.fileName()).arg(int(m_lock.error())));
        return activatePrimary();
    }
    if (!m_server.listen(m_name)) {
        if (m_server.serverError() != QAbstractSocket::AddressInUseError ||
            !QLocalServer::removeServer(m_name) || !m_server.listen(m_name))
            return fail(QStringLiteral("Cannot listen on %1: %2").arg(m_name, m_server.errorString()));
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "GUI instance: removed stale local endpoint server=%s", qPrintable(m_name));
    }
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "GUI instance: primary pid=%lld profile=%s server=%s",
        QCoreApplication::applicationPid(), qPrintable(m_lock.fileName()), qPrintable(m_name));
    return Primary;
}

// До появления канала владелец может ещё инициализироваться; после подключения ждём именно ACK.
SingleInstance::Result SingleInstance::activatePrimary() {
    const QByteArray request = m_pendingUrl.isEmpty() ? activationRequest : QByteArray("OPEN ") + m_pendingUrl.toUtf8() + '\n';
    QLocalSocket socket;
    QEventLoop wait;
    QTimer deadline, retry;
    deadline.setSingleShot(true); retry.setSingleShot(true);
    bool connected = false, accepted = false;
    QByteArray reply;
    QString error;
    connect(&deadline, &QTimer::timeout, &wait, [&] {
        error = QStringLiteral("Running vibechat did not acknowledge activation within 5 seconds"); wait.quit();
    });
    connect(&retry, &QTimer::timeout, &socket, [&] { socket.abort(); socket.connectToServer(m_name); });
    connect(&socket, &QLocalSocket::connected, &wait, [&] {
        connected = true; retry.stop();
        qint64 pid = 0; QString host, application;
        bool ownerKnown = m_lock.getLockInfo(&pid, &host, &application);
#ifdef _WIN32
        if (!ownerKnown || pid <= 0 || quint64(pid) > MAXDWORD)
            DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "GUI instance: cannot identify foreground recipient");
        else if (!AllowSetForegroundWindow(DWORD(pid)))
            DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "GUI instance: foreground permission failed pid=%lld error=%lu",
                pid, static_cast<unsigned long>(GetLastError()));
#endif
        DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "GUI instance: requesting activation owner_pid=%lld known=%d server=%s",
            pid, ownerKnown, qPrintable(m_name));
        if (socket.write(request) != request.size()) {
            error = QStringLiteral("Cannot send activation: %1").arg(socket.errorString()); wait.quit();
        }
    });
    connect(&socket, &QLocalSocket::readyRead, &wait, [&] {
        reply += socket.readAll();
        if (!activationReply.startsWith(reply)) {
            error = QStringLiteral("Invalid activation acknowledgement"); wait.quit();
        } else if (reply == activationReply) { accepted = true; wait.quit(); }
    });
    connect(&socket, &QLocalSocket::disconnected, &wait, [&] {
        if (!accepted) { error = QStringLiteral("Running vibechat disconnected before activation acknowledgement"); wait.quit(); }
    });
    connect(&socket, &QLocalSocket::errorOccurred, &wait, [&](QLocalSocket::LocalSocketError code) {
        if (accepted) return;
        if (!connected && (code == QLocalSocket::ServerNotFoundError || code == QLocalSocket::ConnectionRefusedError)) {
            retry.start(50); return;
        }
        error = QStringLiteral("Cannot activate running vibechat: %1").arg(socket.errorString()); wait.quit();
    });
    socket.setReadBufferSize(activationReply.size() + 1);
    deadline.start(5000); retry.start(0); wait.exec();
    if (!accepted) return fail(error);
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "GUI instance: activation acknowledged; secondary pid=%lld exiting",
        QCoreApplication::applicationPid());
    return Activated;
}

// Окно назначается до первого show/exec; отложенный показ должен примениться после начального --minimized.
void SingleInstance::setWindow(QWidget* window) {
    m_window = window;
    dispatchPending();
}

void SingleInstance::setUrlHandler(std::function<void(const QString&)> handler) {
    m_openUrl = std::move(handler); dispatchPending();
}

void SingleInstance::dispatchPending() {
    if (!m_window || (!m_pending && (m_pendingUrl.isEmpty() || !m_openUrl))) return;
    QTimer::singleShot(0, this, [this] {
        if (m_pending && m_window) { m_pending = false; showAndActivateWindow(m_window); }
        if (m_window && m_openUrl && !m_pendingUrl.isEmpty()) {
            const auto url = std::exchange(m_pendingUrl, {});
            DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "GUI instance: delivering URL bytes=%d", int(url.toUtf8().size()));
            m_openUrl(url);
        }
    });
}

// Фиксированная маленькая команда: частичное чтение допустимо, неверный/зависший клиент закрывается.
void SingleInstance::acceptConnections() {
    while (m_server.hasPendingConnections()) {
        auto* socket = m_server.nextPendingConnection();
        if (!socket) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "GUI instance: missing pending connection"); continue; }
        socket->setReadBufferSize(OBJECT_LINK_MAX + 7);
        auto* deadline = new QTimer(socket); deadline->setSingleShot(true); deadline->start(3000);
        connect(deadline, &QTimer::timeout, socket, [socket] {
            DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "GUI instance: activation client timed out"); socket->abort();
        });
        connect(socket, &QLocalSocket::errorOccurred, socket, [socket](QLocalSocket::LocalSocketError code) {
            if (code != QLocalSocket::PeerClosedError)
                DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "GUI instance: activation connection failed: %s", qPrintable(socket->errorString()));
        });
        connect(socket, &QLocalSocket::disconnected, socket, [socket, deadline] {
            if (deadline->isActive()) DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "GUI instance: client disconnected before sending activation");
            socket->deleteLater();
        });
        connect(socket, &QLocalSocket::readyRead, this, [this, socket, deadline, request = QByteArray{}, done = false]() mutable {
            if (done) return;
            request += socket->readAll();
            bool open = request.startsWith("OPEN ");
            if (request.size() > OBJECT_LINK_MAX + 6 || (!activationRequest.startsWith(request) &&
                !QByteArray("OPEN ").startsWith(request) && !open)) {
                DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "GUI instance: invalid activation command bytes=%lld", qint64(request.size()));
                deadline->stop(); socket->abort(); return;
            }
            if (request != activationRequest && (!open || !request.endsWith('\n'))) return;
            if (open) {
                const auto bytes = request.mid(5, request.size() - 6); const auto url = QString::fromUtf8(bytes);
                if (!url.startsWith(QStringLiteral("utun://")) || bytes != url.toUtf8() ||
                    url.contains(QRegularExpression(QStringLiteral("[\\x00-\\x20\\x7f]")))) {
                    DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "GUI instance: invalid URL payload bytes=%d", int(bytes.size()));
                    deadline->stop(); socket->abort(); return;
                }
                m_pendingUrl = url;
            }
            done = true; deadline->stop(); m_pending = true;
            DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "GUI instance: activation received pid=%lld window_ready=%d",
                QCoreApplication::applicationPid(), !m_window.isNull());
            dispatchPending();
            if (socket->write(activationReply) != activationReply.size())
                DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "GUI instance: cannot acknowledge activation: %s", qPrintable(socket->errorString()));
            socket->disconnectFromServer();
        });
    }
}

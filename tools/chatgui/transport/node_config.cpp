/* GUI-проекция INI; сетевую конфигурацию разбирает ядро при запуске. */
#include "node_config.h"
#include <openssl/crypto.h>
#include "config_updater.h"
#include <QCoreApplication>
#include <QFileInfo>
#include <QHostInfo>
#include <utility>

extern "C" {
#include "../../../src/config_ini.h"
#include "../../../src/config_updater.h"
#include "../../../lib/socket_compat.h"
#include "../../../src/transport_layer/secure_channel.h"
#include "../../../src/transport_layer/connection_order.h"
}

static int boolean_value(const QString& value) {
    const QString text = value.toLower();
    if (text == "yes" || text == "true" || text == "1") return 1;
    if (text == "no" || text == "false" || text == "0") return 0;
    return -1;
}

NodeConfig::NodeConfig(const QString& path) : m_path(path) {}

bool NodeConfig::exists() const {
    return QFileInfo::exists(m_path);
}

/* Публикуем ключи/ID в модели только после успеха обеих криптографических операций. */
bool NodeConfig::generateIdentity() {
    uint8_t public_x[32], seed[32], secret[32], ed[32];
    if (sc_identity_generate(public_x, secret, ed, seed) != SC_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "GUI identity generation failed path=%s", qPrintable(m_path));
        return false;
    }
    OPENSSL_cleanse(secret, sizeof(secret));
    m_pubKey = QByteArray(reinterpret_cast<const char*>(public_x), SC_PUBKEY_SIZE).toHex();
    m_privKey = QByteArray(reinterpret_cast<const char*>(seed), sizeof(seed)).toHex();
    m_nodeId = QByteArray(reinterpret_cast<const char*>(ed), sizeof(ed)).toHex();
    DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "GUI identity generated node=%s path=%s", qPrintable(m_nodeId), qPrintable(m_path));
    return true;
}

bool NodeConfig::create(const QString& nodeName) {
    if (exists()) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "GUI profile creation rejected existing path=%s", qPrintable(m_path));
        return false;
    }
    if (!generateIdentity()) return false;
    m_nodeName = nodeName.isEmpty() ? QHostInfo::localHostName() : nodeName;
    if (m_nodeName.isEmpty()) m_nodeName = "vibechat";
    m_controlAddr = "127.0.0.1:9999";
    m_autoSocketsValue = "yes";
    m_autoSocketProtocols = "tcp+udp";
#ifdef Q_OS_LINUX
    const QString directory = QFileInfo(m_path).absolutePath();
    m_dbPath = directory + "/chat_data";
    m_debugFile = directory + "/chatgui.log";
    DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "GUI new profile paths: config=%s db=%s log=%s",
               qPrintable(m_path), qPrintable(m_dbPath), qPrintable(m_debugFile));
#else
    m_dbPath = "chat_data";
    m_debugFile = "chatgui.log";
#endif
    return save();
}

bool NodeConfig::prepare(bool replaceInvalid) {
    if (!read(false) || hasErrors()) return false;
    /* Проверка новых полей запускает штатный парсер, включая getaddrinfo.
     * Это происходит раньше uasync_create и его инициализации WinSock. */
    config_error_clear();
    if (socket_platform_init()) {
        m_startupStatus = CONFIG_PROFILE_ERROR;
        CONFIG_ERROR("%s: socket subsystem initialization failed before configuration validation", qPrintable(m_path));
        m_errors = QStringList{QString::fromUtf8(config_error_text())};
        return false;
    }
    m_startupStatus = config_prepare_profile(m_path.toUtf8().constData(), replaceInvalid);
    socket_platform_cleanup();
    if (m_startupStatus != CONFIG_PROFILE_READY) {
        m_errorLine = config_error_line();
        m_errorOption = QString::fromUtf8(config_error_option());
        m_errors = QStringList{QString::fromUtf8(config_error_text())};
        return false;
    }
    return load() && !hasErrors();
}

/* Отдельная свежая проекция исключает сохранение удалённых полей после повторного load. */
bool NodeConfig::load() { return read(true); }

QString NodeConfig::startupMessage() const {
    if (m_startupStatus == CONFIG_PROFILE_INVALID_KEY)
        return QCoreApplication::translate("ChatGui", "This device key cannot be used to start the chat.");
    if (m_startupStatus == CONFIG_PROFILE_IO_ERROR)
        return QCoreApplication::translate("ChatGui", "Check access to the profile file and available disk space.\n%1").arg(m_path);
    if (m_startupStatus == CONFIG_PROFILE_IO_AFTER_REPLACE)
        return QCoreApplication::translate("ChatGui", "The updated profile was saved, but syncing it to disk failed. Retry to use the saved key.");
    if (m_errorLine && !m_errorOption.isEmpty())
        return QCoreApplication::translate("ChatGui", "Check the setting “%1” on line %2 of %3.").arg(m_errorOption).arg(m_errorLine).arg(m_path);
    if (m_errorLine)
        return QCoreApplication::translate("ChatGui", "Check line %1 of %2.").arg(m_errorLine).arg(m_path);
    return QCoreApplication::translate("ChatGui", "Could not prepare the chat profile.\n%1").arg(m_path);
}

bool NodeConfig::read(bool checkIdentity) {
    QStringList lines;
    bool ioError = false;
    if (!config_read_lines(m_path, lines, false, &ioError)) {
        m_errorLine = config_error_line(); m_errorOption.clear();
        m_startupStatus = ioError ? CONFIG_PROFILE_IO_ERROR : CONFIG_PROFILE_ERROR;
        m_errors = QStringList{QString::fromUtf8(config_error_text())};
        return false;
    }
    NodeConfig next(m_path);
    QString section, controlIp = "127.0.0.1", controlPort;
    const auto invalid = [&](int line, const QString& field) {
        if (!next.m_errorLine) { next.m_errorLine = line; next.m_errorOption = field; }
        next.m_startupStatus = CONFIG_PROFILE_ERROR;
        next.m_errors.append(QString("line %1: invalid %2").arg(line).arg(field));
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "GUI config invalid field=%s path=%s line=%d",
                    qPrintable(field), qPrintable(m_path), line);
    };
    for (int i = 0; i < lines.size(); ++i) {
        const QByteArray text = lines[i].trimmed().toUtf8();
        if (text.isEmpty() || text.startsWith('#')) continue;
        if (text.startsWith('[')) {
            char name[128];
            if (config_ini_section(text.constData(), name, sizeof(name))) { invalid(i + 1, "section header"); section.clear(); }
            else section = QString::fromUtf8(name).toLower();
            continue;
        }
        char keyBytes[CONFIG_INI_LINE_SIZE], valueBytes[CONFIG_INI_LINE_SIZE];
        if (config_ini_key_value(text.constData(), keyBytes, sizeof(keyBytes), valueBytes, sizeof(valueBytes))) {
            invalid(i + 1, "key=value");
            continue;
        }
        const QString key = QString::fromUtf8(keyBytes), value = QString::fromUtf8(valueBytes);
        if (section == "global") {
            if (key == "my_node_name") {
                next.m_nodeName = value;
                if (value.toUtf8().size() >= 64) invalid(i + 1, key);
            } else if (key == "my_public_key" || key == "my_private_key") {
                (key == "my_public_key" ? next.m_pubKey : next.m_privKey) = value;
                if (checkIdentity && !value.isEmpty() && config_ini_hex(valueBytes, nullptr, SC_PRIVKEY_SIZE)) invalid(i + 1, key);
            } else if (key == "auto_sockets") {
                const int enabled = boolean_value(value);
                if (value.compare("android", Qt::CaseInsensitive) == 0) next.m_autoSocketsValue = "android";
                else if (enabled >= 0) next.m_autoSocketsValue = enabled ? "yes" : "no";
                else invalid(i + 1, key);
            } else if (key == "auto_socket_protocols") {
                const QString protocols = value.toLower();
                if (protocols == "tcp" || protocols == "udp" || protocols == "tcp+udp") next.m_autoSocketProtocols = protocols;
                else invalid(i + 1, key);
            }
        } else if (section == "control") {
            if (key == "ip" || key == "control_ip") controlIp = value;
            else if (key == "port" || key == "control_port") {
                bool valid;
                const uint port = value.toUInt(&valid);
                if (!valid || port > 65535 || value.startsWith('+') || value.startsWith('-')) invalid(i + 1, key);
                else controlPort = QString::number(port);
            }
        } else if (section == "gui") {
            if (key == "bluetooth_flags") {
                bool valid;
                const int flags = value.toInt(&valid);
                if (!valid || flags < 0 || flags > 7) invalid(i + 1, key);
                else next.m_bluetoothFlags = flags;
            } else if (key == "utun_enabled") {
                const int enabled = boolean_value(value);
                if (enabled < 0) invalid(i + 1, key);
                else next.m_utunEnabled = enabled != 0;
            } else if (key == "db_path") next.m_dbPath = value;
            else if (key == "debug_file") next.m_debugFile = value;
            else if (key == "debug_level") next.m_debugLevel = value;
            else if (key == "theme") {
                if (value == "dark" || value == "light") next.m_theme = value;
                else DEBUG_WARN(DEBUG_CATEGORY_CONFIG, "GUI theme: invalid preference; using dark path=%s line=%d",
                                qPrintable(m_path), i + 1);
            } else if (key == "language") {
                if (value == "system" || value == "en" || value == "ru") next.m_language = value;
                else {
                    next.m_language = "system";
                    DEBUG_WARN(DEBUG_CATEGORY_CONFIG, "GUI language: invalid preference, using system language path=%s line=%d",
                               qPrintable(m_path), i + 1);
                }
            }
        } else if (section == "debug") {
            if (!next.m_debugCategories.isEmpty()) next.m_debugCategories += ',';
            next.m_debugCategories += key + '=' + value;
        }
    }
    if (!controlPort.isEmpty()) {
        if (controlIp.contains(':') && !controlIp.startsWith('[')) controlIp = '[' + controlIp + ']';
        next.m_controlAddr = controlIp + ':' + controlPort;
    }
    if (checkIdentity && !next.m_privKey.isEmpty()) {
        uint8_t seed[32], x[32], secret[32], ed[32];
        if (config_ini_hex(next.m_privKey.toLatin1().constData(), seed, sizeof(seed)) || sc_identity_from_seed(seed, x, secret, ed) != SC_OK)
            next.m_errors << QStringLiteral("Invalid Ed25519 identity in my_private_key");
        else next.m_nodeId = QByteArray(reinterpret_cast<const char*>(ed), sizeof(ed)).toHex();
        OPENSSL_cleanse(seed, sizeof(seed)); OPENSSL_cleanse(secret, sizeof(secret));
    }
    *this = std::move(next);
    DEBUG_DEBUG(DEBUG_CATEGORY_CONFIG, "GUI config loaded path=%s errors=%lld auto_sockets=%s protocols=%s language=%s",
                qPrintable(m_path), (long long)m_errors.size(), qPrintable(m_autoSocketsValue),
                qPrintable(m_autoSocketProtocols), qPrintable(m_language));
    return true;
}

/* Обновляем всю GUI-проекцию в памяти; ни одно поле не публикуется отдельно. */
bool NodeConfig::save() {
    const struct { const char *field; bool valid; } checks[] = {
        {"my_public_key", config_ini_hex(m_pubKey.toLatin1().constData(), nullptr, SC_PUBKEY_SIZE) == 0},
        {"my_private_key", config_ini_hex(m_privKey.toLatin1().constData(), nullptr, SC_PRIVKEY_SIZE) == 0},
        {"my_node_name", m_nodeName.toUtf8().size() < 64 && !m_nodeName.contains('#')},
        {"db_path", !m_dbPath.contains('#')}, {"debug_file", !m_debugFile.contains('#')},
        {"auto_socket_protocols", m_autoSocketProtocols == "tcp" || m_autoSocketProtocols == "udp" || m_autoSocketProtocols == "tcp+udp"},
        {"theme", m_theme == "dark" || m_theme == "light"},
        {"language", m_language == "system" || m_language == "en" || m_language == "ru"}
    };
    for (const auto& check : checks) {
        if (check.valid) continue;
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "GUI config save rejected invalid field=%s path=%s", check.field, qPrintable(m_path));
        return false;
    }
    const bool newFile = !exists();
    QStringList lines;
    if (!config_read_lines(m_path, lines, true)) return false;
    if (newFile) {
        lines = QStringList{"[global]", "connection_order=" CONNECTION_ORDER_DEFAULT, "hide_my_sockets=yes", "",
                 "[control]", "", "[gui]", "", "[debug]", "",
                 "[allowed_keys]", "allow_all=yes", "", "[ntp]", "enabled=yes", "server=pool.ntp.org",
                 "server=time.google.com", "server=time.cloudflare.com", "server=time.windows.com",
                 "server=ntp1.vniiftri.ru", "server=ntp2.vniiftri.ru", "interval=3600", ""};
    }
    const struct { const char *section; const char *key; QString value; } fields[] = {
        {"global", "my_node_name", m_nodeName},
        {"global", "my_private_key", m_privKey}, {"global", "my_public_key", m_pubKey},
        {"global", "auto_sockets", m_autoSocketsValue}, {"global", "auto_socket_protocols", m_autoSocketProtocols},
        {"gui", "utun_enabled", m_utunEnabled ? "yes" : "no"}, {"gui", "db_path", m_dbPath.isEmpty() ? "chat_data" : m_dbPath},
        {"gui", "bluetooth_flags", QString::number(m_bluetoothFlags)},
        {"gui", "theme", m_theme}, {"gui", "debug_file", m_debugFile}, {"gui", "language", m_language},
        {"gui", "debug_level", m_debugLevel.isEmpty() ? "info" : m_debugLevel}
    };
    for (const auto& field : fields)
        if (!config_update_in_lines(lines, field.section, field.key, field.value, true)) return false;

    if (!m_controlAddr.isEmpty()) {
        const int separator = m_controlAddr.lastIndexOf(':');
        QString host = m_controlAddr.left(separator);
        const QString portText = m_controlAddr.mid(separator + 1);
        bool valid;
        const uint port = portText.toUInt(&valid);
        if (separator <= 0 || !valid || port > 65535 || portText.startsWith('+') || portText.startsWith('-') || host.contains('#')) {
            DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "GUI config save rejected invalid control address path=%s", qPrintable(m_path));
            return false;
        }
        if (host.startsWith('[') && host.endsWith(']')) host = host.mid(1, host.size() - 2);
        if (!config_update_in_lines(lines, "control", "ip", host, true) ||
            !config_update_in_lines(lines, "control", "port", QString::number(port), true)) return false;
    }
    for (const QString& entry : m_debugCategories.split(',', Qt::SkipEmptyParts)) {
        const int equal = entry.indexOf('=');
        if (equal <= 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "GUI config save rejected malformed debug setting path=%s", qPrintable(m_path));
            return false;
        }
        if (!config_update_in_lines(lines, "debug", entry.left(equal).trimmed(), entry.mid(equal + 1).trimmed(), true)) return false;
    }
    if (!config_write_lines(m_path, lines)) return false;
    if (newFile) DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "GUI profile created path=%s connection_order=%s hide_my_sockets=yes",
                            qPrintable(m_path), CONNECTION_ORDER_DEFAULT);
    return true;
}

quint16 NodeConfig::controlPort() const {
    const int separator = m_controlAddr.lastIndexOf(':');
    bool valid;
    const uint port = m_controlAddr.mid(separator + 1).toUInt(&valid);
    return separator > 0 && valid && port <= 65535 ? quint16(port) : 0;
}

void NodeConfig::setControlPort(quint16 port) {
    const int separator = m_controlAddr.lastIndexOf(':');
    const QString host = separator > 0 ? m_controlAddr.left(separator) : QString("127.0.0.1");
    m_controlAddr = host + ':' + QString::number(port);
}

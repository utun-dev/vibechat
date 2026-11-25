#include "desktop_integration.h"
#include "debug_config.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QProcess>
#include <QCoreApplication>
#include <QMessageBox>
#include <QDesktopServices>
#include <QUrl>

void openConfigurationEditor(const QString& configPath, QWidget* parent) {
    const QString path = QFileInfo(configPath).absoluteFilePath();
#ifdef Q_OS_WIN
    QProcess editor;
    editor.setProgram(QStringLiteral("notepad.exe"));
    editor.setArguments({path});
    const bool opened = editor.startDetached();
    if (!opened) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "cannot start Notepad path=%s error=%s", qPrintable(path), qPrintable(editor.errorString()));
    }
#else
    const bool opened = QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    if (!opened) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "desktop service could not open config path=%s", qPrintable(path));
    }
#endif
    if (!opened) {
        QMessageBox::warning(parent, QCoreApplication::translate("ChatGui", "Edit config"),
            QCoreApplication::translate("ChatGui", "Could not open the configuration file in a text editor:\n%1").arg(path));
        return;
    }
    DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "config editor launched path=%s", qPrintable(path));
}

#ifdef Q_OS_LINUX
// Exec разбирается как desktop string, затем как командная строка; shell не используется.
static QString desktopCommand(const QString& executable) {
    QString command;
    for (QChar c : executable) {
        if (c == '%') command += "%%";
        else { if (c == '\\' || c == '"' || c == '$' || c == '`') command += '\\'; command += c; }
    }
    command = '"' + command + '"';
    command.replace("\\", "\\\\"); command.replace('\n', "\\n"); command.replace('\r', "\\r"); command.replace('\t', "\\t");
    return command;
}

bool desktopAutostartEnabled() {
    const QString path = QStandardPaths::locate(QStandardPaths::GenericConfigLocation, "autostart/org.utun.vibechat.desktop");
    if (path.isEmpty()) return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "Linux autostart: cannot read %s: %s", qPrintable(path), qPrintable(file.errorString()));
        return false;
    }
    // Desktop Entry отличается от INI: кавычки в Exec не должны поглощать последующие строки/ключи.
    bool desktopGroup = false, foundGroup = false, hidden = false, gnomeEnabled = true;
    while (!file.atEnd()) {
        const QByteArray line = file.readLine().trimmed();
        if (line.startsWith('[')) { desktopGroup = line == "[Desktop Entry]"; foundGroup |= desktopGroup; continue; }
        if (!desktopGroup || line.startsWith('#')) continue;
        const int separator = line.indexOf('=');
        if (separator < 0) continue;
        const QByteArray key = line.left(separator).trimmed();
        if (key != "Hidden" && key != "X-GNOME-Autostart-enabled") continue;
        const QByteArray value = line.mid(separator + 1).trimmed();
        if (value != "true" && value != "false") {
            DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "Linux autostart: invalid boolean key=%s value=%s path=%s",
                       key.constData(), value.constData(), qPrintable(path));
            return false;
        }
        if (key == "Hidden") hidden = value == "true"; else gnomeEnabled = value == "true";
    }
    if (file.error() != QFileDevice::NoError || !foundGroup) {
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "Linux autostart: invalid/unreadable entry path=%s desktop_group=%d error=%s",
                   qPrintable(path), foundGroup, qPrintable(file.errorString()));
        return false;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "Linux autostart: enabled=%d hidden=%d gnome_enabled=%d path=%s",
                !hidden && gnomeEnabled, hidden, gnomeEnabled, qPrintable(path));
    return !hidden && gnomeEnabled;
}

bool setDesktopAutostartEnabled(bool enabled, const QString& executable) {
    const QString root = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    if (root.isEmpty()) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "Linux autostart: user configuration directory is unavailable");
        return false;
    }
    const QString relative = QStringLiteral("autostart/org.utun.vibechat.desktop");
    const QString path = root + '/' + relative;
    bool systemEntry = false;
    for (const QString& location : QStandardPaths::standardLocations(QStandardPaths::GenericConfigLocation)) {
        if (location != root && QFileInfo::exists(location + '/' + relative)) { systemEntry = true; break; }
    }
    if (!enabled && !systemEntry) {
        QFile file(path);
        if ((file.exists() || QFileInfo(path).isSymLink()) && !file.remove()) {
            DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "Linux autostart: cannot remove %s: %s", qPrintable(path), qPrintable(file.errorString()));
            return false;
        }
        DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "Linux autostart: disabled path=%s", qPrintable(path));
        return true;
    }
    const QFileInfo executableInfo(executable);
    if (enabled && (!executableInfo.isAbsolute() || !executableInfo.isFile() || !executableInfo.isExecutable())) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "Linux autostart: executable is unavailable path=%s", qPrintable(executable));
        return false;
    }
    if (!QDir().mkpath(root + "/autostart")) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "Linux autostart: cannot create directory=%s", qPrintable(root + "/autostart"));
        return false;
    }
    // env сохраняет литеральный '%' в имени executable после раскрытия desktop field codes.
    const QByteArray content = QString("[Desktop Entry]\nType=Application\nName=VibeChat\n"
        "Exec=/usr/bin/env %1 --minimized\nIcon=org.utun.vibechat\nTerminal=false\nHidden=%2\n")
        .arg(desktopCommand(executable), enabled ? "false" : "true").toUtf8();
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size() || !file.commit()) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "Linux autostart: cannot save %s: %s", qPrintable(path), qPrintable(file.errorString()));
        return false;
    }
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "Linux autostart: enabled=%d minimized=%d system_override=%d executable=%s path=%s",
               enabled, enabled, systemEntry, qPrintable(executable), qPrintable(path));
    return true;
}
#endif

bool registerDesktopApplication(const QString& executable) {
#ifdef Q_OS_LINUX
    const QString relative = QStringLiteral("applications/org.utun.vibechat.desktop");
    const QString root = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    const QString path = root + '/' + relative;
    for (const QString& location : QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation)) {
        if (location == root || !QFileInfo::exists(location + '/' + relative)) continue;
        // Старый автоматически созданный ярлык перекрывает установленный DEB, включая его иконку и Exec.
        if (!root.isEmpty() && QFileInfo::exists(path)) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) {
                DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "Desktop registration: cannot read %s: %s", qPrintable(path), qPrintable(file.errorString()));
                return false;
            }
            const QByteArray content = file.readAll();
            file.close();
            if (content.startsWith("[Desktop Entry]\nType=Application\nName=VibeChat\nExec=/usr/bin/env ") &&
                ((content.endsWith("\nIcon=applications-internet\nTerminal=false\nCategories=Network;InstantMessaging;\n") && content.count('\n') == 7) ||
                 (content.endsWith("\nIcon=applications-internet\nTerminal=false\nCategories=Network;InstantMessaging;\nMimeType=x-scheme-handler/utun;\n") && content.count('\n') == 8))) {
                if (!file.remove()) {
                    DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "Desktop registration: cannot remove %s: %s", qPrintable(path), qPrintable(file.errorString()));
                    return false;
                }
                DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "Desktop registration: removed generated entry %s; using %s",
                           qPrintable(path), qPrintable(location + '/' + relative));
            }
        }
        return true;
    }
    if (QFileInfo::exists(path)) {
        QFile old(path);
        if (!old.open(QIODevice::ReadOnly)) {
            DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "Desktop registration: cannot inspect own entry path=%s", qPrintable(path)); return false;
        }
        const auto content = old.readAll();
        if (!content.startsWith("[Desktop Entry]\nType=Application\nName=VibeChat\nExec=/usr/bin/env ") || content.count('\n') != 7 ||
            !content.endsWith("\nIcon=applications-internet\nTerminal=false\nCategories=Network;InstantMessaging;\n")) return true;
    }
    if (root.isEmpty() || !QDir().mkpath(root + "/applications")) {
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "Desktop registration: cannot create applications directory in %s", qPrintable(root));
        return false;
    }
    // GLib проверяет executable до раскрытия %%; env оставляет путь аргументом и корректно обрабатывает '%' в имени.
    const QByteArray content = QString("[Desktop Entry]\nType=Application\nName=VibeChat\nExec=/usr/bin/env %1 -- %u\n"
        "Icon=applications-internet\nTerminal=false\nCategories=Network;InstantMessaging;\nMimeType=x-scheme-handler/utun;\n")
        .arg(desktopCommand(executable)).toUtf8();
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size() || !file.commit()) {
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "Desktop registration failed path=%s: %s", qPrintable(path), qPrintable(file.errorString()));
        return false;
    }
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "Desktop application registered path=%s", qPrintable(path));
    const auto updater = QStandardPaths::findExecutable(QStringLiteral("update-desktop-database"));
    if (!updater.isEmpty() && QProcess::execute(updater, {root + "/applications"}) != 0)
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "Desktop registration: MIME cache update failed directory=%s", qPrintable(root));
#else
    Q_UNUSED(executable);
#endif
    return true;
}

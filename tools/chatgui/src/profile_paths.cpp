#include "profile_paths.h"
#include "debug_config.h"
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryFile>

// Конфиг сохраняется атомарной заменой: доступность его файла не заменяет проверку записи в каталог.
static bool profileWritable(const QString& directory, bool requireConfig, QString& reason) {
    QFile config(QDir(directory).filePath("vibechat.cfg"));
    if (!config.exists()) {
        if (requireConfig) { reason = QStringLiteral("config missing: %1").arg(config.fileName()); return false; }
    } else if (!config.open(QIODevice::ReadWrite | QIODevice::ExistingOnly)) {
        reason = QStringLiteral("config is not writable: %1: %2").arg(config.fileName(), config.errorString());
        DEBUG_WARN(DEBUG_CATEGORY_CONFIG, "GUI profile rejected: %s", qPrintable(reason));
        return false;
    }
    QTemporaryFile probe(QDir(directory).filePath(".vibechat-write-XXXXXX"));
    if (!probe.open()) {
        reason = QStringLiteral("profile directory is not writable: %1: %2").arg(directory, probe.errorString());
        DEBUG_WARN(DEBUG_CATEGORY_CONFIG, "GUI profile rejected: %s", qPrintable(reason));
        return false;
    }
    if (!probe.remove()) {
        reason = QStringLiteral("cannot remove profile write probe: %1: %2").arg(probe.fileName(), probe.errorString());
        DEBUG_WARN(DEBUG_CATEGORY_CONFIG, "GUI profile rejected: %s", qPrintable(reason));
        return false;
    }
    return true;
}

QString linuxProfileDirectory(const QString& executableDirectory, QString& reason) {
    if (profileWritable(executableDirectory, true, reason)) {
        reason = QStringLiteral("writable config and directory beside executable");
        DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "GUI profile selected: directory=%s reason=%s",
                   qPrintable(executableDirectory), qPrintable(reason));
        return executableDirectory;
    }
    const QString fallbackReason = reason;
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (directory.isEmpty() || !QDir().mkpath(directory)) {
        reason = QStringLiteral("cannot create user profile directory: %1; fallback reason: %2").arg(directory, fallbackReason);
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "GUI profile selection failed: %s", qPrintable(reason));
        return {};
    }
    if (!profileWritable(directory, false, reason)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "GUI user profile is inaccessible: %s; fallback reason: %s",
                    qPrintable(reason), qPrintable(fallbackReason));
        return {};
    }
    reason = fallbackReason;
    DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "GUI profile selected: directory=%s fallback_reason=%s", qPrintable(directory), qPrintable(reason));
    return directory;
}

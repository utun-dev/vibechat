/* Linux-профиль: настоящий доступ к файлам/каталогам, выбор XDG и сохранность существующей идентичности. */
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <unistd.h>
#include "../src/profile_paths.h"

struct RestorePermissions {
    QString path;
    QFileDevice::Permissions permissions;
    explicit RestorePermissions(const QString& file) : path(file), permissions(QFile::permissions(file)) {}
    ~RestorePermissions() { QFile::setPermissions(path, permissions); }
};

class ProfilePathsTest : public QObject {
    Q_OBJECT
    QTemporaryDir m_root;
    QString m_binary, m_home;
    static bool write(const QString& path, const QByteArray& text) {
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(text) == text.size();
    }
    static QByteArray read(const QString& path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }
private slots:
    void init() {
        QVERIFY(m_root.isValid());
        const QString base = m_root.filePath(QUuid::createUuid().toString(QUuid::WithoutBraces));
        m_binary = base + "/binary folder";
        QVERIFY(QDir().mkpath(m_binary));
        qputenv("XDG_DATA_HOME", (base + "/home data пробел").toUtf8());
        QCoreApplication::setApplicationName("vibechat");
        m_home = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    }
    void missingConfigUsesHome() {
        QString reason;
        QCOMPARE(linuxProfileDirectory(m_binary, reason), m_home);
        QVERIFY(reason.contains("config missing"));
        QVERIFY(QDir(m_home).exists());
        QVERIFY(!QFile::exists(m_binary + "/vibechat.cfg"));
        QVERIFY(!QFile::exists(m_home + "/vibechat.cfg")); // Создание — после подтверждения мастера.
        QVERIFY(QDir(m_home).entryList({".vibechat-write-*"}, QDir::Files | QDir::Hidden).isEmpty());
    }
    void existingHomeProfileIsPreserved() {
        QVERIFY(QDir().mkpath(m_home));
        const auto config = m_home + "/vibechat.cfg";
        QVERIFY(write(config, "USER IDENTITY\n"));
        QString reason;
        QCOMPARE(linuxProfileDirectory(m_binary, reason), m_home);
        QCOMPARE(read(config), QByteArray("USER IDENTITY\n"));
    }
    void adjacentConfigHasPriority() {
        QVERIFY(QDir().mkpath(m_home));
        QVERIFY(write(m_home + "/vibechat.cfg", "HOME IDENTITY\n"));
        const auto config = m_binary + "/vibechat.cfg";
        QVERIFY(write(config, "ADJACENT IDENTITY\n"));
        QString reason;
        QCOMPARE(linuxProfileDirectory(m_binary, reason), m_binary);
        QCOMPARE(read(config), QByteArray("ADJACENT IDENTITY\n"));
        QCOMPARE(read(m_home + "/vibechat.cfg"), QByteArray("HOME IDENTITY\n"));
    }
    void readOnlyAdjacentConfigUsesHome() {
        if (geteuid() == 0) QSKIP("Root bypasses file permissions");
        const auto config = m_binary + "/vibechat.cfg";
        QVERIFY(write(config, "ADJACENT IDENTITY\n"));
        RestorePermissions restore(config);
        QVERIFY(QFile::setPermissions(config, QFile::ReadOwner));
        QString reason;
        QCOMPARE(linuxProfileDirectory(m_binary, reason), m_home);
        QVERIFY(reason.contains("config is not writable"));
        QCOMPARE(read(config), QByteArray("ADJACENT IDENTITY\n"));
    }
    void readOnlyAdjacentDirectoryUsesHome() {
        if (geteuid() == 0) QSKIP("Root bypasses directory permissions");
        const auto config = m_binary + "/vibechat.cfg";
        QVERIFY(write(config, "ADJACENT IDENTITY\n"));
        RestorePermissions restore(m_binary);
        QVERIFY(QFile::setPermissions(m_binary, QFile::ReadOwner | QFile::ExeOwner));
        QString reason;
        QCOMPARE(linuxProfileDirectory(m_binary, reason), m_home);
        QVERIFY(reason.contains("directory is not writable"));
        QCOMPARE(read(config), QByteArray("ADJACENT IDENTITY\n"));
    }
    void readOnlyHomeConfigFailsWithoutReplacingIdentity() {
        if (geteuid() == 0) QSKIP("Root bypasses file permissions");
        QVERIFY(QDir().mkpath(m_home));
        const auto config = m_home + "/vibechat.cfg";
        QVERIFY(write(config, "HOME IDENTITY\n"));
        RestorePermissions restore(config);
        QVERIFY(QFile::setPermissions(config, QFile::ReadOwner));
        QString reason;
        QVERIFY(linuxProfileDirectory(m_binary, reason).isEmpty());
        QVERIFY(reason.contains(config));
        QCOMPARE(read(config), QByteArray("HOME IDENTITY\n"));
    }
    void readOnlyHomeDirectoryFails() {
        if (geteuid() == 0) QSKIP("Root bypasses directory permissions");
        QVERIFY(QDir().mkpath(m_home));
        RestorePermissions restore(m_home);
        QVERIFY(QFile::setPermissions(m_home, QFile::ReadOwner | QFile::ExeOwner));
        QString reason;
        QVERIFY(linuxProfileDirectory(m_binary, reason).isEmpty());
        QVERIFY(reason.contains("directory is not writable"));
    }
    void blockedHomeDirectoryFails() {
        const QString data = QString::fromUtf8(qgetenv("XDG_DATA_HOME"));
        QVERIFY(write(data, "OBSTACLE\n"));
        QString reason;
        QVERIFY(linuxProfileDirectory(m_binary, reason).isEmpty());
        QVERIFY(reason.contains("cannot create user profile directory"));
        QCOMPARE(read(data), QByteArray("OBSTACLE\n"));
    }
};
QTEST_GUILESS_MAIN(ProfilePathsTest)
#include "test_profile_paths.moc"

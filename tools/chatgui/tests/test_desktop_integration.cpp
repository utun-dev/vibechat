// Настоящий desktop parser/launcher: путь с пробелами, кавычками и спецсимволами сохраняется без shell-подстановок.
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QUrl>
#include <QStandardPaths>
#include <QTemporaryDir>
#include "../src/desktop_integration.h"
#include "debug_config.h"

class DesktopTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        debug_config_init(); debug_set_level(DEBUG_LEVEL_DEBUG);
    }
    void autostartAndLaunch() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        qputenv("XDG_CONFIG_HOME", directory.path().toUtf8());
        qputenv("XDG_CONFIG_DIRS", directory.filePath("system-config").toUtf8());
        const QString path = directory.filePath("autostart/org.utun.vibechat.desktop");
        QVERIFY(!desktopAutostartEnabled());
        QVERIFY(setDesktopAutostartEnabled(false, QString()));
        QVERIFY(!QFile::exists(path));
        const QString executable = directory.filePath("space quote\" dollar$ backtick` percent% slash\\-тест");
        QFile script(executable); QVERIFY(script.open(QIODevice::WriteOnly));
        script.write("#!/bin/sh\nprintf '%s\\n%s' \"$0\" \"$1\" > \"$DESKTOP_RESULT\"\n"); script.close();
        QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QVERIFY(setDesktopAutostartEnabled(true, executable));
        QVERIFY(desktopAutostartEnabled());
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); const QByteArray original = file.readAll(); file.close();
        QVERIFY(original.contains("--minimized"));
        const auto validate = QStandardPaths::findExecutable("desktop-file-validate");
        if (!validate.isEmpty()) QCOMPARE(QProcess::execute(validate, {path}), 0);
        const auto gio = QStandardPaths::findExecutable("gio");
        if (!gio.isEmpty()) {
            const QString output = directory.filePath("result"); qputenv("DESKTOP_RESULT", output.toUtf8());
            QProcess process; process.start(gio, {"launch", path}); QVERIFY(process.waitForFinished(5000));
            QCOMPARE(process.exitStatus(), QProcess::NormalExit);
            QVERIFY2(process.exitCode() == 0, process.readAllStandardError().constData());
            QFile result(output); QTRY_VERIFY(result.exists()); QVERIFY(result.open(QIODevice::ReadOnly));
            QCOMPARE(QString::fromUtf8(result.readAll()), executable + "\n--minimized");
        }
        for (const QString& key : {QString("Hidden"), QString("X-GNOME-Autostart-enabled")}) {
            QByteArray changed = original;
            if (key == "Hidden") changed.replace("Hidden=false", "Hidden=true");
            else changed += "X-GNOME-Autostart-enabled=false\n";
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QCOMPARE(file.write(changed), qint64(changed.size())); file.close();
            QVERIFY(!desktopAutostartEnabled());
            QVERIFY(setDesktopAutostartEnabled(true, executable));
            QVERIFY(desktopAutostartEnabled());
        }
        QVERIFY(setDesktopAutostartEnabled(false, executable));
        QVERIFY(!QFile::exists(path)); QVERIFY(!desktopAutostartEnabled());
    }
    void systemAutostartOverride() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString user = directory.filePath("user");
        const QString system = directory.filePath("system");
        qputenv("XDG_CONFIG_HOME", user.toUtf8()); qputenv("XDG_CONFIG_DIRS", system.toUtf8());
        QVERIFY(QDir().mkpath(system + "/autostart"));
        QFile installed(system + "/autostart/org.utun.vibechat.desktop"); QVERIFY(installed.open(QIODevice::WriteOnly));
        const QByteArray content = "[Desktop Entry]\nType=Application\nName=VibeChat\nExec=/bin/true\nHidden=false\n";
        QCOMPARE(installed.write(content), qint64(content.size())); installed.close();
        QVERIFY(desktopAutostartEnabled());
        QVERIFY(setDesktopAutostartEnabled(false, QCoreApplication::applicationFilePath()));
        QVERIFY(!desktopAutostartEnabled());
        QVERIFY(QFile::exists(user + "/autostart/org.utun.vibechat.desktop"));
        QVERIFY(installed.open(QIODevice::ReadOnly)); QCOMPARE(installed.readAll(), content);
        QVERIFY(setDesktopAutostartEnabled(true, QCoreApplication::applicationFilePath()));
        QVERIFY(desktopAutostartEnabled());
    }
    void autostartFailure() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        qputenv("XDG_CONFIG_HOME", directory.path().toUtf8());
        qputenv("XDG_CONFIG_DIRS", directory.filePath("system-config").toUtf8());
        QVERIFY(!setDesktopAutostartEnabled(true, directory.filePath("missing-executable")));
        QVERIFY(!QFile::exists(directory.filePath("autostart")));
        QFile obstacle(directory.filePath("autostart")); QVERIFY(obstacle.open(QIODevice::WriteOnly)); obstacle.close();
        QVERIFY(!setDesktopAutostartEnabled(true, QCoreApplication::applicationFilePath()));
        QVERIFY(!desktopAutostartEnabled());
    }
    void registrationAndLaunch() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        qputenv("XDG_DATA_HOME", directory.path().toUtf8());
        qputenv("XDG_DATA_DIRS", directory.filePath("system-data").toUtf8());
        const QString executable = directory.filePath("space quote\" dollar$ backtick` percent% slash\\-тест");
        QFile script(executable); QVERIFY(script.open(QIODevice::WriteOnly));
        script.write("#!/bin/sh\nprintf '%s' \"$0\" > \"$DESKTOP_RESULT\"\nif [ \"$#\" -gt 1 ]; then printf '\\n%s' \"$2\" >> \"$DESKTOP_RESULT\"; fi\n"); script.close();
        QVERIFY(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QVERIFY(registerDesktopApplication(executable));
        const QString path = directory.filePath("applications/org.utun.vibechat.desktop");
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); const auto original = file.readAll(); file.close();
        QVERIFY(original.contains(" -- %u\n") && original.contains("MimeType=x-scheme-handler/utun;"));
        QVERIFY(registerDesktopApplication("/must/not/replace/existing/entry"));
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), original); file.close();
        const auto gio = QStandardPaths::findExecutable("gio");
        if (gio.isEmpty()) QSKIP("gio unavailable: desktop entry created; launcher validation needs GLib tools");
        const QString output = directory.filePath("result"); qputenv("DESKTOP_RESULT", output.toUtf8());
        QProcess process; process.start(gio, {"launch", path}); QVERIFY(process.waitForFinished(5000));
        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QVERIFY2(process.exitCode() == 0, (process.readAllStandardError() + "\nentry:\n" + original).constData());
        QFile result(output); QTRY_VERIFY(result.exists()); QVERIFY(result.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(result.readAll()), executable);
        result.close(); QVERIFY(result.remove());
        const QString url = QStringLiteral("utun://chat/Команда@") + QString(64, 'c') + QStringLiteral("/Сообщение@") + QString(64, 'a');
        process.start(gio, {"launch", path, url}); QVERIFY(process.waitForFinished(5000)); QCOMPARE(process.exitCode(), 0);
        QTRY_VERIFY(result.exists()); QVERIFY(result.open(QIODevice::ReadOnly));
        const auto received = QString::fromUtf8(result.readAll());
        QCOMPARE(received.section('\n', 0, 0), executable);
        QCOMPARE(QUrl(received.section('\n', 1)).toString(QUrl::FullyDecoded), QUrl(url).toString(QUrl::FullyDecoded));
    }
    void registrationFailure() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        qputenv("XDG_DATA_HOME", directory.path().toUtf8());
        QFile obstacle(directory.filePath("applications")); QVERIFY(obstacle.open(QIODevice::WriteOnly)); obstacle.close();
        QVERIFY(!registerDesktopApplication("/example/vibechat"));
    }
    void installedEntryReplacesGeneratedEntry() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        qputenv("XDG_DATA_HOME", directory.path().toUtf8());
        const QString system = directory.filePath("system-data");
        qputenv("XDG_DATA_DIRS", system.toUtf8());
        QVERIFY(registerDesktopApplication("/old/build/vibechat"));
        const QString relative = QStringLiteral("applications/org.utun.vibechat.desktop");
        const QString path = directory.filePath(relative);
        QVERIFY(QFile::exists(path));
        QVERIFY(QDir().mkpath(system + "/applications"));
        QFile installed(system + '/' + relative); QVERIFY(installed.open(QIODevice::WriteOnly));
        const QByteArray content = "[Desktop Entry]\nType=Application\nName=VibeChat\nExec=/opt/vibechat/bin/vibechat\n"
                                   "Icon=org.utun.vibechat\nTerminal=false\n";
        QCOMPARE(installed.write(content), qint64(content.size())); installed.close();
        QVERIFY(registerDesktopApplication("/opt/vibechat/bin/vibechat"));
        QVERIFY(!QFile::exists(path));
        QCOMPARE(QStandardPaths::locate(QStandardPaths::GenericDataLocation, relative), installed.fileName());
        QVERIFY(installed.open(QIODevice::ReadOnly)); QCOMPARE(installed.readAll(), content); installed.close();
        // Чужие/изменённые пользовательские записи сохраняем, даже если они перекрывают системную.
        QFile custom(path); QVERIFY(custom.open(QIODevice::WriteOnly));
        const QByteArray customContent = content + "Hidden=true\n";
        QCOMPARE(custom.write(customContent), qint64(customContent.size())); custom.close();
        QVERIFY(registerDesktopApplication("/opt/vibechat/bin/vibechat"));
        QVERIFY(custom.open(QIODevice::ReadOnly)); QCOMPARE(custom.readAll(), customContent);
    }
};
QTEST_GUILESS_MAIN(DesktopTest)
#include "test_desktop_integration.moc"

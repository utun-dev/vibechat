/* Обновление настоящих INI: дубли, UTF-8, независимые секции и отказ записи. */
#include "../transport/config_updater.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
extern "C" {
#include "debug_config.h"
}
#ifdef Q_OS_UNIX
#include <sys/resource.h>
#include <csignal>
#endif

class ConfigUpdaterTest : public QObject {
    Q_OBJECT
    QTemporaryDir directory;

    QString path() const { return directory.filePath("node.cfg"); }
    bool put(const QByteArray& data) {
        QFile file(path());
        return file.open(QIODevice::WriteOnly) && file.write(data) == data.size() && file.flush();
    }
    QByteArray contents() const {
        QFile file(path());
        if (!file.open(QIODevice::ReadOnly)) return {};
        return file.readAll();
    }
private slots:
    void initTestCase() {
        debug_config_init();
        debug_enable_console(1);
        debug_set_category_level(DEBUG_CATEGORY_CONFIG, DEBUG_LEVEL_DEBUG);
    }
    void init() { QVERIFY(directory.isValid()); QFile::remove(path()); }

    void newFileAndSection() {
        QVERIFY(config_update_option(path(), "global", "my_node_name", QString::fromUtf8("Тест")));
        QVERIFY(config_update_option(path(), "gui", "language", "ru"));
        QCOMPARE(contents(), QByteArray("[global]\nmy_node_name=") + QString::fromUtf8("Тест").toUtf8() +
                 "\n\n[gui]\nlanguage=ru\n");
    }

    void replacementUpdatesAllDuplicates() {
        QVERIFY(put("# keep\n[global]\nmy_node_name=old\nmy_node_name=last\n"
                    "[control]\nport=9\n[GLOBAL]\nmy_node_name=override\n"));
        QVERIFY(config_update_option(path(), "global", "my_node_name", "new"));
        QCOMPARE(contents().count("my_node_name=new"), 3);
        QVERIFY(!contents().contains("old"));
        QVERIFY(!contents().contains("override"));
        QVERIFY(contents().contains("# keep\n"));
        QVERIFY(contents().contains("[control]\nport=9\n"));
    }

    void chatAliasUsesSameSection() {
        QVERIFY(put("[ chat ]\nradio_auto_enabled=0\n[chatserver]\nradio_auto_enabled=0\n"));
        QVERIFY(config_update_option(path(), "chatserver", "radio_auto_enabled", "1"));
        QCOMPARE(contents().count("radio_auto_enabled=1"), 2);
    }

    void appendUsesLastMatchingSection() {
        QStringList lines{"[client:peer]", "link=one", "[client:other]", "link=other",
                          "[client:peer]", "link=two", "[global]", "my_node_name=keep"};
        QVERIFY(config_update_in_lines(lines, "client:peer", "link", "three", false));
        QCOMPARE(lines.indexOf("link=three"), lines.indexOf("link=two") + 1);
        QCOMPARE(lines.count("link=one"), 1);
        QCOMPARE(lines.count("link=other"), 1);
    }

    void insertsMissingKey() {
        QStringList lines{"[global]", "my_node_name=keep", "[gui]", "language=en"};
        QVERIFY(config_update_in_lines(lines, "global", "auto_sockets", "yes", true));
        QCOMPARE(lines, (QStringList{"[global]", "auto_sockets=yes", "my_node_name=keep", "[gui]", "language=en"}));
    }

    void controlAliasesAreUpdatedTogether() {
        QStringList lines{"[control]", "port=1", "control_port=2", "ip=127.0.0.1", "control_ip=127.0.0.2"};
        QVERIFY(config_update_in_lines(lines, "control", "port", "0", true));
        QVERIFY(config_update_in_lines(lines, "control", "control_ip", "::1", true));
        QCOMPARE(lines.count("port=0"), 2);
        QCOMPARE(lines.count("control_ip=::1"), 2);
    }

    void batchReadKeepsOutputOnError() {
        const QStringList original{"preserve caller buffer"};
        auto lines = original;
        QVERIFY(!config_read_lines(path(), lines));
        QCOMPARE(lines, original);
        QVERIFY(config_read_lines(path(), lines, true));
        QVERIFY(lines.isEmpty());
        QVERIFY(put("[gui]\nlanguage=ru\n"));
        QVERIFY(config_read_lines(path(), lines));
        QCOMPARE(lines, (QStringList{"[gui]", "language=ru"}));
    }

    void invalidArgumentsDoNotMutate() {
        const QStringList original{"[global]", "my_node_name=keep"};
        for (const auto& values : {QStringList{"", "name", "value"}, QStringList{"global", "x=y", "value"},
             QStringList{"global", "name", "one\ntwo"}, QStringList{"global]", "name", "value"}}) {
            auto lines = original;
            QVERIFY(!config_update_in_lines(lines, values[0], values[1], values[2], true));
            QCOMPARE(lines, original);
        }
    }

    void rewritesOnlyServerSections() {
        QVERIFY(put("[global]\nmy_node_name=keep\n\n[ SERVER : old ]\naddr=127.0.0.1:1\n"
                    "[client:peer]\nlink=old:127.0.0.1:2\n[server:other]\naddr=[::1]:3\n[gui]\nlanguage=ru\n"));
        QVERIFY(config_rewrite_servers(path(), {{"lan", "127.0.0.1:10", "", "", ""},
                     {"lan6", "[::1]:10", "tcp", "local", "eth0"}}));
        const auto text = contents();
        QVERIFY(!text.contains("[ SERVER : old ]"));
        QVERIFY(!text.contains("[server:other]"));
        QVERIFY(text.contains("[server:lan]\naddr=127.0.0.1:10\ntransport=udp\ntype=public\n"));
        QVERIFY(text.contains("[server:lan6]\naddr=[::1]:10\ntransport=tcp\ntype=local\nnetif=eth0\n"));
        QVERIFY(text.indexOf("[server:lan]") < text.indexOf("[client:peer]"));
        QVERIFY(text.contains("[client:peer]\nlink=old:127.0.0.1:2\n[gui]\nlanguage=ru\n"));
        QVERIFY(config_rewrite_servers(path(), {}));
        QVERIFY(!contents().contains("[server:"));
    }

    void readAndOpenFailures() {
        QVERIFY(!config_update_option(directory.path(), "gui", "language", "en"));
        QVERIFY(!config_rewrite_servers(directory.path(), {}));
        QVERIFY(!config_update_option(directory.filePath("missing/node.cfg"), "gui", "language", "en"));
        QVERIFY(!QFile::exists(directory.filePath("missing/node.cfg")));
    }

    void invalidTextIsNotRewritten() {
        for (const QByteArray& original : {QByteArray("[global]\nname=bad\0tail\n", 23),
             QByteArray("[global]\nname=\xff\n"), QByteArray("[global]\nname=") + QByteArray(1023, 'x') + '\n'}) {
            QVERIFY(put(original));
            QVERIFY(!config_update_option(path(), "global", "my_node_name", "new"));
            QCOMPARE(contents(), original);
            QVERIFY(!config_rewrite_servers(path(), {}));
            QCOMPARE(contents(), original);
        }
    }

    void invalidServerDoesNotReplaceFile() {
        const QByteArray original("[server:keep]\naddr=127.0.0.1:1\n");
        QVERIFY(put(original));
        QVERIFY(!config_rewrite_servers(path(), {{"bad]\n[global", "127.0.0.1:2", "udp", "public", ""}}));
        QCOMPARE(contents(), original);
    }

    void normalizesCrLf() {
        QVERIFY(put("[global]\r\nmy_node_name=old\r\n# keep\r\n"));
        QVERIFY(config_update_option(path(), "global", "my_node_name", "new"));
        QCOMPARE(contents(), QByteArray("[global]\nmy_node_name=new\n# keep\n"));
    }

    void failedWritePreservesOriginal_data() {
        QTest::addColumn<bool>("servers");
        QTest::newRow("option") << false;
        QTest::newRow("servers") << true;
    }
    void failedWritePreservesOriginal() {
#ifdef Q_OS_UNIX
        QFETCH(bool, servers);
        const QByteArray original("[global]\nmy_node_name=keep\n");
        QVERIFY(put(original));
        bool saved;
        {
            struct LimitGuard {
                struct rlimit previous{};
                using Handler = void (*)(int);
                Handler handler = SIG_DFL;
                bool active = false;
                ~LimitGuard() { if (active) { setrlimit(RLIMIT_FSIZE, &previous); std::signal(SIGXFSZ, handler); } }
            } guard;
            QVERIFY(getrlimit(RLIMIT_FSIZE, &guard.previous) == 0);
            struct rlimit limited = guard.previous;
            limited.rlim_cur = 64;
            guard.handler = std::signal(SIGXFSZ, SIG_IGN);
            guard.active = true;
            QVERIFY(setrlimit(RLIMIT_FSIZE, &limited) == 0);
            saved = servers ? config_rewrite_servers(path(), {{"lan", "127.0.0.1:10", "udp", "public", QString(200, 'x')}}) :
                config_update_option(path(), "global", "my_node_name", QString(200, 'x'));
        }
        QVERIFY(!saved);
        QCOMPARE(contents(), original);
        QCOMPARE(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot), QStringList{"node.cfg"});
#else
        QSKIP("RLIMIT_FSIZE is available on Unix");
#endif
    }
};

QTEST_GUILESS_MAIN(ConfigUpdaterTest)
#include "test_config_updater.moc"

/* Реальный desktop-генератор и общий строгий парсер; --validate проверяет также конфиги Android. */
#include "../transport/node_config.h"
#include "../transport/config_updater.h"
#include <QtTest>
#include <QTemporaryDir>
#include <cstring>
#include <memory>
#include <thread>
#ifdef Q_OS_UNIX
#include <sys/resource.h>
#include <csignal>
#endif

extern "C" {
#include "config_parser.h"
#include "config_error.h"
#include "chat/chat_setting.h"
#include "socket_compat.h"
#include "debug_config.h"
#include "../../../src/config_updater.h"
}

#ifdef UTUN_TEST_NODE_CONFIG_FAILURES
static bool failKeyGeneration = false;
extern "C" sc_status_t __real_sc_identity_generate(uint8_t x[32], uint8_t secret[32], uint8_t ed[32], uint8_t seed[32]);
extern "C" sc_status_t __wrap_sc_identity_generate(uint8_t x[32], uint8_t secret[32], uint8_t ed[32], uint8_t seed[32]) {
    return failKeyGeneration ? SC_ERR_CRYPTO : __real_sc_identity_generate(x, secret, ed, seed);
}
#endif

using Config = std::unique_ptr<utun_config, decltype(&free_config)>;

class NodeConfigTest : public QObject {
    Q_OBJECT
private slots:
    void newProfileHidesSocketsAndPreservesExplicitOptOut() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString path = directory.filePath("privacy.cfg");
        NodeConfig config(path); QVERIFY(config.create("Privacy"));
        Config parsed(parse_config(qPrintable(path)), free_config); QVERIFY(parsed);
        QVERIFY(parsed->global.hide_my_sockets);
        QVERIFY(config_update_option(path, "global", "hide_my_sockets", "no"));
        QVERIFY(config.prepare()); QVERIFY(config.save());
        parsed.reset(parse_config(qPrintable(path))); QVERIFY(parsed);
        QVERIFY(!parsed->global.hide_my_sockets);
    }

    void newLinuxProfileStoresAbsolutePathsAndKeepsIdentity() {
#ifdef Q_OS_LINUX
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString profile = directory.filePath("home data пробел");
        QVERIFY(QDir().mkpath(profile));
        const QString path = profile + "/vibechat.cfg";
        NodeConfig config(path); QVERIFY(config.create("First launch"));
        QCOMPARE(config.dbPath(), profile + "/chat_data");
        QCOMPARE(config.debugFile(), profile + "/chatgui.log");
        const QString identity = config.privKey();
        NodeConfig loaded(path); QVERIFY(loaded.prepare());
        QCOMPARE(loaded.dbPath(), config.dbPath()); QCOMPARE(loaded.debugFile(), config.debugFile());
        QCOMPARE(loaded.privKey(), identity);
        QVERIFY(!loaded.create("Must not overwrite"));
        QVERIFY(loaded.load()); QCOMPARE(loaded.privKey(), identity);
#else
        QSKIP("Linux profile paths only");
#endif
    }
    void startupPreparesMissingServerKey() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString path = directory.filePath("startup.cfg");
        NodeConfig config(path); QVERIFY(config.create("Startup"));
        const QString privateKey = config.privKey(), nodeId = config.nodeId();
        QVERIFY(config_update_option(path, "chatserver", "join_policy", "1"));
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(), {"--prepare", path});
        QVERIFY(child.waitForFinished(10000));
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        QVERIFY2(child.exitCode() == 0, child.readAllStandardError().constData());
        QVERIFY(config.load());
        QCOMPARE(config.privKey(), privateKey); QCOMPARE(config.nodeId(), nodeId);
        Config parsed(parse_config(qPrintable(path)), free_config); QVERIFY(parsed);
        QVERIFY(parsed->global.chatserver_has_add_key);
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto prepared = file.readAll(); file.close();
        QVERIFY(config.prepare());
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), prepared);
    }

    void recoverStartupIdentity_data() {
        QTest::addColumn<QByteArray>("seed");
        QTest::addColumn<QByteArray>("publicKey");
        QTest::addColumn<bool>("needsConsent");
        const QByteArray seed = "704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f";
        const QByteArray publicKey = "7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f";
        const QByteArray raw = QByteArray::fromHex(seed);
        uint8_t legacyPublic[32];
        QCOMPARE(sc_compute_public_key_from_private(reinterpret_cast<const uint8_t*>(raw.constData()), legacyPublic), SC_OK);
        QTest::newRow("valid") << seed << publicKey << false;
        QTest::newRow("missing-public") << seed << QByteArray() << false;
        QTest::newRow("old-x25519") << seed << QByteArray(reinterpret_cast<const char*>(legacyPublic), 32).toHex() << true;
        QTest::newRow("noncanonical") << QByteArray(64, '0') << publicKey << true;
        QTest::newRow("malformed") << QByteArray("invalid") << publicKey << true;
        QTest::newRow("missing-seed") << QByteArray() << publicKey << true;
    }

    void recoverStartupIdentity() {
        QFETCH(QByteArray, seed); QFETCH(QByteArray, publicKey); QFETCH(bool, needsConsent);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString path = directory.filePath("vibechat.cfg");
        const QByteArray options = "# profile comment\r\n[gui]\r\nlanguage=ru\r\n";
        const QByteArray original = "[global]\r\nmy_node_id=5\r\nmy_private_key=" + seed +
            "\r\n[global]\r\nmy_node_id=6\r\nmy_public_key=" + publicKey + "\r\n" + options;
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(original), original.size()); file.close();
        NodeConfig cfg(path);
        if (needsConsent) {
            QVERIFY(!cfg.prepare()); QCOMPARE(cfg.startupStatus(), CONFIG_PROFILE_INVALID_KEY);
            QCOMPARE(cfg.language(), QString("ru"));
            QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), original); file.close();
        }
        QVERIFY(cfg.prepare(needsConsent));
        if (needsConsent) QVERIFY(cfg.privKey().toLatin1() != seed);
        else QCOMPARE(cfg.privKey().toLatin1(), seed);
        QVERIFY(file.open(QIODevice::ReadOnly)); const auto saved = file.readAll(); file.close();
        QVERIFY(!saved.contains("my_node_id")); QVERIFY(saved.endsWith(options));
        const QString address = cfg.nodeId(); QCOMPARE(address.size(), 64);
        QVERIFY(cfg.prepare(true)); QCOMPARE(cfg.nodeId(), address);
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), saved);
    }

    void syntaxErrorPrecedesKeyReplacement() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString path = directory.filePath("vibechat.cfg");
        const QByteArray source = "[global]\nmy_node_id=42\nmy_private_key=bogus\nmy_nod_name=Alice\n";
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(source), source.size()); file.close();
        NodeConfig cfg(path);
        for (bool consent : {false, true}) {
            QVERIFY(!cfg.prepare(consent)); QCOMPARE(cfg.startupStatus(), CONFIG_PROFILE_ERROR);
            QVERIFY(cfg.startupMessage().contains("my_nod_name") && cfg.startupMessage().contains("4"));
            QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), source); file.close();
        }
    }

    void detailedErrorsSurviveDisabledLogging() {
        debug_set_level(DEBUG_LEVEL_NONE);
        const QByteArray invalid("[control]\nport=70000\n");
        Config parsed(parse_config_from_buf(invalid.constData(), size_t(invalid.size()), "bad.cfg"), free_config);
        QVERIFY(!parsed);
        const QByteArray error(config_error_text());
        QVERIFY(error.contains("65535")); QVERIFY(error.contains("bad.cfg:2")); QVERIFY(error.contains("port"));
        std::thread worker([] { config_error_clear(); config_error_append("other thread"); });
        worker.join(); QCOMPARE(QByteArray(config_error_text()), error);
        parsed.reset(parse_config_from_buf("", 0, "valid.cfg")); QVERIFY(parsed);
        QVERIFY(QByteArray(config_error_text()).isEmpty());
        debug_set_level(DEBUG_LEVEL_INFO);
    }

    void failedPreparationExplainsFieldWithoutLeakingKeys() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString path = directory.filePath("invalid.cfg");
        NodeConfig config(path); QVERIFY(config.create("Invalid"));
        QVERIFY(config_update_option(path, "chatserver", "add_key", "private-invalid-secret"));
        QVERIFY(!config.prepare());
        const QString reason = config.errors().join('\n');
        QVERIFY(reason.contains("add_key")); QVERIFY(reason.contains(path));
        QVERIFY(!reason.contains("private-invalid-secret"));
    }

    void identityFailureKeepsPreviousValues() {
#ifdef UTUN_TEST_NODE_CONFIG_FAILURES
        QTemporaryDir directory; QVERIFY(directory.isValid());
        NodeConfig config(directory.filePath("identity.cfg")); QVERIFY(config.create("Original"));
        const QString privateKey = config.privKey(), publicKey = config.pubKey(), id = config.nodeId();
        QScopedValueRollback<bool> failure(failKeyGeneration, true);
        QVERIFY(!config.generateIdentity());
        QCOMPARE(config.privKey(), privateKey); QCOMPARE(config.pubKey(), publicKey); QCOMPARE(config.nodeId(), id);
        NodeConfig fresh(directory.filePath("failed.cfg"));
        QVERIFY(!fresh.create("Failure")); QVERIFY(!fresh.exists());
#else
        QSKIP("Linker fault injection is available on Linux");
#endif
    }

    void repairKeepsExistingPrivateKey() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString path = directory.filePath("repair.cfg");
        NodeConfig config(path); QVERIFY(config.create("Original"));
        const QString privateKey = config.privKey(), publicKey = config.pubKey(), id = config.nodeId();
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); QVERIFY(!file.readAll().contains("my_node_id=")); file.close();
        QVERIFY(config_update_option(path, "global", "my_public_key", ""));
        QCOMPARE(config_ensure_keys(path.toUtf8().constData()), 0);
        QVERIFY(config.load()); QVERIFY(!config.hasErrors());
        QCOMPARE(config.privKey(), privateKey); QCOMPARE(config.pubKey(), publicKey); QCOMPARE(config.nodeId(), id);
    }

    void zeroIpv6ControlPortAndAndroidModeRoundTrip() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString path = directory.filePath("control.cfg");
        NodeConfig config(path); QVERIFY(config.create("Control"));
        QVERIFY(config_update_option(path, "global", "auto_sockets", "android"));
        QVERIFY(config_update_option(path, "control", "control_ip", "::1"));
        QVERIFY(config_update_option(path, "control", "control_port", "0"));
        QVERIFY(config.load()); QVERIFY(!config.hasErrors());
        QCOMPARE(config.controlAddr(), QString("[::1]:0")); QCOMPARE(config.controlPort(), quint16(0));
        QVERIFY(config.autoSockets()); QVERIFY(config.save());
        Config parsed(parse_config(qPrintable(path)), free_config); QVERIFY(parsed);
        QCOMPARE(parsed->global.auto_sockets, CFG_AUTO_SOCKETS_ANDROID);
        QCOMPARE(parsed->global.control_sock.ss_family, AF_INET6);
        config.setControlPort(9999); QVERIFY(config.save()); QVERIFY(config.load());
        QCOMPARE(config.controlAddr(), QString("[::1]:9999"));
    }

    void failedLoadKeepsPreviousProjection() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString path = directory.filePath("missing.cfg");
        NodeConfig config(path); QVERIFY(config.create("Previous"));
        QVERIFY(QFile::remove(path)); QVERIFY(!config.load());
        QVERIFY(!config.errors().isEmpty()); QVERIFY(config.errors().join('\n').contains(path));
        QCOMPARE(config.nodeName(), QString("Previous"));
        QCOMPARE(config.nodeId().size(), 64);
    }

    void lastEmptyIdentityAndInvalidLanguageReplacePreviousValues() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString path = directory.filePath("last.cfg");
        NodeConfig config(path); QVERIFY(config.create("Previous"));
        QFile file(path); QVERIFY(file.open(QIODevice::Append));
        const QByteArray tail("[global]\nmy_private_key=\n[gui]\nlanguage=ru\nlanguage=invalid\n");
        QCOMPARE(file.write(tail), qint64(tail.size())); file.close();
        QVERIFY(config.load()); QVERIFY(config.nodeId().isEmpty());
        QCOMPARE(config.language(), QString("system"));
    }

    void failedWriteKeepsOriginalFile() {
#ifdef Q_OS_UNIX
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString path = directory.filePath("write.cfg");
        NodeConfig config(path); QVERIFY(config.create("Previous"));
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto original = file.readAll(); file.close();
        config.setNodeName("Unsaved");
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
            struct rlimit limited = guard.previous; limited.rlim_cur = 64;
            guard.handler = std::signal(SIGXFSZ, SIG_IGN); guard.active = true;
            QVERIFY(setrlimit(RLIMIT_FSIZE, &limited) == 0);
            saved = config.save();
        }
        QVERIFY(!saved);
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), original);
#else
        QSKIP("RLIMIT_FSIZE is available on Unix");
#endif
    }
    void reloadResetsRemovedFields() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString path = directory.filePath("reload.cfg");
        NodeConfig config(path); QVERIFY(config.create("Previous profile"));
        QVERIFY(config.load());
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray minimal("[global]\nauto_socket_protocols=udp\n[gui]\nlanguage=en\n");
        QCOMPARE(file.write(minimal), qint64(minimal.size())); file.close();
        QVERIFY(config.load()); QVERIFY(!config.hasErrors());
        QVERIFY(config.nodeName().isEmpty() && config.nodeId().isEmpty());
        QVERIFY(config.pubKey().isEmpty() && config.privKey().isEmpty());
        QVERIFY(config.controlAddr().isEmpty() && config.dbPath().isEmpty() && config.debugFile().isEmpty());
        QVERIFY(config.debugLevel().isEmpty() && !config.autoSockets());
        QCOMPARE(config.autoSocketProtocols(), QString("udp"));
        QCOMPARE(config.language(), QString("en"));
    }

    void guiProjectionAcceptsNativeSettingsAndComments() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString path = directory.filePath("native.cfg");
        NodeConfig config(path); QVERIFY(config.create("Native options"));
        QFile file(path); QVERIFY(file.open(QIODevice::Append));
        const QByteArray options("[ GLOBAL ]\nclient_type=1\nauto_socket_skip_no_default_route=no\n"
            "my_node_name=Commented # note\nauto_sockets=true # enabled\n"
            "[reality]\nenabled=no\n[socks]\nenabled=no\n[stun]\nenabled=no\n[log_udp]\nip=127.0.0.1\nport=9\n");
        QCOMPARE(file.write(options), qint64(options.size())); file.close();
        Config parsed(parse_config(qPrintable(path)), free_config); QVERIFY(parsed);
        QVERIFY(config.load()); QVERIFY2(!config.hasErrors(), qPrintable(config.errors().join('\n')));
        QCOMPARE(config.nodeName(), QString("Commented"));
        QVERIFY(config.autoSockets());
        QVERIFY(config.save());
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto saved = file.readAll();
        QVERIFY(saved.contains("[reality]\nenabled=no\n[socks]\nenabled=no\n[stun]\nenabled=no"));
        QVERIFY(saved.contains("client_type=1\nauto_socket_skip_no_default_route=no"));
    }

    void rejectsNonHexIdentity() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString path = directory.filePath("identity.cfg");
        NodeConfig config(path); QVERIFY(config.create("Identity"));
        QVERIFY(config_update_option(path, "global", "my_private_key", QString(64, 'z')));
        QVERIFY(config.load()); QVERIFY(config.hasErrors());
    }

    void rejectedEditDoesNotPublishPartialChanges() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString path = directory.filePath("edit.cfg");
        NodeConfig config(path); QVERIFY(config.create("Original"));
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto original = file.readAll(); file.close();
        config.setNodeName("Injected\n[gui]");
        config.setLanguage("ru");
        QVERIFY(!config.save());
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), original);
    }
    void generatedConfigurations_data() {
        QTest::addColumn<QString>("protocols");
        for (const QString& protocols : {QString("tcp"), QString("udp"), QString("tcp+udp")})
            QTest::newRow(qPrintable(protocols)) << protocols;
    }

    void generatedConfigurations() {
        QFETCH(QString, protocols);
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString path = directory.filePath("vibechat.cfg");
        NodeConfig config(path); QVERIFY(config.create(QString::fromUtf8("Тест конфигурации")));
        config.setAutoSocketProtocols(protocols);
        for (bool utun : {false, true}) {
            config.setUtunEnabled(utun);
            for (int pass = 0; pass < 3; ++pass) {
                QVERIFY(config.save());
                QVERIFY(config.load()); QVERIFY2(!config.hasErrors(), qPrintable(config.errors().join('\n')));
                Config parsed(parse_config(qPrintable(path)), free_config); QVERIFY(parsed);
                QCOMPARE(QString::fromUtf8(parsed->global.name), config.nodeName());
                QCOMPARE(QString::fromLatin1(parsed->global.my_public_key_hex), config.pubKey());
                QCOMPARE(QString::fromLatin1(parsed->global.my_private_key_hex), config.privKey());
                QCOMPARE(parsed->global.gui_utun_enabled, int(utun));
                QCOMPARE(parsed->global.auto_sockets, 1);
                const int expected = protocols == "tcp" ? CFG_AUTO_SOCKET_TCP :
                    protocols == "udp" ? CFG_AUTO_SOCKET_UDP : CFG_AUTO_SOCKET_BOTH;
                QCOMPARE(int(parsed->global.auto_socket_protocols), expected);
            }
        }
    }

    void settingsUpdatesPreserveValidConfiguration() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString path = directory.filePath("settings.cfg");
        NodeConfig config(path); QVERIFY(config.create("Settings test"));
        QVERIFY(config_rewrite_servers(path, {{"tcp", "127.0.0.1:1333", "tcp", "private", ""},
            {"udp", "[::1]:1333", "udp", "local", ""}}));
        QVERIFY(config_update_option(path, "chatserver", "storage_unit_size", "23M"));
        QVERIFY(config_update_option(path, "chatserver", "storage_total_size", "7G"));
        QVERIFY(config_update_option(path, "chatserver", "radio_auto_enabled", "1"));
        QVERIFY(config_update_option(path, "chatserver", "radio_auto_mode", "2"));
        QVERIFY(config_update_option(path, "client:seed", "fingerprint", "openssl"));
        QVERIFY(config_update_option(path, "client:seed", "peer_public_key", config.pubKey()));
        QVERIFY(config_update_option(path, "client:seed", "link", "tcp:127.0.0.2:443"));
        QVERIFY(config.load()); QVERIFY2(!config.hasErrors(), qPrintable(config.errors().join('\n')));
        QVERIFY(config.save());
        Config parsed(parse_config(qPrintable(path)), free_config); QVERIFY(parsed);
        QCOMPARE(parsed->global.chatserver_storage_unit_size, UINT64_C(23) * 1024 * 1024);
        QCOMPARE(parsed->global.chatserver_storage_total_size, UINT64_C(7) * 1024 * 1024 * 1024);
        QCOMPARE(chat_setting_get_int_state(&parsed->global.chat_settings, "radio_auto_mode", 0), 2);
        QVERIFY(parsed->clients && parsed->clients->links && parsed->servers && parsed->servers->next);
    }

    void repeatedLoadDoesNotDuplicateDebugOptions() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString path = directory.filePath("debug.cfg");
        NodeConfig config(path); QVERIFY(config.create("Debug settings"));
        QVERIFY(config_update_option(path, "debug", "stream", "debug"));
        for (int attempt = 0; attempt < 3; ++attempt) {
            QVERIFY(config.load());
            QCOMPARE(config.debugCategories(), QString("stream=debug"));
            QVERIFY(config.save());
            Config parsed(parse_config(qPrintable(path)), free_config); QVERIFY(parsed);
        }
    }
};

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    debug_config_init(); debug_set_level(DEBUG_LEVEL_INFO);
    if (argc == 3 && !strcmp(argv[1], "--prepare")) {
        NodeConfig config(QString::fromLocal8Bit(argv[2]));
        if (config.prepare()) return 0;
        fprintf(stderr, "%s\n", qPrintable(config.errors().join('\n')));
        return 1;
    }
    if (socket_platform_init()) return 1;
    int result = 0;
    if (argc >= 3 && !strcmp(argv[1], "--validate")) {
        for (int i = 2; i < argc; ++i) {
            Config parsed(parse_config(argv[i]), free_config);
            if (!parsed) { result = 1; continue; }
            DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "Config accepted: %s storage_unit=%llu storage_total=%llu", argv[i],
                (unsigned long long)parsed->global.chatserver_storage_unit_size,
                (unsigned long long)parsed->global.chatserver_storage_total_size);
        }
    } else {
        NodeConfigTest test; result = QTest::qExec(&test, argc, argv);
    }
    socket_platform_cleanup();
    return result;
}

#include "test_node_config.moc"

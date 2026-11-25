#ifdef _WIN32
#include <winsock2.h>
#endif
#include <QCoreApplication>
#include <QApplication>
#include <QIcon>
#include <QStyleFactory>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QMessageBox>
#include <QTranslator>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QHostInfo>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include "gui_language.h"
#include "gui_theme.h"
#include "video_capture.h"
#include "call_video_engine.h"
#include "single_instance.h"
#include "desktop_integration.h"
#ifdef Q_OS_LINUX
#include "profile_paths.h"
#endif
#include "gui_log.h"
#include "config_parser.h"
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <future>
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#include <io.h>
#endif
#include "mainwindow.h"
#include "startupfailure_dialog.h"
#include "../../../src/db_storage.h"
#include <memory>
#include "../db/db_manager.h"
#include "../transport/node_config.h"
#include "../transport/utun_node.h"
#include "../../lib/debug_config.h"

#ifdef _WIN32
static UtunNode* g_ctrl_node = nullptr;
static MainWindow* g_mainWindow = nullptr;

// GUI EXE не создаёт консоль; подключаем только родительскую, сохраняя перенаправленные потоки.
static bool attach_parent_console(void) {
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        const DWORD error = GetLastError();
        if (error != ERROR_ACCESS_DENIED) {
            if (error != ERROR_INVALID_HANDLE && error != ERROR_INVALID_PARAMETER)
                DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "Windows parent console attach failed: error=%lu", (unsigned long)error);
            return false;
        }
    }
    FILE* streams[] = {stdin, stdout, stderr};
    const char* devices[] = {"CONIN$", "CONOUT$", "CONOUT$"};
    for (int i = 0; i < 3; ++i) {
        const int fd = _fileno(streams[i]);
        const HANDLE handle = fd < 0 ? INVALID_HANDLE_VALUE : (HANDLE)_get_osfhandle(fd);
        if (handle && handle != INVALID_HANDLE_VALUE && GetFileType(handle) != FILE_TYPE_UNKNOWN) continue;
        if (!freopen(devices[i], i == 0 ? "r" : "w", streams[i]))
            DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "Windows console stream reopen failed: stream=%d errno=%d", i, errno);
    }
    return true;
}

static DWORD count_threads(void) {
    DWORD pid = GetCurrentProcessId(), count = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te = { sizeof(te) };
    if (Thread32First(snap, &te)) {
        do { if (te.th32OwnerProcessID == pid) count++; }
        while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return count;
}

static BOOL WINAPI console_ctrl_handler(DWORD dwCtrlType) {
    if (dwCtrlType == CTRL_C_EVENT || dwCtrlType == CTRL_BREAK_EVENT) {
        MainWindow* window = g_mainWindow;
        if (window)
            QMetaObject::invokeMethod(window, [window] {
                window->requestShutdown();
            }, Qt::QueuedConnection);
        else if (g_ctrl_node)
            g_ctrl_node->requestStop();
        return TRUE;
    }
    return FALSE;
}
#endif

static struct ShutdownSentinel {
    ~ShutdownSentinel() {
        DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "main: static destructors done");
#ifdef _WIN32
        DWORD threads = count_threads();
        DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "main: threads at exit=%lu", (unsigned long)threads);
        if (threads > 4) {
            DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "main: %lu hanging threads, TerminateProcess", (unsigned long)threads);
            fflush(nullptr);
            TerminateProcess(GetCurrentProcess(), 0);
        }
#endif
    }
} g_shutdown_sentinel;

int main(int argc, char *argv[]) {
    if (argc >= 2 && std::strcmp(argv[1], "--video-capture-worker") == 0) return video_capture_worker_main(argc, argv);
    debug_config_init();
    /* До чтения конфига видны запуск и IPC; затем уровень заменяет настройка профиля. */
    debug_set_level(DEBUG_LEVEL_INFO);
    installGuiLogHandler();
#ifdef _WIN32
    const bool consoleAttached = attach_parent_console();
#endif
#ifdef Q_OS_LINUX
    /* libva пишет INFO напрямую в stderr; ошибки сохраняем, явную настройку пользователя не заменяем. */
    if (!qEnvironmentVariableIsSet("LIBVA_MESSAGING_LEVEL")) qputenv("LIBVA_MESSAGING_LEVEL", "1");
    /* На Wayland композитор владеет позицией окна (move() игнорируется) —
     * для полноценного ручного move/resize с пропорцией запускаемся через XWayland. */
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "xcb");
#endif
    QApplication app(argc, argv);
    app.setApplicationName("vibechat");
    app.setApplicationVersion(QStringLiteral(VIBECHAT_VERSION));
    app.setDesktopFileName("org.utun.vibechat");
    registerDesktopApplication(QCoreApplication::applicationFilePath());
    app.setWindowIcon(QIcon(":/icons/app_icon.webp"));
    app.setQuitOnLastWindowClosed(false);
    const bool minimized = app.arguments().contains(QStringLiteral("--minimized"));
    QString startupUrl;
    for (const auto& argument : app.arguments().mid(1)) {
        if (argument == QStringLiteral("--") || argument == QStringLiteral("--minimized")) continue;
        if (!startupUrl.isEmpty() || !argument.startsWith(QStringLiteral("utun://"))) {
            DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "GUI startup: invalid arguments"); return 1;
        }
        startupUrl = argument;
    }

    QTranslator guiTranslator, qtTranslator;
    auto loadLanguage = [&](const QString& selected) {
        app.removeTranslator(&guiTranslator);
        app.removeTranslator(&qtTranslator);
        if (!guiTranslator.load(":/translations/chatgui_" + selected + ".qm") || !app.installTranslator(&guiTranslator)) {
            DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "GUI language: failed to load embedded catalog for %s", qPrintable(selected));
            return false;
        }
        if (selected == "ru") {
            if (!qtTranslator.load(QStringLiteral(":/translations/qtbase_ru.qm")) || !app.installTranslator(&qtTranslator)) {
                DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "GUI language: failed to load embedded Qt Russian catalog");
                return false;
            }
        }
        return true;
    };
    QString language = resolveGuiLanguage(QStringLiteral("system"));
    if (!loadLanguage(language)) return 1;

    QString cfgDir = QCoreApplication::applicationDirPath();
#ifdef Q_OS_LINUX
    QString storageReason;
    while ((cfgDir = linuxProfileDirectory(QCoreApplication::applicationDirPath(), storageReason)).isEmpty()) {
        StartupFailureDialog failure(QCoreApplication::translate("ChatGui", "Cannot access the profile folder. Check its permissions and retry.")
                                     + '\n' + storageReason, {}, StartupFailureDialog::Kind::Service);
        failure.exec();
        if (failure.choice() == StartupFailureDialog::Action::Exit) return 1;
    }
#endif
#ifdef _WIN32
    QString storageReason;
    {
        /* Проверяем существующий конфиг без создания, обрезки или изменения содержимого. */
        QFile configFile(cfgDir + "/vibechat.cfg");
        if (!configFile.exists()) {
            storageReason = QStringLiteral("config missing in executable directory");
        } else if (!configFile.open(QIODevice::ReadWrite | QIODevice::ExistingOnly)) {
            storageReason = QStringLiteral("config is not writable: %1").arg(configFile.errorString());
        }
    }
    if (!storageReason.isEmpty()) {
        cfgDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        while (cfgDir.isEmpty() || !QDir().mkpath(cfgDir)) {
            DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "Cannot create LocalAppData directory %s; fallback reason: %s",
                        qPrintable(cfgDir), qPrintable(storageReason));
            StartupFailureDialog failure(QCoreApplication::translate("ChatGui", "Cannot access the profile folder. Check its permissions and retry.")
                                         + '\n' + cfgDir, storageReason, StartupFailureDialog::Kind::Service);
            failure.exec();
            if (failure.choice() == StartupFailureDialog::Action::Exit) return 1;
            cfgDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        }
    } else {
        storageReason = QStringLiteral("writable config found in executable directory");
    }
#endif

    /* Второй экземпляр передаёт запрос владельцу до чтения конфига и запуска ядра. */
    SingleInstance instance(cfgDir);
    const auto role = instance.start(startupUrl);
    if (role == SingleInstance::Activated) return 0;
    if (role == SingleInstance::Error) {
        QMessageBox::critical(nullptr, "vibechat", instance.errorString());
        return 1;
    }

    /* Ошибка подготовки INI возможна ещё до чтения debug_file из профиля. */
    const QString startupLogFile = cfgDir + "/chatgui.log";
    const QByteArray startupLogPath = startupLogFile.toUtf8();
    debug_set_file_rotation(10 * 1024 * 1024, 5);
    debug_enable_file_output(startupLogPath.constData(), 0);
    if (g_debug_config.file_output) debug_rotate_log();
    DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "GUI startup: version=%s config_directory=%s", qPrintable(app.applicationVersion()), qPrintable(cfgDir));
#ifdef _WIN32
    DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "Windows storage selection: %s", qPrintable(storageReason));
#elif defined(Q_OS_LINUX)
    DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "Linux storage selection: %s", qPrintable(storageReason));
#endif

    app.setStyle(QStyleFactory::create("Fusion"));

    QString cfgPath = cfgDir + "/vibechat.cfg";
    /* миграция: старый конфиг chatgui.cfg → vibechat.cfg */
    if (!QFile::exists(cfgPath) && QFile::exists(cfgDir + "/chatgui.cfg"))
        QFile::rename(cfgDir + "/chatgui.cfg", cfgPath);

    NodeConfig cfg(cfgPath);
    const bool firstRun = !cfg.exists();
    if (firstRun) {
        QDialog wizard;
        wizard.setMinimumWidth(380);
        auto* layout = new QVBoxLayout(&wizard);
        auto* languageLabel = new QLabel(QStringLiteral("Language / Язык"), &wizard);
        layout->addWidget(languageLabel);
        auto* languageCombo = new QComboBox(&wizard);
        languageCombo->addItem(QString(), QStringLiteral("system"));
        languageCombo->addItem(QStringLiteral("English"), QStringLiteral("en"));
        languageCombo->addItem(QStringLiteral("Русский"), QStringLiteral("ru"));
        layout->addWidget(languageCombo);
        auto* nicknameLabel = new QLabel(&wizard);
        layout->addWidget(nicknameLabel);
        auto* nickname = new QLineEdit(&wizard);
        nickname->setMaxLength(32);
        const QString host = QHostInfo::localHostName();
        nickname->setText(host.isEmpty() ? QStringLiteral("vibechat") : host);
        layout->addWidget(nickname);
        auto* hint = new QLabel(&wizard);
        hint->setWordWrap(true);
        layout->addWidget(hint);
        layout->addSpacing(12);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &wizard);
        auto* start = buttons->addButton(QString(), QDialogButtonBox::AcceptRole);
        start->setDefault(true);
        layout->addWidget(buttons);
        auto translateWizard = [&] {
            wizard.setWindowTitle(QCoreApplication::translate("ChatGui", "Welcome to vibechat"));
            languageCombo->setItemText(0, QCoreApplication::translate("ChatGui", "System language"));
            nicknameLabel->setText(QCoreApplication::translate("ChatGui", "Your nickname:"));
            hint->setText(QCoreApplication::translate("ChatGui", "Others will see this name. You can change it later in Settings."));
            start->setText(QCoreApplication::translate("ChatGui", "Start chatting"));
        };
        translateWizard();
        bool languageReady = true;
        QObject::connect(languageCombo, qOverload<int>(&QComboBox::currentIndexChanged), &wizard, [&](int) {
            const QString preference = languageCombo->currentData().toString();
            language = resolveGuiLanguage(preference);
            languageReady = loadLanguage(language);
            if (!languageReady) { wizard.reject(); return; }
            translateWizard();
            DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "First-run setup: language preference=%s selected=%s", qPrintable(preference), qPrintable(language));
        });
        QObject::connect(nickname, &QLineEdit::textChanged, &wizard, [start](const QString& text) {
            start->setEnabled(!text.trimmed().isEmpty());
        });
        QObject::connect(buttons, &QDialogButtonBox::accepted, &wizard, [&wizard, nickname] {
            const QByteArray name = nickname->text().trimmed().toUtf8();
            if (name.size() >= MAX_CONN_NAME_LEN) {
                DEBUG_WARN(DEBUG_CATEGORY_CONFIG, "First-run setup: nickname too long, bytes=%d limit=%d", int(name.size()), MAX_CONN_NAME_LEN - 1);
                QMessageBox::warning(&wizard, wizard.windowTitle(),
                    QCoreApplication::translate("ChatGui", "This nickname is too long. Please choose a shorter one."));
                return;
            }
            wizard.accept();
        });
        QObject::connect(buttons, &QDialogButtonBox::rejected, &wizard, &QDialog::reject);
        nickname->setFocus();
        nickname->selectAll();
        DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "First-run setup: requesting nickname and language, config=%s", qPrintable(cfgPath));
        instance.setWindow(&wizard);
        const int setupResult = wizard.exec();
        instance.setWindow(nullptr);
        if (setupResult != QDialog::Accepted) {
            if (!languageReady) return 1;
            DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "First-run setup: cancelled, config not created");
            return 0;
        }
        cfg.setLanguage(languageCombo->currentData().toString());
        if (!cfg.create(nickname->text().trimmed())) {
            DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "First-run setup: failed to create config=%s", qPrintable(cfgPath));
            QMessageBox::critical(nullptr, wizard.windowTitle(),
                QCoreApplication::translate("ChatGui", "Failed to save your profile. Check that the data folder is writable."));
            return 1;
        }
#ifdef Q_OS_LINUX
        if (!setDesktopAutostartEnabled(true, QCoreApplication::applicationFilePath())) {
            DEBUG_WARN(DEBUG_CATEGORY_CONFIG, "First-run setup: profile saved but default Linux autostart could not be enabled");
        }
#endif
    }

    using Kind = StartupFailureDialog::Kind;
    using Action = StartupFailureDialog::Action;
    auto recoveryChoice = [&](const QString& reason, const QString& details, Kind kind) {
        StartupFailureDialog failure(reason, details, kind);
        instance.setWindow(&failure);
        Action action;
        do {
            failure.exec();
            action = failure.choice();
            if (action == Action::Edit) openConfigurationEditor(cfgPath, &failure);
        } while (action == Action::Edit);
        instance.setWindow(nullptr);
        return action;
    };

    std::unique_ptr<DbManager> database;
    std::unique_ptr<MainWindow> window;
    QByteArray debugFilePath;
    bool replaceInvalid = false;
    for (;;) {
        const bool prepared = cfg.prepare(replaceInvalid);
        replaceInvalid = false;
        const QString selected = resolveGuiLanguage(cfg.language());
        if (language != selected && !loadLanguage(selected)) return 1;
        language = selected;
        if (!prepared) {
            const auto status = cfg.startupStatus();
            const QString details = cfg.errors().join('\n');
            QString reason = cfg.startupMessage();
            if (status != CONFIG_PROFILE_INVALID_KEY && !details.isEmpty())
                reason += '\n' + details.section('\n', 0, 0);
            DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "Chat profile rejected status=%d path=%s cause=%s", int(status), qPrintable(cfgPath), qPrintable(details));
            auto action = recoveryChoice(reason, details, status == CONFIG_PROFILE_INVALID_KEY ? Kind::Identity : Kind::Configuration);
            if (action == Action::Exit) return 1;
            replaceInvalid = action == Action::ReplaceKey;
            continue;
        }
        applyGuiTheme(cfg.theme());
        QString dbPath = cfg.dbPath();
        if (dbPath.isEmpty()) dbPath = "chat_data";
        if (!QDir::isAbsolutePath(dbPath))
            dbPath = cfgDir + "/" + dbPath;
        if (!QDir().mkpath(dbPath)) {
            DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "Cannot create GUI database directory: %s", qPrintable(dbPath));
            const QString reason = QCoreApplication::translate("ChatGui", "Cannot create the database folder. Check the path and permissions.\n%1").arg(dbPath);
            if (recoveryChoice(reason, {}, Kind::Configuration) == Action::Exit) return 1;
            continue;
        }

        QString debugFile = cfg.debugFile();
        if (debugFile.isEmpty()) debugFile = "chatgui.log";
        if (!QDir::isAbsolutePath(debugFile))
            debugFile = cfgDir + "/" + debugFile;

        /* ранняя инициализация лога в GUI-потоке: SoundManager::init() вызывается в
         * конструкторе MainWindow ДО старта utun-потока, поэтому без этого его диагностика
         * (ma_engine_init и т.п.) в лог-файл не попадёт. */
        debug_enable_function_name(0);
        debugFilePath = debugFile.toUtf8(); // Logger заимствует путь до закрытия/замены файла.
        debug_enable_file_output(debugFilePath.constData(), 0);
        if (g_debug_config.output_file == debugFilePath.constData() &&
            QFileInfo(debugFile).canonicalFilePath() != QFileInfo(startupLogFile).canonicalFilePath()) debug_rotate_log();
        QString lvl = cfg.debugLevel().trimmed();
        if (lvl.isEmpty()) lvl = QStringLiteral("info");
        debug_apply_global_level(lvl.toUtf8().constData());
        if (!cfg.debugCategories().trimmed().isEmpty()) debug_parse_config(cfg.debugCategories().toUtf8().constData());
        DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "vibechat: version=%s", qPrintable(app.applicationVersion()));
        if (firstRun)
            DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "First-run setup: profile saved, nickname=%s config=%s", qPrintable(cfg.nodeName()), qPrintable(cfgPath));
#ifdef _WIN32
        DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "Windows console: attached=%d", consoleAttached);
        DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "Windows storage: reason=%s config=%s db=%s log=%s lock=%s",
                   qPrintable(storageReason), qPrintable(cfgPath), qPrintable(dbPath), qPrintable(debugFile),
                   qPrintable(cfgDir + "/vibechat.lock"));
#elif defined(Q_OS_LINUX)
        DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "Linux storage: reason=%s config=%s db=%s log=%s lock=%s",
                   qPrintable(storageReason), qPrintable(cfgPath), qPrintable(dbPath), qPrintable(debugFile),
                   qPrintable(cfgDir + "/vibechat.lock"));
#endif
        DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "GUI language: preference=%s system=%s selected=%s",
                   qPrintable(cfg.language()), qPrintable(QLocale::system().uiLanguages().join(',')), qPrintable(language));

        const qint64 startupLogOffset = QFileInfo(debugFile).size();
        database = std::make_unique<DbManager>();
        database->setAsyncReader(gui_bridge_db_read);
        window = std::make_unique<MainWindow>(nullptr, database.get(), cfgPath, dbPath, debugFile,
            cfg.debugLevel(), cfg.debugCategories());
        if (window->utunNode() && window->utunNode()->isRunning()) break;
        QString reason = window->utunNode() ? window->utunNode()->startupError() : QStringLiteral("UtunNode was not created");
        const bool canRecreate = window->utunNode() && window->utunNode()->startupDatabaseIncompatible();
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "GUI startup aborted: %s log=%s", qPrintable(reason), qPrintable(debugFile));
        const bool configFailed = window->utunNode() && window->utunNode()->startupConfigFailed();
        QFile startupLog(debugFile);
        QString details = reason;
        if (startupLog.open(QIODevice::ReadOnly) && startupLog.seek(startupLogOffset)) details += '\n' + QString::fromUtf8(startupLog.readAll());
        window.reset();
        database.reset();
        debug_enable_file_output(startupLogPath.constData(), 0);
        if (!canRecreate) {
            if (recoveryChoice(reason, details, configFailed ? Kind::Configuration : Kind::Service) == Action::Exit) return 1;
            continue;
        }
        reason = QCoreApplication::translate("ChatGui", "The database format is incompatible with this version.\n%1")
                     .arg(QDir(dbPath).filePath("chats.db"));
        for (;;) {
            if (recoveryChoice(reason, details, Kind::Database) != Action::RecreateDatabase) return 1;
            DEBUG_INFO(DEBUG_CATEGORY_SYS, "GUI startup: user requested database recreation directory=%s", qPrintable(dbPath));
            QString error;
            try {
                int result = std::async(std::launch::async, [dbPath] { return db_storage_clear(dbPath.toUtf8().constData()); }).get();
                if (!result) break;
                error = QString::fromLocal8Bit(std::strerror(-result));
            } catch (const std::exception& e) {
                error = QString::fromLocal8Bit(e.what());
            }
            DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Database recreation failed directory=%s error=%s", qPrintable(dbPath), qPrintable(error));
            reason = QCoreApplication::translate("ChatGui", "Could not recreate database:\n%1\n%2").arg(dbPath, error);
        }
    }
    MainWindow& w = *window;
    instance.setWindow(&w);
    instance.setUrlHandler([&w](const QString& url) { w.onMessageLinkActivated(url); });
    if (minimized) w.showMinimized(); else w.show();
    // Пробы не используют камеры и сеть; будущие звонки берут общий кэш без повторной подготовки.
    std::atomic<bool> prepareVideo{true};
    std::future<void> videoPreparation;
    try {
        videoPreparation = std::async(std::launch::async, [&prepareVideo] { CallVideoEngine::capabilities(&prepareVideo); });
    } catch (const std::exception& error) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video codec background preparation could not start: %s", error.what());
    }
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "GUI startup: minimized_requested=%d minimized=%d activation_server=%s",
        minimized, w.isMinimized(), qPrintable(instance.serverName()));

#ifdef _WIN32
    g_ctrl_node = w.utunNode();
    g_mainWindow = &w;
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
#endif

    int ret = app.exec();
    instance.setUrlHandler({});
    prepareVideo = false;
    if (videoPreparation.valid()) {
        try { videoPreparation.get(); }
        catch (const std::exception& error) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video codec background preparation failed: %s", error.what()); }
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "main: app.exec() returned %d", ret);
    return ret;
}

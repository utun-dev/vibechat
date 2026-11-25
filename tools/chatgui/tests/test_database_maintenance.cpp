/* Настоящие SQLite/DbManager/Qt-страница: optimize без строк, строгие integrity-проверки и SQL-отказы. */
#include "../db/db_manager.h"
#include "../src/databasesettingspage.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QTimer>
#include <QScopeGuard>
#include <cstring>
extern "C" {
#include "debug_config.h"
#include "db_async.h"
}

/* RAM-стенд формы: результат отложен в GUI. Настоящий worker проверяет test_gui_db_read. */
static bool memoryReader(sqlite3* db, QObject* context, GuiDbReadWork work, GuiDbReadDone complete) {
    QTimer::singleShot(0, context, [db, work = std::move(work), complete = std::move(complete)] {
        QVariant value; db_async_result result{}; result.sqlite_code = work(db, &value);
        if (result.sqlite_code != SQLITE_OK) {
            result.status = DB_ASYNC_SQL; qstrncpy(result.message, sqlite3_errmsg(db), sizeof(result.message));
        }
        complete(result, value);
    });
    return true;
}

using Operation = IntegrityResult (DbManager::*)() const;
static const Operation operations[] = {&DbManager::integrityCheckQuick, &DbManager::integrityCheckFull, &DbManager::integrityOptimize};
static const char* titles[] = {"Quick Check", "Full Check", "Optimize"};
static const char* actionSlots[] = {"onQuickCheck", "onFullCheck", "onOptimize"};

struct Database {
    QTemporaryDir directory;
    sqlite3* handle = nullptr;
    DbManager manager;
    ~Database() {
        manager.setDb(nullptr);
        if (handle && sqlite3_close(handle) != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "test DB close failed: statements remain");
    }
    bool execute(const char* sql) {
        char* error = nullptr;
        const int rc = sqlite3_exec(handle, sql, nullptr, nullptr, &error);
        if (rc != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "test SQL failed rc=%d: %s", rc, error ? error : sqlite3_errmsg(handle));
        sqlite3_free(error);
        return rc == SQLITE_OK;
    }
    bool open(bool disk = false) {
        if (!directory.isValid()) return false;
        const QByteArray path = disk ? directory.filePath("test.db").toUtf8() : QByteArray(":memory:");
        const int rc = sqlite3_open(path.constData(), &handle);
        if (rc != SQLITE_OK) { DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "test DB open failed rc=%d", rc); return false; }
        manager.setDb(handle);
        manager.setAsyncReader(memoryReader);
        return execute("CREATE TABLE items(value INTEGER CHECK(value>0)); CREATE INDEX item_value ON items(value);"
                       "INSERT INTO items VALUES(1),(2),(3); SELECT value FROM items WHERE value=2;");
    }
};

static int denyPragma(void*, int action, const char*, const char*, const char*, const char*) {
    return action == SQLITE_PRAGMA ? SQLITE_DENY : SQLITE_OK;
}

static QString statusText(DatabaseSettingsPage& page) {
    for (auto* label : page.findChildren<QLabel*>())
        for (const auto* title : titles) if (label->text().startsWith(QString::fromLatin1(title) + ':')) return label->text();
    return {};
}

class DatabaseMaintenanceTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
        debug_set_category_level(DEBUG_CATEGORY_CONFIG, DEBUG_LEVEL_DEBUG);
    }
    void healthyOperations_data() {
        QTest::addColumn<int>("operation"); QTest::addColumn<bool>("disk");
        for (int operation = 0; operation < 3; ++operation)
            for (bool disk : {false, true})
                QTest::newRow(qPrintable(QString("%1-%2").arg(titles[operation]).arg(disk ? "file" : "memory"))) << operation << disk;
    }
    void healthyOperations() {
        QFETCH(int, operation); QFETCH(bool, disk);
        Database db; QVERIFY(db.open(disk));
        for (int repeat = 0; repeat < 2; ++repeat) {
            const auto result = (db.manager.*operations[operation])();
            QVERIFY2(result.ok, qPrintable(result.message));
            QCOMPARE(result.message, QString("ok")); QVERIFY(result.errors.isEmpty()); QVERIFY(result.elapsedMs >= 0);
            QVERIFY(!sqlite3_next_stmt(db.handle, nullptr));
        }
    }
    void missingDatabase_data() {
        QTest::addColumn<int>("operation");
        for (int operation = 0; operation < 3; ++operation) QTest::newRow(titles[operation]) << operation;
    }
    void missingDatabase() {
        QFETCH(int, operation);
        DbManager manager;
        const auto result = (manager.*operations[operation])();
        QVERIFY(!result.ok); QVERIFY(!result.message.isEmpty()); QCOMPARE(result.elapsedMs, 0);
    }
    void sqlFailure_data() {
        QTest::addColumn<int>("operation");
        for (int operation = 0; operation < 3; ++operation) QTest::newRow(titles[operation]) << operation;
    }
    void sqlFailure() {
        QFETCH(int, operation);
        Database db; QVERIFY(db.open());
        QCOMPARE(sqlite3_set_authorizer(db.handle, denyPragma, nullptr), SQLITE_OK);
        const auto result = (db.manager.*operations[operation])();
        QVERIFY(!result.ok); QVERIFY2(result.message.contains("authorized"), qPrintable(result.message));
        QVERIFY(!sqlite3_next_stmt(db.handle, nullptr));
        QCOMPARE(sqlite3_set_authorizer(db.handle, nullptr, nullptr), SQLITE_OK);
        const auto retry = (db.manager.*operations[operation])();
        QVERIFY2(retry.ok, qPrintable(retry.message));
    }
    void integrityViolation_data() {
        QTest::addColumn<int>("operation");
        QTest::newRow("quick") << 0; QTest::newRow("full") << 1;
    }
    void integrityViolation() {
        QFETCH(int, operation);
        Database db; QVERIFY(db.open());
        QVERIFY(db.execute("PRAGMA ignore_check_constraints=ON; INSERT INTO items VALUES(-1),(-2); PRAGMA ignore_check_constraints=OFF;"));
        const auto result = (db.manager.*operations[operation])();
        QVERIFY(!result.ok); QVERIFY2(result.message.contains("CHECK constraint failed"), qPrintable(result.message));
        QVERIFY(!result.errors.isEmpty()); QVERIFY(result.errors.first().contains("CHECK constraint failed"));
    }
    void pageShowsResult_data() {
        QTest::addColumn<int>("operation"); QTest::addColumn<bool>("denied");
        for (int operation = 0; operation < 3; ++operation)
            for (bool denied : {false, true})
                QTest::newRow(qPrintable(QString("%1-%2").arg(titles[operation]).arg(denied ? "denied" : "success")))
                    << operation << denied;
    }
    void pageShowsResult() {
        QFETCH(int, operation); QFETCH(bool, denied);
        Database db; QVERIFY(db.open());
        DatabaseSettingsPage page(&db.manager);
        if (denied) QCOMPARE(sqlite3_set_authorizer(db.handle, denyPragma, nullptr), SQLITE_OK);
        QVERIFY(QMetaObject::invokeMethod(&page, actionSlots[operation], Qt::DirectConnection));
        const auto buttons = page.findChildren<QPushButton*>(); QCOMPARE(buttons.size(), 3);
        for (auto* button : buttons) QVERIFY(!button->isEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(!statusText(page).isEmpty(), 1000);
        QVERIFY2(statusText(page).contains(denied ? ": FAILED" : ": OK"), qPrintable(statusText(page)));
        const auto details = page.findChild<QPlainTextEdit*>()->toPlainText();
        if (denied) QVERIFY2(details.contains("authorized"), qPrintable(details));
        else QCOMPARE(details, QString("ok"));
        for (auto* button : buttons) QVERIFY(button->isEnabled());
    }
    void pageSurvivesManagerDestructionWhileCheckIsPending() {
        Database db; QVERIFY(db.open()); auto* manager = new DbManager;
        manager->setDb(db.handle); manager->setAsyncReader(memoryReader); DatabaseSettingsPage page(manager);
        QVERIFY(QMetaObject::invokeMethod(&page, "onQuickCheck", Qt::DirectConnection)); delete manager;
        QTRY_VERIFY(!statusText(page).isEmpty()); QVERIFY(statusText(page).contains(": FAILED"));
        for (auto* button : page.findChildren<QPushButton*>()) QVERIFY(!button->isEnabled());
        page.refreshStatus(); QVERIFY(QMetaObject::invokeMethod(&page, "onFullCheck", Qt::DirectConnection));
        for (auto* button : page.findChildren<QPushButton*>()) QVERIFY(!button->isEnabled());
    }
    void delayedOptimizeCannotRunOnAReplacedDatabase() {
        Database db; QVERIFY(db.open()); DatabaseSettingsPage page(&db.manager); int optimizations = 0;
        QCOMPARE(sqlite3_set_authorizer(db.handle, [](void* data, int action, const char* pragma, const char*, const char*, const char*) {
            if (action == SQLITE_PRAGMA && pragma && !strcmp(pragma, "optimize")) (*static_cast<int*>(data))++;
            return SQLITE_OK;
        }, &optimizations), SQLITE_OK);
        auto reset = qScopeGuard([&] { sqlite3_set_authorizer(db.handle, nullptr, nullptr); });
        QVERIFY(QMetaObject::invokeMethod(&page, "onOptimize", Qt::DirectConnection));
        db.manager.setDb(nullptr); db.manager.setDb(db.handle); page.refreshStatus();
        for (auto* button : page.findChildren<QPushButton*>()) QVERIFY(!button->isEnabled());
        QTRY_VERIFY(!statusText(page).isEmpty()); QVERIFY(statusText(page).contains(": FAILED")); QCOMPARE(optimizations, 0);
        QVERIFY(QMetaObject::invokeMethod(&page, "onOptimize", Qt::DirectConnection));
        QTRY_VERIFY(!statusText(page).isEmpty()); QVERIFY(statusText(page).contains(": OK")); QCOMPARE(optimizations, 1);
    }
};

QTEST_MAIN(DatabaseMaintenanceTest)
#include "test_database_maintenance.moc"

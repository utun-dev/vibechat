/* Настоящие NodesPage/SQLite: полный снимок, отказ чтения, прерывание и восстановление без частичных данных. */
#include "../src/nodespage.h"
#include "../db/db_manager.h"
#include <QtTest>
#include <cstring>
extern "C" {
#include "debug_config.h"
}

#ifdef UTUN_TEST_NODE_FINALIZE_FAILURE
static bool failFinalize;
extern "C" int __real_sqlite3_finalize(sqlite3_stmt*);
extern "C" int __wrap_sqlite3_finalize(sqlite3_stmt* statement) {
    const int rc = __real_sqlite3_finalize(statement);
    if (!failFinalize || !statement || rc != SQLITE_OK) return rc;
    failFinalize = false;
    DEBUG_WARN(DEBUG_CATEGORY_CONFIG, "[INJECT] nodes statement finalize failure");
    return SQLITE_IOERR;
}
#endif

struct NodesDatabase {
    sqlite3* handle = nullptr;
    DbManager manager;
    ~NodesDatabase() {
        manager.setDb(nullptr);
        if (!handle) return;
        sqlite3_set_authorizer(handle, nullptr, nullptr);
        sqlite3_progress_handler(handle, 0, nullptr, nullptr); sqlite3_trace_v2(handle, 0, nullptr, nullptr);
        if (sqlite3_close(handle) != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "nodes test DB close failed: statements remain");
    }
    bool execute(const char* sql) {
        char* error = nullptr;
        const int rc = sqlite3_exec(handle, sql, nullptr, nullptr, &error);
        if (rc != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "nodes test SQL failed rc=%d: %s", rc, error ? error : sqlite3_errmsg(handle));
        sqlite3_free(error); return rc == SQLITE_OK;
    }
    bool open() {
        const int rc = sqlite3_open(":memory:", &handle);
        if (rc != SQLITE_OK) { DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "nodes test DB open failed rc=%d", rc); return false; }
        manager.setDb(handle);
        return execute("CREATE TABLE nodes(node_id INTEGER PRIMARY KEY,name TEXT,x25519_pubkey BLOB,ed25519_pubkey BLOB,"
            "last_seen_at INTEGER,created_at INTEGER);"
            "CREATE TABLE node_addresses(node_x BLOB NOT NULL,family INTEGER NOT NULL,protocol INTEGER,address BLOB NOT NULL,"
            "port INTEGER,rtt INTEGER,nat_mapping INTEGER,nat_inbound INTEGER,nat_source INTEGER);"
            "INSERT INTO nodes VALUES(1,'Alpha',zeroblob(32),zeroblob(32),1,1),(2,'Beta',NULL,NULL,0,0);"
            "INSERT INTO node_addresses VALUES(zeroblob(32),4,1,x'7F000001',1333,45,0,0,0),"
            "(zeroblob(32),6,6,x'20010DB8000000000000000000000001',8443,0,1,0,1);");
    }
};

struct InterruptState { bool afterRow = false, interrupted = false; int rows = 0; };

class NodesPageTest : public QObject {
    Q_OBJECT
    void checkSnapshot(NodesPage& page) {
        const auto* table = page.findChild<QTableWidget*>();
        const auto text = page.findChild<QPlainTextEdit*>()->toPlainText();
        QCOMPARE(table->rowCount(), 2); QCOMPARE(table->columnCount(), 5);
        QCOMPARE(table->item(0, 0)->text(), QString("0000000000000001"));
        QCOMPARE(table->item(0, 1)->text(), QString("Alpha")); QCOMPARE(table->item(0, 4)->text(), QString("2"));
        QCOMPARE(table->item(1, 1)->text(), QString("Beta")); QCOMPARE(table->item(1, 4)->text(), QString("0"));
        QVERIFY2(text.contains("(Alpha)") && text.contains("(Beta)"), qPrintable(text));
        QVERIFY2(text.contains("127.0.0.1:1333  UDP  NAT not determined"), qPrintable(text));
        QVERIFY2(text.contains("2001:db8:0:0:0:0:0:1:8443  TCP+REALITY  rtt=-ms\n"), qPrintable(text));
        QVERIFY2(text.contains("rtt=4ms") && text.contains("rtt=-ms"), qPrintable(text));
        QVERIFY(text.contains("addresses: none"));
    }
    void checkFailure(NodesPage& page) {
        QCOMPARE(page.findChild<QTableWidget*>()->rowCount(), 0);
        const auto text = page.findChild<QPlainTextEdit*>()->toPlainText();
        QVERIFY2(text.contains("Query error"), qPrintable(text));
        QVERIFY(!text.contains("(Alpha)")); QVERIFY(!text.contains("(Beta)"));
    }
private slots:
    void initTestCase() {
        debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
        debug_set_category_level(DEBUG_CATEGORY_CONFIG, DEBUG_LEVEL_DEBUG);
    }
    void refreshAndEmptySnapshot() {
        NodesDatabase db; QVERIFY(db.open()); NodesPage page(&db.manager);
        page.refreshNodes(); checkSnapshot(page);
        QVERIFY(db.execute("UPDATE nodes SET name='Updated' WHERE node_id=1; DELETE FROM node_addresses WHERE family=4;"));
        page.refreshNodes();
        const auto* table = page.findChild<QTableWidget*>();
        const auto text = page.findChild<QPlainTextEdit*>()->toPlainText();
        QCOMPARE(table->item(0, 1)->text(), QString("Updated")); QCOMPARE(table->item(0, 4)->text(), QString("1"));
        QVERIFY(text.contains("(Updated)")); QVERIFY(!text.contains("(Alpha)")); QVERIFY(!text.contains("127.0.0.1"));
        QVERIFY(db.execute("DELETE FROM node_addresses; DELETE FROM nodes;")); page.refreshNodes();
        QCOMPARE(table->rowCount(), 0);
        QCOMPARE(page.findChild<QPlainTextEdit*>()->toPlainText(), QString("No nodes found."));
        QVERIFY(!sqlite3_next_stmt(db.handle, nullptr));
    }
    void readFailure_data() {
        QTest::addColumn<QByteArray>("table");
        QTest::newRow("nodes") << QByteArray("nodes"); QTest::newRow("addresses") << QByteArray("node_addresses");
    }
    void readFailure() {
        QFETCH(QByteArray, table);
        NodesDatabase db; QVERIFY(db.open()); NodesPage page(&db.manager);
        page.refreshNodes(); checkSnapshot(page);
        QCOMPARE(sqlite3_set_authorizer(db.handle, [](void* context, int action, const char* name, const char*, const char*, const char*) {
            if (action != SQLITE_READ || !name || strcmp(name, static_cast<const char*>(context))) return SQLITE_OK;
            DEBUG_WARN(DEBUG_CATEGORY_CONFIG, "[INJECT] nodes read denied table=%s", name); return SQLITE_DENY;
        }, table.data()), SQLITE_OK);
        page.refreshNodes(); checkFailure(page);
        QVERIFY(!sqlite3_next_stmt(db.handle, nullptr));
        QCOMPARE(sqlite3_set_authorizer(db.handle, nullptr, nullptr), SQLITE_OK);
        page.refreshNodes(); checkSnapshot(page);
    }
    void interruptedSnapshot_data() {
        QTest::addColumn<bool>("afterRow");
        QTest::newRow("before-first-row") << false; QTest::newRow("after-row") << true;
    }
    void interruptedSnapshot() {
        QFETCH(bool, afterRow);
        NodesDatabase db; QVERIFY(db.open()); NodesPage page(&db.manager);
        page.refreshNodes(); checkSnapshot(page);
        InterruptState state{afterRow};
        QCOMPARE(sqlite3_trace_v2(db.handle, SQLITE_TRACE_ROW, [](unsigned, void* context, void*, void*) {
            ++static_cast<InterruptState*>(context)->rows; return 0;
        }, &state), SQLITE_OK);
        sqlite3_progress_handler(db.handle, 1, [](void* context) {
            auto* state = static_cast<InterruptState*>(context);
            if (state->afterRow && state->rows < 2) return 0;
            state->interrupted = true; return 1;
        }, &state);
        page.refreshNodes(); QVERIFY(state.interrupted); if (afterRow) QVERIFY(state.rows >= 2);
        checkFailure(page); QVERIFY(!sqlite3_next_stmt(db.handle, nullptr));
        sqlite3_progress_handler(db.handle, 0, nullptr, nullptr); sqlite3_trace_v2(db.handle, 0, nullptr, nullptr);
        page.refreshNodes(); checkSnapshot(page);
    }
    void invalidAddress_data() {
        QTest::addColumn<QByteArray>("update");
        QTest::newRow("unknown-family") << QByteArray("family=7");
        QTest::newRow("empty-blob") << QByteArray("address=x''");
        QTest::newRow("text-instead-of-blob") << QByteArray("address='addr'");
    }
    void invalidAddress() {
        QFETCH(QByteArray, update);
        NodesDatabase db; QVERIFY(db.open()); NodesPage page(&db.manager);
        page.refreshNodes(); checkSnapshot(page);
        QVERIFY(db.execute(("UPDATE node_addresses SET " + update + " WHERE family=4").constData()));
        page.refreshNodes(); checkFailure(page); QVERIFY(!sqlite3_next_stmt(db.handle, nullptr));
        QVERIFY(db.execute("UPDATE node_addresses SET family=4,address=x'7F000001' WHERE protocol=1"));
        page.refreshNodes(); checkSnapshot(page);
    }
    void disconnectedDatabase() {
        NodesDatabase db; QVERIFY(db.open()); NodesPage page(&db.manager);
        page.refreshNodes(); checkSnapshot(page); db.manager.setDb(nullptr); page.refreshNodes();
        QCOMPARE(page.findChild<QTableWidget*>()->rowCount(), 0);
        QVERIFY(page.findChild<QPlainTextEdit*>()->toPlainText().contains("Database not connected"));
        db.manager.setDb(db.handle); page.refreshNodes(); checkSnapshot(page);
        NodesPage missing(nullptr); missing.refreshNodes();
        QCOMPARE(missing.findChild<QTableWidget*>()->rowCount(), 0);
        QVERIFY(missing.findChild<QPlainTextEdit*>()->toPlainText().contains("Database not connected"));
    }
#ifdef UTUN_TEST_NODE_FINALIZE_FAILURE
    void finalizeFailure() {
        NodesDatabase db; QVERIFY(db.open()); NodesPage page(&db.manager);
        page.refreshNodes(); checkSnapshot(page); failFinalize = true;
        page.refreshNodes(); QVERIFY(!failFinalize); checkFailure(page);
        QVERIFY(!sqlite3_next_stmt(db.handle, nullptr));
        page.refreshNodes(); checkSnapshot(page);
    }
#endif
};

QTEST_MAIN(NodesPageTest)
#include "test_nodes_page.moc"

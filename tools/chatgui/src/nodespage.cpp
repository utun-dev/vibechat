#include <QCoreApplication>
// nodespage.cpp — Nodes table + dump in settings
#include "nodespage.h"
#include "addressprofiletext.h"
#include "../db/db_manager.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QDateTime>
#include <QFont>
#include <QVector>
#include <cstring>
#include <memory>

#include "../../../lib/platform_compat.h"
#include "../../../lib/debug_config.h"
#include "../../../lib/db_async.h"

/* Самостоятельные SQL-данные; worker не форматирует локальное время/переводы и не обращается к виджетам. */
struct NodesSnapshot {
    struct Address {
        QByteArray bytes; // Ровно 4/16 байт в сетевом порядке.
        int family = 0, protocol = 0, port = 0, rtt = 0;
        int mapping = 0, inbound = 0, source = 0;
    };
    struct Node {
        quint64 id = 0;
        QString name;
        QByteArray x25519, ed25519;
        qint64 lastSeen = 0, created = 0;
        QVector<Address> addresses;
    };
    QVector<Node> nodes;
    quint64 addressCount = 0;
};
Q_DECLARE_METATYPE(NodesSnapshot)

static QString hex16(uint64_t v) {
    return QString("%1").arg(v, 16, 16, QChar('0')).toUpper();
}

QString NodesPage::shortenKey(const QByteArray& key) {
    if (key.size() < 8) return key.toHex(' ').toUpper();
    return key.left(4).toHex(' ').toUpper() + ".." + key.right(4).toHex(' ').toUpper();
}

QString NodesPage::addrStr(const void* addr, int family) {
    if (family == 4) {
        const uint8_t* a = (const uint8_t*)addr;
        return QString("%1.%2.%3.%4").arg(a[0]).arg(a[1]).arg(a[2]).arg(a[3]);
    }
    if (family == 6) {
        uint16_t a[8];
        memcpy(a, addr, sizeof(a));
        return QString("%1:%2:%3:%4:%5:%6:%7:%8")
            .arg(ntohs(a[0]), 0, 16).arg(ntohs(a[1]), 0, 16)
            .arg(ntohs(a[2]), 0, 16).arg(ntohs(a[3]), 0, 16)
            .arg(ntohs(a[4]), 0, 16).arg(ntohs(a[5]), 0, 16)
            .arg(ntohs(a[6]), 0, 16).arg(ntohs(a[7]), 0, 16);
    }
    return "?";
}

QString NodesPage::protoStr(int protocol) {
    if (protocol & 0x04) return (protocol & 0x02) ? "TCP+REALITY" : "REALITY";
    switch (protocol) {
    case 1: return "UDP";
    case 2: return "TCP";
    case 3: return "UDP+TCP";
    default: return QString::number(protocol);
    }
}

NodesPage::NodesPage(DbManager* db, QWidget* parent)
    : QWidget(parent)
    , m_db(db)
{
    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(8);
    layout->setContentsMargins(0, 0, 0, 0);

    auto* title = new QLabel(QCoreApplication::translate("ChatGui", "Known Nodes"), this);
    title->setStyleSheet("font-weight: bold; font-size: 13px;");
    layout->addWidget(title);

    m_table = new QTableWidget(0, 5, this);
    m_table->setHorizontalHeaderLabels({QCoreApplication::translate("ChatGui",
        "Node ID"), QCoreApplication::translate("ChatGui",
        "Name"), "x25519", "ed25519", QCoreApplication::translate("ChatGui",
        "Addrs")});
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->setVisible(false);
    m_table->setFont(QFont("monospace", 9));
    m_table->setStyleSheet(
        "QTableWidget { border: 1px solid palette(mid); border-radius: 4px;"
        " gridline-color: palette(midlight); background: palette(base); }"
        "QHeaderView::section { background: palette(window); padding: 4px;"
        " font-weight: bold; font-size: 9px; }");
    layout->addWidget(m_table, 2);

    auto* dumpLabel = new QLabel(QCoreApplication::translate("ChatGui", "Details"), this);
    dumpLabel->setStyleSheet("font-weight: bold; font-size: 12px; margin-top: 8px;");
    layout->addWidget(dumpLabel);

    m_dumpText = new QPlainTextEdit(this);
    m_dumpText->setReadOnly(true);
    m_dumpText->setFont(QFont("monospace", 10));
    m_dumpText->setStyleSheet(
        "QPlainTextEdit { border: 1px solid palette(mid); border-radius: 4px;"
        " padding: 8px; background: palette(base); }");
    m_dumpText->setPlaceholderText(QCoreApplication::translate("ChatGui", "Click Refresh to load nodes"));
    layout->addWidget(m_dumpText, 3);

    auto* btnLayout = new QHBoxLayout();
    btnLayout->addStretch();
    m_refreshBtn = new QPushButton(QCoreApplication::translate("ChatGui", "Refresh"), this);
    m_refreshBtn->setMinimumWidth(90);
    m_refreshBtn->setStyleSheet(
        "QPushButton { border: 1px solid palette(mid); border-radius: 4px;"
        " padding: 6px 16px; font-size: 13px; background: palette(button); }"
        "QPushButton:hover { background: palette(light); }");
    connect(m_refreshBtn, &QPushButton::clicked, this, &NodesPage::onRefreshClicked);
    btnLayout->addWidget(m_refreshBtn);
    layout->addLayout(btnLayout);
}

void NodesPage::onRefreshClicked() { refreshNodes(); }

/* Один запрос до finalize; QObject страницы и заимствованный GUI handle в worker не передаются. */
void NodesPage::refreshNodes() {
    if (!m_db || !m_db->isOpen()) {
        m_generation++; m_pending = m_reload = false; m_refreshBtn->setEnabled(true);
        m_table->setRowCount(0);
        m_dumpText->setPlainText(QCoreApplication::translate("ChatGui", "Database not connected"));
        DEBUG_WARN(DEBUG_CATEGORY_CONFIG, "Nodes refresh unavailable: manager/database missing");
        return;
    }
    if (m_pending) { m_reload = true; return; }
    m_pending = true; m_reload = false; const auto generation = ++m_generation; m_refreshBtn->setEnabled(false);
    auto complete = [this, generation](const db_async_result& status, const QVariant& value) {
        if (generation != m_generation) return;
        m_pending = false; m_refreshBtn->setEnabled(true);
        if (!m_db || !m_db->isOpen()) { refreshNodes(); return; }
        if (status.status != DB_ASYNC_OK || !value.canConvert<NodesSnapshot>()) {
            DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "Nodes refresh result failed status=%d rc=%d: %s", status.status, status.sqlite_code, status.message);
            m_table->setRowCount(0); m_dumpText->setPlainText(QCoreApplication::translate("ChatGui", "Query error"));
        } else applyNodes(value.value<NodesSnapshot>());
        if (m_reload) refreshNodes();
    };
    if (!m_db->hasAsyncReader()) {
        const char* path = sqlite3_db_filename(m_db->m_db, "main");
        QVariant value; int rc = SQLITE_MISUSE;
        if (!path || !*path) { DbReadView view(m_db->m_db); rc = readNodes(view, &value); }
        else DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "Nodes refresh: file database requires asynchronous reader");
        const db_async_result result{rc == SQLITE_OK ? DB_ASYNC_OK : DB_ASYNC_SQL, rc, "RAM nodes read failed"};
        complete(result, value); return;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_CONFIG, "Nodes refresh queued generation=%llu", (unsigned long long)generation);
    if (!m_db->readAsync(this, readNodes, complete)) {
        const db_async_result result{DB_ASYNC_CLOSED, SQLITE_OK, "Nodes read scheduling rejected"}; complete(result, {});
    }
}

/* Только SQLite и собственные Qt-значения; исключение также закрывает statement до worker done. */
int NodesPage::readNodes(DbReadView& view, QVariant* value) {
    sqlite3* d = view.m_db; NodesSnapshot snapshot; auto& nodes = snapshot.nodes;
    quint64 currentNode = 0;
    const char* stage = "prepare";
    QString failure;
    sqlite3_stmt* st = nullptr;
    DEBUG_DEBUG(DEBUG_CATEGORY_CONFIG, "Nodes refresh begin db=%p", static_cast<void*>(d));
    int rc = sqlite3_prepare_v2(d,
        "SELECT n.node_id,n.name,n.x25519_pubkey,n.ed25519_pubkey,n.last_seen_at,n.created_at,"
        " a.node_x,a.family,a.protocol,a.address,a.port,a.rtt,a.nat_mapping,a.nat_inbound,a.nat_source"
        " FROM nodes n LEFT JOIN node_addresses a ON a.node_x=n.x25519_pubkey ORDER BY n.node_id,n.x25519_pubkey,a.family,a.port",
        -1, &st, nullptr);
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(st, sqlite3_finalize);
    if (rc == SQLITE_OK) {
        stage = "step";
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            currentNode = quint64(sqlite3_column_int64(st, 0));
            const QByteArray x = DbReadView::colBlob(st, 2);
            if (nodes.isEmpty() || nodes.constLast().x25519 != x) {
                nodes.append(NodesSnapshot::Node{});
                auto& node = nodes.last();
                node.id = currentNode; node.name = DbReadView::colText(st, 1);
                node.x25519 = x; node.ed25519 = DbReadView::colBlob(st, 3);
                node.lastSeen = sqlite3_column_int64(st, 4); node.created = sqlite3_column_int64(st, 5);
            }
            if (sqlite3_column_type(st, 6) == SQLITE_NULL) continue; // LEFT JOIN: узел без адресов.
            const int family = sqlite3_column_int(st, 7), type = sqlite3_column_type(st, 9);
            const void* address = sqlite3_column_blob(st, 9);
            const int bytes = sqlite3_column_bytes(st, 9);
            if (type != SQLITE_BLOB || !address || (family != 4 && family != 6) || bytes != (family == 4 ? 4 : 16)) {
                stage = "address";
                rc = sqlite3_errcode(d) == SQLITE_NOMEM ? SQLITE_NOMEM : SQLITE_CORRUPT;
                if (rc == SQLITE_CORRUPT) failure = QString("invalid address family=%1 bytes=%2 type=%3").arg(family).arg(bytes).arg(type);
                break;
            }
            auto& node = nodes.last();
            NodesSnapshot::Address row;
            row.bytes = QByteArray(static_cast<const char*>(address), bytes); row.family = family;
            row.protocol = sqlite3_column_int(st, 8); row.port = sqlite3_column_int(st, 10); row.rtt = sqlite3_column_int(st, 11);
            row.mapping = sqlite3_column_int(st, 12); row.inbound = sqlite3_column_int(st, 13); row.source = sqlite3_column_int(st, 14);
            node.addresses.append(std::move(row));
            DEBUG_TRACE(DEBUG_CATEGORY_CONFIG, "Nodes address: node=%016llx family=%d nat_profile=%d",
                static_cast<unsigned long long>(currentNode), family, family == 4);
            ++snapshot.addressCount;
        }
    }
    if (rc != SQLITE_DONE && failure.isEmpty()) failure = QString::fromUtf8(sqlite3_errmsg(d));
    const int finalized = sqlite3_finalize(statement.release());
    if (rc == SQLITE_DONE && finalized != SQLITE_OK) {
        stage = "finalize"; failure = QString::fromUtf8(sqlite3_errstr(finalized));
    }
    if (rc != SQLITE_DONE || finalized != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "Nodes refresh failed stage=%s rc=%d finalize=%d node=%016llx: %s",
                    stage, rc, finalized, static_cast<unsigned long long>(currentNode), qPrintable(failure));
        return rc == SQLITE_DONE ? finalized : rc;
    }
    *value = QVariant::fromValue(std::move(snapshot)); return SQLITE_OK;
}

/* GUI-форматирование не исполняет SQL и использует только целиком прочитанный снимок. */
void NodesPage::applyNodes(const NodesSnapshot& snapshot) {
    const auto& nodes = snapshot.nodes;
    QString dump;
    for (const auto& node : nodes) {
        if (!dump.isEmpty()) dump += '\n';
        dump += QCoreApplication::translate("ChatGui", "Node: %1").arg(hex16(node.id));
        if (!node.name.isEmpty()) dump += QString(" (%1)").arg(node.name);
        dump += QString("  id=0x%1\n").arg(node.id, 16, 16, QChar('0'));
        dump += QString("  x25519: %1\n  ed25519: %2\n")
            .arg(node.x25519.isEmpty() ? "-" : shortenKey(node.x25519), node.ed25519.isEmpty() ? "-" : shortenKey(node.ed25519));
        if (node.lastSeen > 0) dump += QCoreApplication::translate("ChatGui", "  last seen: %1\n")
            .arg(QDateTime::fromSecsSinceEpoch(node.lastSeen).toString("yyyy-MM-dd hh:mm:ss"));
        if (node.created > 0) dump += QCoreApplication::translate("ChatGui", "  created: %1\n")
            .arg(QDateTime::fromSecsSinceEpoch(node.created).toString("yyyy-MM-dd hh:mm:ss"));
        dump += QCoreApplication::translate("ChatGui", node.addresses.isEmpty() ? "  addresses: none\n" : "  addresses:\n");
        for (const auto& address : node.addresses) {
            const auto profile = address.family == 4 ? "  " + addressProfileText(address.mapping, address.inbound, address.source) : QString();
            dump += QString("    %1:%2  %3%4  rtt=%5ms\n").arg(addrStr(address.bytes.constData(), address.family)).arg(address.port)
                .arg(protoStr(address.protocol), profile, address.rtt > 0 ? QString::number(address.rtt / 10) : "-");
        }
    }
    if (nodes.isEmpty()) dump = QCoreApplication::translate("ChatGui", "No nodes found.");
    m_table->setRowCount(0); m_table->setRowCount(nodes.size());
    for (int row = 0; row < nodes.size(); ++row) {
        const auto& node = nodes[row];
        const QString cells[] = {hex16(node.id), node.name, node.x25519.isEmpty() ? "-" : shortenKey(node.x25519),
            node.ed25519.isEmpty() ? "-" : shortenKey(node.ed25519), QString::number(node.addresses.size())};
        for (int column = 0; column < 5; ++column) m_table->setItem(row, column, new QTableWidgetItem(cells[column]));
    }
    m_table->resizeColumnsToContents();
    m_dumpText->setPlainText(dump);
    DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "Nodes refresh complete nodes=%d addresses=%llu", int(nodes.size()),
               static_cast<unsigned long long>(snapshot.addressCount));
}

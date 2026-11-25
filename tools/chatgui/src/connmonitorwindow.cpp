#include <QCoreApplication>
// connmonitorwindow.cpp — ETCP connection monitor (standalone, non-modal window)
#include "connmonitorwindow.h"
#include "../transport/gui_bridge.h"

extern "C" {
#include "chat/chat_core.h"
#include "../../lib/mem.h"
#include "../../../lib/debug_config.h"
}

#include <QVBoxLayout>
#include <QSplitter>
#include <QLabel>
#include <QFont>
#include <QScrollBar>
#include <QHash>
#include <QApplication>
#include <QCloseEvent>
#include <QSignalBlocker>
#include <cstring>

#define CONN_LIST_ENTRY_SIZE 46

static ConnMonitorWindow* g_activeConnMonitor = nullptr;

static void connListCb(const uint8_t* data, int len) {
    if (g_activeConnMonitor) g_activeConnMonitor->onConnList(data, len);
}

static void connMetricsCb(const uint8_t* data, int len) {
    if (g_activeConnMonitor) g_activeConnMonitor->onConnMetrics(data, len);
}

/* Один получатель bridge callbacks для всех меню/настроек, без указателей-владельцев в этих окнах. */
ConnMonitorWindow* ConnMonitorWindow::showMonitor() {
    auto* window = g_activeConnMonitor;
    if (!window) window = new ConnMonitorWindow;
    if (window->isMinimized()) window->showNormal(); else window->show();
    window->raise(); window->activateWindow();
    DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "connection monitor shown: window=%p", (void*)window);
    return window;
}

ConnMonitorWindow::ConnMonitorWindow()
    : QDialog(nullptr)
{
    setWindowTitle(QCoreApplication::translate("ChatGui", "Connection Monitor"));
    resize(900, 550);
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(4);
    mainLayout->setContentsMargins(8, 8, 8, 8);

    auto* title = new QLabel(QCoreApplication::translate("ChatGui", "Active ETCP Connections (10 fps)"), this);
    title->setStyleSheet("font-weight: bold; font-size: 13px;");
    mainLayout->addWidget(title);

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setStyleSheet(
        "QSplitter::handle { background: palette(mid); width: 2px; }");

    m_connList = new QListWidget(splitter);
    m_connList->setFixedWidth(280);
    m_connList->setFont(QFont("monospace", 9));
    m_connList->setStyleSheet(
        "QListWidget { border: 1px solid palette(mid); border-radius: 4px;"
        " background: palette(base); }"
        "QListWidget::item { padding: 4px 6px; }"
        "QListWidget::item:selected { background: palette(highlight); color: palette(highlighted-text); }");

    m_details = new QPlainTextEdit(splitter);
    m_details->setReadOnly(true);
    m_details->setFont(QFont("monospace", 9));
    m_details->setStyleSheet(
        "QPlainTextEdit { border: 1px solid palette(mid); border-radius: 4px;"
        " padding: 8px; background: palette(base); }");
    m_details->setPlaceholderText(QCoreApplication::translate("ChatGui", "Select a connection to view metrics"));

    splitter->addWidget(m_connList);
    splitter->addWidget(m_details);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);

    mainLayout->addWidget(splitter, 1);

    m_timer = new QTimer(this);
    m_timer->setInterval(100);
    connect(m_timer, &QTimer::timeout, this, &ConnMonitorWindow::onTimerTick);
    connect(m_connList, &QListWidget::currentItemChanged,
            this, &ConnMonitorWindow::onConnSelected);

    g_activeConnMonitor = this;
    gui_bridge_set_conn_list_cb(connListCb);
    gui_bridge_set_conn_metrics_cb(connMetricsCb);

    m_timer->start();
}

ConnMonitorWindow::~ConnMonitorWindow() {
    m_timer->stop();
    if (g_activeConnMonitor == this) {
        g_activeConnMonitor = nullptr;
        gui_bridge_set_conn_list_cb(nullptr); gui_bridge_set_conn_metrics_cb(nullptr);
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "connection monitor destroyed: window=%p", (void*)this);
}

void ConnMonitorWindow::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    m_timer->start();
}

void ConnMonitorWindow::hideEvent(QHideEvent* event) {
    m_timer->stop();
    QDialog::hideEvent(event);
}

/* Снять singleton сразу при принятом close, не дожидаясь Qt::DeferredDelete. */
void ConnMonitorWindow::closeEvent(QCloseEvent* event) {
    QDialog::closeEvent(event);
    if (event->isAccepted() && g_activeConnMonitor == this) {
        g_activeConnMonitor = nullptr;
        gui_bridge_set_conn_list_cb(nullptr); gui_bridge_set_conn_metrics_cb(nullptr);
        DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "connection monitor closed: window=%p", (void*)this);
    }
}

void ConnMonitorWindow::onTimerTick() {
    if (m_details->textCursor().hasSelection()) return;
    if (QApplication::mouseButtons()) return;
    if (!gui_bridge_is_uasync_ready() || !gui_bridge_get_inst()) return;
    gui_bridge_post_uasync_fn(chat_core_collect_conn_list_trampoline, gui_bridge_get_inst());
    if (m_selectedNodeId) requestMetricsForSelected();
}

void ConnMonitorWindow::onConnSelected(QListWidgetItem* current, QListWidgetItem* previous) {
    (void)previous;
    if (!current) { m_selectedNodeId = 0; m_details->clear(); return; }
    m_selectedNodeId = (uint64_t)current->data(Qt::UserRole).toULongLong();
    DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "connection monitor selection: node=%016llx", (unsigned long long)m_selectedNodeId);
    requestMetricsForSelected();
}

void ConnMonitorWindow::requestMetricsForSelected() {
    auto* inst = gui_bridge_get_inst();
    if (!inst || !m_selectedNodeId || !gui_bridge_is_uasync_ready()) return;
    struct chat_node_arg* arg = (struct chat_node_arg*)u_malloc(sizeof(struct chat_node_arg));
    if (!arg) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "connection monitor: metrics request allocation failed"); return; }
    arg->inst = inst;
    arg->node_id = m_selectedNodeId;
    if (gui_bridge_post_uasync_owned_fn(chat_core_collect_conn_metrics_trampoline, arg) < 0) u_free(arg);
}

void ConnMonitorWindow::onConnList(const uint8_t* data, int len) {
    if (!data || len < 2) { DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "connection monitor: invalid list header bytes=%d", len); return; }
    uint16_t count; memcpy(&count, data, 2);
    if (count > 250 || len != 2 + count * CONN_LIST_ENTRY_SIZE) {
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "connection monitor: invalid list count=%u bytes=%d", count, len); return;
    }
    const uint8_t* p = data + 2;

    QHash<uint64_t, QListWidgetItem*> existing;
    for (int i = 0; i < m_connList->count(); i++) {
        auto* item = m_connList->item(i);
        existing[(uint64_t)item->data(Qt::UserRole).toULongLong()] = item;
    }

    QSignalBlocker blocker(m_connList);

    for (uint16_t i = 0; i < count; i++) {
        uint64_t nodeId; memcpy(&nodeId, p, 8); p += 8;
        uint8_t flags = *p++;
        uint8_t linkCount = *p++;
        uint16_t rtt; memcpy(&rtt, p, 2); p += 2;
        p += 2; /* inflightKb */
        char name[33]; memcpy(name, p, 32); name[32] = '\0'; p += 32;

        if (!nodeId) continue;

        bool linksUp = (flags & 1) != 0;
        bool initialized = (flags & 2) != 0;
        bool proxyClient = (flags & 4) != 0;
        bool proxyServer = (flags & 8) != 0;
        QString status = linksUp ? "UP" : (initialized ? "init" : "DOWN");

        QString proxyRole;
        if (proxyClient && proxyServer) proxyRole = "CS";
        else if (proxyClient) proxyRole = " C";
        else if (proxyServer) proxyRole = " S";

        QString nodeHex = QString("%1").arg(nodeId, 16, 16, QChar('0')).right(4);
        QString rttStr = rtt > 0 ? QString::number(rtt / 10.0f, 'f', 1) : "-";
        QString text = QString("[%1] %2  %3%4 %5L  rtt=%6ms")
            .arg(nodeHex)
            .arg(QString::fromUtf8(name).left(16), -16)
            .arg(status, -4)
            .arg(proxyRole, -2)
            .arg(linkCount)
            .arg(rttStr);

        auto it = existing.find(nodeId);
        if (it != existing.end()) {
            auto* item = *it;
            item->setText(text);
            existing.erase(it);
        } else {
            auto* item = new QListWidgetItem(text, m_connList);
            item->setData(Qt::UserRole, QVariant((qulonglong)nodeId));
        }
    }

    for (auto it = existing.begin(); it != existing.end(); ++it)
        delete *it;

    if (!m_connList->currentItem() && m_connList->count() > 0) m_connList->setCurrentRow(0);
    auto* current = m_connList->currentItem();
    const uint64_t selected = current ? current->data(Qt::UserRole).toULongLong() : 0;
    blocker.unblock();
    if (selected != m_selectedNodeId) onConnSelected(current, nullptr);
}

void ConnMonitorWindow::onConnMetrics(const uint8_t* data, int len) {
    if (!data || len <= 0) { DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "connection monitor: invalid metrics bytes=%d", len); return; }
    if (!m_selectedNodeId) return;
    int scrollVal = m_details->verticalScrollBar()->value();
    m_details->setPlainText(QString::fromUtf8((const char*)data, len));
    m_details->verticalScrollBar()->setValue(scrollVal);
}

#include <QCoreApplication>
// statuspage.cpp — Status page (NTP + connections) in settings
#include "statuspage.h"
#include "../transport/utun_node.h"

extern "C" {
#include "../transport/gui_bridge.h"
#include "chat/chat_core.h"
#include "debug_config.h"
}

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QFont>
#include <QTimer>
#include <QDateTime>

static StatusPage* g_activeStatusPage = nullptr;

static void statusRefreshCallback(const char* text, int len) {
    if (g_activeStatusPage)
        g_activeStatusPage->setText(QString::fromUtf8(text, len));
}

StatusPage::StatusPage(UtunNode* node, QWidget* parent)
    : QWidget(parent)
    , m_node(node)
{
    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(2);
    layout->setContentsMargins(0, 0, 0, 0);

    /* ── clock block ── */
    m_clockFrame = new QFrame(this);
    m_clockFrame->setFrameStyle(QFrame::StyledPanel | QFrame::Sunken);
    m_clockFrame->setStyleSheet(
        "QFrame { border: 1px solid palette(mid); border-radius: 2px;"
        " padding: 1px; background: palette(alternate-base); }");

    auto* clockLayout = new QVBoxLayout(m_clockFrame);
    clockLayout->setContentsMargins(4, 0, 4, 0);

    m_clockLabel = new QLabel("--:--:--", m_clockFrame);
    QFont clockFont("monospace", 20);
    clockFont.setBold(true);
    m_clockLabel->setFont(clockFont);
    m_clockLabel->setAlignment(Qt::AlignCenter);
    m_clockLabel->setStyleSheet("border: none; color: palette(text);");
    clockLayout->addWidget(m_clockLabel);

    layout->addWidget(m_clockFrame);

    /* ── System Status section ── */
    auto* title = new QLabel(QCoreApplication::translate("ChatGui", "System Status"), this);
    title->setStyleSheet("font-weight: bold; font-size: 13px;");
    layout->addWidget(title);

    m_textEdit = new QPlainTextEdit(this);
    m_textEdit->setReadOnly(true);
    m_textEdit->setFont(QFont("monospace", 10));
    m_textEdit->setStyleSheet(
        "QPlainTextEdit { border: 1px solid palette(mid); border-radius: 4px;"
        " padding: 8px; background: palette(base); }");
    m_textEdit->setPlaceholderText(QCoreApplication::translate("ChatGui", "Click Refresh to load status"));
    layout->addWidget(m_textEdit, 1);

    auto* btnLayout = new QHBoxLayout();
    btnLayout->addStretch();
    m_refreshBtn = new QPushButton(QCoreApplication::translate("ChatGui", "Refresh"), this);
    m_refreshBtn->setMinimumWidth(90);
    m_refreshBtn->setStyleSheet(
        "QPushButton { border: 1px solid palette(mid); border-radius: 4px;"
        " padding: 6px 16px; font-size: 13px; background: palette(button); }"
        "QPushButton:hover { background: palette(light); }");
    connect(m_refreshBtn, &QPushButton::clicked, this, &StatusPage::onRefreshClicked);
    btnLayout->addWidget(m_refreshBtn);
    layout->addLayout(btnLayout);

    m_clockTimer = new QTimer(this);
    m_clockTimer->setSingleShot(true);
    m_clockTimer->setTimerType(Qt::PreciseTimer);
    connect(m_clockTimer, &QTimer::timeout, this, &StatusPage::updateClock);

    g_activeStatusPage = this;
    gui_bridge_set_status_refresh_cb(statusRefreshCallback);
}

StatusPage::~StatusPage() {
    m_clockTimer->stop();
    if (g_activeStatusPage == this) g_activeStatusPage = nullptr;
}

void StatusPage::updateClock() {
    qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    int64_t offsetUs = m_node ? m_node->ntpOffsetUs() : 0;
    bool synced = m_node && m_node->ntpSynced();

    qint64 correctedMs = nowMs - (offsetUs / 1000);
    QDateTime corrected = QDateTime::fromMSecsSinceEpoch(correctedMs).toLocalTime();

    QString t = corrected.toString("HH:mm:ss");

    if (synced)
        m_clockLabel->setText(QString("%1  [NTP ✓]").arg(t));
    else
        m_clockLabel->setText(QCoreApplication::translate("ChatGui", "%1  [LOCAL]").arg(t));

    int msToNext = 1000 - (corrected.time().msec());
    m_clockTimer->start(msToNext);
}

void StatusPage::refreshStatus() {
    updateClock();
    auto* inst = gui_bridge_get_inst();
    if (!m_node || !m_node->isRunning() || !gui_bridge_is_uasync_ready() || !inst) {
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "status refresh: core unavailable");
        m_textEdit->setPlainText(QCoreApplication::translate("ChatGui", "Node is not running."));
        return;
    }
    m_textEdit->setPlainText(QCoreApplication::translate("ChatGui", "Loading..."));
    DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "status refresh: requesting snapshot instance=%p", (void*)inst);
    gui_bridge_post_uasync_fn(chat_core_collect_status_trampoline, inst);
}

/* Поздний ответ не должен заменять сообщение об остановке ядра старым снимком. */
void StatusPage::setText(const QString& text) {
    if (!m_node || !m_node->isRunning() || !gui_bridge_get_inst()) {
        DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "status refresh: late snapshot ignored after core stop"); return;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "status refresh: snapshot received chars=%d", (int)text.size());
    m_textEdit->setPlainText(text);
}

void StatusPage::onRefreshClicked() {
    refreshStatus();
}

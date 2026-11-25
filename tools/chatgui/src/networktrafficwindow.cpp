#include "networktrafficwindow.h"
#include "../transport/gui_bridge.h"
#include "../../../src/chat/chat_network_stats.h"
#include "../../../lib/debug_config.h"

#include <QCloseEvent>
#include <QCoreApplication>
#include <QGridLayout>
#include <QHostAddress>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QSet>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <vector>

static QString text(const char* value) { return QCoreApplication::translate("ChatGui", value); }
static constexpr uint64_t HistoryMs = 30000, RateMs = 1000, MaxGapMs = 750;
static NetworkTrafficWindow* activeMonitor = nullptr;

struct TrafficSample {
    uint64_t at, tx, rx, inflight;
    double txRate, rxRate;
    bool gap;
};
struct TrafficSeries {
    QColor color;
    unsigned number = 0; // Номер в легенде; поколение используется только как ключ истории.
    QToolButton* button = nullptr; // Владеет панель; checked управляет всеми тремя кривыми.
    std::deque<TrafficSample> samples; // Ограниченная история и накопительные счётчики для секундного среднего.
};

/* Единственный холст панели: одна временная шкала, независимый масштаб каждого ряда. */
class TrafficCanvas : public QWidget {
public:
    explicit TrafficCanvas(QWidget* parent) : QWidget(parent) { setObjectName("trafficCanvas"); setFixedHeight(366); }
    QMap<quint64, TrafficSeries> series;
    uint64_t now = 0;
protected:
    void paintEvent(QPaintEvent*) override;
};

/* Владеет историей одного подключения и кнопками общей легенды; DOWN не удаляет историю. */
class TrafficConnection : public QWidget {
public:
    explicit TrafficConnection(quint64 id, QWidget* parent) : QWidget(parent), m_id(id) {
        setObjectName(QStringLiteral("trafficConnection_%1").arg(id, 0, 16));
        auto* layout = new QVBoxLayout(this); layout->setContentsMargins(8, 8, 8, 8); layout->setSpacing(4);
        m_title = new QLabel(this); m_title->setTextFormat(Qt::PlainText);
        QFont font = m_title->font(); font.setBold(true); m_title->setFont(font); layout->addWidget(m_title);
        m_legend = new QGridLayout; m_legend->setContentsMargins(0, 0, 0, 0); m_legend->setSpacing(4);
        m_legend->setAlignment(Qt::AlignLeft); layout->addLayout(m_legend);
        m_canvas = new TrafficCanvas(this); layout->addWidget(m_canvas);
    }

    void apply(const chat_network_stats_conn& conn, const std::vector<chat_network_stats_link>& links, uint64_t at) {
        if (at <= m_canvas->now) {
            DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "traffic graph: stale snapshot connection=%llx at=%llu previous=%llu",
                (unsigned long long)m_id, (unsigned long long)at, (unsigned long long)m_canvas->now); return;
        }
        QString peer = conn.peer_id ? QStringLiteral("%1").arg(conn.peer_id, 16, 16, QChar('0')) : text("Connecting");
        m_title->setText(QStringLiteral("%1 [%2] · %3").arg(QString::fromUtf8(conn.name), peer, conn.up ? "UP" : "DOWN"));
        QSet<quint64> present{0}; uint64_t inflight = 0;
        bool changed = !m_canvas->series.contains(0);
        addSeries(0, 0, QStringLiteral("CONN"), text("All links: inflight / TX / RX"));
        for (const auto& link : links) {
            present.insert(link.id); inflight += link.inflight;
            changed |= !m_canvas->series.contains(link.id);
            QString local = address(link.description.local), remote = address(link.description.remote);
            QString tooltip = QStringLiteral("%1 · %2\n%3 → %4\n%5").arg(QString::fromUtf8(link.description.protocol),
                QString::fromUtf8(link.description.socket_name), local, remote, link.description.status ? "UP" : "DOWN");
            addSeries(link.id, link.description.id, QStringLiteral("LINK%1").arg(link.description.id), tooltip);
            append(m_canvas->series[link.id], at, link.tx_bytes, link.rx_bytes, link.inflight);
        }
        for (auto it = m_canvas->series.begin(); it != m_canvas->series.end();) {
            if (present.contains(it.key())) { ++it; continue; }
            DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "traffic graph link removed: connection=%llx generation=%llu",
                (unsigned long long)m_id, (unsigned long long)it.key());
            delete it->button; it = m_canvas->series.erase(it); changed = true;
        }
        append(m_canvas->series[0], at, conn.tx_bytes, conn.rx_bytes, inflight);
        m_canvas->now = at;
        if (changed) arrangeLegend();
        m_canvas->update();
    }
protected:
    void resizeEvent(QResizeEvent* event) override { QWidget::resizeEvent(event); arrangeLegend(); }
private:
    // Отдельно хранить поколение: local_link_id циклически переиспользуется транспортом.
    quint64 m_id;
    QLabel* m_title;
    QGridLayout* m_legend;
    TrafficCanvas* m_canvas;
    unsigned m_nextColor = 0;

    static QString address(const etcp_link_address& value) {
        if (!value.family) return QStringLiteral("—");
        QHostAddress host;
        if (value.family == 4) {
            quint32 ipv4 = (quint32(value.bytes[0]) << 24) | (quint32(value.bytes[1]) << 16) |
                (quint32(value.bytes[2]) << 8) | value.bytes[3];
            host.setAddress(ipv4);
        } else { Q_IPV6ADDR ipv6; memcpy(ipv6.c, value.bytes, 16); host.setAddress(ipv6); }
        return QStringLiteral("[%1]:%2").arg(host.toString()).arg(value.port);
    }

    void addSeries(quint64 id, unsigned number, const QString& name, const QString& tooltip) {
        auto it = m_canvas->series.find(id);
        if (it == m_canvas->series.end()) {
            TrafficSeries value;
            value.number = number;
            value.color = id ? QColor::fromHsv((35 + m_nextColor++ * 137) % 360, 190, 210) : QColor("#3498db");
            value.button = new QToolButton(this); value.button->setCheckable(true); value.button->setChecked(true);
            value.button->setFixedWidth(90); value.button->setCursor(Qt::PointingHandCursor);
            value.button->setObjectName(QStringLiteral("trafficSeries_%1").arg(id));
            value.button->setStyleSheet(QStringLiteral(
                "QToolButton { border: none; border-radius: 4px; padding: 3px; color: palette(placeholder-text); }"
                "QToolButton:checked { color: %1; } QToolButton:hover { background: palette(midlight); }").arg(value.color.name()));
            connect(value.button, &QToolButton::toggled, m_canvas, [this, id](bool enabled) {
                auto& item = m_canvas->series[id]; QFont font = item.button->font(); font.setStrikeOut(!enabled);
                item.button->setFont(font); m_canvas->update();
                DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "traffic graph series: connection=%llx generation=%llu visible=%d",
                    (unsigned long long)m_id, (unsigned long long)id, enabled);
            });
            it = m_canvas->series.insert(id, value);
            DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "traffic graph series added: connection=%llx generation=%llu color=%s",
                (unsigned long long)m_id, (unsigned long long)id, qPrintable(it->color.name()));
        }
        it->button->setText(QStringLiteral("● %1").arg(name)); it->button->setAccessibleName(name); it->button->setToolTip(tooltip);
    }

    void arrangeLegend() {
        int columns = std::max(1, (width() - 12) / 94), index = 0;
        std::vector<TrafficSeries*> ordered;
        for (auto& item : m_canvas->series) { m_legend->removeWidget(item.button); ordered.push_back(&item); }
        std::sort(ordered.begin(), ordered.end(), [](const auto* a, const auto* b) { return a->number < b->number; });
        for (auto* item : ordered) { m_legend->addWidget(item->button, index / columns, index % columns); ++index; }
    }

    // Среднее фиксируется при получении; скрытие серии не меняет её историю и вычисление скорости.
    void append(TrafficSeries& series, uint64_t at, uint64_t tx, uint64_t rx, uint64_t flight) {
        auto& history = series.samples;
        if (!history.empty() && at <= history.back().at) {
            DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "traffic graph: non-increasing time connection=%llx now=%llu previous=%llu",
                (unsigned long long)m_id, (unsigned long long)at, (unsigned long long)history.back().at); return;
        }
        bool gap = history.empty() || at - history.back().at > MaxGapMs;
        if (!history.empty() && (tx < history.back().tx || rx < history.back().rx)) {
            DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "traffic graph: counters regressed connection=%llx TX=%llu RX=%llu",
                (unsigned long long)m_id, (unsigned long long)tx, (unsigned long long)rx); gap = true;
        }
        TrafficSample sample{at, tx, rx, flight, NAN, NAN, gap};
        if (!gap) {
            const TrafficSample* first = nullptr;
            for (auto it = history.rbegin(); it != history.rend(); ++it) {
                if (at - it->at > RateMs) break;
                first = &*it;
                if (it->gap) break;
            }
            if (first) {
                sample.txRate = (tx - first->tx) * 1000.0 / (at - first->at);
                sample.rxRate = (rx - first->rx) * 1000.0 / (at - first->at);
            }
        }
        history.push_back(sample);
        while (history.size() > 256 || at - history.front().at > HistoryMs + RateMs) history.pop_front();
    }
};

/* Каждая ось начинается с нуля и учитывает только включённые серии в видимом временном окне. */
void TrafficCanvas::paintEvent(QPaintEvent*) {
    QPainter p(this); p.setRenderHint(QPainter::Antialiasing); p.fillRect(rect(), palette().base());
    QColor foreground = palette().text().color(), muted = foreground; muted.setAlphaF(.65);
    auto value = [](const TrafficSample& sample, int row) {
        return row == 0 ? double(sample.inflight) : row == 1 ? sample.txRate : sample.rxRate;
    };
    for (int row = 0; row < 3; ++row) {
        double maximum = 1; bool have = false;
        for (const auto& line : series) if (line.button->isChecked()) for (const auto& sample : line.samples) {
            if (now - sample.at > HistoryMs || !std::isfinite(value(sample, row))) continue;
            maximum = std::max(maximum, value(sample, row)); have = true;
        }
        double power = std::pow(10.0, std::floor(std::log10(maximum))), rounded = maximum / power;
        double top = (rounded <= 1 ? 1 : rounded <= 2 ? 2 : rounded <= 5 ? 5 : 10) * power;
        double factor = top >= 1048576 ? 1048576 : top >= 1024 ? 1024 : 1;
        QString unit = factor == 1048576 ? "MiB" : factor == 1024 ? "KiB" : "B";
        if (row) unit += "/s";
        double y = row * 122.0; QRectF box(78, y + 24, width() - 94, 76);
        p.setPen(foreground);
        p.drawText(QRectF(10, y + 2, width() - 20, 20),
            QStringLiteral("%1 · %2%3").arg(row == 0 ? text("Inflight") : row == 1 ? "TX" : "RX", unit,
                have ? QString() : QStringLiteral(" · ") + text("No samples")));
        auto px = [&](uint64_t at) { return box.right() - double(now - at) / HistoryMs * box.width(); };
        auto py = [&](double v) { return box.bottom() - v / top * box.height(); };
        for (int tick = 0; tick <= 2; ++tick) {
            double v = top * tick / 2, position = py(v); p.setPen(QPen(muted, .4));
            p.drawLine(QPointF(box.left(), position), QPointF(box.right(), position));
            p.setPen(muted); p.drawText(QRectF(0, position - 8, 70, 16), Qt::AlignRight | Qt::AlignVCenter,
                QString::number(v / factor, 'f', factor == 1 ? 0 : 1));
        }
        for (int second = -30; second <= 0; second += 10) {
            double x = box.right() + second / 30.0 * box.width(); p.setPen(QPen(muted, .4));
            p.drawLine(QPointF(x, box.top()), QPointF(x, box.bottom())); p.setPen(muted);
            p.drawText(QRectF(x - 22, box.bottom() + 2, 44, 18), Qt::AlignCenter, QStringLiteral("%1 s").arg(second));
        }
        p.save(); p.setClipRect(box.adjusted(-1, -1, 1, 1));
        for (auto it = series.cbegin(); it != series.cend(); ++it) {
            if (!it->button->isChecked()) continue;
            QPainterPath path; bool started = false;
            for (const auto& sample : it->samples) {
                double v = value(sample, row);
                if (now - sample.at > HistoryMs || !std::isfinite(v)) { started = false; continue; }
                QPointF point(px(sample.at), py(v));
                if (!started || sample.gap) path.moveTo(point); else path.lineTo(point);
                started = true;
            }
            p.setPen(QPen(it->color, it.key() ? 1.5 : 2.5)); p.drawPath(path);
        }
        p.restore();
    }
}

static void networkSnapshot(const uint8_t* data, int len) { if (activeMonitor) activeMonitor->onSnapshot(data, len); }

NetworkTrafficWindow* NetworkTrafficWindow::showMonitor() {
    auto* window = activeMonitor;
    if (!window) window = new NetworkTrafficWindow;
    if (window->isMinimized()) window->showNormal(); else window->show();
    window->raise(); window->activateWindow();
    return window;
}

NetworkTrafficWindow::NetworkTrafficWindow() : QDialog(nullptr) {
    setObjectName("networkTrafficWindow"); setWindowTitle(text("Network Traffic")); resize(900, 850); setMinimumWidth(460);
    setAttribute(Qt::WA_DeleteOnClose); setAttribute(Qt::WA_QuitOnClose, false);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    auto* layout = new QVBoxLayout(this);
    auto* description = new QLabel(text("Last 30 seconds · TX / RX averaged over 1 second · Click legend to show or hide a series"), this);
    description->setWordWrap(true); layout->addWidget(description);
    auto* scroll = new QScrollArea(this); scroll->setWidgetResizable(true); layout->addWidget(scroll);
    auto* content = new QWidget(scroll); m_rows = new QVBoxLayout(content); m_rows->setAlignment(Qt::AlignTop);
    m_rows->setContentsMargins(0, 0, 0, 0); m_rows->setSpacing(12); scroll->setWidget(content);
    m_empty = new QLabel(text("Waiting for network statistics"), content); m_empty->setWordWrap(true); m_rows->addWidget(m_empty);
    m_timer = new QTimer(this); m_timer->setInterval(250);
    connect(m_timer, &QTimer::timeout, this, &NetworkTrafficWindow::onTimerTick);
    activeMonitor = this; gui_bridge_set_network_stats_cb(networkSnapshot);
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "network traffic window created: history_ms=30000 interval_ms=250 rate_ms=1000");
}

NetworkTrafficWindow::~NetworkTrafficWindow() {
    m_timer->stop();
    if (activeMonitor == this) { activeMonitor = nullptr; gui_bridge_set_network_stats_cb(nullptr); }
    DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "network traffic window destroyed: connections=%d", int(m_connections.size()));
}
void NetworkTrafficWindow::showEvent(QShowEvent* event) {
    QDialog::showEvent(event); m_timer->start(); onTimerTick();
}
void NetworkTrafficWindow::hideEvent(QHideEvent* event) {
    m_timer->stop(); m_requestId = 0; QDialog::hideEvent(event);
}
void NetworkTrafficWindow::closeEvent(QCloseEvent* event) {
    QDialog::closeEvent(event);
    if (event->isAccepted() && activeMonitor == this) { activeMonitor = nullptr; gui_bridge_set_network_stats_cb(nullptr); }
}
void NetworkTrafficWindow::onTimerTick() {
    int result = gui_bridge_request_network_stats(&m_requestId);
    if (result == 0) {
        if (!m_connections.isEmpty()) DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "traffic graphs cleared: core unavailable");
        qDeleteAll(m_connections); m_connections.clear(); m_requestId = 0;
        m_empty->setText(text("Network core unavailable")); m_empty->show();
    } else if (result < 0) { m_empty->setText(text("Unable to collect network statistics")); m_empty->show(); }
}

/* Проверяем native POD и границы всех массивов до изменения существующих графиков. */
void NetworkTrafficWindow::onSnapshot(const uint8_t* data, int len) {
    if (!data || len < int(sizeof(chat_network_stats_header))) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "traffic graphs: truncated header bytes=%d", len); return;
    }
    chat_network_stats_header header; memcpy(&header, data, sizeof(header));
    if (!isVisible() || header.request_id != m_requestId || !m_requestId) return;
    if (header.failed == 1 && header.count == 0 && len == int(sizeof(header))) {
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "traffic graphs: collection failed request=%llu", (unsigned long long)header.request_id);
        m_empty->setText(text("Unable to collect network statistics")); m_empty->show(); return;
    }
    struct Connection { chat_network_stats_conn value; std::vector<chat_network_stats_link> links; };
    std::vector<Connection> connections; QSet<quint64> present;
    const uint8_t* p = data + sizeof(header); size_t remaining = len - sizeof(header);
    auto invalid = [&] { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "traffic graphs: invalid snapshot request=%llu count=%u bytes=%d remaining=%zu",
        (unsigned long long)header.request_id, header.count, len, remaining); };
    if (header.failed || !header.at_ms || header.count > remaining / sizeof(chat_network_stats_conn)) { invalid(); return; }
    for (uint32_t i = 0; i < header.count; ++i) {
        if (remaining < sizeof(chat_network_stats_conn)) { invalid(); return; }
        Connection conn; memcpy(&conn.value, p, sizeof(conn.value)); p += sizeof(conn.value); remaining -= sizeof(conn.value);
        if (!conn.value.id || present.contains(conn.value.id) || conn.value.up > 1 ||
            !memchr(conn.value.name, 0, sizeof(conn.value.name)) || conn.value.link_count > remaining / sizeof(chat_network_stats_link)) {
            invalid(); return;
        }
        present.insert(conn.value.id); QSet<quint64> links;
        for (uint32_t j = 0; j < conn.value.link_count; ++j) {
            chat_network_stats_link link; memcpy(&link, p, sizeof(link)); p += sizeof(link); remaining -= sizeof(link);
            if (!link.id || links.contains(link.id) || !link.description.id || link.description.status > 1 ||
                !memchr(link.description.protocol, 0, sizeof(link.description.protocol)) ||
                !memchr(link.description.socket_name, 0, sizeof(link.description.socket_name)) ||
                (link.description.local.family && link.description.local.family != 4 && link.description.local.family != 6) ||
                (link.description.remote.family && link.description.remote.family != 4 && link.description.remote.family != 6)) {
                invalid(); return;
            }
            links.insert(link.id); conn.links.push_back(link);
        }
        connections.push_back(std::move(conn));
    }
    if (remaining) { invalid(); return; }
    for (auto it = m_connections.begin(); it != m_connections.end();) {
        if (present.contains(it.key())) { ++it; continue; }
        DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "traffic graph connection removed: id=%llx", (unsigned long long)it.key());
        delete it.value(); it = m_connections.erase(it);
    }
    for (const auto& conn : connections) {
        auto* panel = m_connections.value(conn.value.id);
        if (!panel) {
            panel = new TrafficConnection(conn.value.id, this); m_rows->addWidget(panel); m_connections.insert(conn.value.id, panel);
            DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "traffic graph connection added: id=%llx peer=%016llx links=%u",
                (unsigned long long)conn.value.id, (unsigned long long)conn.value.peer_id, conn.value.link_count);
        }
        panel->apply(conn.value, conn.links, header.at_ms);
    }
    m_empty->setText(text("No ETCP connections")); m_empty->setVisible(connections.empty());
}

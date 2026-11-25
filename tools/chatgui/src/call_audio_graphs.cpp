#include "call_audio_graphs.h"
#include "../../../src/call/call_audio.h"
#include "../../../lib/debug_config.h"
#include <QCoreApplication>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QSplitter>
#include <algorithm>
#include <cmath>
#include <vector>

static QString text(const char* value) { return QCoreApplication::translate("ChatGui", value); }
struct GraphPoint { double x, y; bool gap; };
struct GraphLine { QColor color; std::vector<GraphPoint> points; };

/* Холст владеет последним снимком; компоновка окна не влияет на координаты графиков. */
class CallAudioGraphCanvas : public QWidget {
public:
    explicit CallAudioGraphCanvas(QWidget* parent) : QWidget(parent) { setMinimumSize(440, 620); }
    call_audio_history m_data{};
    bool m_available = false;
protected:
    void paintEvent(QPaintEvent*) override;
};

CallAudioGraphs::CallAudioGraphs(quint64 id, QWidget* diagnostics, QWidget* parent) : QWidget(parent, Qt::Window), m_callId(id) {
    setObjectName("callAudioGraphs"); setAttribute(Qt::WA_QuitOnClose, false);
    setWindowTitle(text("Diagnostics")); resize(1120, 800);
    auto* layout = new QHBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0);
    auto* splitter = new QSplitter(Qt::Horizontal, this); layout->addWidget(splitter);
    m_canvas = new CallAudioGraphCanvas(splitter);
    auto* details = new QScrollArea(splitter); details->setObjectName("callDiagnosticDetails");
    details->setWidgetResizable(true); details->setMinimumWidth(260); details->setFrameShape(QFrame::NoFrame);
    details->setWidget(diagnostics); diagnostics->show();
    splitter->setSizes({760, 340}); splitter->setStretchFactor(0, 1); splitter->setStretchFactor(1, 0);
    splitter->setChildrenCollapsible(false);
    m_timer = new QTimer(this); m_timer->setInterval(250);
    connect(m_timer, &QTimer::timeout, this, [this] {
        call_audio_history snapshot;
        bool available = call_audio_get_history(m_callId, &snapshot) == 0;
        if (available) setSnapshot(snapshot);
        else if (m_canvas->m_available) {
            DEBUG_INFO(DEBUG_CATEGORY_CALL, "audio graphs frozen call=%llx RX=%u TX=%u",
                       (unsigned long long)m_callId, m_canvas->m_data.rx_count, m_canvas->m_data.tx_count);
            m_canvas->m_available = false; m_canvas->update();
        }
    });
}
void CallAudioGraphs::setSnapshot(const call_audio_history& snapshot) {
    m_canvas->m_data = snapshot; m_canvas->m_available = true; m_canvas->update();
}
void CallAudioGraphs::showEvent(QShowEvent* event) {
    QWidget::showEvent(event); m_timer->start();
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "audio graphs opened call=%llx window_ms=30000 half_life_ms=10000 headroom=after-pcm-block pause=0",
               (unsigned long long)m_callId);
}
void CallAudioGraphs::hideEvent(QHideEvent* event) {
    m_timer->stop(); QWidget::hideEvent(event);
    DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "audio graphs hidden call=%llx", (unsigned long long)m_callId);
}

void CallAudioGraphCanvas::paintEvent(QPaintEvent*) {
    QPainter p(this); p.fillRect(rect(), palette().window()); p.setRenderHint(QPainter::Antialiasing);
    const QColor foreground = palette().text().color(); QColor muted = foreground; muted.setAlphaF(.72);
    const QColor blue("#3498db"), green("#27ae60"), orange("#e67e22"), violet("#9b59b6"), red("#e74c3c");
    p.setPen(foreground); p.drawText(QRectF(16, 8, width() - 32, 22), text("Audio history — last 30 seconds"));
    p.setPen(muted);
    QString status = m_available ? text("Real-time") : text("Waiting for audio / last snapshot");
    double missing = m_data.observed_ms > 0 ? 100 * m_data.missing_ms / m_data.observed_ms : 0;
    p.drawText(QRectF(16, 30, width() - 32, 22), status + " · " + text("Missing audio: %1% (weighted)").arg(missing, 0, 'f', 1));
    double row = (height() - 68) / 4.0;
    auto xValue = [this](uint64_t at) { return (double)CALL_HISTORY_MS - (double)m_data.now_ms + at; };
    auto graph = [&](int index, const QString& title, const QString& legend, const std::vector<GraphLine>& lines,
                     double minimumSpan, const QString& smallUnit, const QString& largeUnit, bool threshold) {
        double low = INFINITY, high = -INFINITY;
        for (const auto& line : lines) for (const auto& v : line.points) {
            low = std::min(low, v.y); high = std::max(high, v.y);
        }
        bool have = std::isfinite(low); if (!have) low = high = 0;
        double factor = !largeUnit.isEmpty() && high >= 1024 ? 1024 : 1;
        QString unit = factor == 1024 ? largeUnit : smallUnit;
        double scaleLow = threshold ? 0.0 : low; // Запас показывается от нуля, включая паузы накопления.
        double span = std::max(high - scaleLow, minimumSpan), bottom = std::max(0.0, (high + scaleLow - span) / 2 - span * .05);
        double top = std::max(high, bottom + span) + span * .05;
        double y = 58 + index * row;
        p.setPen(foreground); p.drawText(QRectF(16, y, width() - 32, 20), title);
        p.setPen(muted);
        p.drawText(QRectF(16, y + 20, width() - 32, 18),
            have ? text("Min: %1 · Max: %2 %3").arg(low / factor, 0, 'f', factor == 1 ? 0 : 1)
                .arg(high / factor, 0, 'f', factor == 1 ? 0 : 1).arg(unit) : text("No samples"));
        p.drawText(QRectF(16, y + 38, width() - 32, 32), Qt::TextWordWrap, legend);
        QRectF box(62, y + 73, width() - 78, row - 96);
        auto px = [&](double time) { return box.left() + time / CALL_HISTORY_MS * box.width(); };
        auto py = [&](double value) { return box.bottom() - (value - bottom) / (top - bottom) * box.height(); };
        p.setPen(muted); p.drawText(QRectF(0, box.top(), 57, 16), Qt::AlignRight, QString::number(top / factor, 'f', factor == 1 ? 0 : 1));
        p.drawText(QRectF(0, box.bottom() - 15, 57, 16), Qt::AlignRight, QString::number(bottom / factor, 'f', factor == 1 ? 0 : 1));
        for (int second = 0; second <= 30; second += 10) {
            double x = px(second * 1000); p.setPen(QPen(muted, .4)); p.drawLine(QPointF(x, box.top()), QPointF(x, box.bottom()));
            p.drawText(QRectF(x - 20, box.bottom() + 2, 40, 17), Qt::AlignCenter, QString::number(second - 30) + " s");
        }
        p.save(); p.setClipRect(box);
        if (threshold) {
            p.setPen(QPen(muted, 1, Qt::DashLine)); p.drawLine(QPointF(box.left(), py(0)), QPointF(box.right(), py(0)));
        }
        if (threshold && bottom <= 3000 && top >= 3000) {
            p.setPen(QPen(red, 1, Qt::DashLine)); p.drawLine(QPointF(box.left(), py(3000)), QPointF(box.right(), py(3000)));
        }
        for (const auto& line : lines) {
            QPainterPath path; bool started = false;
            for (const auto& v : line.points) {
                QPointF pos(px(v.x), py(v.y));
                if (!started || v.gap) path.moveTo(pos); else path.lineTo(pos);
                if (v.gap) { p.setPen(Qt::NoPen); p.setBrush(line.color); p.drawEllipse(pos, 1.5, 1.5); p.setBrush(Qt::NoBrush); }
                started = true;
            }
            p.setPen(QPen(line.color, 1.5)); p.drawPath(path);
        }
        p.restore();
    };
    std::vector<GraphLine> receive{{blue, {}}, {green, {}}, {orange, {}}, {red, {}}};
    for (unsigned i = 0; i < m_data.rx_count; ++i) {
        const auto& v = m_data.rx[i]; bool gap = !i || v.at_ms - m_data.rx[i - 1].at_ms > 150;
        double x = xValue(v.at_ms);
        receive[0].points.push_back({x, (double)v.low_ms, gap}); receive[0].points.push_back({x, (double)v.high_ms, false});
        receive[1].points.push_back({x, (double)v.reserve_ms, gap});
        receive[2].points.push_back({x, (double)v.headroom_low_ms, gap});
        receive[2].points.push_back({x, (double)v.headroom_high_ms, false});
        if (v.missing_samples) receive[3].points.push_back({x, (double)v.low_ms, true});
    }
    graph(0, text("Receive buffer"), text("Blue: buffer · Green: target · Orange: headroom (0 while paused) · Red: missing audio"),
          receive, 100, text("ms"), {}, true);
    std::vector<GraphLine> transmit{{blue, {}}, {orange, {}}, {violet, {}}}, rate{{green, {}}};
    for (unsigned i = 0; i < m_data.tx_count; ++i) {
        const auto& v = m_data.tx[i]; bool gap = !i || v.at_ms - m_data.tx[i - 1].at_ms > 150;
        double x = xValue(v.at_ms);
        transmit[0].points.push_back({x, (double)v.queued_bytes + v.inflight_bytes, gap});
        transmit[1].points.push_back({x, (double)v.inflight_bytes, gap});
        transmit[2].points.push_back({x, (double)v.transport_bytes, gap});
        if (v.wire_bytes_per_sec >= 0) rate[0].points.push_back({x, v.wire_bytes_per_sec,
            gap || (i && (v.peer_id != m_data.tx[i - 1].peer_id || v.tx_epoch != m_data.tx[i - 1].tx_epoch
                || m_data.tx[i - 1].wire_bytes_per_sec < 0))});
    }
    graph(1, text("All transmission queues"), text("Blue: Router + waiting audio · Orange: inflight · Purple: transport separately"),
          transmit, 1024, text("B"), text("KiB"), false);
    graph(2, text("ETCP send rate — call route"), text("All ETCP traffic including retries · Average over 1 second"),
          rate, 256, text("B/s"), text("KiB/s"), false);
    double y = 58 + 3 * row; p.setPen(foreground);
    p.drawText(QRectF(16, y, width() - 32, 20), text("Receive buffer distribution"));
    p.setPen(muted); p.drawText(QRectF(16, y + 38, width() - 32, 32), Qt::TextWordWrap,
        text("Time weighted · Older samples lose half their weight every 10 seconds"));
    int first = 0, last = CALL_HISTORY_BINS - 1;
    while (first < last && m_data.histogram[first] == 0) ++first;
    while (last > first && m_data.histogram[last] == 0) --last;
    if (!m_data.observed_ms) first = 0, last = 1;
    if (first == last) { first = std::max(0, first - 1); last = std::min(CALL_HISTORY_BINS - 1, last + 1); }
    double maximum = 0; for (double v : m_data.histogram) maximum = std::max(maximum, v);
    p.drawText(QRectF(16, y + 20, width() - 32, 18),
        text("Min: %1 · Max: %2 %3").arg(0).arg(maximum * 100, 0, 'f', 1).arg("%"));
    QRectF box(62, y + 73, width() - 78, row - 96); double scale = std::max(.05, maximum) * 1.05;
    double barWidth = box.width() / (last - first + 1); p.setPen(Qt::NoPen); p.setBrush(blue);
    for (int i = first; i <= last; ++i) {
        double h = m_data.histogram[i] / scale * box.height();
        p.drawRect(QRectF(box.left() + (i - first) * barWidth, box.bottom() - h, std::max(1.0, barWidth - 1), h));
    }
    p.setPen(muted); p.drawText(QRectF(0, box.top(), 57, 16), Qt::AlignRight, QString::number(scale * 100, 'f', 0) + "%");
    p.drawText(QRectF(0, box.bottom() - 15, 57, 16), Qt::AlignRight, "0%");
    p.drawText(QRectF(box.left(), box.bottom() + 2, box.width(), 17), Qt::AlignLeft, QString::number(first * CALL_HISTORY_BIN_MS) + " " + text("ms"));
    p.drawText(QRectF(box.left(), box.bottom() + 2, box.width(), 17), Qt::AlignRight,
        (last == 60 ? QStringLiteral("3000+") : QString::number((last + 1) * CALL_HISTORY_BIN_MS)) + " " + text("ms"));
}

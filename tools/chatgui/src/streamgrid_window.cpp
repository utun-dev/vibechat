#include "streamgrid_window.h"
#include "stream_view_button.h"
#include "streamplayer_window.h"
#include "videoplayer_window.h"
#include "stream_sources.h"
#include "../transport/stream_bridge.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QCloseEvent>
#include <QMouseEvent>
#include <QApplication>
#include <QShortcut>
#ifdef HAVE_STREAM_CAPTURE
#include <QVideoWidget>
#include <QVideoSink>
#include <QVideoFrame>
#endif
#include <atomic>
#include <algorithm>
#include <cmath>

namespace {
std::atomic<uint64_t> nextOwner{0};
QString text(const char* value) { return QCoreApplication::translate("ChatGui", value); }
}

StreamGridWindow::StreamGridWindow(const QString& group, QWidget* parent)
    : QWidget(parent), m_group(group), m_owner(++nextOwner) {
    setWindowTitle(text("All active streams")); resize(1100, 750); m_clock.start();
    auto* layout = new QVBoxLayout(this);
    m_controls = new QWidget(this); m_controls->setObjectName(QStringLiteral("streamGridControls"));
    auto* controls = new QHBoxLayout(m_controls); controls->setContentsMargins(0, 0, 0, 0);
    m_status = new QLabel(text("Connecting…"), m_controls); controls->addWidget(m_status, 1);
    auto* fullscreen = streamViewButton(m_controls, text("Fullscreen"), QStyle::SP_TitleBarMaxButton, QStringLiteral("view-fullscreen"));
    controls->addWidget(fullscreen);
    auto* closeButton = streamViewButton(m_controls, text("Close player"), QStyle::SP_TitleBarCloseButton);
    controls->addWidget(closeButton);
    connect(fullscreen, &QPushButton::clicked, this, [this] { if (isFullScreen()) showNormal(); else showFullScreen(); });
    connect(closeButton, &QPushButton::clicked, this, &QWidget::close);
    auto* escape = new QShortcut(QKeySequence(Qt::Key_Escape), this); escape->setContext(Qt::WidgetWithChildrenShortcut);
    connect(escape, &QShortcut::activated, this, [this] {
        if (!m_focused.isEmpty()) restoreGrid();
        else if (isFullScreen()) showNormal();
        else close();
    });
    auto* tiles = new QWidget(this);
    m_grid = new QGridLayout(tiles); m_grid->setContentsMargins(0, 0, 0, 0); m_grid->setSpacing(4);
    layout->addWidget(tiles, 1);
    layout->addWidget(m_controls);
    m_watching = gui_stream_watch(m_group, m_owner);
    connect(&m_timer, &QTimer::timeout, this, &StreamGridWindow::tick); m_timer.start(250);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "desktop multiview opened group=%s owner=%llu",
                qPrintable(m_group), (unsigned long long)m_owner);
}

StreamGridWindow::~StreamGridWindow() { stop(); }

/* Self получает прямую поверхность издателя: ни worker, ни reader, ни вход микшера не создаются. */
void StreamGridWindow::createTile(Tile& tile) {
    if (tile.self) {
        tile.widget = new QWidget(m_grid->parentWidget());
        tile.widget->setObjectName(QStringLiteral("streamPreview_%1").arg(tile.source));
        auto* layout = new QVBoxLayout(tile.widget); layout->setContentsMargins(3, 3, 3, 3); layout->setSpacing(2);
        tile.title = new QLabel(tile.name, tile.widget); tile.title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        tile.title->setTextFormat(Qt::PlainText); tile.title->setToolTip(tile.name);
        tile.status = new QLabel(text("Connecting…"), tile.widget);
#ifdef HAVE_STREAM_CAPTURE
        auto* preview = new VideoRenderWidget(tile.widget);
        tile.video = preview;
#else
        auto* unavailable = new QLabel(text("Video capture requires Qt Multimedia and FFmpeg"), tile.widget);
        unavailable->setAlignment(Qt::AlignLeft | Qt::AlignTop); unavailable->setWordWrap(true); tile.video = unavailable;
#endif
        tile.video->setToolTip(text("Manage stream")); layout->addWidget(tile.video, 1);
        layout->addWidget(tile.title); layout->addWidget(tile.status);
        updatePreview(tile);
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "desktop multiview local preview group=%s source=%s epoch=%llx decoder=0 audio=0",
            qPrintable(m_group), tile.source.toHex().constData(), (unsigned long long)tile.epoch);
    } else {
        tile.player = new StreamPlayerWindow(m_group, tile.source, tile.epoch, tile.name, m_grid->parentWidget(), true);
        tile.widget = tile.player; tile.video = tile.player->findChild<VideoRenderWidget*>();
        connect(tile.player, &StreamPlayerWindow::fullscreenRequested, this,
            [this] { if (isFullScreen()) showNormal(); else showFullScreen(); });
        connect(tile.player, &StreamPlayerWindow::closeRequested, this, &StreamGridWindow::restoreGrid);
    }
    if (!tile.video) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "desktop multiview tile has no video source=%s", tile.source.toHex().constData()); return;
    }
    tile.video->installEventFilter(this);
    for (auto* child : tile.video->findChildren<QWidget*>()) child->installEventFilter(this);
}

/* QVariant хранит CPU QImage; очистка локального preview не затрагивает capture-процесс. */
void StreamGridWindow::setLocalPreview(const QString& group, const QVariant& frame) {
    if (m_closed || group != m_group) return;
    m_preview = frame;
    for (auto& tile : m_tiles) if (tile.self) updatePreview(tile);
}

void StreamGridWindow::updatePreview(Tile& tile) {
#ifdef HAVE_STREAM_CAPTURE
    auto frame = m_preview.value<QImage>();
    static_cast<VideoRenderWidget*>(tile.video)->setFrame(frame);
    tile.status->setVisible(frame.isNull());
    if (!frame.isNull() && !tile.previewPresented)
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "desktop multiview first local frame source=%s size=%dx%d decoder=0 audio=0",
            tile.source.toHex().constData(), frame.width(), frame.height());
    tile.previewPresented = !frame.isNull();
#else
    Q_UNUSED(tile);
#endif
}

/* Изменяется лишь видимость/геометрия: все удалённые players и их аудиочасы продолжают работать. */
void StreamGridWindow::activateTile(const QByteArray& source) {
    auto tile = std::find_if(m_tiles.begin(), m_tiles.end(), [source](const auto& tile) { return tile.source == source; });
    if (m_closed || tile == m_tiles.end()) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "desktop multiview click on removed source=%s", source.toHex().constData()); return;
    }
    if (tile->self) {
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "desktop multiview opens publisher group=%s source=%s",
            qPrintable(m_group), source.toHex().constData());
        emit publishRequested(m_group);
    } else if (m_focused == source) restoreGrid();
    else {
        if (m_focused.isEmpty()) m_gridWindowState = windowState();
        m_focused = source; arrange(); showFullScreen();
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "desktop multiview focus source=%s players_preserved=%zu",
            source.toHex().constData(), m_tiles.size());
    }
}

void StreamGridWindow::restoreGrid() {
    if (m_focused.isEmpty()) return;
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "desktop multiview returns to grid source=%s players_preserved=%zu",
        m_focused.toHex().constData(), m_tiles.size());
    m_focused.clear(); setWindowState(m_gridWindowState); arrange();
}

/* Не перехватываем drag/колесо zoom. Активация после release даёт VideoRenderWidget завершить drag. */
bool StreamGridWindow::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() != QEvent::MouseButtonPress && event->type() != QEvent::MouseMove && event->type() != QEvent::MouseButtonRelease)
        return QWidget::eventFilter(watched, event);
    auto* mouse = static_cast<QMouseEvent*>(event);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const auto position = mouse->globalPosition().toPoint();
#else
    const auto position = mouse->globalPos();
#endif
    if (event->type() == QEvent::MouseButtonPress) {
        if (mouse->button() == Qt::LeftButton) { m_pressed = watched; m_pressPosition = position; m_dragged = false; }
    } else if (event->type() == QEvent::MouseMove && m_pressed == watched) {
        if ((position - m_pressPosition).manhattanLength() >= QApplication::startDragDistance()) m_dragged = true;
    } else if (event->type() == QEvent::MouseButtonRelease && m_pressed == watched) {
        m_pressed = nullptr;
        if (mouse->button() == Qt::LeftButton && !m_dragged &&
            (position - m_pressPosition).manhattanLength() < QApplication::startDragDistance()) {
            for (const auto& tile : m_tiles) if (tile.video && (tile.video == watched || tile.video->isAncestorOf(qobject_cast<QWidget*>(watched)))) {
                QMetaObject::invokeMethod(this, [this, source = tile.source] { activateTile(source); }, Qt::QueuedConnection); break;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

/* Сверяем идентичности, не трогая остальные players при добавлении, удалении или новой эпохе. */
void StreamGridWindow::onSources(const uint8_t* data, int length) {
    if (m_closed) return;
    StreamSourceSnapshot snapshot;
    if (!parseStreamSources(data, length, snapshot) || snapshot.group != m_group) return;
    if (!snapshot.valid) {
        m_status->setText(text("Stream list temporarily unavailable"));
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "desktop multiview preserves tiles: catalogue unavailable group=%s",
                    qPrintable(m_group)); return;
    }
    bool changed = false;
    for (auto& tile : m_tiles) {
        auto source = std::find_if(snapshot.sources.begin(), snapshot.sources.end(), [&](const auto& s) { return s.id == tile.source; });
        if (source == snapshot.sources.end()) {
            if (tile.listed) DEBUG_INFO(DEBUG_CATEGORY_STREAM, "desktop multiview source absent source=%s preserve_viewer=%d",
                tile.source.toHex().constData(), !tile.self);
            tile.listed = false;
            continue;
        }
        tile.listed = true;
        if (tile.epoch == source->epoch && tile.self == source->self) {
            if (tile.name != source->name) {
                const QString previous = tile.name; tile.name = source->name;
                if (tile.self) { tile.title->setText(tile.name); tile.title->setToolTip(tile.name); }
                else tile.player->setSourceName(tile.name);
                DEBUG_INFO(DEBUG_CATEGORY_STREAM, "desktop multiview source renamed group=%s source=%s epoch=%llx self=%d old=%s new=%s",
                    qPrintable(m_group), tile.source.toHex().constData(), (unsigned long long)tile.epoch,
                    tile.self, qPrintable(previous), qPrintable(tile.name));
            }
            continue;
        }
        if (m_focused == tile.source) restoreGrid();
        delete tile.widget;
        tile = Tile{source->id, source->epoch, source->name, source->self}; createTile(tile);
        changed = true;
    }
    for (const auto& source : snapshot.sources) {
        if (std::any_of(m_tiles.begin(), m_tiles.end(), [&](const auto& tile) { return tile.source == source.id; })) continue;
        Tile tile{source.id, source.epoch, source.name, source.self}; createTile(tile);
        m_tiles.push_back(std::move(tile)); changed = true;
    }
    m_status->setText(snapshot.sources.isEmpty() ? text("Waiting for active streams…") : text("Active streams: %1").arg(snapshot.sources.size()));
    if (changed) arrange();
    tick();
}

/* Меняется только геометрия QWidget: decoder, media handle и audio clock остаются прежними. */
void StreamGridWindow::arrange() {
    while (auto* item = m_grid->takeAt(0)) delete item;
    for (int i = 0; i < m_side; ++i) { m_grid->setRowStretch(i, 0); m_grid->setColumnStretch(i, 0); }
    m_side = m_tiles.empty() ? 0 : static_cast<int>(std::ceil(std::sqrt(static_cast<double>(m_tiles.size()))));
    if (!m_focused.isEmpty() && std::none_of(m_tiles.begin(), m_tiles.end(), [this](const auto& tile) { return tile.source == m_focused; })) {
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "desktop multiview focused source removed source=%s", m_focused.toHex().constData());
        m_focused.clear(); setWindowState(m_gridWindowState);
    }
    const int rows = m_side ? (tileCount() + m_side - 1) / m_side : 0;
    for (int i = 0; i < rows; ++i) m_grid->setRowStretch(i, 1);
    for (int i = 0; i < m_side; ++i) m_grid->setColumnStretch(i, 1);
    StreamPlayerWindow* bottomPlayer = nullptr;
    for (const auto& tile : m_tiles) if (tile.player) bottomPlayer = tile.player;
    for (size_t i = 0; i < m_tiles.size(); ++i) {
        auto& tile = m_tiles[i];
        if (tile.player) {
            tile.player->setWindowControlsVisible(m_focused == tile.source);
            tile.player->setPlaybackSwitchVisible(!m_focused.isEmpty() ? m_focused == tile.source : tile.player == bottomPlayer);
        }
        if (!m_focused.isEmpty() && m_focused != tile.source) { tile.widget->hide(); continue; }
        if (!m_focused.isEmpty()) m_grid->addWidget(tile.widget, 0, 0, rows, m_side);
        else m_grid->addWidget(tile.widget, static_cast<int>(i) / m_side, static_cast<int>(i) % m_side);
        tile.widget->show();
    }
    m_controls->setVisible(m_focused.isEmpty());
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "desktop multiview layout group=%s tiles=%zu grid=%dx%d focused=%s mode_switch=bottom_telemetry_row",
                qPrintable(m_group), m_tiles.size(), m_side, rows, m_focused.toHex().constData());
}

/* Сбой одного decoder повторяется с backoff, окончание и исчезновение не сбрасывают соседние плитки. */
void StreamGridWindow::tick() {
    if (m_closed) return;
    if (!m_watching) m_watching = gui_stream_watch(m_group, m_owner);
    bool changed = false;
    qint64 now = m_clock.elapsed();
    for (auto it = m_tiles.begin(); it != m_tiles.end();) {
        if (!it->listed) {
            if (it->self || it->player->finished()) {
                DEBUG_INFO(DEBUG_CATEGORY_STREAM, "desktop multiview absent source finished source=%s self=%d",
                    it->source.toHex().constData(), it->self);
                delete it->widget; it = m_tiles.erase(it); changed = true; continue;
            }
        } else if (!it->self && it->player->failed()) {
            if (!it->retryAt) {
                it->retryAt = now + std::min(10000u, 500u << std::min(it->failures++, 5u));
                DEBUG_WARN(DEBUG_CATEGORY_STREAM, "desktop multiview retry source=%s attempt=%u delay_ms=%lld",
                            it->source.toHex().constData(), it->failures, (long long)(it->retryAt - now));
            } else if (now >= it->retryAt) {
                delete it->widget; createTile(*it);
                it->retryAt = 0; changed = true;
            }
        }
        ++it;
    }
    if (changed) arrange();
}

void StreamGridWindow::stop() {
    if (m_closed) return;
    m_closed = true; m_timer.stop(); gui_stream_watch({}, m_owner);
    for (auto& tile : m_tiles) delete tile.widget;
    m_tiles.clear();
    m_preview.clear();
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "desktop multiview closed group=%s", qPrintable(m_group));
}
void StreamGridWindow::stopForPhone() {
    hide(); m_closed = true; m_timer.stop(); gui_stream_watch({}, m_owner);
    for (auto& tile : m_tiles) if (tile.player) tile.player->stopAsync();
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "multiview stopping for phone group=%s", qPrintable(m_group));
}
bool StreamGridWindow::released() const {
    for (const auto& tile : m_tiles) if (tile.player && !tile.player->released()) return false;
    return true;
}
void StreamGridWindow::closeEvent(QCloseEvent* event) { stop(); QWidget::closeEvent(event); }

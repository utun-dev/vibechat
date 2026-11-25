#include <QCoreApplication>
#include "videoplayer_window.h"
#include "videoplayer_engine.h"
#include "../../lib/debug_config.h"

#include <QSlider>
#include <QPushButton>
#include <QLabel>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QPainter>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QCloseEvent>
#include <QResizeEvent>
#include <QStyle>
#include <QProxyStyle>
#include <algorithm>
#include <cmath>

/* ─── VideoRenderWidget ─── */

VideoRenderWidget::VideoRenderWidget(QWidget* parent, bool zoomEnabled) : QWidget(parent), m_zoomEnabled(zoomEnabled) {
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
}

void VideoRenderWidget::setFrame(const QImage& img) {
    bool changed = m_frame.size() != img.size();
    m_frame = img;
    if (m_zoomEnabled && changed) {
        finishDrag(); m_scale = 0; m_offset = {}; constrainView("frame");
        DEBUG_TRACE(DEBUG_CATEGORY_VIDEO, "VideoView %p frame=%dx%d reset to fit scale=%.4f DPR=%.3f",
                    static_cast<void*>(this), img.width(), img.height(), frameScale(), devicePixelRatioF());
    }
    update();
}

void VideoRenderWidget::clearFrame() {
    finishDrag(); m_scale = 0; m_offset = {}; unsetCursor();
    m_frame = QImage();
    update();
}

/* Масштаб в координатах Qt; предел 1:1 относится к физическим пикселям экрана. */
qreal VideoRenderWidget::frameScale() const {
    if (m_frame.isNull() || width() <= 0 || height() <= 0) return 0;
    qreal dpr = devicePixelRatioF();
    qreal fit = std::min({qreal(1), width() * dpr / m_frame.width(), height() * dpr / m_frame.height()});
    return (m_scale > 0 ? std::clamp(m_scale, fit, qreal(1)) : fit) / dpr;
}

/* Как в Android VideoZoomState: сдвиг ограничен половиной выступающей части каждой оси. */
void VideoRenderWidget::constrainView(const char* reason) {
    qreal scale = frameScale();
    if (scale <= 0) { finishDrag(); unsetCursor(); return; }
    if (m_scale > 0) m_scale = scale * devicePixelRatioF();
    qreal x = std::max(qreal(0), (m_frame.width() - width() / scale) / 2);
    qreal y = std::max(qreal(0), (m_frame.height() - height() / scale) / 2);
    m_offset.setX(std::clamp(m_offset.x(), -x, x));
    m_offset.setY(std::clamp(m_offset.y(), -y, y));
    if (x > 0 || y > 0) setCursor(m_panning ? Qt::ClosedHandCursor : Qt::OpenHandCursor);
    else { finishDrag(); unsetCursor(); }
    DEBUG_TRACE(DEBUG_CATEGORY_VIDEO, "VideoView %p %s viewport=%dx%d frame=%dx%d DPR=%.3f scale=%.4f offset=(%.2f,%.2f) limit=(%.2f,%.2f)",
                static_cast<void*>(this), reason, width(), height(), m_frame.width(), m_frame.height(), devicePixelRatioF(), scale,
                m_offset.x(), m_offset.y(), x, y);
}

void VideoRenderWidget::finishDrag() {
    if (!m_panning) return;
    m_panning = false; setCursor(Qt::OpenHandCursor);
    DEBUG_TRACE(DEBUG_CATEGORY_VIDEO, "VideoView %p drag end offset=(%.2f,%.2f)",
                static_cast<void*>(this), m_offset.x(), m_offset.y());
}

/* Захват мыши принадлежит QWidget; отмена жеста нужна и при потере окна/захвата. */
bool VideoRenderWidget::event(QEvent* event) {
    bool handled = QWidget::event(event);
    if (!m_zoomEnabled) return handled;
    if (event->type() == QEvent::UngrabMouse || event->type() == QEvent::WindowDeactivate || event->type() == QEvent::Hide)
        finishDrag();
    if (event->type() == QEvent::ScreenChangeInternal
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
        || event->type() == QEvent::DevicePixelRatioChange
#endif
    ) {
        constrainView("screen"); update();
        DEBUG_TRACE(DEBUG_CATEGORY_VIDEO, "VideoView %p screen changed DPR=%.3f scale=%.4f",
                    static_cast<void*>(this), devicePixelRatioF(), frameScale());
    }
    return handled;
}

void VideoRenderWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (!m_zoomEnabled) return;
    constrainView("resize");
    DEBUG_TRACE(DEBUG_CATEGORY_VIDEO, "VideoView %p resize=%dx%d scale=%.4f offset=(%.2f,%.2f)",
                static_cast<void*>(this), width(), height(), frameScale(), m_offset.x(), m_offset.y());
}

/* Сохраняем пиксель под курсором; clamp может сдвинуть якорь только возле границ кадра. */
void VideoRenderWidget::wheelEvent(QWheelEvent* event) {
    if (!m_zoomEnabled) { QWidget::wheelEvent(event); return; }
    qreal previous = frameScale();
    if (previous <= 0) { event->accept(); return; }
    int delta = event->angleDelta().y();
    if (!delta) delta = event->pixelDelta().y();
    if (!delta) { event->ignore(); return; }
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    QPointF position = event->position();
#else
    QPointF position = event->posF();
#endif
    qreal steps = std::clamp(delta / qreal(120), qreal(-100), qreal(100));
    m_scale = previous * devicePixelRatioF() * std::pow(1.15, steps);
    qreal next = frameScale();
    if (m_frame.width() * next <= width() + 1e-6 && m_frame.height() * next <= height() + 1e-6) m_scale = 0;
    QPointF relative = position - QPointF(width() / 2.0, height() / 2.0);
    m_offset += relative * (1 / next - 1 / previous);
    constrainView("zoom"); update(); event->accept();
    DEBUG_TRACE(DEBUG_CATEGORY_VIDEO, "VideoView %p zoom delta=%d scale=%.4f->%.4f physical=%.4f anchor=(%.1f,%.1f) offset=(%.2f,%.2f)",
                static_cast<void*>(this), delta, previous, next, next * devicePixelRatioF(), position.x(), position.y(),
                m_offset.x(), m_offset.y());
}

void VideoRenderWidget::mousePressEvent(QMouseEvent* event) {
    qreal scale = frameScale();
    bool canPan = scale > 0 && (m_frame.width() * scale > width() + 1e-6 || m_frame.height() * scale > height() + 1e-6);
    if (!m_zoomEnabled || event->button() != Qt::LeftButton || !canPan) {
        QWidget::mousePressEvent(event); return;
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    m_lastMouse = event->position();
#else
    m_lastMouse = event->localPos();
#endif
    m_panning = true; setCursor(Qt::ClosedHandCursor); event->accept();
    DEBUG_TRACE(DEBUG_CATEGORY_VIDEO, "VideoView %p drag begin scale=%.4f offset=(%.2f,%.2f)",
                static_cast<void*>(this), frameScale(), m_offset.x(), m_offset.y());
}

void VideoRenderWidget::mouseMoveEvent(QMouseEvent* event) {
    if (!m_panning) { QWidget::mouseMoveEvent(event); return; }
    if (!(event->buttons() & Qt::LeftButton)) { finishDrag(); event->accept(); return; }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QPointF position = event->position();
#else
    QPointF position = event->localPos();
#endif
    m_offset += (position - m_lastMouse) / frameScale(); m_lastMouse = position;
    constrainView("drag"); update(); event->accept();
}

void VideoRenderWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (!m_zoomEnabled || event->button() != Qt::LeftButton || !m_panning) { QWidget::mouseReleaseEvent(event); return; }
    finishDrag(); event->accept();
}

void VideoRenderWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    if (m_frame.isNull()) return;

    if (m_zoomEnabled) {
        qreal scale = frameScale();
        QSizeF scaled = QSizeF(m_frame.size()) * scale;
        QPointF origin = QPointF(width() / 2.0, height() / 2.0) + m_offset * scale - QPointF(scaled.width(), scaled.height()) / 2;
        // При 1:1 начало кадра совпадает с физической сеткой пикселей, без размытия текста.
        qreal dpr = devicePixelRatioF();
        if (qFuzzyCompare(scale * dpr, qreal(1))) {
            auto align = [dpr](qreal offset, int pixels, int viewport) {
                qreal rounded = std::round(offset * dpr);
                if (pixels > viewport * dpr) rounded = std::clamp(rounded, std::ceil(viewport * dpr - pixels), qreal(0));
                return rounded / dpr;
            };
            origin = QPointF(align(origin.x(), m_frame.width(), width()), align(origin.y(), m_frame.height(), height()));
        }
        p.setRenderHint(QPainter::SmoothPixmapTransform, scale * dpr < 1);
        p.drawImage(QRectF(origin, scaled), m_frame, QRectF(m_frame.rect()));
        return;
    }
    QSize sz = m_frame.size();
    sz.scale(size(), Qt::KeepAspectRatio);
    QRect dst((width() - sz.width()) / 2, (height() - sz.height()) / 2, sz.width(), sz.height());
    p.drawImage(dst, m_frame);
}

/* ─── VideoPlayerWindow ─── */

static QString fmtTime(qint64 ms) {
    qint64 s = ms / 1000;
    return QString("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QChar('0'));
}

static QPoint mouseGlobalPos(const QMouseEvent* e) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return e->globalPosition().toPoint();
#else
    return e->globalPos();
#endif
}

/* Слайдер перемотки: клик по любому месту шкалы = прыжок к позиции (нативно через стиль). */
class SeekSliderStyle : public QProxyStyle {
public:
    using QProxyStyle::QProxyStyle;

    int styleHint(StyleHint hint, const QStyleOption* opt, const QWidget* w,
                  QStyleHintReturn* ret) const override {
        if (hint == QStyle::SH_Slider_AbsoluteSetButtons)
            return (int)Qt::LeftButton;
        return QProxyStyle::styleHint(hint, opt, w, ret);
    }
};

VideoPlayerWindow::VideoPlayerWindow(QWidget* parent)
    : QWidget(parent, Qt::Window | Qt::FramelessWindowHint)
{
    setWindowTitle(QCoreApplication::translate("ChatGui", "Video"));
    setMinimumSize(320, 240);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);

    m_engine = new VideoPlayerEngine(this);

    m_render = new VideoRenderWidget(this);
    m_render->setAttribute(Qt::WA_TransparentForMouseEvents, true);

    m_controls = new QWidget(this);
    m_controls->setObjectName("videoControls");
    m_controls->setCursor(Qt::ArrowCursor);
    m_controls->installEventFilter(this);
    m_controls->setStyleSheet(
        "#videoControls { background-color: rgba(0,0,0,0.65); }"
        "QPushButton { background: transparent; color: white; border: none; font-size: 16px; }"
        "QPushButton:hover { background: rgba(255,255,255,0.2); }"
        "QLabel { color: white; font-size: 12px; }"
        "QSlider::groove:horizontal { height: 4px; background: #444; }"
        "QSlider::sub-page:horizontal { background: #2a9; }"
        "QSlider::handle:horizontal { width: 12px; margin: -4px 0; background: white; border-radius: 6px; }");

    m_playBtn = new QPushButton(QStringLiteral("⏸"), m_controls);
    m_playBtn->setToolTip(QCoreApplication::translate("ChatGui", "Pause"));
    m_playBtn->setFixedWidth(40);
    m_slider = new QSlider(Qt::Horizontal, m_controls);
    m_slider->setStyle(new SeekSliderStyle());
    m_slider->setRange(0, 0);
    m_timeLabel = new QLabel(QStringLiteral("0:00 / 0:00"), m_controls);

    auto* lay = new QHBoxLayout(m_controls);
    lay->setContentsMargins(8, 3, 8, 6);
    lay->setSpacing(6);
    lay->addWidget(m_playBtn);
    lay->addWidget(m_slider, 1);
    lay->addWidget(m_timeLabel);

    /* крестик закрытия — отдельно, справа вверху */
    m_closeBtn = new QPushButton(QStringLiteral("✕"), this);
    m_closeBtn->setToolTip(QCoreApplication::translate("ChatGui", "Close"));
    m_closeBtn->setCursor(Qt::ArrowCursor);
    m_closeBtn->setStyleSheet(
        "QPushButton { background: rgba(0,0,0,0.5); color: white; border: none; "
        "border-radius: 4px; font-size: 14px; }"
        "QPushButton:hover { background: rgba(255,255,255,0.25); }");

    m_hideTimer = new QTimer(this);
    m_hideTimer->setSingleShot(true);
    m_hideTimer->setInterval(2500);
    connect(m_hideTimer, &QTimer::timeout, this, &VideoPlayerWindow::hideControls);

    connect(m_playBtn, &QPushButton::clicked, this, &VideoPlayerWindow::onPlayPause);
    connect(m_closeBtn, &QPushButton::clicked, this, &VideoPlayerWindow::onClose);
    connect(m_slider, &QSlider::sliderPressed, this, &VideoPlayerWindow::onSliderPressed);
    connect(m_slider, &QSlider::sliderReleased, this, &VideoPlayerWindow::onSliderReleased);

    connect(m_engine, &VideoPlayerEngine::opened, this, &VideoPlayerWindow::onEngineOpened);
    connect(m_engine, &VideoPlayerEngine::positionChanged, this, &VideoPlayerWindow::onPositionChanged);
    connect(m_engine, &VideoPlayerEngine::stateChanged, this, &VideoPlayerWindow::onStateChanged);
    connect(m_engine, &VideoPlayerEngine::error, this, &VideoPlayerWindow::onError);
    connect(m_engine, &VideoPlayerEngine::framePresented, this, &VideoPlayerWindow::onFramePresented);
    connect(m_engine, &VideoPlayerEngine::finished, this, &VideoPlayerWindow::onFinished);

    resize(640, 360);
}

VideoPlayerWindow::~VideoPlayerWindow() {
    DEBUG_DEBUG(DEBUG_CATEGORY_VIDEO, "VideoWindow: destroying player state=%d visible=%d", int(m_engine->state()), isVisible());
    m_hideTimer->stop();
    disconnect(m_engine, nullptr, this, nullptr);
    m_engine->close();
}

void VideoPlayerWindow::playFile(const QString& path) {
    const QRect before = normalGeometry();
    DEBUG_DEBUG(DEBUG_CATEGORY_VIDEO, "VideoWindow: open file=%s visible=%d state=%d normal=%d,%d %dx%d",
                qPrintable(path), isVisible(), int(windowState()), before.x(), before.y(), before.width(), before.height());
    m_render->clearFrame();
    m_slider->setRange(0, 0);
    m_timeLabel->setText(QStringLiteral("0:00 / 0:00"));
    show();
    setControlsVisible(true); unsetCursor();
    if (isFullScreen()) m_hideTimer->start(); else m_hideTimer->stop();
    raise();
    activateWindow();
    m_engine->open(path);
}

void VideoPlayerWindow::onEngineOpened(int width, int height, qint64 durationMs, bool hasAudio) {
    emit fileOpened(m_engine->filePath());
    Q_UNUSED(hasAudio);
    m_slider->setRange(0, (int)durationMs);
    updateTimeLabels(0);

    DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "VideoWindow: %dx%d dur=%lldms size=%dx%d fullscreen=%d",
               width, height, (long long)durationMs, this->width(), this->height(), isFullScreen());
}

void VideoPlayerWindow::onPositionChanged(qint64 ms) {
    if (!m_sliderDragging) {
        m_slider->blockSignals(true);
        m_slider->setValue((int)ms);
        m_slider->blockSignals(false);
    }
    updateTimeLabels(ms);
}

void VideoPlayerWindow::onStateChanged(int state) {
    VideoPlayerEngine::State s = (VideoPlayerEngine::State)state;
    if (s == VideoPlayerEngine::Playing) {
        m_playBtn->setText(QStringLiteral("⏸"));
        m_playBtn->setToolTip(QCoreApplication::translate("ChatGui", "Pause"));
    } else if (s == VideoPlayerEngine::Paused || s == VideoPlayerEngine::Finished) {
        m_playBtn->setText(QStringLiteral("▶"));
        m_playBtn->setToolTip(QCoreApplication::translate("ChatGui", "Play"));
        showControls();
    }
}

void VideoPlayerWindow::onError(const QString& msg) {
    DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "VideoWindow: %s", qPrintable(msg));
    m_timeLabel->setText(msg);
    showControls();
}

void VideoPlayerWindow::onFramePresented() {
    m_render->setFrame(m_engine->currentFrame());
}

void VideoPlayerWindow::onSliderPressed() {
    m_sliderDragging = true;
}

void VideoPlayerWindow::onSliderReleased() {
    m_sliderDragging = false;
    m_engine->seekMs(m_slider->value());
    if (m_engine->state() == VideoPlayerEngine::Paused ||
        m_engine->state() == VideoPlayerEngine::Finished) {
        m_engine->play();
    }
}

void VideoPlayerWindow::onPlayPause() {
    if (m_engine->state() == VideoPlayerEngine::Playing) m_engine->pause();
    else m_engine->togglePlayPause();
}

void VideoPlayerWindow::onClose() {
    m_engine->close();
    close();
}

void VideoPlayerWindow::onFinished() {
    DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "VideoWindow: playback finished, closing");
    onClose();
}

void VideoPlayerWindow::toggleFullscreen() {
    const QRect before = normalGeometry();
    if (isFullScreen()) {
        showNormal();
        if (before.isValid()) setGeometry(before);
        setControlsVisible(true);
        unsetCursor();
    } else {
        showFullScreen();
        m_hideTimer->start();
    }
    const QRect after = normalGeometry();
    DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "VideoWindow: fullscreen=%d normal_before=%d,%d %dx%d normal_after=%d,%d %dx%d",
               isFullScreen(), before.x(), before.y(), before.width(), before.height(), after.x(), after.y(), after.width(), after.height());
}

void VideoPlayerWindow::setControlsVisible(bool visible) {
    m_controls->setVisible(visible);
    m_closeBtn->setVisible(visible);
}

void VideoPlayerWindow::showControls() {
    setControlsVisible(true);
    unsetCursor();
    if (isFullScreen()) m_hideTimer->start();
}

void VideoPlayerWindow::hideControls() {
    if (!isFullScreen()) return;
    if (m_engine && (m_engine->state() == VideoPlayerEngine::Paused ||
                     m_engine->state() == VideoPlayerEngine::Finished)) return;
    setControlsVisible(false);
    setCursor(Qt::BlankCursor);
}

void VideoPlayerWindow::updateTimeLabels(qint64 posMs) {
    qint64 dur = m_engine ? m_engine->durationMs() : 0;
    m_timeLabel->setText(QString("%1 / %2").arg(fmtTime(posMs), fmtTime(dur)));
}

void VideoPlayerWindow::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Escape) {
        if (isFullScreen()) { toggleFullscreen(); return; }
        onClose();
        return;
    }
    if (e->key() == Qt::Key_Return && (e->modifiers() & Qt::AltModifier)) {
        toggleFullscreen();
        return;
    }
    if (e->key() == Qt::Key_Space) {
        onPlayPause();
        return;
    }
    QWidget::keyPressEvent(e);
}

void VideoPlayerWindow::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) {
        if (isFullScreen()) {
            /* клик в fullscreen — показать/скрыть контролы */
            if (m_controls->isVisible()) hideControls();
            else showControls();
            return;
        }
        ResizeMode m = hitTestResize(e->pos());
        if (m != ResizeNone) {
            beginResize(m, mouseGlobalPos(e));
        } else {
            m_dragging = true;
            m_dragStart = mouseGlobalPos(e);
            m_windowStart = frameGeometry().topLeft();
        }
    }
}

void VideoPlayerWindow::mouseMoveEvent(QMouseEvent* e) {
    if (isFullScreen()) {
        showControls();
        return;
    }
    if (m_resizeMode != ResizeNone && (e->buttons() & Qt::LeftButton)) {
        applyResize(mouseGlobalPos(e));
        return;
    }
    if (m_dragging && (e->buttons() & Qt::LeftButton)) {
        move(m_windowStart + (mouseGlobalPos(e) - m_dragStart));
        return;
    }
    if (!(e->buttons() & Qt::LeftButton))
        updateResizeCursor(e->pos());
}

void VideoPlayerWindow::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) {
        m_dragging = false;
        m_resizeMode = ResizeNone;
    }
}

void VideoPlayerWindow::mouseDoubleClickEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) toggleFullscreen();
}

bool VideoPlayerWindow::eventFilter(QObject* obj, QEvent* ev) {
    /* Резрез за нижний край: панель контролов перекрывает нижнюю кромку окна,
     * поэтому маршрутизируем события из её пустых полей в логику resize. */
    if (obj == m_controls && !isFullScreen()) {
        QEvent::Type t = ev->type();
        if (t == QEvent::MouseButtonPress || t == QEvent::MouseMove || t == QEvent::MouseButtonRelease) {
            QMouseEvent* me = static_cast<QMouseEvent*>(ev);
            QPoint winPos = m_controls->mapTo(this, me->pos());
            ResizeMode m = hitTestResize(winPos);
            if (m == ResizeNone) return QWidget::eventFilter(obj, ev);

            QPoint gp = mouseGlobalPos(me);
            if (t == QEvent::MouseButtonPress && me->button() == Qt::LeftButton) {
                beginResize(m, gp);
                return true;
            }
            if (t == QEvent::MouseMove && m_resizeMode != ResizeNone && (me->buttons() & Qt::LeftButton)) {
                applyResize(gp);
                return true;
            }
            if (t == QEvent::MouseButtonRelease && me->button() == Qt::LeftButton) {
                m_resizeMode = ResizeNone;
                return true;
            }
        }
    }
    return QWidget::eventFilter(obj, ev);
}

void VideoPlayerWindow::closeEvent(QCloseEvent* e) {
    m_engine->close();
    emit closed();
    QWidget::closeEvent(e);
}

void VideoPlayerWindow::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    m_render->setGeometry(rect());
    int ch = 48;
    m_controls->setGeometry(0, height() - ch, width(), ch);
    m_controls->raise();
    m_closeBtn->setGeometry(width() - 48, 8, 40, 32);
    m_closeBtn->raise();
}

VideoPlayerWindow::ResizeMode VideoPlayerWindow::hitTestResize(const QPoint& pos) const {
    const int M = 6;
    int w = width(), h = height();
    bool l = pos.x() <= M;
    bool r = pos.x() >= w - M - 1;
    bool t = pos.y() <= M;
    bool b = pos.y() >= h - M - 1;
    if (t && l) return ResizeTopLeft;
    if (t && r) return ResizeTopRight;
    if (b && l) return ResizeBottomLeft;
    if (b && r) return ResizeBottomRight;
    if (l) return ResizeLeft;
    if (r) return ResizeRight;
    if (t) return ResizeTop;
    if (b) return ResizeBottom;
    return ResizeNone;
}

void VideoPlayerWindow::updateResizeCursor(const QPoint& pos) {
    switch (hitTestResize(pos)) {
        case ResizeTopLeft: case ResizeBottomRight: setCursor(Qt::SizeFDiagCursor); break;
        case ResizeTopRight: case ResizeBottomLeft: setCursor(Qt::SizeBDiagCursor); break;
        case ResizeLeft: case ResizeRight:          setCursor(Qt::SizeHorCursor);   break;
        case ResizeTop: case ResizeBottom:          setCursor(Qt::SizeVerCursor);   break;
        default:                                    unsetCursor();                  break;
    }
}

void VideoPlayerWindow::beginResize(ResizeMode m, const QPoint& globalPos) {
    m_resizeMode = m;
    m_resizeStartGeom = geometry();
    m_resizeStartGlobal = globalPos;
}

void VideoPlayerWindow::applyResize(const QPoint& globalPos) {
    QRect g = m_resizeStartGeom;
    double aspect = (g.height() > 0) ? (double)g.width() / g.height() : 1.0;
    int minW = qMax(minimumWidth(), int(std::ceil(minimumHeight() * aspect)));
    int minH = qMax(minimumHeight(), qRound(minW / aspect));
    int dx = globalPos.x() - m_resizeStartGlobal.x();
    int dy = globalPos.y() - m_resizeStartGlobal.y();

    int newW = g.width(), newH = g.height();
    bool widthDriven = true;
    switch (m_resizeMode) {
        case ResizeRight: case ResizeTopRight: case ResizeBottomRight: newW = g.width() + dx; break;
        case ResizeLeft: case ResizeTopLeft: case ResizeBottomLeft:    newW = g.width() - dx; break;
        case ResizeBottom: newH = g.height() + dy; widthDriven = false; break;
        case ResizeTop:    newH = g.height() - dy; widthDriven = false; break;
        default: return;
    }
    if (widthDriven) newH = qRound(newW / aspect);
    else             newW = qRound(newH * aspect);

    if (newW < minW) { newW = minW; newH = minH; }
    if (newH < minH) { newH = minH; newW = minW; }

    int cx = g.center().x(), cy = g.center().y();
    int left = g.left(), top = g.top();
    switch (m_resizeMode) {
        case ResizeRight:       left = g.left();              top = cy - newH / 2; break;
        case ResizeLeft:        left = g.right() - newW + 1;  top = cy - newH / 2; break;
        case ResizeBottom:      left = cx - newW / 2;         top = g.top();       break;
        case ResizeTop:         left = cx - newW / 2;         top = g.bottom() - newH + 1; break;
        case ResizeTopLeft:     left = g.right() - newW + 1;  top = g.bottom() - newH + 1; break;
        case ResizeTopRight:    left = g.left();              top = g.bottom() - newH + 1; break;
        case ResizeBottomLeft:  left = g.right() - newW + 1;  top = g.top();       break;
        case ResizeBottomRight: left = g.left();              top = g.top();       break;
        default: return;
    }
    setGeometry(left, top, newW, newH);
}

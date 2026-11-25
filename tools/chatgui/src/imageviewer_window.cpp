#include <QCoreApplication>
#include "imageviewer_window.h"
#include "../../lib/debug_config.h"
#include <QPainter>
#include <QImageReader>
#include <QWheelEvent>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QCloseEvent>
#include <QResizeEvent>
#include <QPushButton>
#include <QLabel>
#include <QTimer>
#include <QPropertyAnimation>
#include <QGraphicsOpacityEffect>
#include <QEasingCurve>
#include <QtMath>

static QPointF mouseLocalPos(const QMouseEvent* e) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return e->position();
#else
    return e->localPos();
#endif
}

static QPointF wheelPos(const QWheelEvent* e) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return e->position();
#else
    return e->posF();
#endif
}

ImageViewerWindow::ImageViewerWindow(QWidget *parent)
    : QWidget(parent)
{
    setWindowTitle(QCoreApplication::translate("ChatGui", "Photo"));
    setAttribute(Qt::WA_DeleteOnClose, false);
    setMinimumSize(320, 240);
    resize(800, 600);
    setMouseTracking(true);

    m_closeBtn = new QPushButton(QStringLiteral("✕"), this);
    m_closeBtn->setToolTip(QCoreApplication::translate("ChatGui", "Close"));
    m_closeBtn->setCursor(Qt::ArrowCursor);
    m_closeBtn->setStyleSheet(
        "QPushButton { background: rgba(0,0,0,0.5); color: white; border: none; "
        "border-radius: 4px; font-size: 14px; }"
        "QPushButton:hover { background: rgba(255,255,255,0.25); }");
    connect(m_closeBtn, &QPushButton::clicked, this, &ImageViewerWindow::close);

    m_closeEffect = new QGraphicsOpacityEffect(m_closeBtn);
    m_closeBtn->setGraphicsEffect(m_closeEffect);
    m_closeEffect->setOpacity(0.0);

    m_closeFade = new QPropertyAnimation(m_closeEffect, "opacity", this);
    m_closeFade->setEasingCurve(QEasingCurve::InOutQuad);

    m_hintLabel = new QLabel(QCoreApplication::translate("ChatGui", "Ctrl+wheel — zoom"), this);
    m_hintLabel->setAlignment(Qt::AlignCenter);
    m_hintLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_hintLabel->setStyleSheet(
        "QLabel { background: rgba(0,0,0,0.6); color: white; "
        "border-radius: 12px; padding: 6px 16px; font-size: 13px; }");

    m_hintEffect = new QGraphicsOpacityEffect(m_hintLabel);
    m_hintLabel->setGraphicsEffect(m_hintEffect);
    m_hintEffect->setOpacity(0.0);

    m_hintFade = new QPropertyAnimation(m_hintEffect, "opacity", this);
    m_hintFade->setEasingCurve(QEasingCurve::InOutQuad);

    auto finishHide = [this]() {
        if (m_closeEffect->opacity() < 0.01) m_closeBtn->hide();
        if (m_hintEffect->opacity() < 0.01) m_hintLabel->hide();
    };
    connect(m_closeFade, &QPropertyAnimation::finished, this, finishHide);
    connect(m_hintFade, &QPropertyAnimation::finished, this, finishHide);

    m_hideTimer = new QTimer(this);
    m_hideTimer->setSingleShot(true);
    m_hideTimer->setInterval(1000);
    connect(m_hideTimer, &QTimer::timeout, this, &ImageViewerWindow::hideOverlay);

    m_closeBtn->hide();
    m_hintLabel->hide();
}

void ImageViewerWindow::loadFile(const QString& path) {
    QImageReader reader(path);
    reader.setAutoTransform(true);
    QImage im = reader.read();
    if (im.isNull()) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "ImageViewer: cannot load %s", qPrintable(path));
        m_image = QPixmap();
        update();
        return;
    }
    m_image = QPixmap::fromImage(im);
    resetView();
    update();
    emit fileOpened(path);
}

void ImageViewerWindow::resetView() {
    m_scale = 1.0;
    m_offset = QPointF(0, 0);
    m_dragging = false;
}

void ImageViewerWindow::loadImage(const QImage& image) {
    m_image = QPixmap::fromImage(image); resetView(); update();
}

void ImageViewerWindow::clampOffset() {
    if (m_image.isNull()) { m_offset = QPointF(0, 0); return; }
    double baseW = (double)m_image.width() / width();
    double baseH = (double)m_image.height() / height();
    double effScale = (1.0 / qMax(baseW, baseH)) * m_scale;
    double dw = m_image.width() * effScale;
    double dh = m_image.height() * effScale;
    double mx = qMax((dw - width()) / 2.0, 0.0);
    double my = qMax((dh - height()) / 2.0, 0.0);
    m_offset.setX(qBound(-mx, m_offset.x(), mx));
    m_offset.setY(qBound(-my, m_offset.y(), my));
}

void ImageViewerWindow::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(20, 20, 22));

    if (m_image.isNull()) {
        p.setPen(QColor(150, 150, 150));
        p.drawText(rect(), Qt::AlignCenter, m_placeholder.isEmpty() ?
            QCoreApplication::translate("ChatGui", "Failed to load photo") : m_placeholder);
        return;
    }

    /* базовое вписывание: scale так, чтобы изображение влезло целиком */
    double baseW = (double)m_image.width() / width();
    double baseH = (double)m_image.height() / height();
    double fit = 1.0 / qMax(baseW, baseH);
    double effScale = fit * m_scale;

    double dw = m_image.width() * effScale;
    double dh = m_image.height() * effScale;
    QPointF topLeft((width() - dw) / 2.0 + m_offset.x(),
                    (height() - dh) / 2.0 + m_offset.y());

    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.drawPixmap(QRectF(topLeft, QSizeF(dw, dh)), m_image, QRectF(m_image.rect()));
}

void ImageViewerWindow::wheelEvent(QWheelEvent* e) {
    if (!(e->modifiers() & Qt::ControlModifier)) { e->ignore(); return; }
    if (m_image.isNull()) return;

    double factor = (e->angleDelta().y() > 0) ? 1.2 : (1.0 / 1.2);
    double newScale = qBound(1.0, m_scale * factor, 8.0);
    if (qFuzzyCompare(newScale, m_scale)) return;

    /* зум в точку курсора */
    QPointF cursor = wheelPos(e);
    QPointF center((double)width() / 2.0, (double)height() / 2.0);
    double ratio = newScale / m_scale;
    m_offset = (m_offset - (cursor - center)) * ratio + (cursor - center);
    m_scale = newScale;
    clampOffset();
    update();
    e->accept();
}

void ImageViewerWindow::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton && m_scale > 1.0) {
        m_dragging = true;
        m_dragStart = mouseLocalPos(e);
        m_offsetStart = m_offset;
        setCursor(Qt::ClosedHandCursor);
        e->accept();
        return;
    }
    e->ignore();
}

void ImageViewerWindow::mouseMoveEvent(QMouseEvent* e) {
    showOverlay();
    if (m_dragging) {
        m_offset = m_offsetStart + (mouseLocalPos(e) - m_dragStart);
        clampOffset();
        update();
        e->accept();
    }
}

void ImageViewerWindow::mouseReleaseEvent(QMouseEvent* e) {
    if (m_dragging && e->button() == Qt::LeftButton) {
        m_dragging = false;
        unsetCursor();
        e->accept();
    }
}

void ImageViewerWindow::mouseDoubleClickEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) {
        resetView();
        update();
        e->accept();
    }
}

void ImageViewerWindow::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Escape) {
        close();
        e->accept();
        return;
    }
    QWidget::keyPressEvent(e);
}

void ImageViewerWindow::resizeEvent(QResizeEvent* e) {
    clampOffset();
    m_closeBtn->setGeometry(width() - 48, 8, 40, 32);
    m_closeBtn->raise();
    QSize hs = m_hintLabel->sizeHint();
    m_hintLabel->setGeometry((width() - hs.width()) / 2, height() - hs.height() - 24,
                             hs.width(), hs.height());
    m_hintLabel->raise();
    QWidget::resizeEvent(e);
}

void ImageViewerWindow::showOverlay() {
    m_closeBtn->show();
    m_hintLabel->show();
    fadeTo(m_closeFade, m_closeEffect, 100, 1.0);
    fadeTo(m_hintFade, m_hintEffect, 100, 1.0);
    m_hideTimer->start();
}

void ImageViewerWindow::hideOverlay() {
    fadeTo(m_closeFade, m_closeEffect, 300, 0.0);
    fadeTo(m_hintFade, m_hintEffect, 300, 0.0);
}

void ImageViewerWindow::fadeTo(QPropertyAnimation* anim, QGraphicsOpacityEffect* eff,
                               int durMs, double target) {
    anim->stop();
    anim->setDuration(durMs);
    anim->setStartValue(eff->opacity());
    anim->setEndValue(target);
    anim->start();
}

void ImageViewerWindow::closeEvent(QCloseEvent* e) {
    emit closed();
    QWidget::closeEvent(e);
}
